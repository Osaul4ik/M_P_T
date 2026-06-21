// Match.c - PTPCore / ContactMatcher. See Match.h for scope notes.

#include "Driver.h"
#include "Match.h"
#include "Palm.h"

#define TIP_DROP_DEBOUNCE_FRAMES        2
#define TIP_DROP_MAX_REPOSITION_DELTA   300

// Spatial sanity: ~20% of pad width (~20000 units). No finger teleports
// this far in one USB polling interval.
#define MATCH_MAX_CONTINUATION_DELTA 4000

// Matching-cost tuning. Cost is dominated by spatial distance. Slot-hint
// is used ONLY to break near-ties (MATCH_TIE_EPSILON_SQ) - never a fixed
// cost subtraction, which was provably wrong (could override genuinely
// closer candidates at different slots).
#define MATCH_TIE_EPSILON_SQ 4  // ~2 units linear distance, squared

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
            OutCandidates->Count = 0; // blank whole pad
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

        // Below tip threshold - debounce. Find pool entry by LastSlotHint
        // (hint only, never direct index) and bridge to last good position.
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
            // FIX (soft-tap-loss): no anchor means brand-new contact, not
            // noise. Let through as low-confidence birth candidate instead
            // of silently dropping first touch-down.
            cand.X              = rc->X;
            cand.Y              = rc->Y;
            cand.TipDropApplied = 1;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        if (Pool[bestPoolIdx].TipDropCount < TIP_DROP_DEBOUNCE_FRAMES) {
            cand.X              = Pool[bestPoolIdx].ReportX;
            cand.Y              = Pool[bestPoolIdx].ReportY;
            cand.TipDropApplied = (UCHAR)(Pool[bestPoolIdx].TipDropCount + 1);
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
        }
        // else: debounce exhausted with anchor - drop candidate (noise).
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

    // Greedy minimum-cost assignment. For N,M <= 5, greedy-by-ascending-
    // cost gives same practical result as optimal assignment.
    typedef struct { UCHAR candIdx; size_t poolIdx; LONG cost; BOOLEAN slotHintMatch; } PAIR;
    PAIR pairs[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    UCHAR pairCount = 0;

    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        const MATCH_CANDIDATE* cand = &Candidates->Candidates[ci];

        OutResult->CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;

        if (cand->PalmLocal)
            continue;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;

            INT dx = (INT)cand->X - (INT)Pool[p].ReportX;
            INT dy = (INT)cand->Y - (INT)Pool[p].ReportY;
            LONG dist = (LONG)dx * dx + (LONG)dy * dy; // squared distance

            pairs[pairCount].candIdx       = ci;
            pairs[pairCount].poolIdx       = p;
            pairs[pairCount].cost          = dist;
            pairs[pairCount].slotHintMatch = (cand->SlotIndex == Pool[p].LastSlotHint);
            pairCount++;
        }
    }

    // Selection sort by ascending cost, greedy claiming. pairCount <= 25,
    // so O(n^2) is negligible. Slot-hint is tie-breaker only within
    // MATCH_TIE_EPSILON_SQ - never overrides genuinely cheaper match.
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
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
            } else if (withinEpsilon && pairs[k].slotHintMatch && !bestSlotHintMatch) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = TRUE;
            }
        }

        if (!found)
            break;

        pairUsed[bestIdx]                 = TRUE;
        UCHAR  ci = pairs[bestIdx].candIdx;
        size_t p  = pairs[bestIdx].poolIdx;

        // Reject implausible matches - candidate gets no correspondence.
        BOOLEAN spatialReject = pairs[bestIdx].cost >
            (LONG)MATCH_MAX_CONTINUATION_DELTA * MATCH_MAX_CONTINUATION_DELTA;

        // FIX (Task 2.2): time-domain rejection. LastSeenQpc==0 means
        // "never updated" - must not be rejected on time alone.
        BOOLEAN timeReject = FALSE;
        if (Pool[p].LastSeenQpc != 0 && PerfFrequencyHz > 0) {
            LONGLONG deltaTicks = NowQpc - Pool[p].LastSeenQpc;
            LONGLONG maxTicks   = (MATCH_MAX_TIME_DELTA_100NS * PerfFrequencyHz) / 10000000LL;
            timeReject = (NowQpc < Pool[p].LastSeenQpc) || (deltaTicks > maxTicks);
        }

        if (spatialReject || timeReject) {
            candClaimed[ci] = TRUE;
            continue;
        }

        candClaimed[ci] = TRUE;
        poolClaimed[p]  = TRUE;

        OutResult->CorrespondingPoolIndex[ci] = p;
        OutResult->NewIdentity[ci] =
            Candidates->Candidates[ci].IdentityBreak ? TRUE : FALSE;
    }

    // Unclaimed pool entries -> lift.
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State != CONTACT_ACTIVE)
            continue;
        if (!poolClaimed[p]) {
            OutResult->UnmatchedPoolIndices[OutResult->UnmatchedCount++] = p;
        }
    }
}