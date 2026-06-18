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
// FIX BUG-6: g_KbdCallbackObject initialization had a TOCTOU race between
// InterlockedIncrement and ExCreateCallback.  A second device could enter
// the `else` (open-without-create) branch before the first device wrote
// g_KbdCallbackObject, receiving STATUS_OBJECT_NAME_NOT_FOUND and
// decrementing the counter, leaving it at 1 while only one registration
// actually succeeded.  On unregister the last device would then double-
// decrement to -1 and never release the object.
//
// Fix: protect the create/open sequence with a KSPIN_LOCK so only one device
// at a time touches g_KbdCallbackObject and g_KbdCallbackRefCount.  The lock
// is held only around the fast ExCreateCallback call (PASSIVE_LEVEL allowed
// since we use KeAcquireSpinLock at PASSIVE and immediately release).
//
// FIX BUG-7: suppress duration now uses TYPING_SUPPRESS_DURATION_100NS from
// Device.h instead of the inline magic number, so a single change to the
// header updates both the callback and any future callers.
//
// FIX BUG-8 (reference leak on multi-device open):
// ExCreateCallback ALWAYS returns a new object reference in *CallbackObject,
// regardless of whether Create is TRUE or FALSE -- "open without create"
// still adds a reference to the existing object, it doesn't just hand back
// a borrowed pointer. The original code called ExCreateCallback for every
// registration (the first one with Create=TRUE, every subsequent one with
// Create=FALSE to "open" the already-existing object), but only ever
// dereferenced the object ONCE, when the ref-count reaches zero on the
// final unregister.
//
// Net effect: with N devices registering against the shared callback object
// (e.g. AmtPtpDeviceUsbKm + AmtPtpDeviceSpiKm both bound, or repeated
// suspend/resume D0Exit->D0Entry cycles racing a second device's add), each
// "open" call leaks one object reference that is never released -- the
// callback object's internal ref-count grows without bound and the object
// is never freed even after every device unregisters and the driver
// unloads. Over enough sleep/wake cycles this is a slow, unbounded kernel
// object leak.
//
// Fix: immediately drop the reference ExCreateCallback just handed us once
// we're done using the (already-known-valid) pointer for ExRegisterCallback.
// We only need ONE "owning" reference on g_KbdCallbackObject for as long as
// g_KbdCallbackRefCount > 0; that owning reference is the one taken by the
// very first (Create=TRUE) call. Every subsequent open call's reference is
// surplus and must be dereferenced right away.
// ---------------------------------------------------------------------------

#define CALLBACK_OBJECT_NAME L"\\Callback\\AmtPtpKbdActivity"

static PCALLBACK_OBJECT g_KbdCallbackObject   = NULL;
static LONG             g_KbdCallbackRefCount = 0;
static KSPIN_LOCK       g_KbdCallbackLock;
static BOOLEAN          g_KbdCallbackLockInit = FALSE;

//
// Called once from DriverEntry (or equivalent) to initialise the spinlock.
// If multi-device add races DriverEntry we rely on the fact that KMDF
// serialises EvtDeviceAdd calls on a single thread; if you ever call
// Register from a different context, move this into a once-init guard.
//
VOID
AmtPtpInitKeyboardNotificationLock(VOID)
{
    if (!g_KbdCallbackLockInit) {
        KeInitializeSpinLock(&g_KbdCallbackLock);
        g_KbdCallbackLockInit = TRUE;
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

    // FIX BUG-7: derive suppress window from the named constant so it stays
    // consistent with Device.h.  TYPING_SUPPRESS_DURATION_100NS is in
    // 100-ns units; convert to QPC ticks.
    LONGLONG ticksPer100ns = pCtx->PerfFrequency.QuadPart / 10000000LL;
    if (ticksPer100ns == 0) ticksPer100ns = 1;  // guard against very slow QPC
    LONGLONG suppressUntil = now.QuadPart +
        ticksPer100ns * TYPING_SUPPRESS_DURATION_100NS;

    InterlockedExchange64(&pCtx->TypingSuppressUntil, suppressUntil);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
        "%!FUNC! Keyboard activity — suppressing touchpad");
}

VOID
AmtPtpRegisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext)
{
    NTSTATUS          status;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    callbackName;
    KIRQL             oldIrql;
    PCALLBACK_OBJECT  openedObject;

    if (DeviceContext->KbdNotifyHandle != NULL)
        return;

    // Ensure the spinlock is initialised (idempotent).
    AmtPtpInitKeyboardNotificationLock();

    RtlInitUnicodeString(&callbackName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &callbackName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    // FIX BUG-6: hold the spinlock across the increment + ExCreateCallback so
    // concurrent EvtDeviceAdd calls cannot see a stale NULL g_KbdCallbackObject.
    KeAcquireSpinLock(&g_KbdCallbackLock, &oldIrql);

    LONG newCount = InterlockedIncrement(&g_KbdCallbackRefCount);

    if (newCount == 1) {
        status = ExCreateCallback(
            &g_KbdCallbackObject, &oa,
            TRUE,   // create if absent
            TRUE);  // allow multiple registrations
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! ExCreateCallback failed %!STATUS! — typing suppression inactive",
                status);
            InterlockedDecrement(&g_KbdCallbackRefCount);
            KeReleaseSpinLock(&g_KbdCallbackLock, oldIrql);
            return;
        }
        // This is the single owning reference for g_KbdCallbackObject; it is
        // released when the ref-count drops back to zero in Unregister.
    } else {
        // FIX BUG-8: object was already created; this call opens it and
        // returns a NEW reference in openedObject (NOT a borrowed pointer to
        // g_KbdCallbackObject — it is a distinct, independently-refcounted
        // handle to the same underlying object). We only want one long-lived
        // owning reference total, so drop this surplus reference immediately
        // after confirming the open succeeded; g_KbdCallbackObject already
        // points at the live object from the Create=TRUE call above.
        openedObject = NULL;
        status = ExCreateCallback(
            &openedObject, &oa,
            FALSE,  // do not create — must already exist
            TRUE);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
                "%!FUNC! ExCreateCallback (open) failed %!STATUS!", status);
            InterlockedDecrement(&g_KbdCallbackRefCount);
            KeReleaseSpinLock(&g_KbdCallbackLock, oldIrql);
            return;
        }
        // Drop the surplus reference now; g_KbdCallbackObject (from the
        // original Create=TRUE call) remains valid because refCount > 0.
        ObDereferenceObject(openedObject);
    }

    KeReleaseSpinLock(&g_KbdCallbackLock, oldIrql);

    DeviceContext->KbdNotifyHandle = ExRegisterCallback(
        g_KbdCallbackObject,
        AmtPtpKeyboardNotifyCallback,
        DeviceContext);

    if (DeviceContext->KbdNotifyHandle == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExRegisterCallback returned NULL — typing suppression inactive");

        KeAcquireSpinLock(&g_KbdCallbackLock, &oldIrql);
        if (InterlockedDecrement(&g_KbdCallbackRefCount) == 0) {
            ObDereferenceObject(g_KbdCallbackObject);
            g_KbdCallbackObject = NULL;
        }
        KeReleaseSpinLock(&g_KbdCallbackLock, oldIrql);
    } else {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Keyboard notification registered");
    }
}

VOID
AmtPtpUnregisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext)
{
    KIRQL oldIrql;

    if (DeviceContext->KbdNotifyHandle != NULL) {
        ExUnregisterCallback(DeviceContext->KbdNotifyHandle);
        DeviceContext->KbdNotifyHandle = NULL;
    } else {
        return;
    }

    KeAcquireSpinLock(&g_KbdCallbackLock, &oldIrql);
    if (InterlockedDecrement(&g_KbdCallbackRefCount) == 0) {
        if (g_KbdCallbackObject != NULL) {
            ObDereferenceObject(g_KbdCallbackObject);
            g_KbdCallbackObject = NULL;
        }
    }
    KeReleaseSpinLock(&g_KbdCallbackLock, oldIrql);
}