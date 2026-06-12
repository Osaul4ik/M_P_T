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

	RequestContext = WorkerRequestGetContext(SpiHidReadRequest);
	RequestContext->DeviceContext = pDeviceContext;
	RequestContext->RequestMemory = SpiHidReadOutputMemory;

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

	BOOLEAN Matched[PTP_MAX_CONTACT_POINTS];
	BOOLEAN UsedSlot[PTP_MAX_CONTACT_POINTS];
	UINT8 i, j;

	UNREFERENCED_PARAMETER(Target);

	RequestContext = (PWORKER_REQUEST_CONTEXT) Context;
	pDeviceContext = RequestContext->DeviceContext;

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

	PtpReport.ReportID        = REPORTID_MULTITOUCH;
	PtpReport.ContactCount    = AdjustedCount;
	PtpReport.IsButtonClicked = pSpiTrackpadPacket->ClickOccurred;
	PtpReport.ScanTime        = (USHORT) CounterDelta;

	// --- Contact ID assignment via OriginalX/Y matching ---
	RtlZeroMemory(Matched,  sizeof(Matched));
	RtlZeroMemory(UsedSlot, sizeof(UsedSlot));

	for (i = 0; i < AdjustedCount; i++) {
		for (j = 0; j < AdjustedCount; j++) {
			if (UsedSlot[j]) continue;
			if (pSpiTrackpadPacket->Fingers[i].OriginalX == pDeviceContext->PrevOriginalX[j] &&
				pSpiTrackpadPacket->Fingers[i].OriginalY == pDeviceContext->PrevOriginalY[j]) {
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
			pDeviceContext->NextContactID = (pDeviceContext->NextContactID + 1) % 15;
		}
	}

	for (i = 0; i < AdjustedCount; i++) {
		pDeviceContext->PrevOriginalX[i]  = pSpiTrackpadPacket->Fingers[i].OriginalX;
		pDeviceContext->PrevOriginalY[i]  = pSpiTrackpadPacket->Fingers[i].OriginalY;
		pDeviceContext->SlotContactID[i]  = PtpReport.Contacts[i].ContactID;
	}

	for (i = AdjustedCount; i < PTP_MAX_CONTACT_POINTS; i++) {
		pDeviceContext->PrevOriginalX[i] = 0x7FFF;
		pDeviceContext->PrevOriginalY[i] = 0x7FFF;
		pDeviceContext->SlotContactID[i] = 0xFF;
	}

	for (Count = 0; Count < AdjustedCount; Count++)
	{
		RawX  = pSpiTrackpadPacket->Fingers[Count].X;
		NormX = (LONG)RawX - (LONG)pDeviceContext->TrackpadInfo.XMin;
		PtpReport.Contacts[Count].X =
			(NormX <= 0) ? 0 :
			(NormX >= (LONG)pDeviceContext->XRange) ? pDeviceContext->XRange :
			(USHORT)NormX;

		RawY  = pSpiTrackpadPacket->Fingers[Count].Y;
		NormY = (LONG)pDeviceContext->TrackpadInfo.YMax - (LONG)RawY;
		PtpReport.Contacts[Count].Y =
			(NormY <= 0) ? 0 :
			(NormY >= (LONG)pDeviceContext->YRange) ? pDeviceContext->YRange :
			(USHORT)NormY;

		PtpReport.Contacts[Count].TipSwitch =
			(pSpiTrackpadPacket->Fingers[Count].Pressure >= 1) ? 1 : 0;

		PtpReport.Contacts[Count].Confidence =
			(pSpiTrackpadPacket->Fingers[Count].TouchMajor < 2500 ||
			 pSpiTrackpadPacket->Fingers[Count].TouchMinor < 2500) ? 1 : 0;

		TraceEvents(
			TRACE_LEVEL_VERBOSE,
			TRACE_HID_INPUT,
			"%!FUNC! Contact[%d] ID=%d X=%d Y=%d P=%d Maj=%d Min=%d",
			Count,
			PtpReport.Contacts[Count].ContactID,
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
