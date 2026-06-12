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

    RequestContext = (PWORKER_REQUEST_CONTEXT)Context;
    pDeviceContext = RequestContext->DeviceContext;

    Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->HidQueue, &PtpRequest);
    if (!NT_SUCCESS(Status)) {
        goto cleanup;
    }

    if (!pDeviceContext->PtpInputOn || !pDeviceContext->PtpReportTouch) {
        RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
        PtpReport.ReportID = REPORTID_MULTITOUCH;

        if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(PtpRequest, &PtpRequestMemory))) {
            WdfMemoryCopyFromBuffer(PtpRequestMemory, 0, &PtpReport, sizeof(PTP_REPORT));
            WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));
        }
        goto exit;
    }

    SpiRequestLength = (LONG)WdfRequestGetInformation(SpiRequest);

    BufLen = 0;
    pSpiTrackpadPacket = (PSPI_TRACKPAD_PACKET)WdfMemoryGetBuffer(
        Params->Parameters.Ioctl.Output.Buffer, &BufLen);

    if (SpiRequestLength > (LONG)BufLen) {
        Status = STATUS_BUFFER_OVERFLOW;
        goto exit;
    }

    if (SpiRequestLength < 46) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto exit;
    }

    if (pSpiTrackpadPacket->NumOfFingers > SPI_TRACKPAD_MAX_FINGERS) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto exit;
    }

    MinPacketLength = 46 + (LONG)pSpiTrackpadPacket->NumOfFingers *
        (LONG)sizeof(SPI_TRACKPAD_FINGER);

    if (SpiRequestLength < MinPacketLength) {
        Status = STATUS_DEVICE_DATA_ERROR;
        goto exit;
    }

    CurrentTime = KeQueryInterruptTime();
    CounterDelta =
        (LONGLONG)((CurrentTime - pDeviceContext->LastReportTime) / 1000);

    if (CounterDelta < 0) CounterDelta = 0;
    if (CounterDelta > 0xFFFF) CounterDelta = 0xFFFF;

    pDeviceContext->LastReportTime = CurrentTime;
    KeMemoryBarrier();

    RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));

    AdjustedCount =
        (pSpiTrackpadPacket->NumOfFingers > PTP_MAX_CONTACT_POINTS)
        ? PTP_MAX_CONTACT_POINTS
        : pSpiTrackpadPacket->NumOfFingers;

    if (pDeviceContext->PrevReportedCount == 0 && AdjustedCount > 0) {
        if (CounterDelta > IDLE_SCANTIME_THRESHOLD) {
            CounterDelta = FIRST_FRAME_SCANTIME_CAP;
        }
    }

    pDeviceContext->PrevAdjustedCount = AdjustedCount;

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        pDeviceContext->SlotIsPalm[i] = FALSE;
    }

    ReportedCount = 0;
    CurrentReportedMask = 0;

    for (Count = 0; Count < AdjustedCount; Count++) {

        IsPalm =
            (pSpiTrackpadPacket->Fingers[Count].TouchMajor >= PALM_MAJOR_THRESHOLD &&
             pSpiTrackpadPacket->Fingers[Count].TouchMinor >= PALM_MINOR_THRESHOLD);

        pDeviceContext->SlotIsPalm[Count] = IsPalm;

        if (IsPalm) continue;

        PtpReport.Contacts[ReportedCount].ContactID = ReportedCount;

        RawX = pSpiTrackpadPacket->Fingers[Count].X;
        NormX = (LONG)RawX - (LONG)pDeviceContext->TrackpadInfo.XMin;

        PtpReport.Contacts[ReportedCount].X =
            (NormX <= 0) ? 0 :
            (NormX >= (LONG)pDeviceContext->XRange) ? pDeviceContext->XRange :
            (USHORT)NormX;

        RawY = pSpiTrackpadPacket->Fingers[Count].Y;
        NormY = (LONG)pDeviceContext->TrackpadInfo.YMax - (LONG)RawY;

        PtpReport.Contacts[ReportedCount].Y =
            (NormY <= 0) ? 0 :
            (NormY >= (LONG)pDeviceContext->YRange) ? pDeviceContext->YRange :
            (USHORT)NormY;

        pDeviceContext->SlotLastX[ReportedCount] =
            PtpReport.Contacts[ReportedCount].X;

        pDeviceContext->SlotLastY[ReportedCount] =
            PtpReport.Contacts[ReportedCount].Y;

        PtpReport.Contacts[ReportedCount].TipSwitch = 1;
        PtpReport.Contacts[ReportedCount].Confidence = 1;

        CurrentReportedMask |= (UINT8)(1u << ReportedCount);
        ReportedCount++;
    }

    {
        UINT8 DroppedMask =
            pDeviceContext->PrevReportedMask & ~CurrentReportedMask;

        for (UINT8 LiftBit = 0; LiftBit < PTP_MAX_CONTACT_POINTS; LiftBit++) {

            if (!(DroppedMask & (1u << LiftBit))) continue;

            PtpReport.Contacts[ReportedCount].ContactID = LiftBit;
            PtpReport.Contacts[ReportedCount].TipSwitch = 0;
            PtpReport.Contacts[ReportedCount].Confidence = 0;
            PtpReport.Contacts[ReportedCount].X = 0;
            PtpReport.Contacts[ReportedCount].Y = 0;

            ReportedCount++;
        }
    }

    PtpReport.ReportID = REPORTID_MULTITOUCH;
    PtpReport.ContactCount = ReportedCount;
    PtpReport.ScanTime = (USHORT)CounterDelta;

    pDeviceContext->PrevReportedCount = ReportedCount;
    pDeviceContext->PrevReportedMask = CurrentReportedMask;

    if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(PtpRequest, &PtpRequestMemory))) {
        WdfMemoryCopyFromBuffer(PtpRequestMemory, 0, &PtpReport, sizeof(PTP_REPORT));
        WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));
    }

exit:
    WdfRequestComplete(PtpRequest, Status);

cleanup:
    if (DEVICE_STATUS_READ(pDeviceContext) == D0ActiveAndConfigured) {
        AmtPtpSpiInputIssueRequest(pDeviceContext->SpiDevice);
    }
}
