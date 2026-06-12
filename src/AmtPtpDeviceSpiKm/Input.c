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
	RequestContext->DeviceContext  = pDeviceContext;
	RequestContext->RequestMemory  = SpiHidReadOutputMemory;

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

// Palm detection thresholds.
// A contact is a palm when BOTH axes exceed the threshold simultaneously.
// TouchMajor/TouchMinor units ~= 0.01mm for T2 MacBook trackpads.
// 3500 ~= 35mm: a fingertip at angle may exceed 35mm on Major but not Minor.
// 3500 on both axes simultaneously means a flat palm.
#define PALM_MAJOR_THRESHOLD 3500
#define PALM_MINOR_THRESHOLD 3500

// Once a slot is marked as palm, keep it suppressed until lift-off
// (sticky palm: avoids flicker when palm briefly dips below threshold).
static BOOLEAN
IsPalmContact(
	_In_ PDEVICE_CONTEXT pDeviceContext,
	_In_ UINT8 SlotIndex,
	_In_ SHORT TouchMajor,
	_In_ SHORT TouchMinor
)
{
	BOOLEAN IsPalm;

	// Sticky: once flagged as palm, stays palm until lift-off.
	if (pDeviceContext->SlotIsPalm[SlotIndex]) {
		return TRUE;
	}

	IsPalm = (TouchMajor >= PALM_MAJOR_THRESHOLD &&
	           TouchMinor >= PALM_MINOR_THRESHOLD);

	if (IsPalm) {
		pDeviceContext->SlotIsPalm[SlotIndex] = TRUE;
	}

	return IsPalm;
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
	LARGE_INTEGER CurrentSystemTime;
	LONGLONG CounterDelta;
	LONGLONG KeyboardSuppressDelta;
	UINT8 AdjustedCount;
	UINT8 ReportedCount;
	UINT8 Count;
	SHORT RawX;
	SHORT RawY;
	LONG NormX;
	LONG NormY;

	BOOLEAN Matched[PTP_MAX_CONTACT_POINTS];
	BOOLEAN UsedSlot[PTP_MAX_CONTACT_POINTS];
	UINT8 i, j;

	UNREFERENCED_PARAMETER(Target);

	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfIoQueueRetrieveNextRequest failed with %!STATUS!", Status);
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

	// Keyboard suppression: drop all touch input for 1 second after keystroke.
	// This prevents accidental cursor movement or gesture triggering while typing.
	if (pDeviceContext->LastKeyboardEventTime.QuadPart > 0) {
		KeQuerySystemTime(&CurrentSystemTime);
		KeyboardSuppressDelta =
			CurrentSystemTime.QuadPart -
			pDeviceContext->LastKeyboardEventTime.QuadPart;
		if (KeyboardSuppressDelta < KEYBOARD_SUPPRESSION_INTERVAL) {
			// Still within suppression window — send empty report.
			RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
			PtpReport.ReportID = REPORTID_MULTITOUCH;
			Status = WdfRequestRetrieveOutputMemory(PtpRequest, &PtpRequestMemory);
			if (NT_SUCCESS(Status)) {
				WdfMemoryCopyFromBuffer(PtpRequestMemory, 0, &PtpReport, sizeof(PTP_REPORT));
				WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));
			}
			goto exit;
		}
	}

	SpiRequestLength = (LONG) WdfRequestGetInformation(SpiRequest);
	pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET) WdfMemoryGetBuffer(
		Params->Parameters.Ioctl.Output.Buffer, NULL);

	if (SpiRequestLength < 46) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Packet too short for header: %d < 46", SpiRequestLength);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	MinPacketLength = 46 + (LONG)pSpiTrackpadPacket->NumOfFingers *
	                       (LONG)sizeof(SPI_TRACKPAD_FINGER);
	if (SpiRequestLength < MinPacketLength) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! Packet truncated: %d < %d (%d fingers claimed)",
			SpiRequestLength, MinPacketLength,
			pSpiTrackpadPacket->NumOfFingers);
		Status = STATUS_DEVICE_DATA_ERROR;
		goto exit;
	}

	// Compute ScanTime (100us units per HID PTP spec).
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

	RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));

	AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > PTP_MAX_CONTACT_POINTS)
		? PTP_MAX_CONTACT_POINTS
		: pSpiTrackpadPacket->NumOfFingers;

	// Zero ScanTime delta on first frame of a new touch sequence.
	// Without this, the first frame after a pause has a huge delta,
	// making Windows think the gesture started slowly → weak inertia.
	if (pDeviceContext->PrevAdjustedCount == 0 && AdjustedCount > 0) {
		CounterDelta = 0;
	}
	pDeviceContext->PrevAdjustedCount = AdjustedCount;

	// Clear palm state for slots that lifted off.
	for (i = AdjustedCount; i < PTP_MAX_CONTACT_POINTS; i++) {
		pDeviceContext->SlotIsPalm[i] = FALSE;
	}

	// --- Contact ID assignment via OriginalX/Y matching ---
	RtlZeroMemory(Matched,  sizeof(Matched));
	RtlZeroMemory(UsedSlot, sizeof(UsedSlot));

	for (i = 0; i < AdjustedCount; i++) {
		for (j = 0; j < AdjustedCount; j++) {
			if (UsedSlot[j]) continue;
			if (pSpiTrackpadPacket->Fingers[i].OriginalX == pDeviceContext->PrevOriginalX[j] &&
				pSpiTrackpadPacket->Fingers[i].OriginalY == pDeviceContext->PrevOriginalY[j])
			{
				PtpReport.Contacts[i].ContactID = pDeviceContext->SlotContactID[j];
				Matched[i] = TRUE;
				UsedSlot[j] = TRUE;
				break;
			}
		}
	}

	for (i = 0; i < AdjustedCount; i++) {
		if (!Matched[i]) {
			PtpReport.Contacts[i].ContactID = pDeviceContext->NextContactID;
			pDeviceContext->NextContactID = (pDeviceContext->NextContactID + 1) % 8;
		}
	}

	for (i = 0; i < AdjustedCount; i++) {
		pDeviceContext->PrevOriginalX[i] = pSpiTrackpadPacket->Fingers[i].OriginalX;
		pDeviceContext->PrevOriginalY[i] = pSpiTrackpadPacket->Fingers[i].OriginalY;
		pDeviceContext->SlotContactID[i] = PtpReport.Contacts[i].ContactID;
	}
	for (i = AdjustedCount; i < PTP_MAX_CONTACT_POINTS; i++) {
		pDeviceContext->PrevOriginalX[i] = 0x7FFF;
		pDeviceContext->PrevOriginalY[i] = 0x7FFF;
		pDeviceContext->SlotContactID[i] = 0xFF;
	}

	// Build contact records, skipping palms.
	// ReportedCount tracks only non-palm contacts sent to Windows.
	ReportedCount = 0;
	for (Count = 0; Count < AdjustedCount; Count++) {
		BOOLEAN IsPalm = IsPalmContact(
			pDeviceContext,
			Count,
			pSpiTrackpadPacket->Fingers[Count].TouchMajor,
			pSpiTrackpadPacket->Fingers[Count].TouchMinor
		);

		if (IsPalm) {
			TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
				"%!FUNC! Contact[%d] classified as palm, suppressed", Count);
			continue;
		}

		RawX  = pSpiTrackpadPacket->Fingers[Count].X;
		NormX = (LONG)RawX - (LONG)pDeviceContext->TrackpadInfo.XMin;
		PtpReport.Contacts[ReportedCount].X =
			(NormX <= 0) ? 0 :
			(NormX >= (LONG)pDeviceContext->XRange) ? pDeviceContext->XRange :
			(USHORT)NormX;

		RawY  = pSpiTrackpadPacket->Fingers[Count].Y;
		NormY = (LONG)pDeviceContext->TrackpadInfo.YMax - (LONG)RawY;
		PtpReport.Contacts[ReportedCount].Y =
			(NormY <= 0) ? 0 :
			(NormY >= (LONG)pDeviceContext->YRange) ? pDeviceContext->YRange :
			(USHORT)NormY;

		PtpReport.Contacts[ReportedCount].ContactID = PtpReport.Contacts[Count].ContactID;

		PtpReport.Contacts[ReportedCount].TipSwitch =
			(pSpiTrackpadPacket->Fingers[Count].Pressure >= 1) ? 1 : 0;

		// Confidence: always 1 for non-palm contacts.
		// Palm filtering is done above; remaining contacts are valid fingers.
		PtpReport.Contacts[ReportedCount].Confidence = 1;

		TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
			"%!FUNC! Contact[%d->%d] ID=%d X=%d Y=%d P=%d Maj=%d Min=%d",
			Count, ReportedCount,
			PtpReport.Contacts[ReportedCount].ContactID,
			pSpiTrackpadPacket->Fingers[Count].X,
			pSpiTrackpadPacket->Fingers[Count].Y,
			pSpiTrackpadPacket->Fingers[Count].Pressure,
			pSpiTrackpadPacket->Fingers[Count].TouchMajor,
			pSpiTrackpadPacket->Fingers[Count].TouchMinor
		);

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
