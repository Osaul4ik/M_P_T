// Match.c - PTPCore ContactMatcher.

#include "Driver.h"
#include "Match.h"
#include "Palm.h"

// Max per-frame finger movement. Shared with tip-drop anchor search.
#define MATCH_MAX_CONTINUATION_DELTA 4000

// Must match MATCH_MAX_CONTINUATION_DELTA to prevent false Confidence=1
// on fast-but-light drags (soft-drift confidence bug fix).
#define TIP_DROP_MAX_REPOSITION_DELTA MATCH_MAX_CONTINUATION_DELTA

// Slot-hint used only to break near-ties, never fixed-cost subtraction.
#define MATCH_TIE_EPSILON_SQ 4  // ~2 units linear distance, squared

// Stationary deadzone for debounce bridge. Real coords if moving.
#define TIP_DROP_STATIONARY_DELTA       3

static UCHAR
AmtMatchCandidateTip(_In_ USHORT major, _In_ USHORT minor)
{
    return (UCHAR)((INT)major >= 100 || (INT)minor >= 75);
}

// Cheap tie-breaker: keep geometry and pressure similar.
static LONG
AmtMatchShapeDistance(
    _In_ const MATCH_CANDIDATE* Cand,
    _In_ const ACTIVE_CONTACT*  Contact
)
{
    INT dMajor    = AmtAbsDelta((INT)Cand->Major, (INT)Contact->LastMajor);
    INT dMinor    = AmtAbsDelta((INT)Cand->Minor, (INT)Contact->LastMinor);
    INT dPressure = AmtAbsDelta((INT)Cand->Pressure, (INT)Contact->LastPressure);

    return (LONG)dMajor + dMinor + dPressure;
}

// Geometric birth check: is there ALREADY an active pool contact near this
// raw position? Used instead of the firmware Origin==0 flag, which proved
// unreliable on some T2 firmware (never/rarely resets to 0 on a genuine
// new touch, silently defeating the edge-zone hard reject in Palm.c).
// Same distance semantics as the bridging search below, for consistency.
static BOOLEAN
AmtMatchHasNearbyActiveContact(
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _In_ INT                                       RcX,
    _In_ INT                                       RcY
)
{
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        const ACTIVE_CONTACT* poolEntry = &Pool[p];
        if (poolEntry->State != CONTACT_ACTIVE)
            continue;

        INT dxAbs = AmtAbsDelta(RcX, (INT)poolEntry->ReportX);
        INT dyAbs = AmtAbsDelta(RcY, (INT)poolEntry->ReportY);

        if (dxAbs <= MATCH_MAX_CONTINUATION_DELTA &&
            dyAbs <= MATCH_MAX_CONTINUATION_DELTA)
            return TRUE;
    }
    return FALSE;
}

VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_  const AMT_PALM_CONFIG*                   PalmConfig,
    _In_  const AMT_PALM_RUNTIME*                  PalmRuntime,
    _In_  const AMT_POINTER_CONFIG*                 PointerConfig,
    _In_  BOOLEAN                                   SupportsForceTouch,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
)
{
    *LargePalmDetected = FALSE;

    // OPTIMIZATION: was RtlZeroMemory(OutCandidates, sizeof(MATCH_CANDIDATE_SET))
    // - zeroing all PTP_MAX_CONTACT_POINTS Candidates[] slots every frame.
    // Same reasoning as PTP_CORE_FRAME in Ptpcore.c: every consumer (in
    // this file and Ptpcore.c) only ever iterates ci < Count, and every
    // slot that gets counted is assigned a fully-initialized `cand` (see
    // below) before Count is incremented past it. Only Count itself needs
    // a defined starting value.
    OutCandidates->Count = 0;

    for (UCHAR i = 0; i < RawFrame->ContactCount; i++) {
        const RAW_CONTACT* rc = &RawFrame->Contacts[i];

        // Optional strict non-Force-Touch mode: the size gate applies to
        // every frame, not only to the birth of a new contact. A contact is
        // eligible for matching/output only when BOTH dimensions meet the
        // threshold. When either falls below the threshold, this frame is
        // treated as if the contact were absent; the normal contact FSM then
        // emits the proper UP for the old identity and a later qualifying
        // frame can create a fresh DOWN. Major ("M") threshold is
        // GUI-tunable (PointerConfig->SmallContactMajorThreshold); Minor
        // keeps its fixed floor (AMT_SMALL_CONTACT_MINOR_THRESHOLD).
        if (!SupportsForceTouch &&
            PointerConfig != NULL &&
            PointerConfig->SmallContactRejectionEnabled &&
            PointerConfig->SmallContactRejectionStrict &&
            (rc->Major < PointerConfig->SmallContactMajorThreshold ||
             rc->Minor < AMT_SMALL_CONTACT_MINOR_THRESHOLD))
        {
            continue;
        }

        // Same size gate, mirrored for Force-Touch-capable devices. Normally
        // Force Touch hardware relies on the pressure gate above instead,
        // but some contacts (light/ambiguous touches before pressure ramps
        // up) can still benefit from a Major/Minor floor. Off by default
        // (ForceTouchSmallContactRejectionEnabled = 0); when enabled with
        // Strict, applies every frame just like the non-Force-Touch gate,
        // using its own GUI-tunable "M" threshold
        // (PointerConfig->ForceTouchSmallContactMajorThreshold).
        if (SupportsForceTouch &&
            PointerConfig != NULL &&
            PointerConfig->ForceTouchSmallContactRejectionEnabled &&
            PointerConfig->ForceTouchSmallContactRejectionStrict &&
            (rc->Major < PointerConfig->ForceTouchSmallContactMajorThreshold ||
             rc->Minor < AMT_SMALL_CONTACT_MINOR_THRESHOLD))
        {
            continue;
        }

        // Optional strict Force-Touch pressure mode: the pressure gate applies
        // to every frame, not only to the birth of a new contact. A Force-Touch
        // contact is eligible for matching/output only while pressure is
        // positive. If pressure drops to zero, this frame is treated as absent;
        // the normal contact FSM then emits UP for the old identity. A later
        // frame with positive pressure can create a new DOWN.
        if (SupportsForceTouch &&
            PointerConfig != NULL &&
            PointerConfig->ForceTouchEnabled &&
            PointerConfig->RequirePressureContinuously &&
            rc->Pressure == 0)
        {
            continue;
        }

        // Birth = no already-active pool contact sits near this raw
        // position. Geometric, not firmware-flag-based - feeds the
        // birth-in-edge-zone hard reject in AmtPalmClassify (Palm.c).
        BOOLEAN isBirth = !AmtMatchHasNearbyActiveContact(
            Pool, (INT)rc->X, (INT)rc->Y);

        PALM_CLASS palm = AmtPalmClassify(rc->Major, rc->Minor, DevInfo, PalmConfig, PalmRuntime,
                                          (INT)rc->X, (INT)rc->Y, isBirth);

        if (palm == PALM_LARGE) {
            *LargePalmDetected = TRUE;
            OutCandidates->Count = 0; // blank whole pad
            return;
        }

        // Write straight into the destination slot - no stack-temp +
        // struct-copy. Same pattern AmtCoreEmitContact (Ptpcore.c) already
        // uses for PTP_CORE_CONTACT: take the slot pointer once, fill its
        // fields in place, only advance Count when the slot is committed.
        MATCH_CANDIDATE* cand = &OutCandidates->Candidates[OutCandidates->Count];
        RtlZeroMemory(cand, sizeof(*cand));
        cand->SlotIndex     = rc->SlotIndex;
        cand->IdentityBreak = (rc->Origin == 0);
        cand->Major         = rc->Major;
        cand->Minor         = rc->Minor;
        cand->Pressure      = rc->Pressure;

        // High-signal contact (local palm or tip-sized): report raw coords
        // as-is. Merged into one branch - both arms were an identical
        // X/Y-then-commit tail, differing only in the PalmLocal flag.
        // Short-circuit `||` preserves the original call pattern exactly:
        // AmtMatchCandidateTip is still skipped whenever palm == PALM_LOCAL.
        if (palm == PALM_LOCAL || AmtMatchCandidateTip(rc->Major, rc->Minor)) {
            cand->PalmLocal = (palm == PALM_LOCAL);
            cand->X = rc->X;
            cand->Y = rc->Y;
            OutCandidates->Count++;
            continue;
        }

        // Bridge low-signal contacts through the pool when possible.
        //
        // ARCHITECTURE FIX (stale-SlotIndex bridging bug): SlotIndex is a
        // scan-order array position assigned in AmtInputParseFrame (Input.c),
        // not a stable hardware identity - struct TRACKPAD_FINGER
        // (AppleDefinition.h) has no such field, the firmware doesn't expose
        // one. Requiring an EXACT LastSlotHint == SlotIndex match here (as
        // a hard filter, unlike AmtMatchCorrespond below which only uses it
        // to break near-ties) meant any scan-order reshuffle between frames
        // - e.g. a second finger landing and shifting an existing finger's
        // array position - silently broke the bridge for a contact that
        // never physically moved, forcing a spurious rebirth. Symptom:
        // asymmetric "right-then-left" tap failures.
        //
        // Fix: geometry (distance) is now the primary key, gated by
        // TIP_DROP_MAX_REPOSITION_DELTA exactly as before; slot-hint only
        // breaks near-ties between two otherwise equally-plausible
        // candidates - the same tie-break role it already plays in
        // AmtMatchCorrespond, for consistency.
        size_t  bestPoolIdx       = MAX_CONTACTS;
        ULONGLONG bestDistSq     = ~0ULL;
        BOOLEAN bestSlotHintMatch = FALSE;

        // MICRO-OPT: rc->X/rc->Y are loop-invariant across the p-loop below
        // (only poolEntry changes per iteration) - cast and read once here
        // instead of redoing it on every one of the up to MAX_CONTACTS
        // iterations. Same values, same semantics, matches the hoisting
        // style already used elsewhere in this file/Input.c.
        INT rcX = (INT)rc->X;
        INT rcY = (INT)rc->Y;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            const ACTIVE_CONTACT* poolEntry = &Pool[p];
            if (poolEntry->State != CONTACT_ACTIVE)
                continue;

            INT dxAbs = AmtAbsDelta(rcX, (INT)poolEntry->ReportX);
            INT dyAbs = AmtAbsDelta(rcY, (INT)poolEntry->ReportY);

            if (dxAbs > TIP_DROP_MAX_REPOSITION_DELTA ||
                dyAbs > TIP_DROP_MAX_REPOSITION_DELTA)
                continue;

            ULONGLONG distSq        = AmtDistSq(dxAbs, dyAbs);
            BOOLEAN slotHintMatch = (poolEntry->LastSlotHint == rc->SlotIndex);

            if (bestPoolIdx == MAX_CONTACTS) {
                bestPoolIdx       = p;
                bestDistSq        = distSq;
                bestSlotHintMatch = slotHintMatch;
                continue;
            }

            ULONGLONG delta =
                (distSq >= bestDistSq) ? (distSq - bestDistSq) : (bestDistSq - distSq);
            BOOLEAN withinEpsilon = delta < (ULONGLONG)MATCH_TIE_EPSILON_SQ;

            if (distSq < bestDistSq && !withinEpsilon) {
                bestPoolIdx       = p;
                bestDistSq        = distSq;
                bestSlotHintMatch = slotHintMatch;
            } else if (withinEpsilon && slotHintMatch && !bestSlotHintMatch) {
                // Cost tied within epsilon and this candidate's slot-hint
                // agrees while the current best's doesn't - prefer it.
                bestPoolIdx       = p;
                bestDistSq        = distSq;
                bestSlotHintMatch = TRUE;
            }
        }

        if (bestPoolIdx == MAX_CONTACTS) {
            // A new soft tap is tracked but not trusted yet.
            cand->X              = rc->X;
            cand->Y              = rc->Y;
            cand->TipDropApplied = 0;
            cand->Unconfirmed    = 1;
            OutCandidates->Count++;
            continue;
        }

        // Bridge candidate through; real coords if moving, anchor if stationary.
        USHORT anchorX = Pool[bestPoolIdx].ReportX;
        USHORT anchorY = Pool[bestPoolIdx].ReportY;

        INT dxMove = AmtAbsDelta((INT)rc->X, (INT)anchorX);
        INT dyMove = AmtAbsDelta((INT)rc->Y, (INT)anchorY);

        BOOLEAN isStationary = (dxMove <= TIP_DROP_STATIONARY_DELTA) &&
                               (dyMove <= TIP_DROP_STATIONARY_DELTA);

        cand->X = isStationary ? anchorX : rc->X;
        cand->Y = isStationary ? anchorY : rc->Y;

        // Only the stationary bridge reports a stale position.
        cand->TipDropApplied = isStationary ? 1 : 0;

        // Anchored bridges stay confirmed; only true births stay unconfirmed.

        OutCandidates->Count++;
    }
}

VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  MaxTicks,
    _Out_ MATCH_RESULT*                              OutResult
)
{
    RtlZeroMemory(OutResult, sizeof(MATCH_RESULT));

    BOOLEAN poolClaimed[MAX_CONTACTS];
    RtlZeroMemory(poolClaimed, sizeof(poolClaimed));

    // Greedy minimum-cost assignment (N,M <= 5).
    // MICRO-OPT: shapeDist is precomputed once per pair here instead of
    // being recomputed by AmtMatchShapeDistance every time this same pair
    // is re-examined across multiple "pick" iterations below. Same values,
    // same call sites logically - just computed once and cached.
    //
    // MICRO-OPT: poolIdx is UCHAR, not size_t - MAX_CONTACTS is 5, always
    // fits, keeping the pair record compact. The 64-bit cost is intentional
    // so squared-distance arithmetic cannot overflow. pairs[] is scanned
    // repeatedly in the O(pairCount^2) pick loop below
    // (up to 625 accesses/frame), so the smaller footprint means fewer
    // cache lines touched per scan. Purely internal representation -
    // pairs[bestIdx].poolIdx below still widens to size_t on read.
    typedef struct { UCHAR candIdx; UCHAR poolIdx; ULONGLONG cost; LONG shapeDist; BOOLEAN slotHintMatch; } PAIR;
    PAIR pairs[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    UCHAR pairCount = 0;

    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        const MATCH_CANDIDATE* cand = &Candidates->Candidates[ci];

        OutResult->CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;

        if (cand->PalmLocal)
            continue;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            const ACTIVE_CONTACT* poolEntry = &Pool[p];
            if (poolEntry->State != CONTACT_ACTIVE)
                continue;

            INT dx = (INT)cand->X - (INT)poolEntry->ReportX;
            INT dy = (INT)cand->Y - (INT)poolEntry->ReportY;
            ULONGLONG dist = AmtDistSq(dx, dy); // squared distance

            pairs[pairCount].candIdx       = ci;
            pairs[pairCount].poolIdx       = (UCHAR)p;
            pairs[pairCount].cost          = dist;
            pairs[pairCount].shapeDist     = AmtMatchShapeDistance(cand, poolEntry);
            pairs[pairCount].slotHintMatch = (cand->SlotIndex == poolEntry->LastSlotHint);
            pairCount++;
        }
    }

    // Greedy matching by ascending cost. Slot-hint tie-breaker within epsilon.
    BOOLEAN pairUsed[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    RtlZeroMemory(pairUsed, sizeof(pairUsed));

    BOOLEAN candClaimed[PTP_MAX_CONTACT_POINTS];
    RtlZeroMemory(candClaimed, sizeof(candClaimed));

    for (UCHAR pick = 0; pick < pairCount; pick++) {
        ULONGLONG bestCost       = ~0ULL;
        UCHAR   bestIdx           = 0;
        BOOLEAN bestSlotHintMatch = FALSE;
        LONG    bestShapeDist     = 0;
        BOOLEAN found             = FALSE;

        for (UCHAR k = 0; k < pairCount; k++) {
            if (pairUsed[k]) continue;
            if (candClaimed[pairs[k].candIdx]) continue;
            if (poolClaimed[pairs[k].poolIdx]) continue;

            if (!found) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
                bestShapeDist     = pairs[k].shapeDist;
                found             = TRUE;
                continue;
            }

            ULONGLONG delta =
                (pairs[k].cost >= bestCost)
                    ? (pairs[k].cost - bestCost)
                    : (bestCost - pairs[k].cost);
            BOOLEAN withinEpsilon = delta < (ULONGLONG)MATCH_TIE_EPSILON_SQ;

            if (pairs[k].cost < bestCost && !withinEpsilon) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
                bestShapeDist     = pairs[k].shapeDist;
            } else if (withinEpsilon && pairs[k].slotHintMatch && !bestSlotHintMatch) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = TRUE;
                bestShapeDist     = pairs[k].shapeDist;
            } else if (withinEpsilon && pairs[k].slotHintMatch == bestSlotHintMatch) {
                // Cost and slot-hint both tied - fall through to shape.
                LONG kShapeDist = pairs[k].shapeDist;
                if (kShapeDist < bestShapeDist) {
                    bestCost      = pairs[k].cost;
                    bestIdx       = k;
                    bestShapeDist = kShapeDist;
                    // bestSlotHintMatch unchanged - already equal to pairs[k]'s.
                }
            }
        }

        if (!found)
            break;

        pairUsed[bestIdx]                 = TRUE;
        UCHAR  ci = pairs[bestIdx].candIdx;
        size_t p  = pairs[bestIdx].poolIdx;

        // Reject implausible matches.
        BOOLEAN spatialReject = pairs[bestIdx].cost >
            (LONG)MATCH_MAX_CONTINUATION_DELTA * MATCH_MAX_CONTINUATION_DELTA;

        // Time-domain rejection. LastSeenQpc=0 -> never updated, skip time check.
        BOOLEAN timeReject = FALSE;
        if (Pool[p].LastSeenQpc != 0 && MaxTicks > 0) {
            LONGLONG deltaTicks = NowQpc - Pool[p].LastSeenQpc;
            timeReject = (NowQpc < Pool[p].LastSeenQpc) || (deltaTicks > MaxTicks);
        }

        // BUG FIX (lost continuation on a rejected best-cost pair): this
        // used to burn the whole candidate. pairUsed[bestIdx] already
        // prevents re-picking this exact pair; leaving candClaimed[ci]
        // FALSE lets the greedy loop reconsider this candidate against a
        // different, still-unclaimed pool entry on a later pick - the
        // cheapest pairing can be implausible while a legitimate one
        // exists. Previously the sorted-first pair ate the candidate,
        // forcing a spurious lift+rebirth (broken drag / interrupted
        // gesture / dropped tap). poolClaimed[p] stays untouched so the
        // rejected pool entry remains available to other candidates.
        if (spatialReject || timeReject) {
            continue;
        }

        candClaimed[ci] = TRUE;
        poolClaimed[p]  = TRUE;

        OutResult->CorrespondingPoolIndex[ci] = p;
        OutResult->NewIdentity[ci] =
            Candidates->Candidates[ci].IdentityBreak ? TRUE : FALSE;
    }

    // Unclaimed -> lift.
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State != CONTACT_ACTIVE)
            continue;
        if (!poolClaimed[p]) {
            OutResult->UnmatchedPoolIndices[OutResult->UnmatchedCount++] = p;
        }
    }
}