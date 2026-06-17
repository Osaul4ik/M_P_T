/*++

Module Name:

    Device.c - Device handling events.

Abstract:

    This file contains the device entry points and callbacks.

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#endif

// ---------------------------------------------------------------------------
// Forward declaration of the keyboard notification callback defined further
// down in this file.
// ---------------------------------------------------------------------------
static VOID
AmtPtpKeyboardNotifyCallback(
    _In_ PVOID  CallbackContext,
    _In_ PVOID  Argument1,
    _In_ PVOID  Argument2
);

// ---------------------------------------------------------------------------
// AmtPtpGetDeviceConfig
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(
    _In_ USB_DEVICE_DESCRIPTOR deviceInfo
)
{
    USHORT id = deviceInfo.idProduct;
    const struct BCM5974_CONFIG* cfg;

    for (cfg = Bcm5974ConfigTable; cfg->identification; ++cfg) {
        if (cfg->identification == id) {
            return cfg;
        }
    }

    // Generic fallback (first entry, identification == USB_DEVICE_ID_DEFAULT_FALLBACK)
    TraceEvents(
        TRACE_LEVEL_WARNING,
        TRACE_DRIVER,
        "%!FUNC! Selected a generic fallback configuration"
    );

    return &Bcm5974ConfigTable[0];
}

// ---------------------------------------------------------------------------
// AmtPtpDeviceUsbKmCreateDevice
// ---------------------------------------------------------------------------

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
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

    if (NT_SUCCESS(status)) {
        deviceContext = DeviceGetContext(device);

        //
        // Zero the context so all fields start clean (important for
        // TypingSuppressUntil, KbdNotifyHandle, PerfFrequency, etc.).
        //
        RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));

        deviceContext->PtpReportButton = TRUE;
        deviceContext->PtpReportTouch  = TRUE;

        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_AmtPtpDeviceUsbKm,
            NULL
            );

        if (NT_SUCCESS(status)) {
            status = AmtPtpDeviceUsbKmQueueInitialize(device);
        }
    }

    return status;
}

// ---------------------------------------------------------------------------
// AmtPtpDeviceUsbKmEvtDevicePrepareHardware
// ---------------------------------------------------------------------------

NTSTATUS
AmtPtpDeviceUsbKmEvtDevicePrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourceList,
    _In_ WDFCMRESLIST  ResourceListTranslated
    )
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

    //
    // Create USB device handle once; reuse it on resource rebalance.
    //
    if (pDeviceContext->UsbDevice == NULL) {
        status = WdfUsbTargetDeviceCreate(
            Device,
            WDF_NO_OBJECT_ATTRIBUTES,
            &pDeviceContext->UsbDevice
        );

        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                "WdfUsbTargetDeviceCreate failed 0x%x", status);
            return status;
        }
    }

    // Retrieve USB device descriptor.
    WdfUsbTargetDeviceGetDeviceDescriptor(
        pDeviceContext->UsbDevice,
        &pDeviceContext->DeviceDescriptor
    );

    // Obtain device-specific configuration.
    pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(pDeviceContext->DeviceDescriptor);
    if (pDeviceContext->DeviceInfo == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "AmtPtpGetDeviceConfig failed to find device config");
        return STATUS_INVALID_DEVICE_STATE;
    }

    // Retrieve USB device capability flags.
    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);
    status = WdfUsbTargetDeviceRetrieveInformation(
        pDeviceContext->UsbDevice,
        &deviceInfo
    );

    if (NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! IsDeviceHighSpeed: %s",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "TRUE" : "FALSE");
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! IsDeviceSelfPowered: %s",
            (deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED) ? "TRUE" : "FALSE");

        waitWakeEnable = deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! IsDeviceRemoteWakeable: %s",
            waitWakeEnable ? "TRUE" : "FALSE");

        pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
    }
    else {
        pDeviceContext->UsbDeviceTraits = 0;
    }

    // Select and cache the interrupt pipe.
    status = SelectInterruptInterface(Device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! SelectInterruptInterface failed %!STATUS!", status);
        return status;
    }

    // Configure the continuous reader.
    status = AmtPtpConfigContReaderForInterruptEndPoint(pDeviceContext);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! AmtPtpConfigContReaderForInterruptEndPoint failed %!STATUS!", status);
        return status;
    }

    // Default reporting state.
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
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        status;
    BOOLEAN         isTargetStarted;

    pDeviceContext   = DeviceGetContext(Device);
    isTargetStarted  = FALSE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! --> coming from %s",
        DbgDevicePowerString(PreviousState));

    //
    // Cache the QPC frequency so interrupt code avoids repeated lookups.
    // KeQueryPerformanceCounter(NULL) returns the current counter value and
    // optionally writes the frequency into its argument — but its signature is
    // LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *Frequency).
    // Passing a pointer to PerfFrequency captures the frequency directly.
    //
    // Seed LastReportTime in the same call.
    //
    pDeviceContext->LastReportTime =
        KeQueryPerformanceCounter(&pDeviceContext->PerfFrequency);

    //
    // BUG FIX: Original code only activated Wellspring mode when
    // PtpReportButton || IsWellspringModeOn, meaning the very first D0Entry
    // with PtpReportButton == FALSE would skip mode activation entirely.
    //
    // Correct behaviour: activate Wellspring unconditionally on D0Entry if
    // the device type requires it (non-TYPE3).  IsWellspringModeOn tracks
    // whether the USB control transfer actually succeeded, not whether we
    // *want* it on.
    //
    status = AmtPtpSetWellspringMode(pDeviceContext, TRUE);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! AmtPtpSetWellspringMode(TRUE) failed %!STATUS! (non-fatal)",
            status);
        // Non-fatal: device may still function in basic mode.
        status = STATUS_SUCCESS;
    }

    // Start the continuous reader.
    status = WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe)
    );
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfIoTargetStart failed %!STATUS!", status);
        goto end;
    }
    isTargetStarted = TRUE;

    // Register keyboard activity notification for typing suppression.
    AmtPtpRegisterKeyboardNotification(pDeviceContext);

end:
    if (!NT_SUCCESS(status)) {
        if (isTargetStarted) {
            WdfIoTargetStop(
                WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
                WdfIoTargetCancelSentIo
            );
        }
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
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        status;

    PAGED_CODE();

    status         = STATUS_SUCCESS;
    pDeviceContext = DeviceGetContext(Device);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! --> moving to %s",
        DbgDevicePowerString(TargetState));

    // Unregister keyboard callback before stopping the pipe.
    AmtPtpUnregisterKeyboardNotification(pDeviceContext);

    // Stop the interrupt pipe.
    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
        WdfIoTargetCancelSentIo
    );

    // Turn off Wellspring mode.
    status = AmtPtpSetWellspringMode(pDeviceContext, FALSE);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! AmtPtpSetWellspringMode(FALSE) failed %!STATUS!",
            status);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! <--");
    return STATUS_SUCCESS;   // D0Exit failures are not propagated
}

// ---------------------------------------------------------------------------
// SelectInterruptInterface
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
    _In_ WDFDEVICE Device
)
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
        pDeviceContext->UsbDevice,
        WDF_NO_OBJECT_ATTRIBUTES,
        &configParams
    );

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
            pDeviceContext->UsbInterface,
            index,
            &pipeInfo
        );

        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
            "%!FUNC! Found USB pipe type %d", pipeInfo.PipeType);

        if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
            pDeviceContext->InterruptPipe = pipe;
            break;
        }
    }

    if (!pDeviceContext->InterruptPipe) {
        status = STATUS_INVALID_DEVICE_STATE;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! No interrupt pipe found %!STATUS!", status);
        return status;
    }

    return status;
}

// ---------------------------------------------------------------------------
// AmtPtpSetWellspringMode
//
// BUG FIX 1: WDF_MEMORY_DESCRIPTOR_INIT_BUFFER used sizeof(um_size) (== 4
//            bytes, the size of an int) instead of the actual um_size value.
//            Fixed to pass DeviceContext->DeviceInfo->um_size directly.
//
// BUG FIX 2: WdfObjectDelete was called unconditionally in cleanup even when
//            WdfMemoryCreate returned an error and bufHandle was still NULL.
//            Although WdfObjectDelete(NULL) is documented as a no-op for
//            WDF handles obtained from WDF APIs, it is cleaner and safer to
//            guard the call, and we now initialise bufHandle to NULL before
//            the create call so the cleanup path is always safe.
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN         IsWellspringModeOn
)
{
    NTSTATUS                    status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;
    WDF_MEMORY_DESCRIPTOR       memoryDescriptor;
    ULONG                       cbTransferred;
    WDFMEMORY                   bufHandle = NULL;   // always NULL-initialised
    unsigned char*              buffer    = NULL;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    // TYPE3 devices do not use a mode-switch USB message.
    if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
        DeviceContext->IsWellspringModeOn = IsWellspringModeOn;
        return STATUS_SUCCESS;
    }

    //
    // Allocate the control-transfer buffer.
    // um_size is the *value* (number of bytes), not the size of the field
    // that holds it — was sizeof(int) == 4 in the original code.
    //
    status = WdfMemoryCreate(
        WDF_NO_OBJECT_ATTRIBUTES,
        PagedPool,
        POOL_TAG_PTP_CONTROL,
        (SIZE_T)DeviceContext->DeviceInfo->um_size,  // FIX: was sizeof(um_size)
        &bufHandle,
        (PVOID*)&buffer
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! WdfMemoryCreate failed %!STATUS!", status);
        goto cleanup;
    }

    RtlZeroMemory(buffer, (SIZE_T)DeviceContext->DeviceInfo->um_size);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor,
        buffer,
        (ULONG)DeviceContext->DeviceInfo->um_size   // FIX: was sizeof(um_size)
    );

    // Read current mode configuration from device.
    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestDeviceToHost,
        BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
        (USHORT)DeviceContext->DeviceInfo->um_req_val,
        (USHORT)DeviceContext->DeviceInfo->um_req_idx
    );
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice,
        WDF_NO_HANDLE,
        NULL,
        &setupPacket,
        &memoryDescriptor,
        &cbTransferred
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! Control read failed %!STATUS!, cbTransferred=%lu, um_size=%d",
            status, cbTransferred, DeviceContext->DeviceInfo->um_size);
        goto cleanup;
    }

    // Flip the mode-switch byte.
    buffer[DeviceContext->DeviceInfo->um_switch_idx] = IsWellspringModeOn
        ? (unsigned char)DeviceContext->DeviceInfo->um_switch_on
        : (unsigned char)DeviceContext->DeviceInfo->um_switch_off;

    // Write updated configuration back to device.
    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestHostToDevice,
        BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
        (USHORT)DeviceContext->DeviceInfo->um_req_val,
        (USHORT)DeviceContext->DeviceInfo->um_req_idx
    );
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice,
        WDF_NO_HANDLE,
        NULL,
        &setupPacket,
        &memoryDescriptor,
        &cbTransferred
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! Control write failed %!STATUS!", status);
        goto cleanup;
    }

    DeviceContext->IsWellspringModeOn = IsWellspringModeOn;

cleanup:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");

    // bufHandle is NULL if WdfMemoryCreate failed, which is safe here.
    if (bufHandle != NULL) {
        WdfObjectDelete(bufHandle);
        bufHandle = NULL;
    }

    return status;
}

// ---------------------------------------------------------------------------
// Keyboard notification / typing suppression
//
// We use ExRegisterCallback with the \Callback\SetSystemTime object as a
// portable PASSIVE_LEVEL notification that fires at PASSIVE_LEVEL.  That
// object is not keyboard-specific, but the cleanest kernel-mode approach
// that does not require a keyboard filter driver is to hook into the
// keyboard class driver via a WMI or PnP notification.
//
// For a simpler, self-contained solution we register a callback on the
// \Callback\PowerState object (always present) and use it only as a
// placeholder skeleton.  The actual keyboard hookup uses
// IoRegisterPlugPlayNotification on keyboard device interface arrivals and
// sets a flag from a completion routine injected via IoAttachDevice.
//
// HOWEVER — the cleanest approach within a HID minidriver that already sits
// in the USB HID stack is to watch for WM_KEYDOWN via the shared HID queue.
// Since this driver owns the touchpad HID path, not the keyboard path, the
// most practical in-box mechanism is:
//
//   1. Register for PnP keyboard interface change notifications.
//   2. On arrival, open the keyboard HID device and pend a read.
//   3. On each read completion bump TypingSuppressUntil by 500 ms.
//
// That requires substantial additional code (IoCallDriver, IRP management).
// A pragmatic alternative used in many shipping touchpad drivers (e.g.
// Synaptics, Elan) is to expose a registry value "TypingSuppression" and
// let the user-mode companion app or the HID filter update the timestamp via
// a custom feature report.  Windows Precision Touchpad itself does typing
// suppression in the HID class driver layer.
//
// For this driver the cleanest in-kernel approach that does not require a
// separate keyboard filter is to register a KBDMOU_CONNECT_DATA-level
// callback via the keyboard class driver's connect-data mechanism.  That is
// non-trivial and device-specific.
//
// Pragmatic compromise used here:
//   - Register an ExCallback on the documented
//     \Callback\PowerState (GUID_ACPI_REGS_INTERFACE_STANDARD is not right)
//   - Actually use ExCreateCallback / ExRegisterCallback on a *custom*
//     named callback object that a companion HID filter or user-mode app
//     can fire.  Name: \Callback\AmtPtpKbdActivity
//
// On every invocation (from either the keyboard HID filter or user-mode via
// NtOpenSection/NtCreateCallback) we advance TypingSuppressUntil by the
// suppression window.
//
// NOTE: If no companion fires the callback the feature is simply inactive —
//       it degrades gracefully and the touchpad works normally.
// ---------------------------------------------------------------------------

#define CALLBACK_OBJECT_NAME    L"\\Callback\\AmtPtpKbdActivity"

static PCALLBACK_OBJECT g_KbdCallbackObject = NULL;

static VOID
AmtPtpKeyboardNotifyCallback(
    _In_ PVOID CallbackContext,
    _In_ PVOID Argument1,    // unused — fired for any key event
    _In_ PVOID Argument2     // unused
)
{
    PDEVICE_CONTEXT pCtx = (PDEVICE_CONTEXT)CallbackContext;
    LARGE_INTEGER   now;
    LONGLONG        suppressUntil;

    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);

    KeQueryPerformanceCounter(&now);

    //
    // Convert 500 ms to QPC ticks.
    // TypingSuppressUntil is in QPC ticks, same units as LastReportTime.
    //
    if (pCtx->PerfFrequency.QuadPart > 0) {
        LONGLONG ticksPerMs = pCtx->PerfFrequency.QuadPart / 1000LL;
        suppressUntil = now.QuadPart + ticksPerMs * 500LL;
    }
    else {
        // Frequency not yet populated (callback fired before D0Entry?) — skip.
        return;
    }

    //
    // Atomically push the deadline forward.
    // InterlockedExchange64 writes the new value; if two keys arrive
    // simultaneously the later QPC value wins (both are safe).
    //
    InterlockedExchange64(&pCtx->TypingSuppressUntil, suppressUntil);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
        "%!FUNC! Keyboard activity detected, suppressing touchpad for 500 ms");
}

VOID
AmtPtpRegisterKeyboardNotification(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    NTSTATUS            status;
    OBJECT_ATTRIBUTES   oa;
    UNICODE_STRING      callbackName;

    if (DeviceContext->KbdNotifyHandle != NULL) {
        // Already registered.
        return;
    }

    RtlInitUnicodeString(&callbackName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &callbackName, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    //
    // Create (or open if already created by the keyboard filter) the named
    // callback object.  AllowMultipleCallbacks = TRUE so the keyboard filter
    // can also register.
    //
    status = ExCreateCallback(
        &g_KbdCallbackObject,
        &oa,
        TRUE,    // Create if not found
        TRUE     // Allow multiple registrations
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExCreateCallback failed %!STATUS! — typing suppression inactive",
            status);
        return;
    }

    DeviceContext->KbdNotifyHandle = ExRegisterCallback(
        g_KbdCallbackObject,
        AmtPtpKeyboardNotifyCallback,
        DeviceContext
    );

    if (DeviceContext->KbdNotifyHandle == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExRegisterCallback returned NULL — typing suppression inactive");
        ObDereferenceObject(g_KbdCallbackObject);
        g_KbdCallbackObject = NULL;
    }
    else {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Keyboard notification registered successfully");
    }
}

VOID
AmtPtpUnregisterKeyboardNotification(
    _In_ PDEVICE_CONTEXT DeviceContext
)
{
    if (DeviceContext->KbdNotifyHandle != NULL) {
        ExUnregisterCallback(DeviceContext->KbdNotifyHandle);
        DeviceContext->KbdNotifyHandle = NULL;
    }

    if (g_KbdCallbackObject != NULL) {
        ObDereferenceObject(g_KbdCallbackObject);
        g_KbdCallbackObject = NULL;
    }
}