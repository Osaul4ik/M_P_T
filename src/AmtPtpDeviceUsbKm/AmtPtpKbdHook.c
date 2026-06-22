// AmtPtpKbdHook.c - Keyboard upper-filter driver. Fires
// \Callback\AmtPtpKbdActivity on key-down for 500 ms touchpad suppression.

#include <ntddk.h>
#include <wdf.h>
#include <ntddkbd.h>     // KEYBOARD_INPUT_DATA, KEY_BREAK

// WPP tracing
#include "trace.h"
#include "AmtPtpKbdHook.tmh"

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

// ---------------------------------------------------------------------
// DriverEntry
// ---------------------------------------------------------------------
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

// ---------------------------------------------------------------------
// Cleanup callback (releases callback object)
// ---------------------------------------------------------------------
VOID
KbdHookEvtDeviceContextCleanup(_In_ WDFOBJECT Device)
{
    PKBD_FILTER_CONTEXT pCtx = KbdFilterGetContext((WDFDEVICE)Device);
    if (pCtx->CallbackObject != NULL) {
        ObDereferenceObject(pCtx->CallbackObject);
        pCtx->CallbackObject = NULL;
    }
}

// ---------------------------------------------------------------------
// AddDevice
// ---------------------------------------------------------------------
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

// ---------------------------------------------------------------------
// Forward read IRP to lower driver
// ---------------------------------------------------------------------
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

    if (!sent) {
        NTSTATUS status = WdfRequestGetStatus(Request);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestSend failed %!STATUS!", status);
        WdfRequestComplete(Request, status);
    }
}

// ---------------------------------------------------------------------
// Completion routine: fire callback only on key-down events
// ---------------------------------------------------------------------
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
                // Skip key-up to avoid extending suppression window
                if (kbdData[k].Flags & KEY_BREAK)
                    continue;

                // At least one key-down: fire once per read
                ExNotifyCallback(pCtx->CallbackObject, NULL, NULL);
                break;
            }
        }
    }

    WdfRequestComplete(Request, status);
}