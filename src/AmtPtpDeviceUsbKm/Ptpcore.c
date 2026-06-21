// PTPCore.c - PTPCore_ProcessFrame: the single frame-orchestration
// entry point. Owns Phase A (lift) -> Phase B (birth) -> Phase C
// (update/report) sequencing, palm session bookkeeping, and gesture
// session bookkeeping. This is everything that used to live inline in
// Interrupt.c's USB completion routine - see audit/04_correction_plan.md
// for why it moved here.
//
// Frame-determinism rule (unchanged from the old Track.h): after
// AmtMatchCorrespond resolves correspondences, no ACTIVE_CONTACT may be
// mutated except through the ordered phase sequence below. Pre-
// transition state must be read BEFORE AmtContactKill/EnterGrace.

#include "Driver.h"
#include "PTPCore.h"
#include "PTPCore.tmh"
#include "ActiveContact.h"
#include "Match.h"
#include "Gesture.h"

// ---------------------------------------------------------------------
// Recent-lift ring buffer (slot-independent retap memory)
// ---------------------------------------------------------------------

VOID
AmtRecentLiftRecord(
    _Inout_ RECENT_LIFT_RING* Ring,
    _In_    LONGLONG          NowQpc,
    _In_    USHORT            X,
    _In_    USHORT            Y
)
{
    UCHAR idx = Ring->NextWriteIndex;
    Ring->Entries[idx].Valid   = TRUE;
    Ring->Entries[idx].LiftQpc = NowQpc;
    Ring->Entries[idx].X       = X;
    Ring->Entries[idx].Y       = Y;
    Ring->NextWriteIndex = (UCHAR)((idx + 1) % RECENT_LIFT_CAPACITY);
}

BOOLEAN
AmtRecentLiftFindNearby(
    _In_  const RECENT_LIFT_RING* Ring,
    _In_  LONGLONG                NowQpc,
    _In_  LONGLONG                PerfFrequencyHz,
    _In_  USHORT                  CandX,
    _In_  USHORT                  CandY,
    _Out_ USHORT*                 OutX,
    _Out_ USHORT*                 OutY
)
{
    if (PerfFrequencyHz <= 0)
        return FALSE;

    LONGLONG windowTicks = (RETAP_WINDOW_100NS * PerfFrequencyHz) / 10000000LL;

    LONG     bestDistSq = -1;
    BOOLEAN  found       = FALSE;
    USHORT   bestX = 0, bestY = 0;

    for (UCHAR i = 0; i < RECENT_LIFT_CAPACITY; i++) {
        const RECENT_LIFT* e = &Ring->Entries[i];
        if (!e->Valid)
            continue;
        if (NowQpc < e->LiftQpc)
            continue; // defensive: QPC must be monotonic
        if (NowQpc - e->LiftQpc > windowTicks)
            continue;

        INT dx = (INT)CandX - (INT)e->X;
        INT dy = (INT)CandY - (INT)e->Y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if (dx > RETAP_MAX_DISTANCE || dy > RETAP_MAX_DISTANCE)
            continue;

        LONG distSq = (LONG)dx * dx + (LONG)dy * dy;
        if (!found || distSq < bestDistSq) {
            bestDistSq = distSq;
            bestX      = e->X;
            bestY      = e->Y;
            found      = TRUE;
        }
    }

    if (found) {
        *OutX = bestX;
        *OutY = bestY;
    }
    return found;
}

// Writes one lift-off into OutResult, or defers to the device's
// overflow queue if the frame is already full. Mirrors the old
// AmtEmitLift/AmtDrainOverflow discipline (Interrupt.c history) - never
// silently drops a lift-off, and never writes past
// OutResult->Contacts[PTP_MAX_CONTACT_POINTS-1].
static VOID
AmtCoreEmitLift(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_CORE_FRAME* OutResult,
    _In_    ULONG           ContactID,
    _In_    USHORT          X,
    _In_    USHORT          Y
)
{
    if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
        PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
        outC->ContactID = ContactID;
        outC->X         = X;
        outC->Y         = Y;
        outC->Phase     = CONTACT_PHASE_UP;
        outC->Confident = TRUE;
        OutResult->ContactCount++;
        return;
    }

    if (pCtx->OverflowCount < PTP_MAX_CONTACT_POINTS) {
        pCtx->OverflowContactID[pCtx->OverflowCount] = ContactID;
        pCtx->OverflowX[pCtx->OverflowCount]         = X;
        pCtx->OverflowY[pCtx->OverflowCount]         = Y;
        pCtx->OverflowCount++;
    } else {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT,
            "%!FUNC! lift-off overflow queue saturated - ContactID=%u lost",
            ContactID);
    }
}

// Drains deferred lift-offs from the previous frame into this frame's
// result. Called first, before any of this frame's own lift-offs, so
// overflow entries get at most one extra frame of latency.
static VOID
AmtCoreDrainOverflow(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_CORE_FRAME* OutResult
)
{
    for (UCHAR k = 0; k < pCtx->OverflowCount && OutResult->ContactCount < PTP_MAX_CONTACT_POINTS; k++) {
        PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
        outC->ContactID = pCtx->OverflowContactID[k];
        outC->X         = pCtx->OverflowX[k];
        outC->Y         = pCtx->OverflowY[k];
        outC->Phase     = CONTACT_PHASE_UP;
        outC->Confident = TRUE;
        OutResult->ContactCount++;
    }
    pCtx->OverflowCount = 0;
}

// ---------------------------------------------------------------------
// PTPCore_ProcessFrame
// ---------------------------------------------------------------------

VOID
PTPCore_ProcessFrame(
    _Inout_ PDEVICE_CONTEXT  DeviceContext,
    _In_    const RAW_FRAME* RawFrame,
    _In_    LONGLONG         NowQpc,
    _Out_   PTP_CORE_FRAME*  OutResult
)
{
    PDEVICE_CONTEXT pCtx = DeviceContext;

    RtlZeroMemory(OutResult, sizeof(PTP_CORE_FRAME));
    OutResult->TimestampQpc = NowQpc;

    // ---- ContactMatcher: build candidates (palm + tip-debounce) ----
    MATCH_CANDIDATE_SET candidates;
    BOOLEAN              largePalm = FALSE;

    AmtMatchBuildCandidates(RawFrame, pCtx->DeviceInfo, pCtx->ActiveContacts,
                            &candidates, &largePalm);

    // ---- Palm session orchestration (sticky "still palm-adjacent") ----
    // Session-state symmetry with GestureSession: decides whether to
    // keep suppressing candidates across frames after a large-palm
    // event, until the pad is fully clear. Per-sample scoring lives in
    // Palm.c (called from Match.c); this is the only orchestration
    // layer that sees session history.
    if (largePalm) {
        pCtx->PalmDetected = TRUE;
        OutResult->LargePalmBlanked = TRUE;
    } else if (pCtx->PalmDetected) {
        BOOLEAN anyContact = (candidates.Count > 0);
        if (!anyContact) {
            pCtx->PalmDetected = FALSE;
            // Pad cleared after palm - Phase A below lifts all contacts.
        } else {
            // Still palm-adjacent - suppress all candidates this frame.
            candidates.Count = 0;
        }
    }

    // ---- ContactMatcher: cost-based correspondence ----
    MATCH_RESULT matchResult;
    AmtMatchCorrespond(&candidates, pCtx->ActiveContacts,
                       NowQpc, pCtx->PerfFrequency.QuadPart,
                       &matchResult);

    // ---- GestureEngine: session FSM, computed from this frame's
    // matched-or-new alive contacts (never stale - see ActiveContact.c
    // history note on why staleness here previously caused a real bug) ----
    UCHAR aliveCount = 0;
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal)
            aliveCount++;
    }

    AmtGestureSessionUpdate(&pCtx->GestureSession, aliveCount);
    BOOLEAN gestureThisFrame = AmtGestureIsMultiFingerFrame(aliveCount);

    // Drain deferred lift-offs from the previous frame first - see
    // AmtCoreDrainOverflow doc comment.
    AmtCoreDrainOverflow(pCtx, OutResult);

    // ---- Phase A (lift): every pool entry with no surviving
    // correspondence this frame lifts. Gesture-tainted routes through
    // GRACE; others via Kill - UNLESS the contact hasn't yet reached
    // MIN_CONTACT_LIFETIME_FRAMES, in which case the kill is deferred
    // by one frame (Task 4.2, soft-tap-loss audit). ----
    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];

        ULONG  oldId; USHORT oldX, oldY;

        if (pCtx->ActiveContacts[p].WasInGesture) {
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
        } else if (pCtx->ActiveContacts[p].FramesAlive < MIN_CONTACT_LIFETIME_FRAMES) {
            // FIX (Task 4.2): too-fresh solo contact would otherwise be
            // killed before Windows' PTP gesture recognizer ever sees
            // the DOWN that preceded it. Defer the kill: re-report it
            // as MOVE this frame at its last known position (consuming
            // one more frame of life) instead of lifting. If it's still
            // unmatched on a later frame once FramesAlive has reached
            // the floor, it lifts normally then.
            pCtx->ActiveContacts[p].FramesAlive++;

            if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
                PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
                outC->ContactID   = pCtx->ActiveContacts[p].ContactID;
                outC->X           = pCtx->ActiveContacts[p].ReportX;
                outC->Y           = pCtx->ActiveContacts[p].ReportY;
                outC->Phase       = CONTACT_PHASE_MOVE;
                outC->Confident   = TRUE;
                outC->PalmSuspect = FALSE;
                OutResult->ContactCount++;
                pCtx->ActiveContacts[p].ReportedLastFrame = TRUE;
            }
            continue; // no lift-off this frame - skip the emit below
        } else {
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
        }

        AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
        AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);
    }

    // A correspondence that was found but flagged NewIdentity (firmware
    // origin==0) is ALSO a lift-of-old, immediately followed by a
    // birth-of-new at Phase B below - the old pool entry must be killed
    // here, not just left ACTIVE, or Phase C would wrongly continue it.
    // (Deliberately NOT subject to MIN_CONTACT_LIFETIME_FRAMES: an
    // identity break is a hard firmware signal, not an absence, and
    // must always resolve immediately.)
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (candidates.Candidates[ci].PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;
        if (!matchResult.NewIdentity[ci]) continue;

        ULONG  oldId; USHORT oldX, oldY;
        if (pCtx->ActiveContacts[p].WasInGesture) {
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
        } else {
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
        }

        AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
        AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);

        // This candidate now has no live correspondence - mark it so
        // Phase B treats it as a fresh birth, not Phase C continuation.
        matchResult.CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // ---- Phase B (birth): every candidate with no correspondence
    // births a new pool entry. Uses retap smoothing when recent-lift
    // memory indicates a fast re-tap nearby. ----
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;
        if (matchResult.CorrespondingPoolIndex[ci] != MATCH_NO_CORRESPONDENCE)
            continue; // handled in Phase C

        size_t freeIdx = AmtContactPoolFindFree(pCtx->ActiveContacts);
        if (freeIdx == MAX_CONTACTS) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT,
                "%!FUNC! contact pool exhausted - candidate dropped");
            continue;
        }

        USHORT liftX, liftY;
        BOOLEAN looksLikeRetap =
            AmtRecentLiftFindNearby(&pCtx->RecentLifts, NowQpc,
                                    pCtx->PerfFrequency.QuadPart,
                                    cand->X, cand->Y, &liftX, &liftY);

        if (looksLikeRetap) {
            AmtContactBirthWithRetapSmoothing(
                pCtx->ActiveContacts, freeIdx, &pCtx->NextContactId,
                liftX, liftY, cand->SlotIndex);
        } else {
            AmtContactBirth(
                pCtx->ActiveContacts, freeIdx, &pCtx->NextContactId,
                cand->X, cand->Y, cand->SlotIndex);
        }

        if (gestureThisFrame) {
            pCtx->ActiveContacts[freeIdx].WasInGesture = TRUE;
        }

        // This candidate is now an ACTIVE contact - route it through
        // Phase C immediately below by recording the correspondence.
        matchResult.CorrespondingPoolIndex[ci] = freeIdx;
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // ---- Phase C (update / report): every candidate now has a
    // correspondence (either pre-existing from this frame's matcher, or
    // just born in Phase B). Update once, report once. ----
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue; // shouldn't happen post-Phase-B

        BOOLEAN justBorn = (pCtx->ActiveContacts[p].LastSeenQpc == 0);

        if (gestureThisFrame) {
            pCtx->ActiveContacts[p].WasInGesture = TRUE;
        }

        USHORT repX, repY;
        AmtContactUpdate(&pCtx->ActiveContacts[p], cand->X, cand->Y,
                         cand->SlotIndex, NowQpc,
                         (BOOLEAN)(aliveCount == 1), &repX, &repY);

        // NOTE on asymmetry: lift-offs (Phase A, AmtCoreEmitLift) have
        // an overflow queue and are never silently dropped. Live
        // updates here do not - if the frame is already full (only
        // possible when this frame's lift-off count plus live-update
        // count exceeds PTP_MAX_CONTACT_POINTS, which cannot happen
        // since both lift-offs and live updates draw from the same
        // <=5-contact pool and a contact is in exactly one Phase A/B/C
        // bucket per frame), this candidate's update is skipped this
        // frame. Preserved unchanged from the old Interrupt.c Phase C
        // behavior - documented here rather than silently inherited.
        if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
            PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
            outC->ContactID = pCtx->ActiveContacts[p].ContactID;
            outC->X         = repX;
            outC->Y         = repY;
            outC->Phase     = justBorn ? CONTACT_PHASE_DOWN : CONTACT_PHASE_MOVE;
            outC->Confident = (cand->TipDropApplied == 0);
            outC->PalmSuspect = FALSE; // candidates reaching here already
                                       // passed palm filtering in Match.c
            OutResult->ContactCount++;
            pCtx->ActiveContacts[p].ReportedLastFrame = TRUE;
        }
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // FIX (Task 4.1, instrumentation): per-frame diagnostic summary for
    // soft/double-tap-loss investigation. Verbose level, rate-gated via
    // the same hot-path gate Interrupt.c uses (shared
    // DEVICE_CONTEXT.LastHotPathTraceQpc) so this is safe to leave
    // compiled into release builds - it only ever fires when WPP
    // verbose tracing for TRACE_INPUT is actually enabled, and even
    // then at most once per TRACE_HOT_PATH_MIN_INTERVAL_100NS.
    if (AmtHotPathTraceGate(pCtx, NowQpc)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
            "%!FUNC! raw=%u cand=%u unmatched=%u out=%u palm=%d gesture=%d alive=%u",
            RawFrame->ContactCount,
            candidates.Count,
            matchResult.UnmatchedCount,
            OutResult->ContactCount,
            largePalm,
            gestureThisFrame,
            aliveCount);
    }
}