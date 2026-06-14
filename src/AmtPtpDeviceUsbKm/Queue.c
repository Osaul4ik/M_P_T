/*++

Module Name:

    Queue.c

Abstract:

    I/O queue setup and callbacks.

    The default queue handles all IOCTL requests (HID descriptors, feature
    get/set, read-report dispatch) at DISPATCH_LEVEL via the parallel
    dispatcher.

    Read-report (IOCTL_HID_READ_REPORT) requests are forwarded into the
    manual InputQueue.  The processing thread drains that queue at
    PASSIVE_LEVEL.  When a new read request arrives and the processing
    thread might be parked waiting for a USB packet, we signal ProcEvent so
    it wakes and delivers any report it already has buffered (backpressure
    relief).

Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "queue.tmh"
#include "include/ProcessingThread.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, AmtPtpDeviceUsbKmQueueInitialize)
#endif

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device)
{
    WDFQUEUE        queue;
    NTSTATUS        status;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PDEVICE_CONTEXT pCtx;

    PAGED_CODE();

    pCtx = DeviceGetContext(Device);

    // ---- default parallel queue (all IOCTLs) ----------------------------
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig, WdfIoQueueDispatchParallel);

    queueConfig.EvtIoInternalDeviceControl = AmtPtpDeviceUsbKmEvtIoDeviceControl;
    queueConfig.EvtIoStop                  = AmtPtpDeviceUsbKmEvtIoStop;

    status = WdfIoQueueCreate(Device, &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            "WdfIoQueueCreate (default) failed %!STATUS!", status);
        return status;
    }

    // ---- manual, power-unmanaged HID input queue ------------------------
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(Device, &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES, &pCtx->InputQueue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            "%!FUNC! WdfIoQueueCreate (InputQueue) failed %!STATUS!", status);
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
AmtPtpDeviceUsbKmEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode)
{
    NTSTATUS  status;
    WDFDEVICE device          = WdfIoQueueGetDevice(Queue);
    BOOLEAN   requestPending  = FALSE;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_QUEUE,
        "%!FUNC! %s", DbgIoControlGetString(IoControlCode));

    switch (IoControlCode)
    {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = AmtPtpGetHidDescriptor(device, Request);
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = AmtPtpGetDeviceAttribs(device, Request);
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = AmtPtpGetReportDescriptor(device, Request);
        break;

    case IOCTL_HID_READ_REPORT:
        status = AmtPtpDispatchReadReportRequests(device, Request, &requestPending);
        break;

    case IOCTL_HID_GET_FEATURE:
        status = AmtPtpReportFeatures(device, Request);
        break;

    case IOCTL_HID_SET_FEATURE:
        status = AmtPtpSetFeatures(device, Request);
        break;

    case IOCTL_HID_GET_STRING:
    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
    case IOCTL_UMDF_HID_GET_INPUT_REPORT:
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (!requestPending) {
        WdfRequestComplete(Request, status);
    }
}

NTSTATUS
AmtPtpDispatchReadReportRequests(
    _In_  WDFDEVICE  Device,
    _In_  WDFREQUEST Request,
    _Out_ BOOLEAN*   Pending)
{
    NTSTATUS        status;
    PDEVICE_CONTEXT pCtx;

    *Pending = FALSE;
    pCtx     = DeviceGetContext(Device);

    if (pCtx->InputQueue == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            "%!FUNC! InputQueue not initialised");
        return STATUS_INVALID_DEVICE_STATE;
    }

    status = WdfRequestForwardToIoQueue(Request, pCtx->InputQueue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
            "%!FUNC! ForwardToIoQueue failed %!STATUS!", status);
        return status;
    }

    *Pending = TRUE;

    // Kick the processing thread: it may have a packet ready but was parked
    // because no HID request was available (backpressure relief).
    if (pCtx->ThreadRunning) {
        ProcThreadSignal(pCtx);
    }

    return STATUS_SUCCESS;
}

VOID
AmtPtpDeviceUsbKmEvtIoStop(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG      ActionFlags)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
        "%!FUNC! Queue=0x%p Request=0x%p ActionFlags=0x%lx",
        Queue, Request, ActionFlags);

    // The InputQueue is power-unmanaged so this callback fires for requests
    // on the default queue only.  For the HID read path the framework holds
    // the request and the processing thread owns completion — nothing to do.
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ActionFlags);
}
