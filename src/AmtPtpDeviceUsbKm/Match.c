// Match.c - PTPCore / ContactMatcher. See Match.h for scope notes and
// audit/04_correction_plan.md for why this replaced the slot-indexed
// version.

#include "Driver.h"
#include "Match.h"
#include "Palm.h"

#define TIP_DROP_DEBOUNCE_FRAMES        2
#define TIP_DROP_MAX_REPOSITION_DELTA   300

// Spatial sanity threshold: ~20% of pad width (~20000 units). No real
// finger teleports this far in one USB polling interval.
#define MATCH_MAX_CONTINUATION_DELTA 4000

// Matching-cost tuning. Cost is dominated by spatial distance. The
// slot-hint match is used ONLY to break near-ties (see the epsilon
// comparison in AmtMatchCorrespond) - it must never be able to flip a
// genuinely-closer candidate at a different slot into losing, or this
// silently reintroduces slot-as-identity behavior through the back
// door. An earlier version of this file used a fixed cost subtraction
// for slot-hint matches, which was provably wrong: at typical
// frame-to-frame finger movement (a few units, squared distance in the
// low hundreds), a fixed subtraction of a few thousand could override a
// genuinely closer candidate at a different slot. Fixed by replacing it
// with a same-magnitude-only epsilon tie-break instead - see below.
#define MATCH_TIE_EPSILON_SQ 4  // ~2 units of linear distance, squared

static UCHAR
AmtMatchCandidateTip(_In_ USHORT major, _In_ USHORT minor)
{
    return (UCHAR)(((INT)major << 1) >= 200 || ((INT)minor << 1) >= 150);
}

VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
)
{
    *LargePalmDetected = FALSE;
    RtlZeroMemory(OutCandidates, sizeof(MATCH_CANDIDATE_SET));

    for (UCHAR i = 0; i < RawFrame->ContactCount; i++) {
        const RAW_CONTACT* rc = &RawFrame->Contacts[i];

        PALM_CLASS palm = AmtPalmClassify(rc->Major, rc->Minor, DevInfo,
                                          (INT)rc->X, (INT)rc->Y);

        if (palm == PALM_LARGE) {
            *LargePalmDetected = TRUE;
            OutCandidates->Count = 0; // caller blanks the whole pad
            return;
        }

        MATCH_CANDIDATE cand;
        RtlZeroMemory(&cand, sizeof(cand));
        cand.SlotIndex     = rc->SlotIndex;
        cand.IdentityBreak = (rc->Origin == 0);

        if (palm == PALM_LOCAL) {
            cand.PalmLocal = TRUE;
            cand.X = rc->X;
            cand.Y = rc->Y;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        if (AmtMatchCandidateTip(rc->Major, rc->Minor)) {
            cand.X = rc->X;
            cand.Y = rc->Y;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        // Below tip-size threshold - tip-size debounce. Find the pool
        // entry most recently associated with this hardware slot (by
        // LastSlotHint - a hint, used here only to find a debounce
        // anchor, never as a direct index) and bridge to its last good
        // position if it's plausibly the same physical contact.
        size_t bestPoolIdx = MAX_CONTACTS;
        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;
            if (Pool[p].LastSlotHint != rc->SlotIndex)
                continue;

            INT dxAbs = (INT)rc->X - (INT)Pool[p].ReportX;
            if (dxAbs < 0) dxAbs = -dxAbs;
            INT dyAbs = (INT)rc->Y - (INT)Pool[p].ReportY;
            if (dyAbs < 0) dyAbs = -dyAbs;

            if (dxAbs <= TIP_DROP_MAX_REPOSITION_DELTA &&
                dyAbs <= TIP_DROP_MAX_REPOSITION_DELTA) {
                bestPoolIdx = p;
                break;
            }
        }

        if (bestPoolIdx == MAX_CONTACTS) {
            // FIX (soft-tap-loss audit, Phase 4): no anchor means this
            // is a brand-new contact, not a noisy re-appearance of an
            // existing one. The debounce below exists to bridge a
            // momentary small-size sample WITHIN an already-tracked
            // contact's lifetime - it must never gate the very first
            // sample of a touch-down, or genuinely soft/quick taps are
            // silently dropped before they ever reach the matcher, and
            // the contact never births at all. Let it through as a
            // low-confidence birth candidate instead of discarding it.
            cand.X              = rc->X;
            cand.Y              = rc->Y;
            cand.TipDropApplied = 1; // mark low-confidence, not "no contact"
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        if (Pool[bestPoolIdx].TipDropCount < TIP_DROP_DEBOUNCE_FRAMES) {
            cand.X              = Pool[bestPoolIdx].ReportX;
            cand.Y              = Pool[bestPoolIdx].ReportY;
            cand.TipDropApplied = (UCHAR)(Pool[bestPoolIdx].TipDropCount + 1);
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
        }
        // else: debounce exhausted with an existing anchor - drop this
        // candidate (treat as absent over-debounced noise). This path
        // is unchanged from before the fix - it only applies when an
        // anchor DOES exist, i.e. mid-contact noise, never first touch.
    }
}

VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
)
{
    RtlZeroMemory(OutResult, sizeof(MATCH_RESULT));

    BOOLEAN poolClaimed[MAX_CONTACTS];
    RtlZeroMemory(poolClaimed, sizeof(poolClaimed));

    // Greedy minimum-cost assignment. For N,M <= PTP_MAX_CONTACT_POINTS
    // (5), this is well within the regime where greedy-by-ascending-cost
    // gives the same practical result as an optimal assignment, at a
    // fraction of the complexity - see Match.h doc comment.
    //
    // Build all (candidate, pool) cost pairs, then repeatedly take the
    // globally cheapest unclaimed pair until either side is exhausted.
    typedef struct { UCHAR candIdx; size_t poolIdx; LONG cost; BOOLEAN slotHintMatch; } PAIR;
    PAIR pairs[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    UCHAR pairCount = 0;

    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        const MATCH_CANDIDATE* cand = &Candidates->Candidates[ci];

        OutResult->CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;

        if (cand->PalmLocal)
            continue; // palm candidates never match/birth via this path

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;

            INT dx = (INT)cand->X - (INT)Pool[p].ReportX;
            INT dy = (INT)cand->Y - (INT)Pool[p].ReportY;
            LONG dist = (LONG)dx * dx + (LONG)dy * dy; // squared distance,
                                                        // the ONLY cost term

            pairs[pairCount].candIdx       = ci;
            pairs[pairCount].poolIdx       = p;
            pairs[pairCount].cost          = dist;
            pairs[pairCount].slotHintMatch = (cand->SlotIndex == Pool[p].LastSlotHint);
            pairCount++;
        }
    }

    // Selection sort by ascending cost, claiming greedily. pairCount is
    // bounded by PTP_MAX_CONTACT_POINTS * MAX_CONTACTS == 25, so an
    // O(n^2) selection pass is negligible on this hot path.
    //
    // Slot-hint is used ONLY as a tie-breaker: when two candidate pairs
    // are within MATCH_TIE_EPSILON_SQ of each other in cost, prefer the
    // one whose SlotIndex matches Pool[p].LastSlotHint. This can never
    // override a genuinely cheaper match outside the epsilon band - see
    // the file-header comment on why a fixed cost bonus was wrong.
    BOOLEAN pairUsed[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    RtlZeroMemory(pairUsed, sizeof(pairUsed));

    BOOLEAN candClaimed[PTP_MAX_CONTACT_POINTS];
    RtlZeroMemory(candClaimed, sizeof(candClaimed));

    for (UCHAR pick = 0; pick < pairCount; pick++) {
        LONG    bestCost          = -1;
        UCHAR   bestIdx           = 0;
        BOOLEAN bestSlotHintMatch = FALSE;
        BOOLEAN found             = FALSE;

        for (UCHAR k = 0; k < pairCount; k++) {
            if (pairUsed[k]) continue;
            if (candClaimed[pairs[k].candIdx]) continue;
            if (poolClaimed[pairs[k].poolIdx]) continue;

            if (!found) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
                found             = TRUE;
                continue;
            }

            LONG delta = pairs[k].cost - bestCost;
            BOOLEAN withinEpsilon = (delta > -MATCH_TIE_EPSILON_SQ) &&
                                    (delta < MATCH_TIE_EPSILON_SQ);

            if (pairs[k].cost < bestCost && !withinEpsilon) {
                // Strictly cheaper outside the tie band - always wins,
                // regardless of slot hint.
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
            } else if (withinEpsilon && pairs[k].slotHintMatch && !bestSlotHintMatch) {
                // Near-tie: prefer the slot-hint match.
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = TRUE;
            }
        }

        if (!found)
            break; // every remaining candidate or pool entry is claimed

        pairUsed[bestIdx]                 = TRUE;
        UCHAR  ci = pairs[bestIdx].candIdx;
        size_t p  = pairs[bestIdx].poolIdx;

        // Reject implausible matches outright - this candidate gets no
        // correspondence (-> new birth) rather than a bogus continuation.
        BOOLEAN spatialReject = pairs[bestIdx].cost >
            (LONG)MATCH_MAX_CONTINUATION_DELTA * MATCH_MAX_CONTINUATION_DELTA;

        // FIX (Task 2.2): explicit time-domain rejection, independent of
        // the spatial one. LastSeenQpc==0 means "born but never updated
        // yet" - this frame's own Phase B contacts can't reach here
        // (the matcher runs before Phase B), so a zero here only ever
        // means "no time information available" and must NOT be
        // rejected on time alone.
        BOOLEAN timeReject = FALSE;
        if (Pool[p].LastSeenQpc != 0 && PerfFrequencyHz > 0) {
            LONGLONG deltaTicks = NowQpc - Pool[p].LastSeenQpc;
            LONGLONG maxTicks   = (MATCH_MAX_TIME_DELTA_100NS * PerfFrequencyHz) / 10000000LL;
            timeReject = (NowQpc < Pool[p].LastSeenQpc) || (deltaTicks > maxTicks);
        }

        if (spatialReject || timeReject) {
            candClaimed[ci] = TRUE; // don't reconsider this candidate
            continue;               // leave pool entry p available
        }

        candClaimed[ci] = TRUE;
        poolClaimed[p]  = TRUE;

        OutResult->CorrespondingPoolIndex[ci] = p;
        OutResult->NewIdentity[ci] =
            Candidates->Candidates[ci].IdentityBreak ? TRUE : FALSE;
    }

    // Any pool entry never claimed has no corresponding candidate this
    // frame -> lift.
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State != CONTACT_ACTIVE)
            continue;
        if (!poolClaimed[p]) {
            OutResult->UnmatchedPoolIndices[OutResult->UnmatchedCount++] = p;
        }
    }
}