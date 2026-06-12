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
}

VOID
AmtPtpSpiInputIssueRequest(
	WDFDEVICE Device
)
{
	NTSTATUS Status;
	PDEVICE_CONTEXT pDeviceContext;
	BOOLEAN RequestStatus;
	WDF_REQUEST_REUSE_PARAMS ReuseParams;

	pDeviceContext = DeviceGetContext(Device);

	WDF_REQUEST_REUSE_PARAMS_INIT(&ReuseParams, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
	Status = WdfRequestReuse(pDeviceContext->SpiHidReadRequest, &ReuseParams);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"%!FUNC! WdfRequestReuse fails, status = %!STATUS!", Status);
		return;
	}

	RequestStatus = WdfRequestSend(
		pDeviceContext->SpiHidReadRequest,
		pDeviceContext->SpiTrackpadIoTarget,
		NULL
	);
	if (!RequestStatus) {
		// FIX: retrieve and log the actual failure status so it appears in
		// traces instead of a generic "failed" with no diagnostic value.
		Status = WdfRequestGetStatus(pDeviceContext->SpiHidReadRequest);
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"%!FUNC! WdfRequestSend failed, status = %!STATUS!", Status);
	}
}

#define PALM_MAJOR_THRESHOLD 3500
#define PALM_MINOR_THRESHOLD 3500

#define FIRST_FRAME_SCANTIME_CAP  80
#define IDLE_SCANTIME_THRESHOLD   5000

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
	SIZE_T BufLen;
	PSPI_TRACKPAD_PACKET pSpiTrackpadPacket;

	WDFREQUEST PtpRequest;
	PTP_REPORT PtpReport;
	WDFMEMORY PtpRequestMemory;

	ULONGLONG CurrentTime;
	LONGLONG CounterDelta;
	UINT8 AdjustedCount;
	UINT8 ReportedCount;
	// Bitmask of ContactIDs written into the current report with TipSwitch=1.
	// Bit N set => ContactID N is live in this frame.
	UINT8 CurrentReportedMask;
	UINT8 Count;
	UINT8 i;
	SHORT RawX;
	SHORT RawY;
	LONG NormX;
	LONG NormY;
	BOOLEAN IsPalm;

	UNREFERENCED_PARAMETER(Target);
	UNREFERENCED_PARAMETER(SpiRequest);

	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
			"%!FUNC! No PTP request pending (%!STATUS!), skipping report", Status);
		goto cleanup;
	}

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

	// FIX (buffer safety): retrieve the actual allocated buffer size alongside
	// the pointer.  Validates that Information (bytes transferred as reported
	// by the lower driver) does not exceed the memory object's real capacity —
	// a misbehaving lower driver could report more bytes than the buffer holds.
	BufLen = 0;
	pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET) WdfMemoryGetBuffer(
		Params->Parameters.Ioctl.Output.Buffer, &BufLen);

	if (SpiRequestLength > (LONG)BufLen) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Information (%d) exceeds buffer size (%Iu), rejecting",
			SpiRequestLength, BufLen);
		Status = STATUS_BUFFER_OVERFLOW;
		goto exit;
	}

	if (SpiRequestLength < 46) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Packet too short for header: %d < 46", SpiRequestLength);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// FIX (OOB): clamp NumOfFingers before using it in the packet length
	// calculation.  Rejects malformed packets that claim an impossible finger
	// count before we perform the multiply, and before AdjustedCount clamping
	// which happens later in the data path.
	if (pSpiTrackpadPacket->NumOfFingers > SPI_TRACKPAD_MAX_FINGERS) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! NumOfFingers %d exceeds maximum %d, rejecting",
			pSpiTrackpadPacket->NumOfFingers, SPI_TRACKPAD_MAX_FINGERS);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	MinPacketLength = 46 + (LONG)pSpiTrackpadPacket->NumOfFingers *
	                       (LONG)sizeof(SPI_TRACKPAD_FINGER);
	if (SpiRequestLength < MinPacketLength) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Packet truncated: %d < %d (%d fingers claimed)",
			SpiRequestLength, MinPacketLength, pSpiTrackpadPacket->NumOfFingers);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// KeQueryInterruptTime returns 100ns units since boot.
	// Divide by 1000 to convert 100ns -> 100us for HID PTP ScanTime.
	// FIX (sync): LastReportTime is now ULONGLONG — subtract unsigned to
	// unsigned, avoiding the old signed cast that could produce spurious
	// negatives on wrap or if the field was corrupted.
	CurrentTime  = KeQueryInterruptTime();
	CounterDelta = (LONGLONG)((CurrentTime - pDeviceContext->LastReportTime) / 1000);
	if (CounterDelta < 0) {
		CounterDelta = 0;
	}
	if (CounterDelta > 0xFFFF) {
		CounterDelta = 0xFFFF;
	}
	// FIX (sync): KeMemoryBarrier() ensures the compiler does not reorder this
	// store past the DeviceStatus read in the cleanup branch below.
	pDeviceContext->LastReportTime = CurrentTime;
	KeMemoryBarrier();

	RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));

	AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > PTP_MAX_CONTACT_POINTS)
		? PTP_MAX_CONTACT_POINTS
		: pSpiTrackpadPacket->NumOfFingers;

	// FIX (cursor jump #3): use PrevReportedCount — contacts actually sent to
	// the host — for the first-frame ScanTime cap, not PrevAdjustedCount.
	//
	// When all hardware fingers were palm-suppressed in the previous frame,
	// PrevAdjustedCount is non-zero but PrevReportedCount is zero.  Using
	// PrevAdjustedCount skipped the cap in that case, producing an inflated
	// ScanTime delta that the host interpreted as high-velocity movement and
	// interpolated as cursor teleportation.
	if (pDeviceContext->PrevReportedCount == 0 && AdjustedCount > 0) {
		if (CounterDelta > IDLE_SCANTIME_THRESHOLD) {
			CounterDelta = FIRST_FRAME_SCANTIME_CAP;
		}
	}
	// Still update PrevAdjustedCount for any code that needs the raw count.
	pDeviceContext->PrevAdjustedCount = AdjustedCount;

	for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
		pDeviceContext->SlotIsPalm[i] = FALSE;
	}

	ReportedCount       = 0;
	CurrentReportedMask = 0;

	for (Count = 0; Count < AdjustedCount; Count++) {

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

		// FIX (cursor jump #1): assign ContactID from ReportedCount, not from
		// the hardware slot index (Count).
		//
		// Using Count created gaps when a slot was palm-suppressed:
		//   Count=0 -> ContactID=0 (reported)
		//   Count=1 -> palm, skipped
		//   Count=2 -> ContactID=2 (reported, but slot 1 was never closed)
		//
		// The Windows PTP host stack held ContactID=1 open indefinitely with
		// its last known X/Y.  On the next finger-down the stack re-activated
		// that ghost contact and the cursor jumped back to the coordinates of
		// the vanished finger — which is exactly the right-click cursor jump.
		PtpReport.Contacts[ReportedCount].ContactID = ReportedCount;

		RawX  = pSpiTrackpadPacket->Fingers[Count].X;
		NormX = (LONG)RawX - (LONG)pDeviceContext->TrackpadInfo.XMin;
		PtpReport.Contacts[ReportedCount].X =
			(NormX <= 0)                            ? 0 :
			(NormX >= (LONG)pDeviceContext->XRange) ? pDeviceContext->XRange :
			                                          (USHORT)NormX;

		RawY  = pSpiTrackpadPacket->Fingers[Count].Y;
		NormY = (LONG)pDeviceContext->TrackpadInfo.YMax - (LONG)RawY;
		PtpReport.Contacts[ReportedCount].Y =
			(NormY <= 0)                            ? 0 :
			(NormY >= (LONG)pDeviceContext->YRange) ? pDeviceContext->YRange :
			                                          (USHORT)NormY;

		// FIX (inertia): store clamped coordinates as last-known position for
		// this slot. Lift frames use these instead of (0,0) so the host sees
		// the finger disappear at its real last position and computes the
		// correct exit velocity for inertial scroll.
		pDeviceContext->SlotLastX[ReportedCount] = PtpReport.Contacts[ReportedCount].X;
		pDeviceContext->SlotLastY[ReportedCount] = PtpReport.Contacts[ReportedCount].Y;

		// FIX (inertia): TipSwitch driven by presence in NumOfFingers, not
		// Pressure. On edge exits hardware may report Pressure=0 for one or
		// more frames before dropping the slot — treating that as a lift cuts
		// the velocity sample the host needs to compute inertial scroll.
		PtpReport.Contacts[ReportedCount].TipSwitch  = 1;
		PtpReport.Contacts[ReportedCount].Confidence = 1;

		// Mark this ContactID as live in the current frame.
		CurrentReportedMask |= (UINT8)(1u << ReportedCount);

		TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
			"%!FUNC! Contact[hw=%d->rpt=%d] ID=%d X=%d Y=%d P=%d Maj=%d Min=%d",
			Count, ReportedCount, ReportedCount,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y,
			pSpiTrackpadPacket->Fingers[Count].Pressure,
			pSpiTrackpadPacket->Fingers[Count].TouchMajor,
			pSpiTrackpadPacket->Fingers[Count].TouchMinor);

		ReportedCount++;
	}

	// FIX (cursor jump #2): emit lift frames (TipSwitch=0, Confidence=0) for
	// any ContactID that was live in the previous report but absent this frame.
	//
	// Without this, the Windows PTP host stack retains the last-known X/Y of
	// the missing contact indefinitely.  On the next finger-down it
	// re-activates that stale contact and the cursor teleports back — which is
	// the second half of the right-click cursor-jump scenario (the first half
	// is fixed by ContactID = ReportedCount above).
	//
	// We compute DroppedMask = bits that were set previously but not now.
	// Each set bit corresponds to a ContactID that needs a lift frame.
	{
		UINT8 DroppedMask = pDeviceContext->PrevReportedMask & ~CurrentReportedMask;
		UINT8 LiftBit;
		for (LiftBit = 0; LiftBit < PTP_MAX_CONTACT_POINTS; LiftBit++) {
			if (!(DroppedMask & (UINT8)(1u << LiftBit))) {
				continue;
			}
			if (ReportedCount >= PTP_MAX_CONTACT_POINTS) {
				// Contacts[] is full — no room for more lift frames.
				// This can only happen if PrevReportedMask had more bits than
				// PTP_MAX_CONTACT_POINTS, which violates our invariant.
				// Log and stop rather than write out of bounds.
				TraceEvents(TRACE_LEVEL_WARNING, TRACE_HID_INPUT,
					"%!FUNC! No room for lift frame ContactID=%d "
					"(PrevMask=0x%02x CurrMask=0x%02x)",
					LiftBit,
					pDeviceContext->PrevReportedMask,
					CurrentReportedMask);
				break;
			}
			PtpReport.Contacts[ReportedCount].ContactID  = LiftBit;
			PtpReport.Contacts[ReportedCount].TipSwitch  = 0;
			PtpReport.Contacts[ReportedCount].Confidence = 0;
			PtpReport.Contacts[ReportedCount].X          = 0;
			PtpReport.Contacts[ReportedCount].Y          = 0;

			TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
				"%!FUNC! Lift frame emitted for ContactID=%d", LiftBit);

			ReportedCount++;
		}
	}

	PtpReport.ReportID        = REPORTID_MULTITOUCH;
	PtpReport.ContactCount    = ReportedCount;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;
	PtpReport.ScanTime        = (USHORT) CounterDelta;

	// Update per-frame tracking state.
	// PrevReportedCount tracks only "real" contacts (not lift frames) because
	// lift frames are synthetic — they must not influence the first-frame
	// ScanTime cap on the next touch sequence.
	// CurrentReportedMask already excludes lift entries by construction
	// (lift bits are in DroppedMask which is the complement of CurrentReportedMask).
	{
		UINT8 LiftCount = (UINT8)__popcnt(
			pDeviceContext->PrevReportedMask & ~CurrentReportedMask);
		pDeviceContext->PrevReportedCount =
			(ReportedCount > LiftCount) ? (ReportedCount - LiftCount) : 0;
	}
	pDeviceContext->PrevReportedMask = CurrentReportedMask;

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
	// FIX (sync): use DEVICE_STATUS_READ (InterlockedCompareExchange) for a
	// safe cross-IRQL read of DeviceStatus before re-issuing the request.
	if (DEVICE_STATUS_READ(pDeviceContext) == D0ActiveAndConfigured) {
		AmtPtpSpiInputIssueRequest(pDeviceContext->SpiDevice);
	}
}
