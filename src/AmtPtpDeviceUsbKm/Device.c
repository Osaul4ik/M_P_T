// Device.c - Device handling events. Kernel-mode Driver Framework

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#endif

// Forward declarations
static VOID
AmtPtpKeyboardNotifyCallback(
    _In_ PVOID CallbackContext,
    _In_ PVOID Argument1,
    _In_ PVOID Argument2
);

// AmtPtpGetDeviceConfig
//
// AUDIT FIX (#7): takes the descriptor by pointer instead of by value -
// avoids an unnecessary full-struct copy onto the stack on every call.

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(_In_ const PUSB_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    USHORT id = DeviceDescriptor->idProduct;
    const struct BCM5974_CONFIG* cfg;

    for (cfg = Bcm5974ConfigTable; cfg->identification; ++cfg) {
        if (cfg->identification == id)
            return cfg;
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
        "%!FUNC! Selected generic fallback configuration");
    return &Bcm5974ConfigTable[0];
}

// AmtPtpDeviceUsbKmCreateDevice

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

// AmtPtpDeviceUsbKmEvtDevicePrepareHardware

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

    pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(&pDeviceContext->DeviceDescriptor);
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

// AmtPtpEvtDeviceD0Entry

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

    // Reseed ContactID counter and reset the contact pool on D0Entry.
    // Prevents stale ContactIDs from surviving sleep/wake cycles.
    // NextContactId=0 reserved; first birth pre-increments to 1.
    pDeviceContext->NextContactId        = 0;
    AmtGestureSessionInit(&pDeviceContext->GestureSession);
    pDeviceContext->LastHotPathTraceQpc  = 0;
    pDeviceContext->OverflowCount        = 0;
    AmtContactPoolInit(pDeviceContext->ActiveContacts);

    // Zero RecentLifts on D0Entry to prevent stale retap-smoothing
    // hints from a previous power session.
    RtlZeroMemory(&pDeviceContext->RecentLifts, sizeof(pDeviceContext->RecentLifts));

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

// AmtPtpEvtDeviceD0Exit

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

// SelectInterruptInterface

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

// AmtPtpSetWellspringMode

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

// ---------------------------------------------------------------------
// Keyboard notification / typing suppression
//
// AUDIT FIX (#1, CRITICAL): the original implementation called
// ExCreateCallback()/ExRegisterCallback() - PASSIVE_LEVEL-only APIs -
// while holding a KSPIN_LOCK, which unconditionally raises IRQL to
// DISPATCH_LEVEL. That is an IRQL contract violation on every single
// registration, with a real risk of a bug-check (or, on some build
// configurations, silently undefined behavior) the very first time a
// device is powered up. Fixed by switching the serialization primitive
// from KSPIN_LOCK to a FAST_MUTEX. A fast mutex only raises IRQL to
// APC_LEVEL (never DISPATCH_LEVEL) and is the standard WDM primitive for
// guarding PASSIVE_LEVEL-only object-manager calls like this one.
//
// AUDIT FIX (#6): the lazy one-shot initialisation of the lock itself is
// now a proper atomic check-and-set (InterlockedCompareExchange) instead
// of a bare "if (!flag) { init(); flag = TRUE; }", which was a classic
// unsynchronized double-checked-init data race if two device instances
// ever reached AmtPtpRegisterKeyboardNotification concurrently.
// ---------------------------------------------------------------------

#define CALLBACK_OBJECT_NAME L"\\Callback\\AmtPtpKbdActivity"

static PCALLBACK_OBJECT g_KbdCallbackObject   = NULL;
static LONG             g_KbdCallbackRefCount = 0;
static FAST_MUTEX       g_KbdCallbackMutex;
static LONG             g_KbdCallbackLockInitState = 0; // 0 = not init, 1 = init done

VOID
AmtPtpInitKeyboardNotificationLock(VOID)
{
    // Atomic one-shot init: only the thread that transitions the state
    // from 0 -> 1 performs ExInitializeFastMutex; every other caller
    // (including ones that lose the race) simply returns, exactly as
    // intended by the original (but unsynchronized) lazy-init pattern.
    if (InterlockedCompareExchange(&g_KbdCallbackLockInitState, 1, 0) == 0) {
        ExInitializeFastMutex(&g_KbdCallbackMutex);
    }
}

static VOID
AmtPtpKeyboardNotifyCallback(
    _In_ PVOID CallbackContext,
    _In_ PVOID Argument1,
    _In_ PVOID Argument2)
{
    PDEVICE_CONTEXT pCtx = (PDEVICE_CONTEXT)CallbackContext;
    LARGE_INTEGER   now;

    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    KeQueryPerformanceCounter(&now);

    if (pCtx->PerfFrequency.QuadPart == 0)
        return;

    LONGLONG ticksPer100ns = pCtx->PerfFrequency.QuadPart / 10000000LL;
    if (ticksPer100ns == 0) ticksPer100ns = 1;
    LONGLONG suppressUntil = now.QuadPart +
        ticksPer100ns * TYPING_SUPPRESS_DURATION_100NS;

    InterlockedExchange64(&pCtx->TypingSuppressUntil, suppressUntil);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
        "%!FUNC! Keyboard activity - suppressing touchpad");
}

VOID
AmtPtpRegisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext)
{
    NTSTATUS          status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    callbackName;
    PCALLBACK_OBJECT  openedObject;
    LONG              newCount;

    if (DeviceContext->KbdNotifyHandle != NULL)
        return;

    AmtPtpInitKeyboardNotificationLock();

    RtlInitUnicodeString(&callbackName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &callbackName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    // AUDIT FIX (#1): ExAcquireFastMutex only raises IRQL to APC_LEVEL -
    // safe for the PASSIVE_LEVEL-only ExCreateCallback() calls below,
    // unlike the spinlock previously used here.
    ExAcquireFastMutex(&g_KbdCallbackMutex);

    newCount = InterlockedIncrement(&g_KbdCallbackRefCount);

    if (newCount == 1) {
        status = ExCreateCallback(
            &g_KbdCallbackObject, &oa, TRUE, TRUE);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! ExCreateCallback failed %!STATUS! - typing suppression inactive",
                status);
            InterlockedDecrement(&g_KbdCallbackRefCount);
            ExReleaseFastMutex(&g_KbdCallbackMutex);
            return;
        }
    } else {
        openedObject = NULL;
        status = ExCreateCallback(&openedObject, &oa, FALSE, TRUE);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! ExCreateCallback (open) failed %!STATUS!", status);
            InterlockedDecrement(&g_KbdCallbackRefCount);
            ExReleaseFastMutex(&g_KbdCallbackMutex);
            return;
        }
        ObDereferenceObject(openedObject);
    }

    ExReleaseFastMutex(&g_KbdCallbackMutex);

    DeviceContext->KbdNotifyHandle = ExRegisterCallback(
        g_KbdCallbackObject,
        AmtPtpKeyboardNotifyCallback,
        DeviceContext);

    if (DeviceContext->KbdNotifyHandle == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExRegisterCallback returned NULL - typing suppression inactive");

        ExAcquireFastMutex(&g_KbdCallbackMutex);
        if (InterlockedDecrement(&g_KbdCallbackRefCount) == 0) {
            ObDereferenceObject(g_KbdCallbackObject);
            g_KbdCallbackObject = NULL;
        }
        ExReleaseFastMutex(&g_KbdCallbackMutex);
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
        return;
    }

    // AUDIT FIX (#1): fast mutex instead of spinlock - ObDereferenceObject
    // is also not guaranteed-safe at DISPATCH_LEVEL in all configurations,
    // so this path benefits from the same fix.
    ExAcquireFastMutex(&g_KbdCallbackMutex);
    if (InterlockedDecrement(&g_KbdCallbackRefCount) == 0) {
        if (g_KbdCallbackObject != NULL) {
            ObDereferenceObject(g_KbdCallbackObject);
            g_KbdCallbackObject = NULL;
        }
    }
    ExReleaseFastMutex(&g_KbdCallbackMutex);
}