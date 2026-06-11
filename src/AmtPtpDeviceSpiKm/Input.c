#include "driver.h"
#include "Input.tmh"

VOID
AmtPtpSpiInputRoutineWorker(
	WDFDEVICE Device,
	WDFREQUEST PtpRequest
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	pDeviceContext = DeviceGetContext(Device);

	Status = WdfRequestForwardToIoQueue(
		PtpRequest,
		pDeviceContext->HidQueue
	);

	if (!NT_SUCCESS(Status)) {
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestForwardToIoQueue fails, status = %!STATUS!",
			Status
		);

		WdfRequestComplete(PtpRequest, Status);
		return;
	}

	// Only issue request when fully configured.
	// Otherwise we will let power recovery process to triage it
	if (pDeviceContext->DeviceStatus == D0ActiveAndConfigured) {
		AmtPtpSpiInputIssueRequest(Device);
	}
}

VOID
AmtPtpSpiInputIssueRequest(
	WDFDEVICE Device
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	WDF_OBJECT_ATTRIBUTES Attributes;
	BOOLEAN RequestStatus = FALSE;
	WDFREQUEST SpiHidReadRequest;
	WDFMEMORY SpiHidReadOutputMemory;
	PWORKER_REQUEST_CONTEXT RequestContext;
	pDeviceContext = DeviceGetContext(Device);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, WORKER_REQUEST_CONTEXT);
	Attributes.ParentObject = Device;

	Status = WdfRequestCreate(
		&Attributes,
		pDeviceContext->SpiTrackpadIoTarget,
		&SpiHidReadRequest
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DEVICE,
			"%!FUNC! WdfRequestCreate fails, status = %!STATUS!",
			Status
		);

		return;
	}

	Status = WdfMemoryCreateFromLookaside(
		pDeviceContext->HidReadBufferLookaside,
		&SpiHidReadOutputMemory
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DEVICE,
			"%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!",
			Status
		);

		WdfObjectDelete(SpiHidReadRequest);
		return;
	}

	// Assign context information
	RequestContext = WorkerRequestGetContext(SpiHidReadRequest);
	RequestContext->DeviceContext = pDeviceContext;
	RequestContext->RequestMemory = SpiHidReadOutputMemory;

	// Invoke HID read request to the device.
	Status = WdfIoTargetFormatRequestForInternalIoctl(
		pDeviceContext->SpiTrackpadIoTarget,
		SpiHidReadRequest,
		IOCTL_HID_READ_REPORT,
		NULL,
		0,
		SpiHidReadOutputMemory,
		0
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DEVICE,
			"%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!",
			Status
		);

		if (SpiHidReadOutputMemory != NULL) {
			WdfObjectDelete(SpiHidReadOutputMemory);
		}

		if (SpiHidReadRequest != NULL) {
			WdfObjectDelete(SpiHidReadRequest);
		}

		return;
	}

	WdfRequestSetCompletionRoutine(
		SpiHidReadRequest,
		AmtPtpRequestCompletionRoutine,
		RequestContext
	);

	RequestStatus = WdfRequestSend(
		SpiHidReadRequest,
		pDeviceContext->SpiTrackpadIoTarget,
		NULL
	);

	if (!RequestStatus)
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DEVICE,
			"%!FUNC! AmtPtpSpiInputRoutineWorker request failed to sent"
		);

		if (SpiHidReadOutputMemory != NULL) {
			WdfObjectDelete(SpiHidReadOutputMemory);
		}

		if (SpiHidReadRequest != NULL) {
			WdfObjectDelete(SpiHidReadRequest);
		}
	}
}

VOID
AmtPtpRequestCompletionRoutine(
	WDFREQUEST SpiRequest,
	WDFIOTARGET Target,
	PWDF_REQUEST_COMPLETION_PARAMS Params,
	WDFCONTEXT Context
)
{
	NTSTATUS Status;
	PWORKER_REQUEST_CONTEXT RequestContext;
	PDEVICE_CONTEXT pDeviceContext;

	LONG SpiRequestLength;
	PSPI_TRACKPAD_PACKET pSpiTrackpadPacket;

	WDFREQUEST PtpRequest;
	PTP_REPORT PtpReport;
	WDFMEMORY PtpRequestMemory;

	LARGE_INTEGER CurrentCounter;
	LARGE_INTEGER Frequency;
	LONGLONG CounterDelta;
	UINT8 SlotContactID[5]; // stable per-slot contact IDs for this report

	UNREFERENCED_PARAMETER(Target);

	// Get context
	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	// Read report and fulfill PTP request.
	// If no report is found, just exit.
	Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfIoQueueRetrieveNextRequest failed with %!STATUS!",
			Status
		);

		goto cleanup;
	}

	SpiRequestLength = (LONG) WdfRequestGetInformation(SpiRequest);
	pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET) WdfMemoryGetBuffer(Params->Parameters.Ioctl.Output.Buffer, NULL);

	// Safe measurement for buffer overrun and device state reset
	if (SpiRequestLength < 46) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! Input too small: %d < 46. Attempt to re-enable the device.",
			SpiRequestLength
		);

		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Get Counter
	// ScanTime units are 100 microseconds per HID PTP spec.
	// KeQueryPerformanceCounter returns current tick count.
	// KeQueryPerformanceFrequency returns ticks per second.
	LARGE_INTEGER Frequency;
    CurrentCounter = KeQueryPerformanceCounter(&Frequency);
	// Compute delta in 100us units: (delta_ticks * 10000) / freq_Hz
	if (Frequency.QuadPart > 0 && pDeviceContext->LastReportTime.QuadPart > 0) {
		CounterDelta = ((CurrentCounter.QuadPart - pDeviceContext->LastReportTime.QuadPart) * 10000LL)
			/ Frequency.QuadPart;
	} else {
		CounterDelta = 0;
	}
	pDeviceContext->LastReportTime.QuadPart = CurrentCounter.QuadPart;

	// Write report
	RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
	PtpReport.ReportID = REPORTID_MULTITOUCH;
	PtpReport.ContactCount = pSpiTrackpadPacket->NumOfFingers;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;

	UINT8 AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > 5) ? 5 : pSpiTrackpadPacket->NumOfFingers;

	// Use slot index directly as ContactID (0..4).
	// Apple SPI protocol assigns fingers to fixed slots and keeps them
	// there until lift-off, so slot index is a naturally stable ID.
	// A coordinate-hash ID (old code) changes every frame as the finger
	// moves, causing Windows to think the contact disappeared and
	// reappeared - which makes the cursor jump back to a previous position.
	for (UINT8 Count = 0; Count < AdjustedCount; Count++)
	{
		SlotContactID[Count] = Count; // slot index = stable contact ID
		PtpReport.Contacts[Count].ContactID = SlotContactID[Count];
		PtpReport.Contacts[Count].X = ((pSpiTrackpadPacket->Fingers[Count].X - pDeviceContext->TrackpadInfo.XMin) > 0) ? 
			(USHORT)(pSpiTrackpadPacket->Fingers[Count].X - pDeviceContext->TrackpadInfo.XMin) : 0;
		PtpReport.Contacts[Count].Y = ((pDeviceContext->TrackpadInfo.YMax - pSpiTrackpadPacket->Fingers[Count].Y) > 0) ? 
			(USHORT)(pDeviceContext->TrackpadInfo.YMax - pSpiTrackpadPacket->Fingers[Count].Y) : 0;
		// Use pressure > 1 (not > 0) to avoid ghost touches from near-zero readings
		PtpReport.Contacts[Count].TipSwitch = (pSpiTrackpadPacket->Fingers[Count].Pressure > 1) ? 1 : 0;

		// $S = \pi * (Touch_{Major} * Touch_{Minor}) / 4$
		// $S = \pi * r^2$
		// $r^2 = (Touch_{Major} * Touch_{Minor}) / 4$
		// Using i386 in 2018 is evil
		PtpReport.Contacts[Count].Confidence = (pSpiTrackpadPacket->Fingers[Count].TouchMajor < 2500 &&
			pSpiTrackpadPacket->Fingers[Count].TouchMinor < 2500) ? 1 : 0;

		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_HID_INPUT,
			"%!FUNC! PTP Contact %d OX %d, OY %d, X %d, Y %d",
			Count,
			pSpiTrackpadPacket->Fingers[Count].OriginalX,
			pSpiTrackpadPacket->Fingers[Count].OriginalY,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y
		);
	}

	// ScanTime is USHORT - clamp to 0xFFFF, NOT 0xFF.
	// The old 0xFF clamp caused the timer to wrap every ~25ms, confusing
	// Windows gesture recognizer and causing the "must wait before next gesture" bug.
	if (CounterDelta >= 0xFFFF)
	{
		PtpReport.ScanTime = 0xFFFF;
	}
	else
	{
		PtpReport.ScanTime = (USHORT) CounterDelta;
	}

	Status = WdfRequestRetrieveOutputMemory(
		PtpRequest,
		&PtpRequestMemory
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestRetrieveOutputBuffer failed with %!STATUS!",
			Status
		);

		goto exit;
	}

	Status = WdfMemoryCopyFromBuffer(
		PtpRequestMemory,
		0,
		(PVOID) &PtpReport,
		sizeof(PTP_REPORT)
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!",
			Status
		);

		goto exit;
	}

	// Set information
	WdfRequestSetInformation(
		PtpRequest,
		sizeof(PTP_REPORT)
	);

exit:
	WdfRequestComplete(
		PtpRequest,
		Status
	);

cleanup:
	// Clean up
	pSpiTrackpadPacket = NULL;
	WdfObjectDelete(SpiRequest);
	if (RequestContext->RequestMemory != NULL) {
		WdfObjectDelete(RequestContext->RequestMemory);
	}
}
