// Match.h - PTPCore / ContactMatcher.
//
// CORRECTED scope (see audit/04_correction_plan.md): this is now a real
// cost-based matcher between RawFrame contacts and the ACTIVE_CONTACT
// pool. Hardware slot index is used ONLY as one term in the matching
// cost (cheap, usually-correct hint - slot indices are typically
// stable frame-to-frame) - it is never used to index into the contact
// pool directly. A contact that moves to a different hardware slot
// between frames is still correctly matched by position + recency,
// just with a slightly higher cost than a slot-stable match.
//
// This replaces the earlier (incorrect) version of this file, which
// matched MATCH_SAMPLE[SlotIndex] directly against Tracks[SlotIndex] -
// that made slot index the identity key, which is exactly what this
// refactor exists to remove.

#pragma once

#include "PTPCore.h"
#include "ActiveContact.h"

EXTERN_C_START

// One decoded, palm/tip-debounce-classified candidate contact for this
// frame. Array is sized PTP_MAX_CONTACT_POINTS but is a dense list of
// candidates, NOT slot-indexed - SlotIndex is carried as a field, not
// implied by array position.
typedef struct _MATCH_CANDIDATE
{
    USHORT  SlotIndex;      // hw slot this candidate came from - HINT only
    USHORT  X;
    USHORT  Y;
    BOOLEAN PalmLocal;       // excluded from matching/reporting, doesn't blank pad
    BOOLEAN IdentityBreak;   // firmware origin==0 signal for this slot
    UCHAR   TipDropApplied;  // non-zero if position substituted by debounce
} MATCH_CANDIDATE;

typedef struct _MATCH_CANDIDATE_SET
{
    UCHAR           Count;
    MATCH_CANDIDATE Candidates[PTP_MAX_CONTACT_POINTS];
} MATCH_CANDIDATE_SET;

// Correspondence result: for each candidate, either a matched pool
// index (existing ACTIVE_CONTACT to update) or "no match" (new birth).
#define MATCH_NO_CORRESPONDENCE  ((size_t)-1)

typedef struct _MATCH_RESULT
{
    // Parallel to MATCH_CANDIDATE_SET.Candidates[]. Value is the pool
    // index in ActiveContacts[] this candidate corresponds to, or
    // MATCH_NO_CORRESPONDENCE if this candidate should birth a new contact.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // TRUE if the matched pool entry's identity should be considered
    // broken (i.e. treat as lift-of-old + birth-of-new rather than
    // continue) even though a correspondence was found - e.g. firmware
    // origin==0 signal fired, or the matched candidate is implausibly
    // far from the pool entry's last known position.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

    // Pool indices that have NO corresponding candidate this frame
    // (i.e. should lift). Terminated by MAX_CONTACTS sentinel value if
    // fewer than MAX_CONTACTS entries are unmatched.
    size_t  UnmatchedPoolIndices[MAX_CONTACTS];
    UCHAR   UnmatchedCount;
} MATCH_RESULT;

// L1.5a: Builds the candidate set from a RawFrame - decodes palm
// classification (delegated to Palm.c) and tip-size debounce (reads
// the ActiveContact pool by LastSlotHint, not by direct index - this is
// the stateful step that cannot live in InputAdapter; see PTPCore.h #2).
// Sets *LargePalmDetected for full-pad blanking.
VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
);

// L1.5b: Cost-based correspondence between Candidates and the ACTIVE
// pool. Cost = spatial distance (primary) with a bonus (cost reduction)
// for SlotIndex == Pool[i].LastSlotHint (cheap common-case win, not a
// requirement). Greedy minimum-cost assignment, O(N*M) for N,M <= 5 -
// a full Hungarian/optimal assignment is unnecessary at this scale.
// A candidate's correspondence is rejected (-> NewIdentity / new birth)
// if firmware signalled IdentityBreak, OR if the best-cost match still
// exceeds MATCH_MAX_CONTINUATION_DELTA (implausible jump).
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _Out_ MATCH_RESULT*                             OutResult
);

EXTERN_C_END