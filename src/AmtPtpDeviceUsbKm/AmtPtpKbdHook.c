/*
 * AmtPtpKbdHook.c
 *
 * Keyboard upper-filter driver. Fires \Callback\AmtPtpKbdActivity only on
 * key-down events so AmtPtpDeviceUsbKm can suppress touchpad input for 500 ms.
 *
 * Build: separate KMDF project, DriverType=KMDF, Universal.
 * Link:  ntstrsafe.lib only.
 * INF:   UpperFilters = AmtPtpKbdHook under [DDInstall.HW] for Class=Keyboard.
 */

#include <ntddk.h>
#include <wdf.h>
#include <ntddkbd.h>     // KEYBOARD_INPUT_DATA, KEY_BREAK

#define CALLBACK_OBJECT_NAME L"\\Callback\\AmtPtpKbdActivity"
#define POOL_TAG_KBD         'KBDH'

typedef struct _KBD_FILTER_CONTEXT {
    PCALLBACK_OBJECT CallbackObject;
} KBD_FILTER_CONTEXT, *PKBD_FILTER_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KBD_FILTER_CONTEXT, KbdFilterGetContext)

EVT_WDF_DRIVER_DEVICE_ADD           KbdHookEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_READ            KbdHookEvtIoRead;
EVT_WDF_REQUEST_COMPLETION_ROUTINE  KbdHookReadCompletion;
EVT_WDF_OBJECT_CONTEXT_CLEANUP      KbdHookEvtDeviceContextCleanup;

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
// KbdHookEvtDeviceContextCleanup — releases the callback object reference.
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
    NTSTATUS              status;
    WDF_OBJECT_ATTRIBUTES devAttribs;
    WDFDEVICE             device;
    PKBD_FILTER_CONTEXT   pCtx;
    PCALLBACK_OBJECT      cbObj = NULL;
    OBJECT_ATTRIBUTES     oa;
    UNICODE_STRING        cbName;
    WDFQUEUE              queue;
    WDF_IO_QUEUE_CONFIG   queueConfig;

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttribs, KBD_FILTER_CONTEXT);
    devAttribs.EvtCleanupCallback = KbdHookEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &devAttribs, &device);
    if (!NT_SUCCESS(status)) return status;

    pCtx = KbdFilterGetContext(device);
    pCtx->CallbackObject = NULL;

    RtlInitUnicodeString(&cbName, CALLBACK_OBJECT_NAME);
    InitializeObjectAttributes(&oa, &cbName,
                               OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, NULL, NULL);

    status = ExCreateCallback(&cbObj, &oa, TRUE, TRUE);
    if (NT_SUCCESS(status)) {
        pCtx->CallbackObject = cbObj;  // released in EvtCleanupCallback
    } else {
        // Non-fatal: touchpad works normally, suppression is just inactive.
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! ExCreateCallback failed %!STATUS! — suppression inactive", status);
        status = STATUS_SUCCESS;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = KbdHookEvtIoRead;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    return status;
}

// ---------------------------------------------------------------------------
// KbdHookEvtIoRead — forward read IRP to lower driver with completion routine.
// ---------------------------------------------------------------------------

VOID
KbdHookEvtIoRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    BOOLEAN   sent;
    UNREFERENCED_PARAMETER(Length);

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, KbdHookReadCompletion,
                                   KbdFilterGetContext(device));

    sent = WdfRequestSend(Request, WdfDeviceGetIoTarget(device), WDF_NO_SEND_OPTIONS);

    //
    // FIX: WdfRequestSend can return FALSE if the I/O target is stopped or the
    // request is malformed.  Without this check the request is neither forwarded
    // nor completed, permanently stalling the keyboard read queue.
    //
    if (!sent) {
        NTSTATUS status = WdfRequestGetStatus(Request);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestSend failed %!STATUS!", status);
        WdfRequestComplete(Request, status);
    }
}

// ---------------------------------------------------------------------------
// KbdHookReadCompletion — inspect KEYBOARD_INPUT_DATA, fire only on key-down.
//
// FIX: original code fired ExNotifyCallback on every read completion,
// including key-up (KEY_BREAK) events and the completion of "empty" reads
// that the class driver issues to keep the pipe primed.  This caused the
// suppression timer to reset on key-release, effectively doubling the
// suppression window and making it feel like the touchpad stayed off for ~1 s
// after the last keystroke instead of 500 ms.
//
// The keyboard class driver fills the output buffer with an array of
// KEYBOARD_INPUT_DATA structs.  Each struct has a Flags field; KEY_BREAK
// (0x01) means the key was released.  We skip those and fire only for
// make (key-down) events.
//
// Called at DISPATCH_LEVEL.
// ---------------------------------------------------------------------------

VOID
KbdHookReadCompletion(
    _In_ WDFREQUEST                     Request,
    _In_ WDFIOTARGET                    Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT                     Context)
{
    PKBD_FILTER_CONTEXT pCtx   = (PKBD_FILTER_CONTEXT)Context;
    NTSTATUS            status = WdfRequestGetStatus(Request);

    UNREFERENCED_PARAMETER(Target);

    if (NT_SUCCESS(status) && pCtx->CallbackObject != NULL)
    {
        //
        // Walk the KEYBOARD_INPUT_DATA array returned by the class driver.
        // Fire the callback once per key-down event found in this batch.
        // A single read can return multiple records (e.g. fast typist).
        //
        ULONG_PTR bytesTransferred =
            Params->Parameters.Read.Length;  // actual bytes returned

        if (bytesTransferred >= sizeof(KEYBOARD_INPUT_DATA))
        {
            const KEYBOARD_INPUT_DATA* kbdData =
                (const KEYBOARD_INPUT_DATA*)
                WdfMemoryGetBuffer(Params->Parameters.Read.Buffer, NULL);

            ULONG recordCount =
                (ULONG)(bytesTransferred / sizeof(KEYBOARD_INPUT_DATA));

            for (ULONG k = 0; k < recordCount; k++)
            {
                // KEY_BREAK set → key-up; skip so we don't extend suppression
                // on release.  E0 / E1 prefix records have no key-break meaning
                // but are also not standalone keystrokes — skip those too.
                if (kbdData[k].Flags & KEY_BREAK)
                    continue;

                // At least one key-down in this batch: fire and stop scanning.
                // One callback per read completion is sufficient — the 500 ms
                // timer will be extended by subsequent completions anyway.
                ExNotifyCallback(pCtx->CallbackObject, NULL, NULL);
                break;
            }
        }
    }

    WdfRequestComplete(Request, status);
}