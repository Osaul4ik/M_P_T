// Match.c - PTPCore / ContactMatcher. See Match.h for scope notes.

#include "Driver.h"
#include "Match.h"
#include "Palm.h"

// Spatial sanity: ~20% of pad width (~20000 units). No finger teleports
// this far in one USB polling interval. Used both for the real identity
// matcher (AmtMatchCorrespond) AND, as of the fix below, as the SAME
// radius used by the debounce-anchor search in AmtMatchBuildCandidates -
// see TIP_DROP_MAX_REPOSITION_DELTA comment for why these two radii must
// agree.
#define MATCH_MAX_CONTINUATION_DELTA 4000

// FIX (Soft-drift confidence bug): this used to be a much smaller,
// independent constant (300). Problem: AmtMatchBuildCandidates used it
// to decide "did I find a continuing low-pressure contact, or is this a
// brand-new touch?" - and answered "brand-new" (TipDropApplied=0,
// Confidence=1) whenever a single frame's movement exceeded 300 units,
// even though the touch pressure was still genuinely below the tip-size
// threshold. Meanwhile AmtMatchCorrespond (the actual identity matcher,
// called right after) used MATCH_MAX_CONTINUATION_DELTA=4000 and WOULD
// still match that same contact as a continuation. Net effect: a fast
// but light drag/swipe (300 < per-frame delta < 4000) got a spurious
// one-frame Confidence=1 + a spurious reset of TipDropCount, even
// though the finger never left the pad and pressure never crossed the
// tip threshold. Using the SAME radius as the real matcher here removes
// the contradiction: "anchor found" and "AmtMatchCorrespond will treat
// this as the same contact" are now the same condition.
#define TIP_DROP_MAX_REPOSITION_DELTA MATCH_MAX_CONTINUATION_DELTA

// Matching-cost tuning. Cost is dominated by spatial distance. Slot-hint
// is used ONLY to break near-ties (MATCH_TIE_EPSILON_SQ) - never a fixed
// cost subtraction, which was provably wrong (could override genuinely
// closer candidates at different slots).
#define MATCH_TIE_EPSILON_SQ 4  // ~2 units linear distance, squared

// FIX (#1): stationary deadzone for debounce bridge coordinate decision.
// If finger hasn't moved more than this, use anchor (stale) coords.
// If finger IS moving, use real coords even when below tip threshold.
// This prevents Confidence=0 + moving stale position on soft taps.
#define TIP_DROP_STATIONARY_DELTA       3

// FIX (Soft-tap phantom-cycle bug): previously, a candidate that stayed
// below tip threshold for >= a fixed frame count (with a known anchor)
// was DROPPED as "noise". That silently un-matched the pool entry every
// couple of frames, causing PTPCore Phase A to LIFT it even though the
// finger never physically left the pad - then the very next frame, with
// no anchor left in the pool, the same finger was re-born as a BRAND
// NEW contact (new ContactID). Net effect on any sustained light/soft
// touch: an infinite DOWN->MOVE->MOVE->UP->DOWN->... cycle, roughly
// every 3-4 frames, instead of one continuous contact. This also
// corrupted RecentLifts (each phantom UP got recorded), which could
// make a genuine second tap of a soft double-tap retap-smooth against a
// PHANTOM lift from the first tap instead of (or as well as) the real
// one, confusing tap/double-tap timing.
//
// Real lift-off detection does NOT need a frame counter here at all:
// Input.c already drops a finger from RAW_FRAME entirely once
// major<=0 && minor<=0 (true contact loss). So once a candidate has a
// valid anchor, it keeps being reported (Confidence=0) for as long as
// RAW_FRAME keeps reporting *some* contact at that slot - never
// auto-dropped purely because of frame count. TIP_DROP_COUNT_MAX below
// only protects the UCHAR counter from wrapping (which would otherwise
// transiently fake Confidence=1 every 256 frames of sustained soft
// touch); it does not gate whether the candidate is reported.
#define TIP_DROP_COUNT_MAX 255

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
        //
        // FIX (Soft-drift confidence bug, part 2): scan ALL pool entries
        // sharing this slot hint and keep the NEAREST one within
        // TIP_DROP_MAX_REPOSITION_DELTA, instead of stopping at the
        // first one found. This matters now that the radius equals
        // MATCH_MAX_CONTINUATION_DELTA (4000, much wider than the old
        // 300): with a wide radius, picking the first match instead of
        // the closest one would risk bridging coordinates against a
        // stale/unrelated contact that happens to share an old
        // LastSlotHint value, producing a wrong one-frame coordinate.
        size_t bestPoolIdx  = MAX_CONTACTS;
        LONG   bestDistSq   = -1;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;
            if (Pool[p].LastSlotHint != rc->SlotIndex)
                continue;

            INT dxAbs = (INT)rc->X - (INT)Pool[p].ReportX;
            if (dxAbs < 0) dxAbs = -dxAbs;
            INT dyAbs = (INT)rc->Y - (INT)Pool[p].ReportY;
            if (dyAbs < 0) dyAbs = -dyAbs;

            if (dxAbs > TIP_DROP_MAX_REPOSITION_DELTA ||
                dyAbs > TIP_DROP_MAX_REPOSITION_DELTA)
                continue;

            LONG distSq = (LONG)dxAbs * dxAbs + (LONG)dyAbs * dyAbs;
            if (bestPoolIdx == MAX_CONTACTS || distSq < bestDistSq) {
                bestPoolIdx = p;
                bestDistSq  = distSq;
            }
        }

        if (bestPoolIdx == MAX_CONTACTS) {
            // No anchor = brand-new contact below tip threshold.
            // Pass through as full-confidence birth candidate.
            // TipDropApplied=0: real sampled position, Confidence=1.
            // See the long comment in the original code for why this is
            // correct and NOT noise.
            cand.X              = rc->X;
            cand.Y              = rc->Y;
            cand.TipDropApplied = 0;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        // FIX (Soft-tap phantom-cycle bug): always bridge the candidate
        // through while an anchor exists - never drop it purely because
        // TipDropCount crossed a fixed frame threshold (see
        // TIP_DROP_COUNT_MAX comment above for why the old "drop as
        // noise after N frames" behavior caused an infinite phantom
        // DOWN/UP cycle on sustained soft touches).
        //
        // FIX (#1 - Issue #1): coordinate selection for debounce bridge.
        //
        // OLD behavior: always use stale anchor coords (ReportX/Y).
        // Problem: a soft tap that moves slightly gets Confidence=0 AND
        // stale (wrong) coordinates. Windows PTP stack sees a contact
        // that is both unconfident AND not where the finger actually is.
        //
        // NEW behavior: use REAL coords if finger has moved more than
        // TIP_DROP_STATIONARY_DELTA; use anchor only if truly stationary.
        INT dxMove = (INT)rc->X - (INT)Pool[bestPoolIdx].ReportX;
        INT dyMove = (INT)rc->Y - (INT)Pool[bestPoolIdx].ReportY;
        if (dxMove < 0) dxMove = -dxMove;
        if (dyMove < 0) dyMove = -dyMove;

        BOOLEAN isStationary = (dxMove <= TIP_DROP_STATIONARY_DELTA) &&
                               (dyMove <= TIP_DROP_STATIONARY_DELTA);

        cand.X = isStationary ? Pool[bestPoolIdx].ReportX : rc->X;
        cand.Y = isStationary ? Pool[bestPoolIdx].ReportY : rc->Y;

        // Clamp instead of letting a UCHAR wrap back to 0 (which would
        // transiently and incorrectly read as Confidence=1 one frame
        // every 256 frames of sustained soft touch).
        cand.TipDropApplied = (Pool[bestPoolIdx].TipDropCount < TIP_DROP_COUNT_MAX)
            ? (UCHAR)(Pool[bestPoolIdx].TipDropCount + 1)
            : TIP_DROP_COUNT_MAX;

        OutCandidates->Candidates[OutCandidates->Count++] = cand;
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
