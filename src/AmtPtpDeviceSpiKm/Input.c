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
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
			"%!FUNC! WdfRequestForwardToIoQueue fails, status = %!STATUS!", Status);
		WdfRequestComplete(PtpRequest, Status);
		return;
	}

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
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! WdfRequestCreate fails, status = %!STATUS!", Status);
		return;
	}

	Status = WdfMemoryCreateFromLookaside(
		pDeviceContext->HidReadBufferLookaside,
		&SpiHidReadOutputMemory
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! WdfMemoryCreateFromLookaside fails, status = %!STATUS!", Status);
		WdfObjectDelete(SpiHidReadRequest);
		return;
	}

	RequestContext = WorkerRequestGetContext(SpiHidReadRequest);
	RequestContext->DeviceContext = pDeviceContext;
	RequestContext->RequestMemory = SpiHidReadOutputMemory;

	Status = WdfIoTargetFormatRequestForInternalIoctl(
		pDeviceContext->SpiTrackpadIoTarget,
		SpiHidReadRequest,
		IOCTL_HID_READ_REPORT,
		NULL, 0,
		SpiHidReadOutputMemory, 0
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! WdfIoTargetFormatRequestForInternalIoctl fails, status = %!STATUS!", Status);
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
	if (!RequestStatus) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
			"%!FUNC! WdfRequestSend failed");
		WdfObjectDelete(SpiHidReadOutputMemory);
		WdfObjectDelete(SpiHidReadRequest);
	}
}

// Palm rejection thresholds (Apple SPI raw units, ~0.01mm each).
// A palm has both axes large simultaneously.
// A finger resting at an angle: large Major, small Minor — must NOT be rejected.
#define PALM_MAJOR_THRESHOLD 3500
#define PALM_MINOR_THRESHOLD 3500

// ScanTime cap for the first frame of a new touch sequence (100us units).
// 80 = 8ms, a typical 125Hz interval. Avoids sending ScanTime=0, which
// Windows treats as zero elapsed time => infinite initial velocity =>
// the first gesture frame appears instantaneous to the gesture engine.
#define FIRST_FRAME_SCANTIME_CAP 80

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
	UINT8 ReportedCount;
	UINT8 Count;
	UINT8 i;
	SHORT RawX;
	SHORT RawY;
	LONG NormX;
	LONG NormY;
	BOOLEAN IsPalm;

	UNREFERENCED_PARAMETER(Target);

	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	// Retrieve the pending PTP request before doing any work.
	Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfIoQueueRetrieveNextRequest failed with %!STATUS!", Status);
		goto cleanup;
	}

	// If Windows switched to mouse mode or disabled surface reports,
	// complete with an empty report immediately.
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

	// Validate header presence.
	if (SpiRequestLength < 46) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Packet too short for header: %d < 46", SpiRequestLength);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Validate that the packet holds all claimed finger records.
	// Without this, reading Fingers[N] when the packet only has M < N
	// fingers is an out-of-bounds read.
	MinPacketLength = 46 + (LONG)pSpiTrackpadPacket->NumOfFingers *
	                       (LONG)sizeof(SPI_TRACKPAD_FINGER);
	if (SpiRequestLength < MinPacketLength) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Packet truncated: %d < %d (%d fingers claimed)",
			SpiRequestLength, MinPacketLength, pSpiTrackpadPacket->NumOfFingers);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Compute ScanTime (100us units per HID PTP spec).
	// PerformanceFrequency is a hardware constant cached at D0Entry.
	CurrentCounter = KeQueryPerformanceCounter(NULL);
	if (pDeviceContext->PerformanceFrequency > 0) {
		CounterDelta =
			((CurrentCounter.QuadPart - pDeviceContext->LastReportTime.QuadPart)
			 * 10000LL)
			/ pDeviceContext->PerformanceFrequency;
	} else {
		CounterDelta = 0;
	}
	if (CounterDelta > 0xFFFF) {
		CounterDelta = 0xFFFF;
	}
	pDeviceContext->LastReportTime.QuadPart = CurrentCounter.QuadPart;

	// Zero the entire report. PTP_REPORT is a stack variable; unused contact
	// slots must not contain garbage that Windows could interpret as phantom fingers.
	RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));

	AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > PTP_MAX_CONTACT_POINTS)
		? PTP_MAX_CONTACT_POINTS
		: pSpiTrackpadPacket->NumOfFingers;

	// FIX: ScanTime on first contact frame.
	//
	// On the first frame of a new touch (PrevAdjustedCount == 0 && AdjustedCount > 0),
	// CounterDelta reflects the idle time since the last report — potentially seconds.
	// ScanTime = 0 is also bad: Windows treats it as zero elapsed time, making the
	// gesture engine think infinite velocity on the first movement.
	// Cap at FIRST_FRAME_SCANTIME_CAP (8ms) for a realistic first-frame delta.
	if (pDeviceContext->PrevAdjustedCount == 0 && AdjustedCount > 0) {
		if (CounterDelta > FIRST_FRAME_SCANTIME_CAP) {
			CounterDelta = FIRST_FRAME_SCANTIME_CAP;
		}
	}
	pDeviceContext->PrevAdjustedCount = AdjustedCount;

	// FIX: Palm state — clear ALL slots on each frame, then re-evaluate.
	//
	// The previous approach only cleared slots >= AdjustedCount. This left stale
	// palm state in lower slots when a palm lifted while another finger stayed down:
	//
	//   Frame N:   slots [0]=palm [1]=finger  → AdjustedCount=2, SlotIsPalm[0]=TRUE
	//   Frame N+1: palm lifts, finger stays  → AdjustedCount=1, slots [0]=finger
	//              Old code: only clears slots >= 1, so SlotIsPalm[0] stays TRUE
	//              Result: the remaining real finger is forever classified as a palm
	//
	// Fix: reset all palm state each frame and re-derive it from current touch data.
	// The PALM_MAJOR/MINOR thresholds provide enough hysteresis on their own.
	for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
		pDeviceContext->SlotIsPalm[i] = FALSE;
	}

	// FIX: ContactID assignment — use slot index directly.
	//
	// The previous approach matched fingers by OriginalX == PrevOriginalX.
	// OriginalX and OriginalY are raw touch COORDINATES; they change every frame
	// as the finger moves. An exact equality match virtually never fires, so every
	// frame fell through to the NextContactID counter path, assigning a new ID each
	// frame. Windows saw a lift+reappear on every movement → cursor snapped back.
	//
	// Apple T2 SPI firmware assigns each active finger to a fixed slot (0..N-1)
	// and keeps it there from touchdown to lift-off. The slot index IS the stable
	// finger identity; no cross-frame matching is needed.
	//
	// ContactID field is 3 bits (values 0-7); slot indices 0-4 fit without wrapping.

	// Build contact records, skipping palms.
	ReportedCount = 0;
	for (Count = 0; Count < AdjustedCount; Count++) {

		// Palm rejection: reject only when BOTH axes are large simultaneously.
		// A fingertip at an angle produces large TouchMajor but small TouchMinor
		// and must not be rejected.
		IsPalm = (pSpiTrackpadPacket->Fingers[Count].TouchMajor >= PALM_MAJOR_THRESHOLD &&
		          pSpiTrackpadPacket->Fingers[Count].TouchMinor >= PALM_MINOR_THRESHOLD);
		pDeviceContext->SlotIsPalm[Count] = IsPalm;

		if (IsPalm) {
			TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
				"%!FUNC! Contact[%d] classified as palm (Maj=%d Min=%d), suppressed",
				Count,
				pSpiTrackpadPacket->Fingers[Count].TouchMajor,
				pSpiTrackpadPacket->Fingers[Count].TouchMinor);
			continue;
		}

		// Stable ContactID = slot index. See comment above.
		PtpReport.Contacts[ReportedCount].ContactID = Count;

		// Normalize X: map [XMin..XMax] → [0..XRange] with saturating clamp.
		// LONG intermediates prevent signed SHORT overflow before comparison.
		RawX  = pSpiTrackpadPacket->Fingers[Count].X;
		NormX = (LONG)RawX - (LONG)pDeviceContext->TrackpadInfo.XMin;
		PtpReport.Contacts[ReportedCount].X =
			(NormX <= 0)                          ? 0 :
			(NormX >= (LONG)pDeviceContext->XRange) ? pDeviceContext->XRange :
			                                          (USHORT)NormX;

		// Normalize Y: Apple Y grows downward; PTP Y grows upward (invert).
		RawY  = pSpiTrackpadPacket->Fingers[Count].Y;
		NormY = (LONG)pDeviceContext->TrackpadInfo.YMax - (LONG)RawY;
		PtpReport.Contacts[ReportedCount].Y =
			(NormY <= 0)                          ? 0 :
			(NormY >= (LONG)pDeviceContext->YRange) ? pDeviceContext->YRange :
			                                          (USHORT)NormY;

		// TipSwitch: include Pressure == 1.
		// T2 reports Pressure=1 on the first contact frame (finger forming).
		// Excluding it (old threshold > 1) made Windows miss the touchdown,
		// treating the next frame as a new contact mid-gesture.
		PtpReport.Contacts[ReportedCount].TipSwitch =
			(pSpiTrackpadPacket->Fingers[Count].Pressure >= 1) ? 1 : 0;

		// Confidence: always 1 for non-palm contacts.
		// Palm filtering is done above; everything that reaches here is a valid finger.
		PtpReport.Contacts[ReportedCount].Confidence = 1;

		TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
			"%!FUNC! Contact[%d->%d] ID=%d X=%d Y=%d P=%d Maj=%d Min=%d",
			Count, ReportedCount, Count,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y,
			pSpiTrackpadPacket->Fingers[Count].Pressure,
			pSpiTrackpadPacket->Fingers[Count].TouchMajor,
			pSpiTrackpadPacket->Fingers[Count].TouchMinor);

		ReportedCount++;
	}

	PtpReport.ReportID        = REPORTID_MULTITOUCH;
	PtpReport.ContactCount    = ReportedCount;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;
	PtpReport.ScanTime        = (USHORT) CounterDelta;

	Status = WdfRequestRetrieveOutputMemory(PtpRequest, &PtpRequestMemory);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfRequestRetrieveOutputMemory failed with %!STATUS!", Status);
		goto exit;
	}

	Status = WdfMemoryCopyFromBuffer(
		PtpRequestMemory, 0,
		(PVOID) &PtpReport,
		sizeof(PTP_REPORT)
	);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfMemoryCopyFromBuffer failed with %!STATUS!", Status);
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
