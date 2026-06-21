// Interrupt.c: USB completion handling.
//
// CORRECTED scope: this file owns exactly four things:
//   1. USB read completion mechanics (buffer validation, request plumbing)
//   2. RawFrame construction (delegated to Input.c - no decisions here)
//   3. ONE call to PTPCore_ProcessFrame (all matching, lifecycle FSM,
//      palm/gesture session bookkeeping lives there - see PTPCore.c)
//   4. Serializing PTP_CORE_FRAME into wire-format PTP_REPORT
//
// It does NOT decide grace-vs-kill, retap-vs-raw, or any other lifecycle
// routing - those decisions are not visible in this file at all anymore.

#include "Driver.h"
#include "PTPCore.h"
#include "Input.h"
#include "Interrupt.tmh"

// Hot-path trace rate limiting - max 1 verbose/info trace per interval.
#define TRACE_HOT_PATH_MIN_INTERVAL_100NS  (50LL * 10000LL)  // 50 ms

// FIX (Task 4.1): no longer static - PTPCore.c shares this rate-gate
// (and DEVICE_CONTEXT.LastHotPathTraceQpc state) for its own diagnostic
// trace. Declared in PTPCore.h.
BOOLEAN
AmtHotPathTraceGate(_Inout_ PDEVICE_CONTEXT pCtx, _In_ LONGLONG NowQpc100ns)
{
    if (NowQpc100ns - pCtx->LastHotPathTraceQpc < TRACE_HOT_PATH_MIN_INTERVAL_100NS)
        return FALSE;
    pCtx->LastHotPathTraceQpc = NowQpc100ns;
    return TRUE;
}

#if DBG
static VOID
AmtReportCheckInvariants(_In_ const PTP_REPORT* Report)
{
    NT_ASSERT(Report->ContactCount <= PTP_MAX_CONTACT_POINTS);

    for (UCHAR a = 0; a < Report->ContactCount; a++) {
        NT_ASSERT(Report->Contacts[a].TipSwitch == 0 || Report->Contacts[a].TipSwitch == 1);
        for (UCHAR b = (UCHAR)(a + 1); b < Report->ContactCount; b++) {
            NT_ASSERT(Report->Contacts[a].ContactID != Report->Contacts[b].ContactID);
        }
    }
}
#else
#define AmtReportCheckInvariants(Report) ((VOID)0)
#endif

// Serializes PTP_CORE_FRAME into wire-format PTP_REPORT. Pure formatting
// - no decisions about what is reported or why.
static VOID
AmtSerializeCoreFrameToReport(
    _In_  const PTP_CORE_FRAME* CoreFrame,
    _Out_ PTP_REPORT*           Report
)
{
    UCHAR n = CoreFrame->ContactCount;
    if (n > PTP_MAX_CONTACT_POINTS) n = PTP_MAX_CONTACT_POINTS;

    for (UCHAR i = 0; i < n; i++) {
        const PTP_CORE_CONTACT* c = &CoreFrame->Contacts[i];

        Report->Contacts[i].ContactID  = c->ContactID;
        Report->Contacts[i].X          = c->X;
        Report->Contacts[i].Y          = c->Y;
        Report->Contacts[i].TipSwitch  = (c->Phase == CONTACT_PHASE_UP) ? 0 : 1;
        Report->Contacts[i].Confidence = c->Confident ? 1 : 0;
    }

    Report->ContactCount = n;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t   transferLength = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1: transferLength = HEADER_TYPE1 + FSIZE_TYPE1 * MAX_FINGERS; break;
    case TYPE2: transferLength = HEADER_TYPE2 + FSIZE_TYPE2 * MAX_FINGERS; break;
    case TYPE3: transferLength = HEADER_TYPE3 + FSIZE_TYPE3 * MAX_FINGERS; break;
    case TYPE4: transferLength = HEADER_TYPE4 + FSIZE_TYPE4 * MAX_FINGERS; break;
    case TYPE5: transferLength = HEADER_TYPE5 + FSIZE_TYPE5 * MAX_FINGERS; break;
    default:
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! Unknown tp_type %d", DeviceContext->DeviceInfo->tp_type);
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
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
        DeviceContext->InterruptPipe, &contReaderConfig);

    if (!NT_SUCCESS(status))
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfUsbTargetPipeConfigContinuousReader failed %!STATUS!", status);

exit:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return status;
}

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pCtx       = Context;
    size_t          headerSize = (unsigned int)pCtx->DeviceInfo->tp_header;
    size_t          fingerSize = (unsigned int)pCtx->DeviceInfo->tp_fsize;
    size_t          raw_n;
    UCHAR*          TouchBuffer = NULL;

    LONGLONG      PerfDelta;
    LARGE_INTEGER Now;
    NTSTATUS      Status;
    PTP_REPORT    Report;
    WDFREQUEST    Request;
    WDFMEMORY     RequestMemory;

    // ---- 1. USB read completion mechanics ----

    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Malformed packet, len=%llu", (ULONG64)NumBytesTransferred);
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! NULL buffer");
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request);
    if (!NT_SUCCESS(Status))
        return;

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! RetrieveOutputMemory failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    KeQueryPerformanceCounter(&Now);
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;
    if (pCtx->PerfFrequency.QuadPart > 0)
        PerfDelta = PerfDelta * 10000LL / pCtx->PerfFrequency.QuadPart;
    else
        PerfDelta /= 100LL;
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    if (PerfDelta < 0)      PerfDelta = 0;
    Report.ScanTime = (USHORT)PerfDelta;
    pCtx->LastReportTime = Now;

    BOOLEAN buttonSnapshot =
        pCtx->PtpReportButton && TouchBuffer[pCtx->DeviceInfo->tp_button];

    // Typing suppression: "should we process input at all" gate, not a
    // PTPCore frame decision - stays here. When active, frame is treated
    // as empty, driving PTPCore_ProcessFrame to lift every active contact
    // through normal Phase A path (EnterGrace+ExpireGrace for gesture-
    // tainted contacts, same observable result as old direct Kill).
    BOOLEAN suppressingTyping = FALSE;
    {
        LONGLONG suppressUntil = InterlockedCompareExchange64(
            &pCtx->TypingSuppressUntil, 0, 0);
        suppressingTyping = (suppressUntil > Now.QuadPart);

        if (suppressingTyping && AmtHotPathTraceGate(pCtx, Now.QuadPart)) {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! Typing suppression active");
        }
    }

    // ---- 2. RawFrame construction (InputAdapter - no decisions) ----
    RAW_FRAME rawFrame;
    RtlZeroMemory(&rawFrame, sizeof(rawFrame));
    rawFrame.TimestampQpc = Now.QuadPart;

    if (pCtx->PtpReportTouch && !suppressingTyping) {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        if (raw_n * fingerSize > (NumBytesTransferred - headerSize)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Buffer size mismatch");
            WdfRequestComplete(Request, STATUS_DATA_ERROR);
            return;
        }

        UCHAR* f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;
        AmtInputParseFrame(f_base, fingerSize, raw_n, pCtx->DeviceInfo,
                           Now.QuadPart, &rawFrame);
    }
    // else: empty RawFrame -> PTPCore_ProcessFrame lifts all active contacts.

    // ---- 3. PTPCore: the single orchestration entry point ----
    PTP_CORE_FRAME coreFrame;
    PTPCore_ProcessFrame(pCtx, &rawFrame, Now.QuadPart, &coreFrame);

    // ---- 4. Serialize into PTP_REPORT ----
    AmtSerializeCoreFrameToReport(&coreFrame, &Report);

    if (buttonSnapshot) {
        Report.IsButtonClicked = TRUE;
        if (AmtHotPathTraceGate(pCtx, Now.QuadPart)) {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT, "%!FUNC! Button clicked");
        }
    }

    AmtReportCheckInvariants(&Report);

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);
    return TRUE;
}