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

	// The SPI read pipeline is now driven entirely from
	// AmtPtpRequestCompletionRoutine. The first read is kicked off
	// from AmtPtpEvtDeviceSelfManagedIoInitOrRestart (or power recovery)
	// via AmtPtpSpiInputIssueRequest. After that, each completion
	// immediately issues the next read without waiting for Windows to
	// send another HID request — this prevents scroll stutter at 125Hz.
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

	// Reuse the pre-allocated request.
	// Format and completion routine are set once at device creation
	// and survive WdfRequestReuse — no per-frame setup needed.
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
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
			"%!FUNC! WdfRequestSend failed");
	}
}

// Palm rejection thresholds (Apple SPI raw units, ~0.01mm each).
// Reject only when BOTH axes are large simultaneously.
// A finger at an angle: large Major, small Minor — must NOT be rejected.
#define PALM_MAJOR_THRESHOLD 3500
#define PALM_MINOR_THRESHOLD 3500

// ScanTime cap for the first frame of a new touch sequence (100us units).
// 80 = 8ms, a typical 125Hz interval. Only applied when idle >= IDLE_SCANTIME_THRESHOLD.
#define FIRST_FRAME_SCANTIME_CAP  80

// Idle threshold: only cap ScanTime when the gap exceeds this value.
// 5000 units * 100us = 500ms — "trackpad was not touched for a while".
// Below this threshold the real delta is passed through so Windows can
// correctly distinguish rapid gesture sequences (e.g. triple-tap →
// two-finger tap → single tap) without swallowing the first click.
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
	PSPI_TRACKPAD_PACKET pSpiTrackpadPacket;

	WDFREQUEST PtpRequest;
	PTP_REPORT PtpReport;
	WDFMEMORY PtpRequestMemory;

	ULONGLONG CurrentTime;
	ULONGLONG QpcBias;
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
	UNREFERENCED_PARAMETER(SpiRequest); // pre-allocated, not deleted here

	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

	// Retrieve the pending PTP request. If the queue is empty (Windows
	// hasn't sent a new HID request yet), skip the report and fall through
	// to cleanup so we still issue the next SPI read. No request is leaked:
	// there is nothing in the queue to complete.
	Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
	if (!NT_SUCCESS(Status)) {
		TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
			"%!FUNC! No PTP request pending (%!STATUS!), skipping report", Status);
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
	// KeQueryInterruptTimeToPrecise returns 100ns units directly —
	// no division by PerformanceFrequency needed, and it is cheaper
	// than KeQueryPerformanceCounter (no RDTSC serialization fence).
	// Divide by 1000 to convert 100ns → 100us.
	CurrentTime = KeQueryInterruptTimeToPrecise(&QpcBias);
	CounterDelta = (LONGLONG)((CurrentTime - pDeviceContext->LastReportTime.QuadPart) / 1000);
	if (CounterDelta < 0) {
		CounterDelta = 0;
	}
	if (CounterDelta > 0xFFFF) {
		CounterDelta = 0xFFFF;
	}
	pDeviceContext->LastReportTime.QuadPart = (LONGLONG)CurrentTime;

	RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));

	AdjustedCount = (pSpiTrackpadPacket->NumOfFingers > PTP_MAX_CONTACT_POINTS)
		? PTP_MAX_CONTACT_POINTS
		: pSpiTrackpadPacket->NumOfFingers;

	// ScanTime cap: only when truly idle (gap > 500ms).
	// For short inter-gesture pauses the real delta is preserved so Windows
	// correctly identifies each new gesture as distinct and does not swallow
	// the first click of the next tap.
	if (pDeviceContext->PrevAdjustedCount == 0 && AdjustedCount > 0) {
		if (CounterDelta > IDLE_SCANTIME_THRESHOLD) {
			CounterDelta = FIRST_FRAME_SCANTIME_CAP;
		}
	}
	pDeviceContext->PrevAdjustedCount = AdjustedCount;

	// Clear palm state each frame; re-derive from current touch data.
	for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
		pDeviceContext->SlotIsPalm[i] = FALSE;
	}

	ReportedCount = 0;
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

		PtpReport.Contacts[ReportedCount].ContactID = Count;

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

		PtpReport.Contacts[ReportedCount].TipSwitch =
			(pSpiTrackpadPacket->Fingers[Count].Pressure >= 1) ? 1 : 0;

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
	// SpiRequest is the pre-allocated pDeviceContext->SpiHidReadRequest —
	// do NOT delete it here. It is reused via WdfRequestReuse next cycle.

	// Issue the next SPI read immediately, without waiting for Windows to
	// send another HID request. This keeps the SPI pipeline full at 125Hz
	// and prevents scroll from stalling under scheduler jitter.
	if (pDeviceContext->DeviceStatus == D0ActiveAndConfigured) {
		AmtPtpSpiInputIssueRequest(pDeviceContext->SpiDevice);
	}
}
