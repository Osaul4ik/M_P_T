// Interrupt.c: USB interrupt pipe setup and the top-level read-complete
// callback. Per-finger slot tracking lives in InterruptTouch.c
// (AmtPtpProcessTouchFrame).

#include "Driver.h"
#include "Interrupt.tmh"

// ---- continuous-reader configuration ----------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t transferLength = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1: transferLength = HEADER_TYPE1 + FSIZE_TYPE1 * MAX_FINGERS; break;
    case TYPE2: transferLength = HEADER_TYPE2 + FSIZE_TYPE2 * MAX_FINGERS; break;
    case TYPE3: transferLength = HEADER_TYPE3 + FSIZE_TYPE3 * MAX_FINGERS; break;
    case TYPE4: transferLength = HEADER_TYPE4 + FSIZE_TYPE4 * MAX_FINGERS; break;
    case TYPE5: transferLength = HEADER_TYPE5 + FSIZE_TYPE5 * MAX_FINGERS; break;
    }

    if (transferLength == 0) {
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &contReaderConfig,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    contReaderConfig.EvtUsbTargetPipeReadersFailed = AmtPtpEvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe,
        &contReaderConfig);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfUsbTargetPipeConfigContinuousReader failed %!STATUS!", status);
    }

exit:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    // Always return SUCCESS: the framework drops all frames if this returns
    // failure, which is worse than running with a bad transfer length.
    return STATUS_SUCCESS;
}

// ---- interrupt completion routine -------------------------------------------

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT         pCtx       = Context;
    const size_t            headerSize = (size_t)pCtx->DeviceInfo->tp_header;
    const size_t            fingerSize = (size_t)pCtx->DeviceInfo->tp_fsize;
    size_t                  raw_n;

    UCHAR*                  TouchBuffer;
    LARGE_INTEGER           CurrentPerfCounter;
    LONGLONG                PerfCounterDelta;
    NTSTATUS                Status;
    PTP_REPORT              PtpReport;
    UCHAR                   reportSlots = 0;

    WDFREQUEST              Request;
    WDFMEMORY               RequestMemory;

    // ---- basic packet validation ----------------------------------------
    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Malformed packet, length=%llu — dropped", NumBytesTransferred);
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! NULL TouchBuffer — dropped");
        return;
    }

    // ---- dequeue pending HID read request -------------------------------
    Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! No pending PTP request — dropped");
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestRetrieveOutputMemory failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    // ---- prepare report skeleton ----------------------------------------
    RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
    PtpReport.ReportID        = REPORTID_MULTITOUCH;
    PtpReport.IsButtonClicked = 0;

    // Scan time in 100 µs units, clamped to USHORT.
    KeQueryPerformanceCounter(&CurrentPerfCounter);
    PerfCounterDelta = (CurrentPerfCounter.QuadPart -
                        pCtx->LastReportTime.QuadPart) / 100;
    if (PerfCounterDelta > 0xFF) PerfCounterDelta = 0xFF;
    PtpReport.ScanTime = (USHORT)PerfCounterDelta;

    // ---- touch contacts (slot state machine in InterruptTouch.c) --------
    if (pCtx->PtpReportTouch) {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        AmtPtpProcessTouchFrame(pCtx, TouchBuffer, raw_n, &PtpReport, &reportSlots);
        PtpReport.ContactCount = reportSlots;
    }

    // ---- button ---------------------------------------------------------
    if (pCtx->PtpReportButton) {
        if (TouchBuffer[pCtx->DeviceInfo->tp_button]) {
            PtpReport.IsButtonClicked = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Trackpad button clicked");
        }
    }

    // ---- write report back to HID stack ---------------------------------
    pCtx->LastReportTime = CurrentPerfCounter;

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&PtpReport, sizeof(PTP_REPORT));

    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, Status);
}

// ---- reader failure callback ------------------------------------------------

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);

    return TRUE;  // restart the reader
}