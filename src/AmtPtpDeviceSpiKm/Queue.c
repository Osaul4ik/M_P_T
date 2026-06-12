/*++
Module Name:
    queue.c
Abstract:
    This file contains the queue entry points and callbacks.
Environment:
    Kernel-mode Driver Framework
--*/

#include "driver.h"
#include "queue.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceSpiKmQueueInitialize)
#endif

NTSTATUS
AmtPtpDeviceSpiKmQueueInitialize(
    _In_ WDFDEVICE Device
    )
{
    WDFQUEUE Queue;
    NTSTATUS Status;
    WDF_IO_QUEUE_CONFIG QueueConfig;
	PDEVICE_CONTEXT pDeviceContext;

    PAGED_CODE();

	pDeviceContext = DeviceGetContext(Device);

	//
	// Default queue: receives all IOCTLs that are not forwarded elsewhere.
	// Parallel dispatch — each IOCTL handler is responsible for its own
	// synchronization.  EvtIoStop is intentionally minimal here: the default
	// queue only handles short synchronous IOCTLs (descriptor fetch, feature
	// get/set) that complete before the framework's power-down timeout, so
	// taking no action is safe and avoids complexity.
	//
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &QueueConfig,
        WdfIoQueueDispatchParallel
    );

	QueueConfig.EvtIoInternalDeviceControl = AmtPtpDeviceSpiKmEvtIoInternalDeviceControl;
    QueueConfig.EvtIoStop                  = AmtPtpDeviceSpiKmEvtIoStop;

    Status = WdfIoQueueCreate(
        Device,
        &QueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &Queue
    );

    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_QUEUE,
			"WdfIoQueueCreate (default) failed %!STATUS!", Status);
		goto exit;
    }

	//
	// Manual HID read queue: holds pending IOCTL_HID_READ_REPORT requests
	// until the SPI completion routine dequeues and satisfies them.
	// PowerManaged = WdfFalse because we manage power transitions ourselves
	// via SelfManagedIo callbacks and the PowerOnRecoveryTimer.
	//
	// FIX: register AmtPtpEvtIoStop so KMDF can drain this queue during
	// power transitions (S3/S4) and surprise removal.  Without it the
	// framework cannot guarantee all requests complete before power-down,
	// which causes DRIVER_POWER_STATE_FAILURE (Bug Check 0x9F).
	// AmtPtpEvtIoStop is implemented in device.c.
	//
	WDF_IO_QUEUE_CONFIG_INIT(&QueueConfig, WdfIoQueueDispatchManual);
	QueueConfig.EvtIoStop    = AmtPtpEvtIoStop;  // FIX: was "queueConfig" (typo, would not compile)
	QueueConfig.PowerManaged = WdfFalse;

	Status = WdfIoQueueCreate(
		Device,
		&QueueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&pDeviceContext->HidQueue
	);

	if (!NT_SUCCESS(Status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR, TRACE_QUEUE,
			"%!FUNC! WdfIoQueueCreate (HidQueue) failed %!STATUS!",
			Status
		);
	}

exit:
    return Status;
}

PCHAR
DbgIoControlGetString(
	_In_ ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:           return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:           return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:           return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_GET_STRING:                      return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_READ_REPORT:                     return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_WRITE_REPORT:                    return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_UMDF_HID_GET_INPUT_REPORT:           return "IOCTL_UMDF_HID_GET_INPUT_REPORT";
	case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:          return "IOCTL_UMDF_HID_SET_OUTPUT_REPORT";
	case IOCTL_UMDF_HID_GET_FEATURE:                return "IOCTL_UMDF_HID_GET_FEATURE";
	case IOCTL_UMDF_HID_SET_FEATURE:                return "IOCTL_UMDF_HID_SET_FEATURE";
	case IOCTL_HID_ACTIVATE_DEVICE:                 return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:               return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:  return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_GET_FEATURE:                     return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_SET_FEATURE:                     return "IOCTL_HID_SET_FEATURE";
	default:                                        return "IOCTL_UNKNOWN";
	}
}

VOID
AmtPtpDeviceSpiKmEvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
{
	UNREFERENCED_PARAMETER(InputBufferLength);
	UNREFERENCED_PARAMETER(OutputBufferLength);

	NTSTATUS Status = STATUS_SUCCESS;
	WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
	BOOLEAN RequestPending = FALSE;

	TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_QUEUE,
		"%!FUNC! %s", DbgIoControlGetString(IoControlCode));

	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		Status = AmtPtpGetHidDescriptor(Device, Request);
		break;
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		Status = AmtPtpGetDeviceAttribs(Device, Request);
		break;
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		Status = AmtPtpGetReportDescriptor(Device, Request);
		break;
	case IOCTL_HID_GET_STRING:
		Status = AmtPtpGetStrings(Device, Request, &RequestPending);
		break;
	case IOCTL_HID_READ_REPORT:
		AmtPtpSpiInputRoutineWorker(Device, Request);
		RequestPending = TRUE;
		break;
	case IOCTL_HID_GET_FEATURE:
		Status = AmtPtpReportFeatures(Device, Request);
		break;
	case IOCTL_HID_SET_FEATURE:
		Status = AmtPtpSetFeatures(Device, Request);
		break;
	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
	case IOCTL_UMDF_HID_GET_INPUT_REPORT:
	case IOCTL_HID_ACTIVATE_DEVICE:
	case IOCTL_HID_DEACTIVATE_DEVICE:
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
	default:
		Status = STATUS_NOT_SUPPORTED;
		TraceEvents(TRACE_LEVEL_WARNING, TRACE_QUEUE,
			"%!FUNC!: %s is not supported",
			DbgIoControlGetString(IoControlCode));
		break;
	}

	if (RequestPending != TRUE) {
		WdfRequestComplete(Request, Status);
	}

    return;
}

// EvtIoStop for the default (IOCTL dispatch) queue.
// All IOCTLs on this queue are synchronous and short-lived — they complete
// well within the framework's power-transition timeout.  Taking no action
// here is correct: the framework will wait for the in-flight request to
// complete naturally before proceeding with power-down or removal.
// This will NOT cause Bug Check 0x9F because the default queue only holds
// requests for the brief duration of a descriptor fetch or feature get/set.
VOID
AmtPtpDeviceSpiKmEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(Request);
	UNREFERENCED_PARAMETER(ActionFlags);

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_QUEUE,
		"%!FUNC! ActionFlags=0x%lx, allowing request to complete naturally",
		ActionFlags);

    return;
}
