// Queue entry points and callbacks. Kernel-mode Driver Framework

#include "driver.h"
#include "queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmQueueInitialize)
#endif

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device
    )
// Creates default parallel queue and manual queue for touch reads.
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG    queueConfig;
	PDEVICE_CONTEXT	       pDeviceContext;

    PAGED_CODE();

	pDeviceContext = DeviceGetContext(Device);
    
    // Default queue for non-forwarded requests.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

    queueConfig.EvtIoInternalDeviceControl = AmtPtpDeviceUsbKmEvtIoDeviceControl;
    queueConfig.EvtIoStop = AmtPtpDeviceUsbKmEvtIoStop;

    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig,
                 WDF_NO_OBJECT_ATTRIBUTES,
                 &queue
                 );

    if( !NT_SUCCESS(status) ) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE, "WdfIoQueueCreate failed %!STATUS!", status);
        return status;
    }

	// Manual queue for touch read requests.
	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(
		Device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&pDeviceContext->InputQueue
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR, TRACE_QUEUE,
			"%!FUNC! WdfIoQueueCreate (Input) failed %!STATUS!",
			status
		);
	}

    return status;
}

VOID
AmtPtpDeviceUsbKmEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
// Dispatches HID IOCTLs to handler functions.
{
	NTSTATUS status;
	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	BOOLEAN requestPending = FALSE;

	UNREFERENCED_PARAMETER(InputBufferLength);
	UNREFERENCED_PARAMETER(OutputBufferLength);

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

	if (requestPending != TRUE) {
		WdfRequestComplete(Request, status);
	}

    return;
}

NTSTATUS
AmtPtpDispatchReadReportRequests(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_Out_ BOOLEAN* Pending
)
// Forwards HID read requests to the manual input queue.
{
	NTSTATUS status;
	PDEVICE_CONTEXT pDevContext;

	status = STATUS_SUCCESS;
	pDevContext = DeviceGetContext(Device);

	status = WdfRequestForwardToIoQueue(
		Request,
		pDevContext->InputQueue
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestForwardToIoQueue failed with %!STATUS!",
			status
		);
		goto exit;
	}

	if (NULL != Pending) {
		*Pending = TRUE;
	}

exit:
	return status;
}

VOID
AmtPtpDeviceUsbKmEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
// Called before device leaves D0 for power-managed queues.
{
    TraceEvents(TRACE_LEVEL_INFORMATION, 
                TRACE_QUEUE, 
                "%!FUNC! Queue 0x%p, Request 0x%p ActionFlags %d", 
                Queue, Request, ActionFlags);

    return;
}