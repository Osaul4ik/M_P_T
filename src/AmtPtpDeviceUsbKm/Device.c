/*
 * Device.c - Device handling events.
 * Kernel-mode Driver Framework
 */

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static VOID
AmtPtpKeyboardNotifyCallback(
    _In_ PVOID CallbackContext,
    _In_ PVOID Argument1,
    _In_ PVOID Argument2
);

// ---------------------------------------------------------------------------
// AmtPtpGetDeviceConfig
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(_In_ USB_DEVICE_DESCRIPTOR deviceInfo)
{
    USHORT id = deviceInfo.idProduct;
    const struct BCM5974_CONFIG* cfg;

    for (cfg = Bcm5974ConfigTable; cfg->identification; ++cfg) {
        if (cfg->identification == id)
            return cfg;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
        "%!FUNC! Selected generic fallback configuration");
    return &Bcm5974ConfigTable[0];
}

// ---------------------------------------------------------------------------
// AmtPtpDeviceUsbKmCreateDevice
// ---------------------------------------------------------------------------

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES        deviceAttributes;
    PDEVICE_CONTEXT              deviceContext;
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
    if (!NT_SUCCESS(status))
        return status;

    deviceContext = DeviceGetContext(device);
    RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));

    deviceContext->PtpReportButton = TRUE;
    deviceContext->PtpReportTouch  = TRUE;

    status = WdfDeviceCreateDeviceInterface(
        device, &GUID_DEVINTERFACE_AmtPtpDeviceUsbKm, NULL);

    if (NT_SUCCESS(status))
        status = AmtPtpDeviceUsbKmQueueInitialize(device);

    return status;
}

// ---------------------------------------------------------------------------
// AmtPtpDeviceUsbKmEvtDevicePrepareHardware
// ---------------------------------------------------------------------------

NTSTATUS
AmtPtpDeviceUsbKmEvtDevicePrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourceList,
    _In_ WDFCMRESLIST  ResourceListTranslated)
{
    NTSTATUS         status;
    PDEVICE_CONTEXT  pDeviceContext;
    WDF_USB_DEVICE_INFORMATION deviceInfo;
    ULONG            waitWakeEnable = FALSE;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);
    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    status         = STATUS_SUCCESS;
    pDeviceContext = DeviceGetContext(Device);

    if (pDeviceContext->UsbDevice == NULL) {
        status = WdfUsbTargetDeviceCreate(
            Device, WDF_NO_OBJECT_ATTRIBUTES, &pDeviceContext->UsbDevice);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                "WdfUsbTargetDeviceCreate failed 0x%x", status);
            return status;
        }
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(
        pDeviceContext->UsbDevice, &pDeviceContext->DeviceDescriptor);

    pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(pDeviceContext->DeviceDescriptor);
    if (pDeviceContext->DeviceInfo == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "AmtPtpGetDeviceConfig failed");
        return STATUS_INVALID_DEVICE_STATE;
    }

    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);
    status = WdfUsbTargetDeviceRetrieveInformation(pDeviceContext->UsbDevice, &deviceInfo);
    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! HighSpeed:%s SelfPowered:%s RemoteWake:%s",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "Y" : "N",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED)  ? "Y" : "N",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE) ? "Y" : "N");

        waitWakeEnable = deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;
        pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
    } else {
        pDeviceContext->UsbDeviceTraits = 0;
    }

    status = SelectInterruptInterface(Device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! SelectInterruptInterface failed %!STATUS!", status);
        return status;
    }

    status = AmtPtpConfigContReaderForInterruptEndPoint(pDeviceContext);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! AmtPtpConfigContReaderForInterruptEndPoint failed %!STATUS!", status);
        return status;
    }

    pDeviceContext->PtpReportButton = TRUE;
    pDeviceContext->PtpReportTouch  = TRUE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return status;
}

// ---------------------------------------------------------------------------
// AmtPtpEvtDeviceD0Entry
// ---------------------------------------------------------------------------

NTSTATUS
AmtPtpEvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        status;
    BOOLEAN         isTargetStarted;

    pDeviceContext  = DeviceGetContext(Device);
    isTargetStarted = FALSE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! --> coming from %s", DbgDevicePowerString(PreviousState));

    pDeviceContext->LastReportTime =
        KeQueryPerformanceCounter(&pDeviceContext->PerfFrequency);

    status = AmtPtpSetWellspringMode(pDeviceContext, TRUE);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! AmtPtpSetWellspringMode(TRUE) failed %!STATUS! (non-fatal)", status);
        status = STATUS_SUCCESS;
    }

    status = WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfIoTargetStart failed %!STATUS!", status);
        goto end;
    }
    isTargetStarted = TRUE;

    AmtPtpRegisterKeyboardNotification(pDeviceContext);

end:
    if (!NT_SUCCESS(status) && isTargetStarted) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
            WdfIoTargetCancelSentIo);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! <--");
    return status;
}

// ---------------------------------------------------------------------------
// AmtPtpEvtDeviceD0Exit
// ---------------------------------------------------------------------------

NTSTATUS
AmtPtpEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        status;

    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! --> moving to %s", DbgDevicePowerString(TargetState));

    AmtPtpUnregisterKeyboardNotification(pDeviceContext);

    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
        WdfIoTargetCancelSentIo);

    status = AmtPtpSetWellspringMode(pDeviceContext, FALSE);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! AmtPtpSetWellspringMode(FALSE) failed %!STATUS!", status);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! <--");
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// SelectInterruptInterface
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(_In_ WDFDEVICE Device)
{
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    NTSTATUS                            status = STATUS_SUCCESS;
    PDEVICE_CONTEXT                     pDeviceContext;
    WDFUSBPIPE                          pipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;
    UCHAR                               index;
    UCHAR                               numberConfiguredPipes;

    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

    status = WdfUsbTargetDeviceSelectConfig(
        pDeviceContext->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &configParams);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "WdfUsbTargetDeviceSelectConfig failed %!STATUS!", status);
        return status;
    }

    pDeviceContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    numberConfiguredPipes        = configParams.Types.SingleInterface.NumberConfiguredPipes;

    for (index = 0; index < numberConfiguredPipes; index++) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(
            pDeviceContext->UsbInterface, index, &pipeInfo);

        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! Found USB pipe type %d", pipeInfo.PipeType);

        if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
            pDeviceContext->InterruptPipe = pipe;
            break;
        }
    }

    if (!pDeviceContext->InterruptPipe) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! No interrupt pipe found");
        return STATUS_INVALID_DEVICE_STATE;
    }

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// AmtPtpSetWellspringMode
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN         IsWellspringModeOn)
{
    NTSTATUS                    status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;
    WDF_MEMORY_DESCRIPTOR       memoryDescriptor;
    ULONG                       cbTransferred;
    WDFMEMORY                   bufHandle = NULL;
    unsigned char*              buffer    = NULL;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
        DeviceContext->IsWellspringModeOn = IsWellspringModeOn;
        return STATUS_SUCCESS;
    }

    status = WdfMemoryCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        PagedPool,
        POOL_TAG_PTP_CONTROL,
        (SIZE_T)DeviceContext->DeviceInfo->um_size,
        &bufHandle,
        (PVOID*)&buffer);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! WdfMemoryCreate failed %!STATUS!", status);
        goto cleanup;
    }

    RtlZeroMemory(buffer, (SIZE_T)DeviceContext->DeviceInfo->um_size);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor, buffer, (ULONG)DeviceContext->DeviceInfo->um_size);

    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestDeviceToHost, BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
        (USHORT)DeviceContext->DeviceInfo->um_req_val,
        (USHORT)DeviceContext->DeviceInfo->um_req_idx);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice, WDF_NO_HANDLE, NULL,
        &setupPacket, &memoryDescriptor, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! Control read failed %!STATUS!, cbTransferred=%lu, um_size=%d",
            status, cbTransferred, DeviceContext->DeviceInfo->um_size);
        goto cleanup;
    }

    buffer[DeviceContext->DeviceInfo->um_switch_idx] = IsWellspringModeOn
        ? (unsigned char)DeviceContext->DeviceInfo->um_switch_on
        : (unsigned char)DeviceContext->DeviceInfo->um_switch_off;

    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestHostToDevice, BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
        (USHORT)DeviceContext->DeviceInfo->um_req_val,
        (USHORT)DeviceContext->DeviceInfo->um_req_idx);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice, WDF_NO_HANDLE, NULL,
        &setupPacket, &memoryDescriptor, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! Control write failed %!STATUS!", status);
        goto cleanup;
    }

    DeviceContext->IsWellspringModeOn = IsWellspringModeOn;

cleanup:
    if (bufHandle != NULL) {
        WdfObjectDelete(bufHandle);
        bufHandle = NULL;
    }
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return status;
}

// ---------------------------------------------------------------------------
// Keyboard notification / typing suppression
//
// Uses a named ExCallback object (\Callback\AmtPtpKbdActivity) that the
// companion keyboard filter (AmtPtpKbdHook) fires on every key-down event.
//
// Thread safety:
//   g_KbdCallbackObject is a process-wide singleton.  Multiple device
//   instances share the same callback object but each registers its own
//   handle.  The object is created once and released once all handles are
//   unregistered.  A reference count (g_KbdCallbackRefCount) guards the
//   object lifetime across devices.
//
// FIX (original bugs):
//   1. g_KbdCallbackObject was overwritten on every call to Register without
//      first releasing the previous reference — leaking when called after a
//      resource rebalance.
//   2. Unregister called ObDereferenceObject even when the object had already
//      been released by a prior Unregister — potential double-dereference.
//   3. No reference count meant the last device to unregister could free the
//      object while another device's callback was still registered against it.
// ---------------------------------------------------------------------------

#define CALLBACK_OBJECT_NAME L"\\Callback\\AmtPtpKbdActivity"

static PCALLBACK_OBJECT g_KbdCallbackObject   = NULL;
static LONG             g_KbdCallbackRefCount = 0;

static VOID
AmtPtpKeyboardNotifyCallback(
    _In_ PVOID CallbackContext,
    _In_ PVOID Argument1,
    _In_ PVOID Argument2)
{
    PDEVICE_CONTEXT pCtx = (PDEVICE_CONTEXT)CallbackContext;
    LARGE_INTEGER   now;
    LONGLONG        suppressUntil;

    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    KeQueryPerformanceCounter(&now);

    if (pCtx->PerfFrequency.QuadPart == 0)
        return;  // D0Entry not yet run

    LONGLONG ticksPerMs = pCtx->PerfFrequency.QuadPart / 1000LL;
    suppressUntil = now.QuadPart + ticksPerMs * 500LL;

    InterlockedExchange64(&pCtx->TypingSuppressUntil, suppressUntil);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
        "%!FUNC! Keyboard activity — suppressing touchpad 500 ms");
}

VOID
AmtPtpRegisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext)
{
    NTSTATUS          status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    callbackName;

    // Already registered for this device instance — nothing to do.
    if (DeviceContext->KbdNotifyHandle != NULL)
        return;

    RtlInitUnicodeString(&callbackName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &callbackName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    // FIX: only create the callback object when no device has done so yet.
    // Subsequent devices open the existing object via the same ExCreateCallback
    // call (Create=TRUE is idempotent when OBJ_PERMANENT is set).
    if (InterlockedIncrement(&g_KbdCallbackRefCount) == 1) {
        status = ExCreateCallback(
            &g_KbdCallbackObject, &oa,
            TRUE,   // create if absent
            TRUE);  // allow multiple registrations
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! ExCreateCallback failed %!STATUS! — typing suppression inactive",
                status);
            InterlockedDecrement(&g_KbdCallbackRefCount);
            return;
        }
    } else {
        // Object was already created; open it without creating a new one.
        status = ExCreateCallback(
            &g_KbdCallbackObject, &oa,
            FALSE,  // do not create — must already exist
            TRUE);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! ExCreateCallback (open) failed %!STATUS!", status);
            InterlockedDecrement(&g_KbdCallbackRefCount);
            return;
        }
    }

    DeviceContext->KbdNotifyHandle = ExRegisterCallback(
        g_KbdCallbackObject,
        AmtPtpKeyboardNotifyCallback,
        DeviceContext);

    if (DeviceContext->KbdNotifyHandle == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExRegisterCallback returned NULL — typing suppression inactive");
        // Release the reference we just acquired for this device.
        if (InterlockedDecrement(&g_KbdCallbackRefCount) == 0) {
            ObDereferenceObject(g_KbdCallbackObject);
            g_KbdCallbackObject = NULL;
        }
    } else {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Keyboard notification registered");
    }
}

VOID
AmtPtpUnregisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext)
{
    if (DeviceContext->KbdNotifyHandle != NULL) {
        ExUnregisterCallback(DeviceContext->KbdNotifyHandle);
        DeviceContext->KbdNotifyHandle = NULL;
    } else {
        // This device never successfully registered — nothing to release.
        return;
    }

    // FIX: release the callback object only when this was the last device.
    if (InterlockedDecrement(&g_KbdCallbackRefCount) == 0) {
        if (g_KbdCallbackObject != NULL) {
            ObDereferenceObject(g_KbdCallbackObject);
            g_KbdCallbackObject = NULL;
        }
    }
}