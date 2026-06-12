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
	// Otherwise we will let power recovery process to triage it.
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

		WdfObjectDelete(SpiHidReadOutputMemory);
		WdfObjectDelete(SpiHidReadRequest);
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
			"%!FUNC! WdfRequestSend failed"
		);

		WdfObjectDelete(SpiHidReadOutputMemory);
		WdfObjectDelete(SpiHidReadRequest);
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
	LONG MinPacketLength;
	PSPI_TRACKPAD_PACKET pSpiTrackpadPacket;

	WDFREQUEST PtpRequest;
	PTP_REPORT PtpReport;
	WDFMEMORY PtpRequestMemory;

	LARGE_INTEGER CurrentCounter;
	LONGLONG CounterDelta;
	UINT8 AdjustedCount;
	UINT8 Count;
	SHORT RawX;
	SHORT RawY;
	LONG NormX;
	LONG NormY;

	UNREFERENCED_PARAMETER(Target);

	// Get context
	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	// Retrieve the pending PTP request before doing any work.
	// If Windows has not posted a read (e.g. during mode switch), just drop
	// the SPI packet silently — no point processing input nobody will receive.
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

	// Check PTP mode flags set by Windows via SET_FEATURE.
	// If the host switched to mouse mode (PtpInputOn = FALSE), or disabled
	// surface reports (PtpReportTouch = FALSE), complete the request with an
	// empty report rather than spending cycles translating touch data.
	if (!pDeviceContext->PtpInputOn || !pDeviceContext->PtpReportTouch) {
		RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
		PtpReport.ReportID = REPORTID_MULTITOUCH;

		Status = WdfRequestRetrieveOutputMemory(PtpRequest, &PtpRequestMemory);
		if (NT_SUCCESS(Status)) {
			WdfMemoryCopyFromBuffer(PtpRequestMemory, 0, &PtpReport, sizeof(PTP_REPORT));
			WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));
		}
		goto exit;
	}

	SpiRequestLength = (LONG) WdfRequestGetInformation(SpiRequest);
	pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET) WdfMemoryGetBuffer(
		Params->Parameters.Ioctl.Output.Buffer, NULL);

	// Validate the packet header is present.
	// sizeof(SPI_TRACKPAD_PACKET) header (without finger array) = 46 bytes.
	if (SpiRequestLength < 46) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! Packet too short for header: %d < 46",
			SpiRequestLength
		);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Validate that the buffer contains all claimed finger records.
	// Without this check, reading Fingers[N] when the packet only contains
	// M < N fingers is an out-of-bounds read into adjacent memory.
	// sizeof(SPI_TRACKPAD_FINGER) = 30 bytes.
	MinPacketLength = 46 + (LONG)pSpiTrackpadPacket->NumOfFingers *
	                       (LONG)sizeof(SPI_TRACKPAD_FINGER);
	if (SpiRequestLength < MinPacketLength) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! Packet truncated: %d < %d (%d fingers claimed)",
			SpiRequestLength,
			MinPacketLength,
			pSpiTrackpadPacket->NumOfFingers
		);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Compute ScanTime.
	// ScanTime unit per HID PTP spec: 100 microseconds.
	// PerformanceFrequency is cached at D0Entry (it is a hardware constant).
	// Formula: delta_ticks * 10000 / freq_Hz = delta in 100us units.
	CurrentCounter = KeQueryPerformanceCounter(NULL);

	if (pDeviceContext->PerformanceFrequency > 0) {
		CounterDelta =
			((CurrentCounter.QuadPart - pDeviceContext->LastReportTime.QuadPart)
			 * 10000LL)
			/ pDeviceContext->PerformanceFrequency;
	} else {
		CounterDelta = 0;
	}

	// Clamp to USHORT. Values > 6.5 seconds only happen on the very first
	// frame after power-on and are harmless at the ceiling.
	if (CounterDelta > 0xFFFF) {
		CounterDelta = 0xFFFF;
	}

	pDeviceContext->LastReportTime.QuadPart = CurrentCounter.QuadPart;

	// Zero the entire report.
	// PTP_REPORT is a stack variable; unused contact slots would contain
	// stack garbage — random coordinates, stale ContactIDs, and TipSwitch=1
	// — which Windows interprets as phantom fingers.
	RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));

	// Clamp contact count to what the report struct can hold.
	AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > PTP_MAX_CONTACT_POINTS)
		? PTP_MAX_CONTACT_POINTS
		: pSpiTrackpadPacket->NumOfFingers;

	PtpReport.ReportID        = REPORTID_MULTITOUCH;
	PtpReport.ContactCount    = AdjustedCount;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;
	PtpReport.ScanTime        = (USHORT) CounterDelta;

	for (Count = 0; Count < AdjustedCount; Count++)
	{
		// ContactID = slot index.
		// Apple T2 SPI firmware assigns each finger to a fixed slot for its
		// entire lifetime (touchdown to lift-off). The slot index is the only
		// stable identifier — coordinate-derived hashes change every frame
		// and cause Windows to see a new contact on every movement, snapping
		// the cursor back to the previous frame's position.
		PtpReport.Contacts[Count].ContactID = Count;

		// Normalize X: map [XMin..XMax] → [0..XRange].
		// XRange is precomputed as (XMax - XMin) at D0Entry.
		// Clamp with saturating arithmetic; use LONG intermediates to avoid
		// signed SHORT overflow before the clamp comparison.
		RawX  = pSpiTrackpadPacket->Fingers[Count].X;
		NormX = (LONG)RawX - (LONG)pDeviceContext->TrackpadInfo.XMin;
		PtpReport.Contacts[Count].X =
			(NormX <= 0) ? 0 :
			(NormX >= (LONG)pDeviceContext->XRange) ? pDeviceContext->XRange :
			(USHORT)NormX;

		// Normalize Y: Apple Y grows downward; PTP Y grows upward.
		// Map [YMin..YMax] → [YRange..0] (invert).
		RawY  = pSpiTrackpadPacket->Fingers[Count].Y;
		NormY = (LONG)pDeviceContext->TrackpadInfo.YMax - (LONG)RawY;
		PtpReport.Contacts[Count].Y =
			(NormY <= 0) ? 0 :
			(NormY >= (LONG)pDeviceContext->YRange) ? pDeviceContext->YRange :
			(USHORT)NormY;

		// TipSwitch: report the contact as touching whenever Pressure >= 1.
		// The T2 firmware reports Pressure == 1 on the very first contact frame
		// (finger still forming contact). Excluding it (old threshold > 1)
		// caused Windows to miss the touchdown and treat the next frame as a
		// new contact mid-gesture, making the cursor jump.
		PtpReport.Contacts[Count].TipSwitch =
			(pSpiTrackpadPacket->Fingers[Count].Pressure >= 1) ? 1 : 0;

		// Confidence: mark the contact as a valid finger unless it looks like
		// a palm. A palm has BOTH TouchMajor and TouchMinor exceeding the
		// threshold simultaneously. A finger at an angle has a large Major
		// but a small Minor — that is a valid touch and must not be dropped.
		// Units match the X/Y space (~1 unit ≈ 0.01 mm for MBP16,1).
		// Threshold 2500 ≈ 25 mm; a palm exceeds this on both axes.
		PtpReport.Contacts[Count].Confidence =
			(pSpiTrackpadPacket->Fingers[Count].TouchMajor < 2500 ||
			 pSpiTrackpadPacket->Fingers[Count].TouchMinor < 2500) ? 1 : 0;

		TraceEvents(
			TRACE_LEVEL_VERBOSE,
			TRACE_HID_INPUT,
			"%!FUNC! Contact[%d] X=%d Y=%d P=%d Maj=%d Min=%d",
			Count,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y,
			pSpiTrackpadPacket->Fingers[Count].Pressure,
			pSpiTrackpadPacket->Fingers[Count].TouchMajor,
			pSpiTrackpadPacket->Fingers[Count].TouchMinor
		);
	}

	Status = WdfRequestRetrieveOutputMemory(PtpRequest, &PtpRequestMemory);
	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfRequestRetrieveOutputMemory failed with %!STATUS!",
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

	WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));

exit:
	WdfRequestComplete(PtpRequest, Status);

cleanup:
	WdfObjectDelete(SpiRequest);
	if (RequestContext->RequestMemory != NULL) {
		WdfObjectDelete(RequestContext->RequestMemory);
	}
}
