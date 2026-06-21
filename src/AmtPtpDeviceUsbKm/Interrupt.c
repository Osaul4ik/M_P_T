// Interrupt.c: Handles device input event. Kernel-mode Driver Framework
//
// Phase sequence (frame determinism — see Track.h):
//   L0 Button snapshot -> L1 Parse -> L1.5 Match -> Session Gesture FSM
//   -> Overflow drain -> Phase A Lift -> Phase B Birth -> Phase C Report
//
// Session owns: GestureSessionActive, alive-count, SlotLastLift*.
// Track owns: ReportX/Y, HystX/Y, ContactID, WasInGesture.
// Data flow: Session -> per-track WasInGesture (one-directional).
//
// FIX: GestureSessionActive is now a two-edge FSM (was write-only latch).
// FALSE on aliveCount==0, TRUE on >=2, unchanged on ==1.

#include "Driver.h"
#include "Track.h"
#include "Match.h"
#include "Interrupt.tmh"

// Hot-path trace rate limiting — max 1 verbose/info trace per interval.
// Error/warning paths never rate limited.
#define TRACE_HOT_PATH_MIN_INTERVAL_100NS  (50LL * 10000LL)  // 50 ms

static inline BOOLEAN
AmtHotPathTraceGate(_Inout_ PDEVICE_CONTEXT pCtx, _In_ LONGLONG NowQpc100ns)
{
    if (NowQpc100ns - pCtx->LastHotPathTraceQpc < TRACE_HOT_PATH_MIN_INTERVAL_100NS)
        return FALSE;
    pCtx->LastHotPathTraceQpc = NowQpc100ns;
    return TRUE;
}

// Debug-only PTP_REPORT invariant check — ContactID uniqueness,
// valid TipSwitch, ContactCount <= PTP_MAX_CONTACT_POINTS.
#if DBG
static VOID
AmtReportCheckInvariants(_In_ const PTP_REPORT* Report)
{
    // Assert ContactCount <= PTP_MAX_CONTACT_POINTS — guards against
    // future increment sites missing the bounds check.
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

// Captures lift position/time into session-scoped recent-lift memory.
// Called from all Phase A lift paths. Position-only, never feeds ContactID.
static inline VOID
AmtRecordSlotLift(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _In_    size_t          index,
    _In_    LONGLONG        NowQpc,
    _In_    USHORT          X,
    _In_    USHORT          Y)
{
    pCtx->SlotLastLiftQpc[index] = NowQpc;
    pCtx->SlotLastLiftX[index]   = X;
    pCtx->SlotLastLiftY[index]   = Y;
}

// Drains deferred lift-offs from previous frame into the current report.
// Clears OverflowCount. Max 1 frame extra latency.
static VOID
AmtDrainOverflow(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_REPORT*     Report,
    _Inout_ UCHAR*          ContactCount)
{
    for (UCHAR k = 0; k < pCtx->OverflowCount && *ContactCount < PTP_MAX_CONTACT_POINTS; k++) {
        Report->Contacts[*ContactCount].ContactID  = pCtx->OverflowContactID[k];
        Report->Contacts[*ContactCount].TipSwitch  = 0;
        Report->Contacts[*ContactCount].Confidence = 1;
        Report->Contacts[*ContactCount].X          = pCtx->OverflowX[k];
        Report->Contacts[*ContactCount].Y          = pCtx->OverflowY[k];
        (*ContactCount)++;
    }
    pCtx->OverflowCount = 0;
}

// Writes one lift-off PTP_CONTACT. If report full, defers to Overflow*
// for next frame — never silently drops a lift-off.
static VOID
AmtEmitLift(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_REPORT*     Report,
    _Inout_ UCHAR*          ContactCount,
    _In_    ULONG           ContactID,
    _In_    USHORT          X,
    _In_    USHORT          Y)
{
    if (*ContactCount < PTP_MAX_CONTACT_POINTS) {
        Report->Contacts[*ContactCount].ContactID  = ContactID;
        Report->Contacts[*ContactCount].TipSwitch  = 0;
        Report->Contacts[*ContactCount].Confidence = 1;
        Report->Contacts[*ContactCount].X          = X;
        Report->Contacts[*ContactCount].Y          = Y;
        (*ContactCount)++;
        return;
    }

    // Overflow capacity = PTP_MAX_CONTACT_POINTS — defensive bound.
    if (pCtx->OverflowCount < PTP_MAX_CONTACT_POINTS) {
        pCtx->OverflowContactID[pCtx->OverflowCount] = ContactID;
        pCtx->OverflowX[pCtx->OverflowCount]         = X;
        pCtx->OverflowY[pCtx->OverflowCount]         = Y;
        pCtx->OverflowCount++;
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT,
            "%!FUNC! lift-off overflow queue saturated — ContactID=%u lost",
            ContactID);
    }
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
    size_t          raw_n, i;
    UCHAR*          TouchBuffer = NULL;

    LONGLONG      PerfDelta;
    LARGE_INTEGER Now;
    NTSTATUS      Status;
    PTP_REPORT    Report;
    WDFREQUEST    Request;
    WDFMEMORY     RequestMemory;

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
    // Clamp ScanTime to [0, 0xFFFF]. QPC may be non-monotonic on some
    // platforms; negative delta would wrap to a huge value.
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    if (PerfDelta < 0)      PerfDelta = 0;
    Report.ScanTime = (USHORT)PerfDelta;
    pCtx->LastReportTime = Now;

    // L0 — Button snapshot. Read once at frame start to avoid race.
    BOOLEAN buttonSnapshot =
        pCtx->PtpReportButton && TouchBuffer[pCtx->DeviceInfo->tp_button];

    // Typing suppression
    {
        LONGLONG suppressUntil = InterlockedCompareExchange64(
            &pCtx->TypingSuppressUntil, 0, 0);

        if (suppressUntil > Now.QuadPart) {
            if (AmtHotPathTraceGate(pCtx, Now.QuadPart)) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! Typing suppression active");
            }

            if (buttonSnapshot)
                Report.IsButtonClicked = TRUE;

            UCHAR liftCount = 0;
            AmtDrainOverflow(pCtx, &Report, &liftCount);

            for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
                if (pCtx->Tracks[i].State != TRACK_ACTIVE) continue;

                ULONG  oldId; USHORT oldX, oldY;
                AmtTrackKill(pCtx->Tracks, i, &oldId, &oldX, &oldY);
                AmtRecordSlotLift(pCtx, i, Now.QuadPart, oldX, oldY);

                AmtEmitLift(pCtx, &Report, &liftCount, oldId, oldX, oldY);
            }
            Report.ContactCount = liftCount;

            // Pad empty during suppression — close gesture session.
            pCtx->GestureSessionActive = FALSE;

            AmtTrackCheckInvariants(pCtx->Tracks);
            AmtReportCheckInvariants(&Report);

            Status = WdfMemoryCopyFromBuffer(
                RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
            WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
            WdfRequestComplete(Request, NT_SUCCESS(Status) ? STATUS_SUCCESS : Status);
            return;
        }
    }

    raw_n = (NumBytesTransferred - headerSize) / fingerSize;
    UCHAR* f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;

    UCHAR contactCount = 0;

    // Drain deferred lift-offs from previous frame.
    AmtDrainOverflow(pCtx, &Report, &contactCount);

    if (pCtx->PtpReportTouch)
    {
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        if (raw_n * fingerSize > (NumBytesTransferred - headerSize)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Buffer size mismatch");
            WdfRequestComplete(Request, STATUS_DATA_ERROR);
            return;
        }

        // L1 — Parse. Pure decode, no mutation. Delegated to Match.c.
        MATCH_SAMPLE samples[PTP_MAX_CONTACT_POINTS];
        BOOLEAN      largePalm = FALSE;

        AmtMatchParseFrame(f_base, fingerSize, raw_n, pCtx->DeviceInfo,
                           pCtx->Tracks, samples, &largePalm);

        BOOLEAN palmFullyCleared = FALSE;

        if (largePalm) {
            pCtx->PalmDetected = TRUE;
        } else if (pCtx->PalmDetected) {
            BOOLEAN anyContact = FALSE;
            for (i = 0; i < raw_n; i++) {
                if (samples[i].Present || samples[i].PalmLocal) {
                    anyContact = TRUE;
                    break;
                }
            }
            if (!anyContact) {
                pCtx->PalmDetected = FALSE;
                palmFullyCleared   = TRUE;
                // Pad cleared after palm — Phase A lifts all tracks.
            } else {
                // Still palm-adjacent — suppress all samples.
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    samples[i].Present = FALSE;
            }
        }

        // L1.5 — Match. CONTINUES vs NEW_IDENTITY. No mutation.
        MATCH_VERDICT verdicts[PTP_MAX_CONTACT_POINTS];
        AmtMatchFrame(samples, raw_n, pCtx->Tracks, verdicts);

        // Session gesture FSM — no per-track mutation.
        // aliveCount==0 -> FALSE, >=2 -> TRUE, ==1 -> unchanged.
        // Computed from samples[] (same source Phase A uses), so never stale.
        UCHAR aliveCount = 0;
        for (i = 0; i < raw_n; i++) {
            if (samples[i].Present && !samples[i].PalmLocal)
                aliveCount++;
        }

        if (aliveCount == 0) {
            pCtx->GestureSessionActive = FALSE;
        } else if (aliveCount >= 2) {
            pCtx->GestureSessionActive = TRUE;
        }

        BOOLEAN gestureThisFrame = (aliveCount >= 2);

        // Phase A — Lift, no exceptions. Transition out tracks that
        // should not remain ACTIVE before Phase B. Gesture-tainted
        // routes through GRACE; others via Kill. Captures SlotLastLift*.
        UNREFERENCED_PARAMETER(palmFullyCleared); // handled by Phase A
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (pCtx->Tracks[i].State != TRACK_ACTIVE)
                continue;

            BOOLEAN slotHasFinger =
                (i < raw_n) && samples[i].Present && !samples[i].PalmLocal;
            BOOLEAN newIdentity =
                (i < raw_n) && slotHasFinger && (verdicts[i] == MATCH_NEW_IDENTITY);

            if (slotHasFinger && !newIdentity)
                continue; // continues to Phase C

            ULONG  oldId; USHORT oldX, oldY;

            if (pCtx->Tracks[i].WasInGesture) {
                AmtTrackEnterGrace(pCtx->Tracks, i, Now.QuadPart,
                                  &oldId, &oldX, &oldY);
                AmtTrackExpireGrace(pCtx->Tracks, i);
            } else {
                AmtTrackKill(pCtx->Tracks, i, &oldId, &oldX, &oldY);
            }

            AmtRecordSlotLift(pCtx, i, Now.QuadPart, oldX, oldY);
            AmtEmitLift(pCtx, &Report, &contactCount, oldId, oldX, oldY);

            if (AmtHotPathTraceGate(pCtx, Now.QuadPart)) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: lift-off (newIdentity=%d)",
                    (ULONG64)i, (int)newIdentity);
            }
        }

        AmtTrackCheckInvariants(pCtx->Tracks);

        // Phase B — Birth. Birth new tracks on slots with Present
        // samples and no ACTIVE track. Uses retap smoothing when
        // recent-lift memory indicates fast re-tap. Disables retap
        // smoothing for same-frame NEW_IDENTITY (different finger).
        for (i = 0; i < raw_n; i++) {
            if (!samples[i].Present || samples[i].PalmLocal)
                continue;
            if (pCtx->Tracks[i].State == TRACK_ACTIVE)
                continue; // handled in Phase C

            BOOLEAN sameFrameNewIdentity =
                (verdicts[i] == MATCH_NEW_IDENTITY);

            BOOLEAN looksLikeRetap =
                !sameFrameNewIdentity &&
                AmtTrackIsRecentLiftNearby(
                    pCtx->SlotLastLiftQpc[i], pCtx->SlotLastLiftX[i], pCtx->SlotLastLiftY[i],
                    Now.QuadPart, pCtx->PerfFrequency.QuadPart, samples[i].X, samples[i].Y);

            if (looksLikeRetap) {
                AmtTrackBirthWithRetapSmoothing(
                    pCtx->Tracks, i, &pCtx->NextContactId,
                    pCtx->SlotLastLiftX[i], pCtx->SlotLastLiftY[i]);
            } else {
                AmtTrackBirth(pCtx->Tracks, i, &pCtx->NextContactId,
                             samples[i].X, samples[i].Y);
            }

            if (gestureThisFrame) {
                // Born into multi-finger frame — mark gesture-tainted.
                pCtx->Tracks[i].WasInGesture = TRUE;
            }

            if (AmtHotPathTraceGate(pCtx, Now.QuadPart)) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: birth, new ContactID=%u x=%u y=%u retapSmoothed=%d",
                    (ULONG64)i, pCtx->Tracks[i].ContactID,
                    samples[i].X, samples[i].Y, (int)looksLikeRetap);
            }
        }

        AmtTrackCheckInvariants(pCtx->Tracks);

        // Phase C — Update / Report. Update each ACTIVE track once.
        for (i = 0; i < raw_n; i++) {
            if (pCtx->Tracks[i].State != TRACK_ACTIVE)
                continue;
            if (!samples[i].Present || samples[i].PalmLocal)
                continue; // unreachable post A/B

            if (gestureThisFrame) {
                pCtx->Tracks[i].WasInGesture = TRUE;
            }

            USHORT repX, repY;
            AmtTrackUpdate(&pCtx->Tracks[i], samples[i].X, samples[i].Y,
                          (BOOLEAN)(aliveCount == 1), &repX, &repY);

            if (contactCount < PTP_MAX_CONTACT_POINTS) {
                Report.Contacts[contactCount].ContactID  = pCtx->Tracks[i].ContactID;
                Report.Contacts[contactCount].X          = repX;
                Report.Contacts[contactCount].Y          = repY;
                Report.Contacts[contactCount].TipSwitch  = 1;
                // TipDropApplied -> Confidence=0 (position carried over
                // from debounce, not measured this frame).
                Report.Contacts[contactCount].Confidence =
                    (samples[i].TipDropApplied > 0) ? 0 : 1;
                contactCount++;
                pCtx->Tracks[i].ReportedLastFrame = TRUE;
            }
        }

        AmtTrackCheckInvariants(pCtx->Tracks);

        Report.ContactCount = contactCount;
    } else {
        Report.ContactCount = contactCount;
    }

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