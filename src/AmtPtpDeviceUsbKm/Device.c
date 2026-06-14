/*++

Module Name:

    Device.c

Abstract:

    Device lifecycle callbacks:
        - AmtPtpDeviceUsbKmCreateDevice
        - AmtPtpDeviceUsbKmEvtDevicePrepareHardware
        - AmtPtpEvtDeviceD0Entry   (starts USB reader + processing thread)
        - AmtPtpEvtDeviceD0Exit    (stops both in the right order)
        - SelectInterruptInterface
        - AmtPtpSetWellspringMode
        - AmtPtpResetSlotState

    Threading:
        All functions in this file run at PASSIVE_LEVEL.

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"
#include "include/ProcessingThread.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text(PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#pragma alloc_text(PAGE, AmtPtpEvtDeviceD0Entry)
#pragma alloc_text(PAGE, AmtPtpEvtDeviceD0Exit)
#pragma alloc_text(PAGE, SelectInterruptInterface)
#pragma alloc_text(PAGE, AmtPtpSetWellspringMode)
#pragma alloc_text(PAGE, AmtPtpResetSlotState)
#endif

// ---- private helpers -------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(
    _In_ USB_DEVICE_DESCRIPTOR deviceInfo)
{
    USHORT id = deviceInfo.idProduct;
    const struct BCM5974_CONFIG* cfg;

    for (cfg = Bcm5974ConfigTable; cfg->identification; ++cfg) {
        if (cfg->identification == id) {
            return cfg;
        }
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
        "%!FUNC! No matching config found — using generic fallback");

    return &Bcm5974ConfigTable[0];
}

// ---- slot reset ------------------------------------------------------------

VOID
AmtPtpResetSlotState(
    _In_ PDEVICE_CONTEXT pCtx)
{
    RtlZeroMemory(pCtx->SlotInUse,          sizeof(pCtx->SlotInUse));
    RtlZeroMemory(pCtx->SlotPendingRelease, sizeof(pCtx->SlotPendingRelease));
    RtlZeroMemory(pCtx->SlotCooldown,       sizeof(pCtx->SlotCooldown));
    RtlZeroMemory(pCtx->SlotTipConfirmed,   sizeof(pCtx->SlotTipConfirmed));
    RtlZeroMemory(pCtx->SlotFingerKey,      sizeof(pCtx->SlotFingerKey));
    RtlZeroMemory(pCtx->LastNormX,          sizeof(pCtx->LastNormX));
    RtlZeroMemory(pCtx->LastNormY,          sizeof(pCtx->LastNormY));
    RtlZeroMemory(pCtx->HystX,              sizeof(pCtx->HystX));
    RtlZeroMemory(pCtx->HystY,              sizeof(pCtx->HystY));
}

// ---- CreateDevice ----------------------------------------------------------

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES        deviceAttributes;
    PDEVICE_CONTEXT              pCtx;
    WDFDEVICE                    device;
    NTSTATUS                     status;

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = AmtPtpEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = AmtPtpEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pCtx = DeviceGetContext(device);

    // PTP reporting defaults.
    pCtx->PtpReportButton = TRUE;
    pCtx->PtpReportTouch  = TRUE;

    // Ring buffer.
    RingBufferInit(&pCtx->RingBuffer);

    // Processing-thread events.
    KeInitializeEvent(&pCtx->ProcEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&pCtx->StopEvent, NotificationEvent,    FALSE);

    status = WdfDeviceCreateDeviceInterface(
        device,
        &GUID_DEVINTERFACE_AmtPtpDeviceUsbKm,
        NULL);

    if (NT_SUCCESS(status)) {
        status = AmtPtpDeviceUsbKmQueueInitialize(device);
    }

    return status;
}

// ---- PrepareHardware -------------------------------------------------------

NTSTATUS
AmtPtpDeviceUsbKmEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated)
{
    NTSTATUS        status;
    PDEVICE_CONTEXT pCtx;
    WDF_USB_DEVICE_INFORMATION devInfo;
    ULONG           waitWakeEnable = FALSE;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    pCtx   = DeviceGetContext(Device);
    status = STATUS_SUCCESS;

    // Create USB device target once.
    if (pCtx->UsbDevice == NULL) {
        status = WdfUsbTargetDeviceCreate(
            Device, WDF_NO_OBJECT_ATTRIBUTES, &pCtx->UsbDevice);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                "WdfUsbTargetDeviceCreate failed 0x%x", status);
            return status;
        }
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(pCtx->UsbDevice, &pCtx->DeviceDescriptor);

    pCtx->DeviceInfo = AmtPtpGetDeviceConfig(pCtx->DeviceDescriptor);
    if (pCtx->DeviceInfo == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! AmtPtpGetDeviceConfig failed");
        return STATUS_INVALID_DEVICE_STATE;
    }

    WDF_USB_DEVICE_INFORMATION_INIT(&devInfo);
    status = WdfUsbTargetDeviceRetrieveInformation(pCtx->UsbDevice, &devInfo);
    if (NT_SUCCESS(status)) {
        waitWakeEnable = devInfo.Traits & WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! HighSpeed=%s SelfPowered=%s RemoteWake=%s",
            (devInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED)  ? "Y" : "N",
            (devInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED)   ? "Y" : "N",
            waitWakeEnable ? "Y" : "N");
        pCtx->UsbDeviceTraits = devInfo.Traits;
    } else {
        pCtx->UsbDeviceTraits = 0;
    }

    status = SelectInterruptInterface(Device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! SelectInterruptInterface failed %!STATUS!", status);
        return status;
    }

    status = AmtPtpConfigContReaderForInterruptEndPoint(pCtx);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! ConfigContReader failed %!STATUS!", status);
        return status;
    }

    // Re-init ring and slot state (survives PrepareHardware re-entry).
    RingBufferInit(&pCtx->RingBuffer);
    AmtPtpResetSlotState(pCtx);

    pCtx->PtpReportButton = TRUE;
    pCtx->PtpReportTouch  = TRUE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}

// ---- D0 Entry --------------------------------------------------------------

NTSTATUS
AmtPtpEvtDeviceD0Entry(
    _In_ WDFDEVICE          Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT pCtx;
    NTSTATUS        status;

    PAGED_CODE();

    pCtx = DeviceGetContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! from %s", DbgDevicePowerString(PreviousState));

    // Re-entering D0: clear stale contacts and ring.
    AmtPtpResetSlotState(pCtx);
    RingBufferInit(&pCtx->RingBuffer);

    // Reset StopEvent so the thread doesn't exit immediately.
    KeClearEvent(&pCtx->StopEvent);

    // Enable Wellspring mode if needed.
    if (pCtx->PtpReportButton || pCtx->PtpReportTouch) {
        status = AmtPtpSetWellspringMode(pCtx, TRUE);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! SetWellspringMode failed %!STATUS! (continuing)", status);
        }
    }

    KeQueryPerformanceCounter(&pCtx->LastReportTime);

    // Start the processing thread BEFORE the USB reader so that no packet
    // can arrive before the thread is ready to consume it.
    status = ProcThreadStart(pCtx);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! ProcThreadStart failed %!STATUS!", status);
        return status;
    }

    // Start the USB interrupt pipe reader.
    status = WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(pCtx->InterruptPipe));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfIoTargetStart failed %!STATUS!", status);
        // Stop the thread we just created.
        ProcThreadStop(pCtx);
        return status;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! OK");
    return STATUS_SUCCESS;
}

// ---- D0 Exit ---------------------------------------------------------------

NTSTATUS
AmtPtpEvtDeviceD0Exit(
    _In_ WDFDEVICE          Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT pCtx;

    PAGED_CODE();

    pCtx = DeviceGetContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! to %s", DbgDevicePowerString(TargetState));

    // 1. Stop USB reader first so no new packets enter the ring.
    if (pCtx->InterruptPipe != NULL) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(pCtx->InterruptPipe),
            WdfIoTargetCancelSentIo);
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! InterruptPipe is NULL — skip pipe stop");
    }

    // 2. Signal processing thread to exit and wait for it.
    ProcThreadStop(pCtx);

    // 3. Cancel Wellspring mode.
    NTSTATUS status = AmtPtpSetWellspringMode(pCtx, FALSE);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! SetWellspringMode(OFF) failed %!STATUS! (ignored)", status);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! dropped=%d processed=%d",
        pCtx->DroppedPackets, pCtx->ProcessedPackets);

    return STATUS_SUCCESS;
}

// ---- SelectInterruptInterface ----------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
    _In_ WDFDEVICE Device)
{
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    NTSTATUS                            status;
    PDEVICE_CONTEXT                     pCtx;
    WDFUSBPIPE                          pipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;
    UCHAR                               index;
    UCHAR                               numPipes;

    PAGED_CODE();

    pCtx   = DeviceGetContext(Device);
    status = STATUS_SUCCESS;

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);
    status = WdfUsbTargetDeviceSelectConfig(
        pCtx->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &configParams);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WdfUsbTargetDeviceSelectConfig failed %!STATUS!", status);
        return status;
    }

    pCtx->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    numPipes           = configParams.Types.SingleInterface.NumberConfiguredPipes;

    for (index = 0; index < numPipes; index++) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(pCtx->UsbInterface, index, &pipeInfo);
        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! pipe[%d] type=%d", index, pipeInfo.PipeType);

        if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
            pCtx->InterruptPipe = pipe;
            break;
        }
    }

    if (!pCtx->InterruptPipe) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! No interrupt pipe found");
        return STATUS_INVALID_DEVICE_STATE;
    }

    return STATUS_SUCCESS;
}

// ---- AmtPtpSetWellspringMode -----------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ BOOLEAN         IsOn)
{
    NTSTATUS                     status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;
    WDF_MEMORY_DESCRIPTOR        memDesc;
    ULONG                        cbTransferred;
    WDFMEMORY                    bufHandle = NULL;
    UCHAR*                       buffer;

    PAGED_CODE();
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry IsOn=%d", IsOn);

    // TYPE3 needs no USB control message.
    if (pCtx->DeviceInfo->tp_type == TYPE3) {
        pCtx->IsWellspringModeOn = IsOn;
        return STATUS_SUCCESS;
    }

    status = WdfMemoryCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        PagedPool,
        POOL_TAG_PTP_CONTROL,
        pCtx->DeviceInfo->um_size,
        &bufHandle,
        (PVOID*)&buffer);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    RtlZeroMemory(buffer, pCtx->DeviceInfo->um_size);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, buffer,
        (ULONG)pCtx->DeviceInfo->um_size);

    // Read current mode register.
    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestDeviceToHost,
        BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
        (USHORT)pCtx->DeviceInfo->um_req_val,
        (USHORT)pCtx->DeviceInfo->um_req_idx);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        pCtx->UsbDevice, WDF_NO_HANDLE, NULL,
        &setupPacket, &memDesc, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! Control read failed %!STATUS! cbTransferred=%lu um_size=%d",
            status, cbTransferred, pCtx->DeviceInfo->um_size);
        goto cleanup;
    }

    // Flip the mode switch byte.
    buffer[pCtx->DeviceInfo->um_switch_idx] = IsOn
        ? (UCHAR)pCtx->DeviceInfo->um_switch_on
        : (UCHAR)pCtx->DeviceInfo->um_switch_off;

    // Write back.
    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestHostToDevice,
        BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
        (USHORT)pCtx->DeviceInfo->um_req_val,
        (USHORT)pCtx->DeviceInfo->um_req_idx);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        pCtx->UsbDevice, WDF_NO_HANDLE, NULL,
        &setupPacket, &memDesc, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! Control write failed %!STATUS!", status);
        goto cleanup;
    }

    pCtx->IsWellspringModeOn = IsOn;

cleanup:
    if (bufHandle != NULL) {
        WdfObjectDelete(bufHandle);
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit %!STATUS!", status);
    return status;
}
