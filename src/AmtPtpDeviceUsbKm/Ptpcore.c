// PTPCore.c - Frame orchestration: Phase A (lift) -> Phase B (birth) -> Phase C (update).

#include "Driver.h"
#include "PTPCore.h"
#include "PTPCore.tmh"
#include "ActiveContact.h"
#include "Match.h"
#include "Gesture.h"

// Recent-lift ring buffer (slot-independent retap memory)

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

// Emit lift-off; overflow queue if frame full.
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

// Drain deferred lift-offs from previous frame.
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

// PTPCore_ProcessFrame

VOID
PTPCore_ProcessFrame(
    _Inout_ PDEVICE_CONTEXT  DeviceContext,
    _In_    const RAW_FRAME* RawFrame,
    _In_    LONGLONG         NowQpc,
    _In_    BOOLEAN          ButtonDown,
    _Out_   PTP_CORE_FRAME*  OutResult
)
{
    PDEVICE_CONTEXT pCtx = DeviceContext;

    // Click-edge detection for the button-rebirth workaround (Phase A.5
    // below). Only the RISING edge (0->1) matters: that's the frame where
    // Windows' PTP integrated-button anti-jitter logic would otherwise
    // compare the live HID coordinate against the current cursor position
    // and "snap" the click to the cursor if the delta looks like jitter.
    // Forcing a real Kill->Birth of the live contact's ContactID at its
    // own current position routes this click through the ordinary
    // soft-tap TipSwitch path instead, which isn't subject to that snap.
    BOOLEAN buttonClickEdge = ButtonDown && !pCtx->PrevButtonClicked;
    pCtx->PrevButtonClicked = ButtonDown;

    RtlZeroMemory(OutResult, sizeof(PTP_CORE_FRAME));
    OutResult->TimestampQpc = NowQpc;

    // Build candidates (palm + tip-debounce)
    MATCH_CANDIDATE_SET candidates;
    BOOLEAN              largePalm = FALSE;

    AmtMatchBuildCandidates(RawFrame, pCtx->DeviceInfo, pCtx->ActiveContacts,
                            &candidates, &largePalm);

    // Palm session: suppress candidates when palm active.
    // Palm-induced lifts are NOT recorded in RecentLifts.
    BOOLEAN palmSuppressedFrame = FALSE;

    if (largePalm) {
        pCtx->PalmDetected = TRUE;
        OutResult->LargePalmBlanked = TRUE;
        palmSuppressedFrame = TRUE;
    } else if (pCtx->PalmDetected) {
        BOOLEAN anyContact = (candidates.Count > 0);
        if (!anyContact) {
            pCtx->PalmDetected = FALSE;
        } else {
            candidates.Count = 0; // still palm-adjacent - suppress all
            palmSuppressedFrame = TRUE;
        }
    }

    // Cost-based correspondence
    MATCH_RESULT matchResult;
    AmtMatchCorrespond(&candidates, pCtx->ActiveContacts,
                       NowQpc, pCtx->PerfFrequency.QuadPart,
                       &matchResult);

    // Gesture session FSM
    UCHAR aliveCount = 0;
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal)
            aliveCount++;
    }

    AmtGestureSessionUpdate(&pCtx->GestureSession, aliveCount);
    BOOLEAN gestureThisFrame = AmtGestureIsMultiFingerFrame(aliveCount);

    
    // Drain deferred lift-offs
    AmtCoreDrainOverflow(pCtx, OutResult);

    // Phase A (lift): unmatched pool entries lift.
    // Gesture-tainted: defer kill on last finger; solo: kill immediately.

    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];

        ULONG  oldId; USHORT oldX, oldY;

        if (pCtx->ActiveContacts[p].WasInGesture) {
            // Gesture-tainted: defer if fresh, last finger, AND never moved.
            // A contact that actually moved (drag/scroll) must report its
            // true final position on lift-off with no frozen filler frame -
            // otherwise Windows' Precision Touchpad inertia calculation
            // sees a zero-velocity sample right before ContactCount drops
            // to 0 and never starts the fling (no scroll momentum).
            if (pCtx->ActiveContacts[p].FramesAlive < MIN_CONTACT_LIFETIME_FRAMES
                && aliveCount == 0
                && !pCtx->ActiveContacts[p].HadRecentMotion)
            {
                // Defer one frame for gesture recognizer.
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

            // Gesture lift: not recorded in RecentLifts.
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // No AmtRecentLiftRecord here - intentional (Issue #4 fix).
            AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);

        } else {
            // Solo contact: kill immediately.
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);

            // Record in RecentLifts unless palm-suppressed.
            if (!palmSuppressedFrame) {
                AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
            }
            AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);
        }
    }

    // NewIdentity (origin==0): lift old + birth new.
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (candidates.Candidates[ci].PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;
        if (!matchResult.NewIdentity[ci]) continue;

        ULONG  oldId; USHORT oldX, oldY;
        if (pCtx->ActiveContacts[p].WasInGesture) {
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // Gesture lift: not recorded in RecentLifts.
        } else {
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            // Palm-suppressed: candidates.Count==0, loop won't execute.
            AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
        }

        AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);

        matchResult.CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // Phase A.5 (button-click forced rebirth): on the rising edge of the
    // integrated button, force a REAL Kill->Birth of every still-live,
    // pre-existing matched contact, at its own current position. This is
    // the only sanctioned way to mint a new ContactID (ActiveContact.h:
    // "ContactID monotonic... the only permitted NEW_IDENTITY path is
    // Kill->Birth") - there is deliberately no separate in-place identity
    // mutation here. WasInGesture/FramesAlive are carried across the swap
    // via AmtContactBirthForButtonRebirth, since the same physical finger
    // never actually left the pad. Pre-existing == LastSeenQpc != 0: a
    // contact birthed earlier in THIS frame (NewIdentity path above, or a
    // genuinely new touch in Phase B below) has no "old" cursor-latched
    // identity for Windows to be snapping against yet, so it's skipped.
    if (buttonClickEdge) {
        for (UCHAR ci = 0; ci < candidates.Count; ci++) {
            if (candidates.Candidates[ci].PalmLocal) continue;

            size_t p = matchResult.CorrespondingPoolIndex[ci];
            if (p == MATCH_NO_CORRESPONDENCE) continue;
            if (pCtx->ActiveContacts[p].LastSeenQpc == 0) continue; // born this frame

            BOOLEAN wasInGesture    = pCtx->ActiveContacts[p].WasInGesture;
            UCHAR   framesAlive     = pCtx->ActiveContacts[p].FramesAlive;
            BOOLEAN hadRecentMotion = pCtx->ActiveContacts[p].HadRecentMotion;

            ULONG  oldId; USHORT oldX, oldY;
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtCoreEmitLift(pCtx, OutResult, oldId, oldX, oldY);
            // Deliberately NOT AmtRecentLiftRecord'd - this isn't a real
            // lift and must not seed retap-smoothing for unrelated future
            // taps in the same area.

            size_t freeIdx = AmtContactPoolFindFree(pCtx->ActiveContacts);
            if (freeIdx == MAX_CONTACTS) {
                // Pool exhausted re-acquiring our own just-freed slot
                // should not happen (we just killed one), but fail safe:
                // leave this candidate unmatched: Phase B will treat it
                // as a fresh untainted touch instead of dropping it.
                matchResult.CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
                continue;
            }

            AmtContactBirthForButtonRebirth(
                pCtx->ActiveContacts, freeIdx, &pCtx->NextContactId,
                oldX, oldY, candidates.Candidates[ci].SlotIndex,
                wasInGesture, framesAlive, hadRecentMotion);

            matchResult.CorrespondingPoolIndex[ci] = freeIdx;
        }
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // Phase B (birth): unmatched candidates birth new pool entries.
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
            // RetapSeeded: seed survives first AmtContactUpdate.
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

    // Phase C (update / report): update once, report once.
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;

        BOOLEAN justBorn = (pCtx->ActiveContacts[p].LastSeenQpc == 0);

        if (gestureThisFrame) {
            pCtx->ActiveContacts[p].WasInGesture = TRUE;
        }

        // Mirror tip-drop verdict back into pool.
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