/*
 * AmtPtpKbdHook.c
 *
 * Keyboard upper-filter driver that fires \Callback\AmtPtpKbdActivity on
 * every key-down completion, allowing AmtPtpDeviceUsbKm to suppress touchpad
 * input for 500 ms after each keystroke.
 *
 * Build: separate KMDF project, DriverType=KMDF, Universal.
 * Link:  ntstrsafe.lib only.
 * INF:   add UpperFilters = AmtPtpKbdHook under [DDInstall.HW] for
 *        Class=Keyboard.
 */

#include <ntddk.h>
#include <wdf.h>

#define CALLBACK_OBJECT_NAME L"\\Callback\\AmtPtpKbdActivity"
#define POOL_TAG_KBD         'KBDH'

typedef struct _KBD_FILTER_CONTEXT {
    PCALLBACK_OBJECT CallbackObject;
} KBD_FILTER_CONTEXT, *PKBD_FILTER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KBD_FILTER_CONTEXT, KbdFilterGetContext)

EVT_WDF_DRIVER_DEVICE_ADD              KbdHookEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_READ               KbdHookEvtIoRead;
EVT_WDF_REQUEST_COMPLETION_ROUTINE     KbdHookReadCompletion;

// FIX: release the callback object reference when the device is cleaned up.
// Original code had no cleanup callback, so CallbackObject was leaked on
// device removal.
EVT_WDF_OBJECT_CONTEXT_CLEANUP         KbdHookEvtDeviceContextCleanup;

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
// KbdHookEvtDeviceContextCleanup
// FIX: was missing entirely — CallbackObject reference was leaked on removal.
// ---------------------------------------------------------------------------

VOID
KbdHookEvtDeviceContextCleanup(_In_ WDFOBJECT Device)
{
    PKBD_FILTER_CONTEXT pCtx = KbdFilterGetContext((WDFDEVICE)Device);

    if (pCtx->CallbackObject != NULL) {
        ObDereferenceObject(pCtx->CallbackObject);
        pCtx->CallbackObject = NULL;
    }
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

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttribs, KBD_FILTER_CONTEXT);

    // FIX: register the cleanup callback so CallbackObject is released on removal.
    devAttribs.EvtCleanupCallback = KbdHookEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &devAttribs, &device);
    if (!NT_SUCCESS(status)) return status;

    pCtx = KbdFilterGetContext(device);
    pCtx->CallbackObject = NULL;

    // Open or create the named callback object.
    RtlInitUnicodeString(&cbName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &cbName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    status = ExCreateCallback(&cbObj, &oa,
                              TRUE,   // create if absent
                              TRUE);  // allow multiple registrations
    if (NT_SUCCESS(status)) {
        // Hold one reference; released in KbdHookEvtDeviceContextCleanup.
        pCtx->CallbackObject = cbObj;
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExCreateCallback failed %!STATUS! (non-fatal)", status);
        // Non-fatal: touchpad driver still works, suppression is simply absent.
        status = STATUS_SUCCESS;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = KbdHookEvtIoRead;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES, &queue);
    return status;
}

// ---------------------------------------------------------------------------
// KbdHookEvtIoRead
// ---------------------------------------------------------------------------

VOID
KbdHookEvtIoRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    UNREFERENCED_PARAMETER(Length);

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, KbdHookReadCompletion,
                                   KbdFilterGetContext(device));
    WdfRequestSend(Request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS);
}

// ---------------------------------------------------------------------------
// KbdHookReadCompletion
// Called at DISPATCH_LEVEL when the lower keyboard driver completes a read.
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

    if (pCtx->CallbackObject != NULL)
        ExNotifyCallback(pCtx->CallbackObject, NULL, NULL);

    WdfRequestComplete(Request, WdfRequestGetStatus(Request));
}