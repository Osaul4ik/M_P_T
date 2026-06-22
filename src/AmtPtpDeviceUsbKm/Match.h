// Match.h - Cost-based matcher between RawFrame contacts and ACTIVE_CONTACT pool.
// Slot index is a matching hint only, never direct index.

#pragma once

#include "PTPCore.h"
#include "ActiveContact.h"

EXTERN_C_START

// Palm/tip-debounce-classified candidate. Dense list, not slot-indexed.
typedef struct _MATCH_CANDIDATE
{
    USHORT  SlotIndex;      // hw slot - HINT only
    USHORT  X;
    USHORT  Y;
    BOOLEAN PalmLocal;       // excluded from matching/reporting
    BOOLEAN IdentityBreak;   // firmware origin==0 signal
    UCHAR   TipDropApplied;  // non-zero when X/Y is stale (debounce bridge)
} MATCH_CANDIDATE;

typedef struct _MATCH_CANDIDATE_SET
{
    UCHAR           Count;
    MATCH_CANDIDATE Candidates[PTP_MAX_CONTACT_POINTS];
} MATCH_CANDIDATE_SET;

// Correspondence result: pool index or MATCH_NO_CORRESPONDENCE.
#define MATCH_NO_CORRESPONDENCE  ((size_t)-1)

typedef struct _MATCH_RESULT
{
    // Parallel to MATCH_CANDIDATE_SET.Candidates[]. Pool index or
    // MATCH_NO_CORRESPONDENCE if this candidate should birth new contact.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // Identity broken (lift + birth) despite correspondence.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

    // Pool indices with no corresponding candidate (should lift).
    size_t  UnmatchedPoolIndices[MAX_CONTACTS];
    UCHAR   UnmatchedCount;
} MATCH_RESULT;

// Build candidate set: palm classification + tip-size debounce.
// Below-tip with no anchor = full-confidence birth candidate.
VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
);

// Max time gap for same-ContactID continuation (150ms).
#define MATCH_MAX_TIME_DELTA_100NS (150LL * 10000LL)

// Cost-based correspondence. Greedy min-cost, N,M<=5. Rejected on spatial/time gap or IdentityBreak.
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
);

EXTERN_C_END