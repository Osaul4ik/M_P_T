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
    // Sticky suppression across frames after large-palm, until pad clears.
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

    // ---- Phase A (lift): unmatched pool entries lift. Gesture-tainted
    // routes through GRACE; others via Kill - unless contact is too fresh
    // (FramesAlive < MIN_CONTACT_LIFETIME_FRAMES), defer by one frame. ----
    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];

        ULONG  oldId; USHORT oldX, oldY;

        if (pCtx->ActiveContacts[p].WasInGesture) {
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
        } else if (pCtx->ActiveContacts[p].FramesAlive < MIN_CONTACT_LIFETIME_FRAMES) {
            // FIX (Task 4.2): too-fresh solo contact - defer kill by one
            // frame so Windows' gesture recognizer sees the DOWN first.
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
        } else {
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
        }

        AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
        AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);
    }

    // NewIdentity (firmware origin==0) is also a lift-of-old + birth-of-new.
    // Not subject to MIN_CONTACT_LIFETIME_FRAMES - identity break is a
    // hard firmware signal, must resolve immediately.
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

        // Mark for Phase B as fresh birth.
        matchResult.CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // ---- Phase B (birth): unmatched candidates birth new pool entries.
    // Uses retap smoothing when recent-lift memory indicates fast re-tap. ----
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

        // Route through Phase C immediately.
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

        // BUG FIX: Match.c's tip-size debounce reads
        // ActiveContacts[p].TipDropCount to decide when to give up
        // bridging a weakening contact and drop it as noise, but nothing
        // in the codebase ever WROTE to TipDropCount after birth (it was
        // always 0, frozen from AmtContactBirth) - the debounce-exhausted
        // path in AmtMatchBuildCandidates was unreachable dead code, and
        // a sagging-pressure contact would bridge to its last good
        // position (Confidence=0) forever instead of recovering or being
        // dropped after TIP_DROP_DEBOUNCE_FRAMES. Mirror Match.c's
        // per-candidate verdict back into the pool entry every frame so
        // the counter is real again.
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

    // FIX (Task 4.1): per-frame diagnostic summary. Rate-gated via
    // shared DEVICE_CONTEXT.LastHotPathTraceQpc - safe in release builds.
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