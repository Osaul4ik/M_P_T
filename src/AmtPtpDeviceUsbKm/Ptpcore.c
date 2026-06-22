// PTPCore.c - PTPCore_ProcessFrame: single frame-orchestration entry point.
// Owns Phase A (lift) -> Phase B (birth) -> Phase C (update/report)
// sequencing, palm session, and gesture session bookkeeping. Everything
// that used to live inline in Interrupt.c's USB completion routine.
//
// Frame-determinism rule: after AmtMatchCorrespond resolves
// correspondences, no ACTIVE_CONTACT may be mutated except through the
// ordered phase sequence below.

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
            continue; // QPC must be monotonic
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

// Writes one lift-off into OutResult, or defers to overflow queue if
// frame is full. Never silently drops a lift-off.
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

// Drains deferred lift-offs from previous frame into this frame's result.
// Called first, before this frame's own lift-offs.
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

    // ---- Palm session orchestration ----
    if (largePalm) {
        pCtx->PalmDetected = TRUE;
        OutResult->LargePalmBlanked = TRUE;
    } else if (pCtx->PalmDetected) {
        BOOLEAN anyContact = (candidates.Count > 0);
        if (!anyContact) {
            pCtx->PalmDetected = FALSE;
        } else {
            candidates.Count = 0; // still palm-adjacent - suppress all
        }
    }

    // ---- ContactMatcher: cost-based correspondence ----
    MATCH_RESULT matchResult;
    AmtMatchCorrespond(&candidates, pCtx->ActiveContacts,
                       NowQpc, pCtx->PerfFrequency.QuadPart,
                       &matchResult);

    // ---- GestureEngine: session FSM ----
    UCHAR aliveCount = 0;
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal)
            aliveCount++;
    }

    AmtGestureSessionUpdate(&pCtx->GestureSession, aliveCount);
    BOOLEAN gestureThisFrame = AmtGestureIsMultiFingerFrame(aliveCount);

    // Drain deferred lift-offs from previous frame first.
    AmtCoreDrainOverflow(pCtx, OutResult);

    // ---- Phase A (lift): unmatched pool entries lift. ----
    //
    // FIX (Issue #2): MIN_CONTACT_LIFETIME_FRAMES deferral removed for
    // solo contacts.
    //
    // OLD behavior: any contact with FramesAlive < 2 would get a fake
    // MOVE injected for one extra frame before Kill. Intent was to give
    // Windows' gesture recognizer time to see DOWN before UP.
    //
    // Problem: for soft taps this produces:
    //   DOWN(frame N) -> fake MOVE(frame N+1) -> UP(frame N+2)
    // The fake MOVE extends perceived contact time and shifts timing.
    // At 120Hz each frame is ~8ms. A real 1-frame tap is 8ms; with the
    // deferral Windows sees 16ms - still within tap window, but the
    // artificial MOVE can confuse gesture recognizers that look for
    // clean DOWN->UP sequences.
    //
    // NEW behavior: deferral is kept ONLY for gesture-tainted contacts
    // (WasInGesture=TRUE) when this is the last finger lifting (aliveCount
    // after this Phase A pass will be 0). For gesture cleanup, the
    // recognizer genuinely needs DOWN before UP. For solo taps, immediate
    // kill is correct.
    //
    // To check "is this the last finger", we use aliveCount from the
    // candidate set (contacts still down this frame). An unmatched pool
    // entry that is the ONLY active contact lifting with aliveCount==0
    // is a solo tap scenario.

    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];

        ULONG  oldId; USHORT oldX, oldY;

        if (pCtx->ActiveContacts[p].WasInGesture) {
            // Gesture-tainted: check deferral for last-finger case.
            // FIX (Issue #2): only defer if contact is very fresh AND
            // this was a multi-finger session ending - not for solo taps.
            if (pCtx->ActiveContacts[p].FramesAlive < MIN_CONTACT_LIFETIME_FRAMES
                && aliveCount == 0)
            {
                // Last finger of a gesture session, too fresh - defer one
                // frame so gesture recognizer sees DOWN+UP properly.
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
                continue; // no lift-off this frame
            }

            // FIX (Issue #4): do NOT record gesture lift in RecentLifts.
            // Post-gesture lifts should not trigger retap smoothing for
            // subsequent solo taps. The lift position after a scroll/pinch
            // is where the finger happened to stop - not a meaningful "last
            // tap position" that a new solo tap should smooth against.
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // No AmtRecentLiftRecord here - intentional (Issue #4 fix).
            AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);

        } else {
            // FIX (Issue #2): solo contact - kill immediately, no deferral.
            // Clean DOWN -> UP is what Windows needs for tap recognition.
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);

            // Solo lift: record for retap smoothing (cursor continuity).
            AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
            AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);
        }
    }

    // NewIdentity (firmware origin==0) is also a lift-of-old + birth-of-new.
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (candidates.Candidates[ci].PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;
        if (!matchResult.NewIdentity[ci]) continue;

        ULONG  oldId; USHORT oldX, oldY;
        if (pCtx->ActiveContacts[p].WasInGesture) {
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // FIX (Issue #4): gesture lift not recorded in RecentLifts.
        } else {
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
        }

        AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);

        matchResult.CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // ---- Phase B (birth): unmatched candidates birth new pool entries. ----
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
            // FIX (Issue #3): BirthWithRetapSmoothing now sets
            // PendingFirstSample=TRUE, so first DOWN reports real
            // finger position, not the old lift position. The lift
            // coords (liftX/Y) are stored only as EMA seed baseline
            // (HystX/Y, ReportX/Y) for subsequent MOVE smoothing.
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

        matchResult.CorrespondingPoolIndex[ci] = freeIdx;
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // ---- Phase C (update / report): every candidate now has a
    // correspondence. Update once, report once. ----
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;

        BOOLEAN justBorn = (pCtx->ActiveContacts[p].LastSeenQpc == 0);

        if (gestureThisFrame) {
            pCtx->ActiveContacts[p].WasInGesture = TRUE;
        }

        // Mirror tip-drop verdict back into pool so debounce counter
        // is real (see Match.c comment for full explanation).
        pCtx->ActiveContacts[p].TipDropCount = cand->TipDropApplied;

        USHORT repX, repY;
        AmtContactUpdate(&pCtx->ActiveContacts[p], cand->X, cand->Y,
                         cand->SlotIndex, NowQpc,
                         (BOOLEAN)(aliveCount == 1), &repX, &repY);

        if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
            PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
            outC->ContactID = pCtx->ActiveContacts[p].ContactID;
            outC->X         = repX;
            outC->Y         = repY;
            outC->Phase     = justBorn ? CONTACT_PHASE_DOWN : CONTACT_PHASE_MOVE;
            outC->Confident = (cand->TipDropApplied == 0);
            outC->PalmSuspect = FALSE;
            OutResult->ContactCount++;
            pCtx->ActiveContacts[p].ReportedLastFrame = TRUE;
        }
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

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