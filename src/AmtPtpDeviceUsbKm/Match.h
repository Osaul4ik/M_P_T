// Match.h - PTPCore / ContactMatcher.
//
// CORRECTED scope: cost-based matcher between RawFrame contacts and the
// ACTIVE_CONTACT pool. Hardware slot index is used ONLY as one term in
// matching cost (cheap hint) - never to index the pool directly.
// A contact moving to a different hw slot between frames is still
// correctly matched by position + recency.

#pragma once

#include "PTPCore.h"
#include "ActiveContact.h"

EXTERN_C_START

// One decoded, palm/tip-debounce-classified candidate. Dense list, NOT
// slot-indexed - SlotIndex is a field, not array position.
typedef struct _MATCH_CANDIDATE
{
    USHORT  SlotIndex;      // hw slot - HINT only
    USHORT  X;
    USHORT  Y;
    BOOLEAN PalmLocal;       // excluded from matching/reporting
    BOOLEAN IdentityBreak;   // firmware origin==0 signal
    UCHAR   TipDropApplied;  // non-zero ONLY when X/Y was substituted with
                              // a stale anchor position by tip-size
                              // debounce (PTPCore.c reports Confidence=0
                              // in that case). Zero for a genuine new
                              // touch-down with no anchor (X/Y is real,
                              // just-sampled data - reported with
                              // Confidence=1). Mirrored every frame into
                              // ActiveContacts[p].TipDropCount by
                              // PTPCore.c so AmtMatchBuildCandidates' own
                              // debounce-exhausted check is meaningful.
} MATCH_CANDIDATE;

typedef struct _MATCH_CANDIDATE_SET
{
    UCHAR           Count;
    MATCH_CANDIDATE Candidates[PTP_MAX_CONTACT_POINTS];
} MATCH_CANDIDATE_SET;

// Correspondence result: for each candidate, either a matched pool index
// (existing ACTIVE_CONTACT to update) or "no match" (new birth).
#define MATCH_NO_CORRESPONDENCE  ((size_t)-1)

typedef struct _MATCH_RESULT
{
    // Parallel to MATCH_CANDIDATE_SET.Candidates[]. Pool index or
    // MATCH_NO_CORRESPONDENCE if this candidate should birth new contact.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // TRUE if matched pool entry's identity is broken (lift + birth)
    // despite correspondence - e.g. firmware origin==0 or implausible
    // spatial/time gap.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

    // Pool indices with no corresponding candidate (should lift).
    size_t  UnmatchedPoolIndices[MAX_CONTACTS];
    UCHAR   UnmatchedCount;
} MATCH_RESULT;

// L1.5a: Builds candidate set from RawFrame - palm classification (Palm.c)
// and tip-size debounce (reads pool by LastSlotHint, not direct index).
// Sets *LargePalmDetected for full-pad blanking.
//
// FIX (soft-tap-loss): below-tip-size candidate with no debounce anchor
// is a first touch-down, not noise - passed through as a full-confidence
// birth candidate (TipDropApplied=0) instead of silently dropped or
// (the bug this header previously documented as intentional) marked
// low-confidence. See the long comment at the call site in Match.c.
VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
);

// Max time since pool entry's last match (LastSeenQpc) before a spatially
// close candidate is rejected as implausible continuation (Task 2.2).
// Independent of RETAP_WINDOW_100NS (700ms) - that governs deliberate
// re-tap path (new ContactID), this governs same-ContactID continuation.
// 150ms is generous relative to USB polling interval.
#define MATCH_MAX_TIME_DELTA_100NS (150LL * 10000LL)

// L1.5b: Cost-based correspondence. Cost = spatial distance (primary)
// with slot-hint tie-breaker (MATCH_TIE_EPSILON_SQ). Greedy minimum-cost
// assignment, O(N*M) for N,M <= 5 - Hungarian unnecessary at this scale.
// Correspondence rejected (-> NewIdentity) if IdentityBreak, spatial
// jump > MATCH_MAX_CONTINUATION_DELTA, or (Task 2.2) time gap >
// MATCH_MAX_TIME_DELTA_100NS.
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
);

EXTERN_C_END