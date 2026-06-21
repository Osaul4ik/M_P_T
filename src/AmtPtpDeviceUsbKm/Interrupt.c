// Interrupt.c: Handles device input event. Kernel-mode Driver Framework
//
// ---------------------------------------------------------------------
// Phase structure (frame determinism rule — see Track.h)
// ---------------------------------------------------------------------
// Every interrupt completion runs the SAME fixed sequence, with no
// exceptions to the ordering. This is also the canonical answer to task
// #6 (matching pipeline shape) — the sequence below already IS
// "parse contacts -> build candidates -> match active -> kill unmatched
// -> birth new -> emit report", just under the names this codebase uses:
//
//   L0   Button snapshot   - read the raw button byte ONCE, before any
//                            track mutation, so nothing later in the
//                            frame can observe a torn/changed button
//                            state (FIX: race in button state).
//   L1   Parse              - AmtMatchParseFrame: raw bytes -> normalized
//                            MATCH_SAMPLE[] ("build candidates"). No
//                            Track state mutated.
//   L1.5 Match               - AmtMatchFrame: decide CONTINUES vs
//                            NEW_IDENTITY per raw slot ("match active").
//                            No Track state mutated.
//   Session  Gesture FSM     - update GestureSessionActive from this
//                            frame's alive count. No per-track mutation.
//                            See SESSION/TRACK OWNERSHIP below (task #7).
//   Overflow drain           - emit any lift-offs deferred by a PREVIOUS
//                            frame's starvation (see AmtEmitLift), ahead
//                            of anything this frame produces.
//   Phase A  Lift (no exceptions, "kill unmatched") - every track whose
//                            slot has no Present sample this frame, OR
//                            whose verdict is NEW_IDENTITY, is lifted
//                            off HERE, before anything else touches it.
//                            A track can only be born/recycled in Phase B
//                            AFTER Phase A has fully completed for that
//                            slot. Also captures SlotLastLift{Qpc,X,Y}
//                            (task #2 — see AmtRecordSlotLift below).
//   Phase B  Birth ("birth new") - tracks with a Present sample and no
//                            live Track occupying their slot are born
//                            here. Uses AmtTrackBirthWithRetapSmoothing
//                            instead of AmtTrackBirth when this slot's
//                            recent-lift memory says this looks like a
//                            fast re-tap (task #2 fix) — see the long
//                            comment in that phase below.
//   Phase C  Update/Report ("emit report") - every ACTIVE track gets
//                            AmtTrackUpdate called exactly once and is
//                            written into the outgoing PTP_REPORT.
//
// This replaces the previous design where lift-off, identity-break
// handling, and fresh-baseline initialisation were interleaved inside a
// single loop body keyed by raw index — which is what made a 1-frame
// ambiguity possible (a slot's old and new identity could both touch the
// same shared arrays within one iteration).
//
// ---------------------------------------------------------------------
// SESSION / TRACK OWNERSHIP (task #7)
// ---------------------------------------------------------------------
// Per task #7's request to make gesture/session ownership explicit
// rather than implicit, the contract enforced by this file is:
//
//   DEVICE_CONTEXT (Session) owns:
//     - GestureSessionActive  (is a >=2-finger gesture happening RIGHT
//       NOW, across the whole pad)
//     - the alive-count computation each frame feeds into
//     - SlotLastLift{Qpc,X,Y}  (per-slot recent-lift memory, which is
//       deliberately session/device-context-scoped rather than
//       track-scoped — see the long comment on it in Device.h: it must
//       outlive the TRACK struct's own zeroing on kill)
//
//   TRACK (per-slot) owns:
//     - ReportX/Y, HystX/Y, smoothing/deadzone state (coordinates)
//     - ContactID (identity)
//     - WasInGesture  (a PER-TRACK derived fact — "was I touched by a
//       session-level gesture during my current ACTIVE lifetime" — that
//       is SET FROM a Session-level GestureSessionActive transition by
//       this file, never the reverse; Track.c never reads or writes
//       GestureSessionActive, and Interrupt.c never lets a per-track
//       fact flow back up into the session flag)
//
// The data flow is strictly one-directional: Session state -> (read by
// Interrupt.c) -> per-track WasInGesture. Nothing in Track.c depends on
// DEVICE_CONTEXT, and nothing in this file lets a track's state leak
// back into GestureSessionActive.
//
// ---------------------------------------------------------------------
// FIX (GestureSessionActive was write-only, not an FSM):
// The previous revision of this file only ever set
// pCtx->GestureSessionActive = TRUE on a >=2-finger frame and NEVER
// cleared it back to FALSE anywhere — not even when every finger lifted
// and the pad went completely idle. The field was also never read by
// anything. That is not "session FSM", that's dead, monotonically-stuck
// state, and directly contradicts the field's own doc comment in
// Device.h ("is a multi-finger gesture in progress on the pad RIGHT
// NOW"). Fixed below: the session ends (FALSE) the moment the pad has
// zero alive contacts, and begins (TRUE) on any >=2-finger frame. A
// single-finger frame in between leaves it unchanged, by design — that
// continuity is exactly what TRACK.WasInGesture (per-track, set FROM a
// TRUE transition here) depends on to decide whether a SPECIFIC finger
// still needs the post-gesture EMA-skip treatment.
// ---------------------------------------------------------------------

#include "Driver.h"
#include "Track.h"
#include "Match.h"
#include "Interrupt.tmh"

// ---------------------------------------------------------------------
// FIX (rate limiting hot-path TraceEvents):
// TraceEvents/WPP still pays argument-marshalling and ETW-session-check
// cost even when nothing is listening, and this driver's interrupt
// completion runs at native USB polling rate. TRACE_HOT_PATH_MIN_INTERVAL
// gates the handful of TRACE_LEVEL_VERBOSE/INFORMATION calls in the
// per-frame path (identity-break / gesture-taint / button diagnostics)
// to at most once per interval, using QPC ticks. TRACE_LEVEL_ERROR/
// WARNING paths are NEVER rate limited — those are by definition rare
// (malformed packet, buffer mismatch, overflow-queue saturation) and
// must not be silently dropped.
// ---------------------------------------------------------------------
#define TRACE_HOT_PATH_MIN_INTERVAL_100NS  (50LL * 10000LL)  // 50 ms

static inline BOOLEAN
AmtHotPathTraceGate(_Inout_ PDEVICE_CONTEXT pCtx, _In_ LONGLONG NowQpc100ns)
{
    if (NowQpc100ns - pCtx->LastHotPathTraceQpc < TRACE_HOT_PATH_MIN_INTERVAL_100NS)
        return FALSE;
    pCtx->LastHotPathTraceQpc = NowQpc100ns;
    return TRUE;
}

// ---------------------------------------------------------------------
// FIX (missing report-level invariant): AmtTrackCheckInvariants (in
// Track.c) asserts ContactID uniqueness/non-zero across the TRACK pool,
// but nothing previously verified that the *outgoing PTP_REPORT* itself
// never carries a duplicate ContactID (e.g. via an overflow-drain entry
// colliding with a same-frame fresh contact) or a malformed TipSwitch
// bit. This is a separate invariant from the pool check — the pool can
// be perfectly consistent while a report-assembly bug still duplicates
// an entry — so it gets its own debug-only assertion pass, run right
// before the report leaves this function on every path.
// ---------------------------------------------------------------------
#if DBG
static VOID
AmtReportCheckInvariants(_In_ const PTP_REPORT* Report)
{
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

// AmtRecordSlotLift - captures this slot's lift position/time into the
// session-scoped recent-lift memory (task #2 fix). Called from every
// Phase A transition that lifts a track (both the untainted AmtTrackKill
// path and the gesture-tainted AmtTrackEnterGrace/ExpireGrace path —
// task #2's repro scenario goes through the gesture-tainted path, but a
// fast re-tap after a plain single-finger tap deserves the same
// smoothing treatment, so this is NOT conditioned on WasInGesture).
// Position-only — see TRACK_RETAP_POLICY in Track.h and the
// SlotLastLift* field comment in Device.h: this never feeds ContactID
// assignment.
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

// AmtDrainOverflow — writes any lift-offs queued by a PREVIOUS frame's
// AmtEmitLift starvation fallback into the FRONT of *ContactCount, ahead
// of anything this frame is about to add. Called once, right after the
// overflow queue is read but before Phase A runs, so a deferred lift-off
// is delivered with at most one extra frame of latency. Clears
// pCtx->OverflowCount unconditionally — once drained (or attempted), the
// queue does not persist a second frame; see the saturation-log branch
// in AmtEmitLift for the only way an entry could still be lost.
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

// AmtEmitLift — writes one lift-off PTP_CONTACT into Report at
// *ContactCount, using values captured BEFORE the track transition (see
// the frame-determinism comment in Track.h: never read Track fields
// AFTER AmtTrackKill/AmtTrackRecycle/AmtTrackEnterGrace expecting old
// values — these parameters are the only supported channel for them).
//
// FIX (starvation case): if the report is already full
// (*ContactCount == PTP_MAX_CONTACT_POINTS), the lift-off is NOT
// silently dropped. The underlying track transition has ALREADY
// happened by the time this is called (a track must always be freed
// when its finger truly lifts, regardless of report capacity) — only
// the notification of that lift-off to Windows is deferred, into
// pCtx->Overflow*, to be drained at the front of the NEXT frame's
// report by AmtDrainOverflow. This guarantees every lift-off is
// eventually reported exactly once, with bounded (one extra frame)
// latency, rather than Windows being left believing a contact is still
// down because its TipSwitch=0 report never arrived.
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

    // PTP_MAX_CONTACT_POINTS is the hard upper bound on simultaneous
    // live tracks, so the overflow list can never need more capacity
    // than that either — but the bound is enforced defensively rather
    // than assumed, since this fallback exists precisely for the case
    // where that invariant is violated by a future change.
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
    // FIX (ScanTime truncation on non-monotonic QPC): only the upper
    // bound was clamped here previously. QPC is documented as monotonic
    // but is not architecturally immune to a same-or-earlier reading on
    // some firmware/virtualization combinations; if PerfDelta were ever
    // negative, `(USHORT)PerfDelta` below truncates a negative LONGLONG,
    // which produces a huge (wrapped) ScanTime value rather than a small
    // one — corrupting whatever velocity/timing computation Windows'
    // PTP stack derives from ScanTime. Clamping below 0 as well as above
    // 0xFFFF makes this defensive regardless of platform QPC behaviour.
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    if (PerfDelta < 0)      PerfDelta = 0;
    Report.ScanTime = (USHORT)PerfDelta;
    pCtx->LastReportTime = Now;

    // ---------------------------------------------------------------
    // L0 — Button snapshot.
    // FIX (race in button state): the raw button byte is read into a
    // local exactly once, here, at the very start of frame processing.
    // Every later use of button state in this function — including the
    // typing-suppression early-return path below — reads ONLY this
    // local, never TouchBuffer[...] again, eliminating any possibility
    // of the button line being sampled at two different points within
    // the same frame and producing an inconsistent IsButtonClicked
    // verdict.
    // ---------------------------------------------------------------
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

            // Every track just got killed (or already was dead) — the
            // pad is unambiguously empty for the duration of typing
            // suppression. Close out the gesture session here too,
            // rather than leaving it stuck TRUE until the next normal
            // frame happens to observe aliveCount==0 on its own.
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

    // Drain any lift-offs deferred by a PREVIOUS frame's starvation
    // fallback (AmtEmitLift) before anything new is added this frame —
    // bounded one-extra-frame latency for an event that would otherwise
    // be silently lost. A no-op on the (overwhelmingly common) frame
    // where OverflowCount is 0.
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

        // -----------------------------------------------------------
        // L1 — Parse. Pure decode, no Track mutation. Fully delegated
        // to Match.c per the L1/matching/gesture separation in Match.h.
        // ("build candidates" — task #6.)
        // -----------------------------------------------------------
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
                // Pad fully cleared after a palm event: nothing further
                // to do here — Phase A below naturally lifts every
                // ACTIVE track since no slot has a Present sample.
            } else {
                // Still palm-adjacent: suppress all samples this frame.
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    samples[i].Present = FALSE;
            }
        }

        // -----------------------------------------------------------
        // L1.5 — Match. Decide CONTINUES vs NEW_IDENTITY ("match
        // active" — task #6). No mutation.
        // -----------------------------------------------------------
        MATCH_VERDICT verdicts[PTP_MAX_CONTACT_POINTS];
        AmtMatchFrame(samples, raw_n, pCtx->Tracks, verdicts);

        // -----------------------------------------------------------
        // Session gesture FSM — no per-track mutation. Counts only
        // slots that will actually reach Phase C as a live, non-palm
        // contact this frame. Session-owned state only (task #7) — see
        // SESSION/TRACK OWNERSHIP above.
        //
        // FIX (GestureSessionActive write-only bug): this is now a real
        // two-edge FSM instead of a latch that only ever turned on:
        //   aliveCount == 0   -> session ends   (FALSE)
        //   aliveCount >= 2   -> session begins  (TRUE)
        //   aliveCount == 1   -> unchanged (mid-session single-finger
        //                        continuation is exactly what
        //                        TRACK.WasInGesture's "first solo frame
        //                        after a gesture" handling depends on;
        //                        ending the SESSION on a one-frame dip
        //                        to a single finger would be wrong —
        //                        only a fully empty pad legitimately
        //                        ends a gesture session).
        // No "parallel idle/missed" tracking exists alongside this —
        // aliveCount==0 is the single, unambiguous idle signal.
        //
        // NOTE: this aliveCount is computed from `samples[]`, the SAME
        // L1 output Phase A below iterates to decide every lift — so it
        // is, by construction, never stale relative to what Phase A is
        // about to do this frame. The per-track WasInGesture consume-on-
        // spend fix (see AmtTrackCommitSample in Track.c) relies on
        // exactly this: aliveCountIsOne, computed from this same
        // aliveCount further down for Phase C, reflects this frame's
        // post-Phase-A reality, not a value cached from a previous
        // frame.
        // -----------------------------------------------------------
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

        // -----------------------------------------------------------
        // Phase A — Lift, no exceptions ("kill unmatched" — task #6).
        //
        // FIX (Phase A lift-first guarantee): every track that should
        // no longer be ACTIVE this frame is transitioned out HERE,
        // unconditionally, before Phase B looks at any slot. This
        // includes:
        //   (a) tracks whose raw slot has no Present sample this frame
        //       (raw_n shrank, or hardware simply stopped reporting it,
        //       or the slot is PalmLocal this frame, or the pad was
        //       just fully cleared after a palm event),
        //   (b) tracks whose match verdict is NEW_IDENTITY (firmware
        //       reindexed a different physical finger into this slot —
        //       the OLD track must be lifted, not silently overwritten).
        // There is no code path that births or updates a track before
        // its slot has been through this loop. A track that was part of
        // a multi-finger gesture during its lifetime (Tracks[i].
        // WasInGesture) is routed through TRACK_GRACE — entered then
        // immediately expired back to DEAD, since current policy treats
        // GRACE as a non-matchable, same-frame quarantine marker rather
        // than a held reservation (see TRACK_RETAP_POLICY in Track.h) —
        // so the slot is available for Phase B in THIS SAME frame if a
        // new identity needs it. A non-tainted track goes straight to
        // DEAD via AmtTrackKill. Either way, AmtRecordSlotLift captures
        // the lift position/time for Phase B's retap-smoothing decision
        // (task #2) — unconditionally, not just on the gesture-tainted
        // path, since an ordinary fast re-tap deserves the same
        // smoothing treatment.
        // -----------------------------------------------------------
        UNREFERENCED_PARAMETER(palmFullyCleared); // documented above; no special-case needed, Phase A handles it uniformly
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (pCtx->Tracks[i].State != TRACK_ACTIVE)
                continue;

            BOOLEAN slotHasFinger =
                (i < raw_n) && samples[i].Present && !samples[i].PalmLocal;
            BOOLEAN newIdentity =
                (i < raw_n) && slotHasFinger && (verdicts[i] == MATCH_NEW_IDENTITY);

            if (slotHasFinger && !newIdentity)
                continue; // this track continues into Phase C unchanged

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

        // -----------------------------------------------------------
        // Phase B — Birth ("birth new" — task #6).
        // Every raw slot with a Present, non-palm sample and no ACTIVE
        // track occupying it (guaranteed by Phase A above — the slot is
        // either still DEAD from before, or was JUST freed this frame)
        // gets a fresh track born here.
        //
        // FIX (task #2 — raw-snap-on-fast-retap): if this slot's
        // recent-lift memory (set by Phase A above, this frame or a
        // previous one) says the new touch-down at (samples[i].X,
        // samples[i].Y) is close in time AND space to where a track
        // JUST lifted from this exact slot, use
        // AmtTrackBirthWithRetapSmoothing instead of plain AmtTrackBirth
        // — see the long comment on that function in Track.h/Track.c.
        // This still assigns a brand-new ContactID (TRACK_RETAP_POLICY
        // in Track.h is unconditional and untouched by this check); the
        // only difference is whether the first reported sample is
        // smoothed against the recent lift position or reported raw.
        //
        // FIX (bug A — same-frame NEW_IDENTITY must never be smoothed):
        // when the lift that populated this slot's recent-lift memory
        // happened in THIS SAME frame via a MATCH_NEW_IDENTITY verdict
        // (Phase A above), firmware has EXPLICITLY told us this is a
        // different physical finger from whatever just occupied the
        // slot — that is what MATCH_NEW_IDENTITY means (see Match.h:
        // either the origin bit, or the spatial-teleport sanity check).
        // Spatial proximity alone is not a reliable retap signal in that
        // case — two different fingers landing/lifting in roughly the
        // same spot within one frame (e.g. a fast two-handed handoff)
        // would otherwise get incorrectly smoothed toward the OTHER
        // finger's lift position. sameFrameNewIdentity tracks exactly
        // this per slot from Phase A and disables retap smoothing for
        // it, regardless of what AmtTrackIsRecentLiftNearby would
        // otherwise say. An ordinary lift-then-later-retap (the common,
        // intended case) is unaffected — sameFrameNewIdentity is FALSE
        // whenever the lift happened on an earlier frame.
        // -----------------------------------------------------------
        for (i = 0; i < raw_n; i++) {
            if (!samples[i].Present || samples[i].PalmLocal)
                continue;
            if (pCtx->Tracks[i].State == TRACK_ACTIVE)
                continue; // continuing track, handled in Phase C

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
                // A track born directly into a multi-finger frame is
                // itself part of that gesture from its first sample.
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

        // -----------------------------------------------------------
        // Phase C — Update / Report ("emit report" — task #6).
        // Every ACTIVE track (whether continuing from a prior frame or
        // just born in Phase B) is updated exactly once and emitted.
        // -----------------------------------------------------------
        for (i = 0; i < raw_n; i++) {
            if (pCtx->Tracks[i].State != TRACK_ACTIVE)
                continue;
            if (!samples[i].Present || samples[i].PalmLocal)
                continue; // defensive: should be unreachable post Phase A/B

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
                // FIX (dead/no-op ternary): the previous expression
                // `(samples[i].TipDropApplied > 0) ? 1 : 1` evaluated to
                // 1 on BOTH branches — the TipDropApplied check had no
                // effect whatsoever, silently. Per the PTP digitizer
                // usage (Confidence: "this is a real, intended contact"
                // vs noise), a tip-drop-substituted sample is exactly
                // the case Confidence exists to flag: its position was
                // NOT measured this frame, it was carried over from the
                // track's last good position because the raw contact
                // was too small/borderline to trust (see
                // AmtMatchParseFrame's tip-drop debounce in Match.c).
                // Report it with reduced confidence (0) instead of a
                // value indistinguishable from a fully-measured sample.
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