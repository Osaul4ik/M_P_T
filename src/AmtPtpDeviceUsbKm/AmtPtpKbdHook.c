/*
 * AmtPtpKbdHook.c
 *
 * Minimal keyboard upper-filter driver that fires the named callback
 * \Callback\AmtPtpKbdActivity on every key-down event.
 *
 * This driver installs as an upper filter on the keyboard HID device
 * (Class = Keyboard) and is loaded alongside AmtPtpDeviceUsbKm.
 *
 * Build: add as a second KMDF project in the same solution, DriverType=KMDF,
 * DriverTargetPlatform=Universal.  Link against ntstrsafe.lib only.
 *
 * INF: add UpperFilters = AmtPtpKbdHook under [DDInstall.HW] for
 * Class=Keyboard.
 *
 * -------------------------------------------------------------------------
 * How it connects to AmtPtpDeviceUsbKm:
 *
 *   AmtPtpDeviceUsbKm (D0Entry)  ─── ExCreateCallback ──►  \Callback\AmtPtpKbdActivity
 *                                      ExRegisterCallback ──►  AmtPtpKeyboardNotifyCallback()
 *
 *   AmtPtpKbdHook (key down)  ────── ExNotifyCallback ───►  (same object)
 *                                      └─► AmtPtpKeyboardNotifyCallback() fires
 *                                           └─► TypingSuppressUntil += 500 ms
 * -------------------------------------------------------------------------
 */

#include <ntddk.h>
#include <wdf.h>

#define CALLBACK_OBJECT_NAME L"\\Callback\\AmtPtpKbdActivity"
#define POOL_TAG_KBD         'KBDH'

typedef struct _KBD_FILTER_CONTEXT {
    PCALLBACK_OBJECT CallbackObject;
} KBD_FILTER_CONTEXT, *PKBD_FILTER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KBD_FILTER_CONTEXT, KbdFilterGetContext)

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
EVT_WDF_DRIVER_DEVICE_ADD     KbdHookEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_READ      KbdHookEvtIoRead;
EVT_WDF_REQUEST_COMPLETION_ROUTINE KbdHookReadCompletion;

// ---------------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------------
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, KbdHookEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

// ---------------------------------------------------------------------------
// KbdHookEvtDeviceAdd
// ---------------------------------------------------------------------------
NTSTATUS
KbdHookEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS               status;
    WDF_OBJECT_ATTRIBUTES  devAttribs;
    WDFDEVICE              device;
    PKBD_FILTER_CONTEXT    pCtx;
    PCALLBACK_OBJECT       cbObj = NULL;
    OBJECT_ATTRIBUTES      oa;
    UNICODE_STRING         cbName;
    WDFQUEUE               queue;
    WDF_IO_QUEUE_CONFIG    queueConfig;

    UNREFERENCED_PARAMETER(Driver);

    // Mark as filter driver.
    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttribs, KBD_FILTER_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &devAttribs, &device);
    if (!NT_SUCCESS(status)) return status;

    pCtx = KbdFilterGetContext(device);
    pCtx->CallbackObject = NULL;

    // Open (or create) the named callback object.
    RtlInitUnicodeString(&cbName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &cbName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    status = ExCreateCallback(&cbObj, &oa,
                              TRUE,   // create if absent
                              TRUE);  // allow multiple registrations
    if (NT_SUCCESS(status)) {
        pCtx->CallbackObject = cbObj;
    }
    // Non-fatal: if the touchpad driver hasn't registered yet we still create
    // the object; the touchpad driver will find it when it calls ExCreateCallback.

    // Install a read-dispatch queue to intercept keyboard read IRPs.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = KbdHookEvtIoRead;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &queue);
    return status;
}

// ---------------------------------------------------------------------------
// KbdHookEvtIoRead
// Forward the read to the lower driver and attach a completion routine.
// ---------------------------------------------------------------------------
VOID
KbdHookEvtIoRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    UNREFERENCED_PARAMETER(Length);

    // Forward the read with a completion routine.
    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, KbdHookReadCompletion,
                                   KbdFilterGetContext(device));
    WdfRequestSend(Request,
                   WdfDeviceGetIoTarget(device),
                   WDF_NO_SEND_OPTIONS);
}

// ---------------------------------------------------------------------------
// KbdHookReadCompletion
// Called at DISPATCH_LEVEL when the lower keyboard driver completes a read.
// Fire the named callback so AmtPtpDeviceUsbKm bumps its suppression timer.
// ---------------------------------------------------------------------------
VOID
KbdHookReadCompletion(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context)
{
    PKBD_FILTER_CONTEXT pCtx = (PKBD_FILTER_CONTEXT)Context;

    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Params);

    if (pCtx->CallbackObject != NULL) {
        // Argument1 and Argument2 are unused by our callback; pass NULL.
        ExNotifyCallback(pCtx->CallbackObject, NULL, NULL);
    }

    WdfRequestComplete(Request, WdfRequestGetStatus(Request));
}