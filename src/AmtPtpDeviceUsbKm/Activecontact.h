// ActiveContact.h - Contact lifecycle FSM, addressed by pool slot, NOT
// by hardware slot index.
//
// This replaces Track.h/TRACK[PTP_MAX_CONTACT_POINTS] indexed by raw
// USB slot. That scheme made hardware slot index the identity key,
// which is wrong: if a finger lifts from hw slot 2 and a new touch
// lands in hw slot 0 on the very next frame, the old scheme had no way
// to express "maybe these are related" - it could only ever see "hw
// slot 0 has a new contact". Matching decisions belong to the matcher
// (ActiveContact.c's caller, ContactMatcher in Match.c), not to array
// indexing.
//
// ACTIVE_CONTACT pool slots are an implementation detail (which free
// pool entry a contact happens to occupy) - never exposed as identity.
// ContactID is the only identity. LastSlotHint is a matching
// OPTIMIZATION ONLY: "this contact was last seen at hardware slot N",
// used by ContactMatcher to cheaply check the common case (slot stable
// across frames) before falling back to full cost evaluation. Removing
// LastSlotHint entirely would only cost CPU cycles, never correctness -
// that is the test for whether something is a hint or an identity.
//
// ---------------------------------------------------------------------
// State machine (same shape as before, decoupled from slot)
// ---------------------------------------------------------------------
//   FREE --(birth)--> ACTIVE --(lift)--> FREE
//                        |
//                        |--(gesture lift)--> GRACE --(expire)--> FREE
//
// ContactID is NEVER reused while "warm" - monotonic counter via
// DEVICE_CONTEXT.NextContactId. Smooth, non-jumpy re-tap is achieved
// via PendingFirstSample bypass, NOT ContactID reuse (TRACK_RETAP_POLICY,
// unchanged from the old Track.h).
//
// ---------------------------------------------------------------------
// Frame determinism rule (unchanged)
// ---------------------------------------------------------------------
// After ContactMatcher resolves correspondences, no contact may be
// mutated except through the ordered phase sequence: Phase A (lift) ->
// Phase B (birth) -> Phase C (update/report). All phase ordering now
// lives in PTPCore.c (PTPCore_ProcessFrame), not in the USB completion
// routine.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

typedef enum _CONTACT_STATE
{
    CONTACT_FREE = 0,  // free pool slot, no identity
    CONTACT_ACTIVE,    // finger down, identity live
    CONTACT_GRACE,     // same-frame quarantine after gesture lift; never
                       // persists past the frame it was entered.
} CONTACT_STATE;

// One pool entry. Pool position is NOT identity - ContactID is.
typedef struct _ACTIVE_CONTACT
{
    CONTACT_STATE State;

    // Windows-facing identity. Valid only while State != CONTACT_FREE.
    // Never reused while "warm" - see TRACK_RETAP_POLICY (Track.h
    // history; same policy, this is its continuation).
    ULONG ContactID;

    // Reported (post deadzone + EMA) position, in normalized device units.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline. Distinct from ReportX/Y (post-EMA).
    USHORT HystX;
    USHORT HystY;

    // Tip-size debounce counter.
    UCHAR TipDropCount;

    // TRUE if contact was part of a >=2-finger frame during its ACTIVE
    // lifetime. Causes EMA skip on first solo frame (aliveCountIsOne).
    BOOLEAN WasInGesture;

    // TRUE on first frame after birth/recycle. Bypasses deadzone+EMA
    // on first sample.
    BOOLEAN PendingFirstSample;

    // Reported last frame? Drives Phase A lift-off detection.
    BOOLEAN ReportedLastFrame;

    // ---- Matching-hint fields. NOT identity. See file header. ----
    USHORT   LastSlotHint;    // hw slot this contact was matched to last
                               // frame; used only to speed up matching.
    LONGLONG LastSeenQpc;      // QPC of last successful match; used for
                               // grace/retap timing instead of a
                               // slot-indexed side array.
} ACTIVE_CONTACT, *PACTIVE_CONTACT;

#define MAX_CONTACTS PTP_MAX_CONTACT_POINTS  // pool capacity, not a slot count

// Zero/FREE-initialise the whole pool. Call at device creation and D0Entry.
VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool);

// Finds a FREE pool entry. Returns its index, or MAX_CONTACTS if the
// pool is full (should never happen: pool size == hardware max fingers).
size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);

// FREE -> ACTIVE. Assigns fresh ContactID, seeds baseline.
// Precondition: Pool[index].State == CONTACT_FREE (debug-asserted).
VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
);

// Like AmtContactBirth but seeds baseline to RecentLiftX/Y and leaves
// PendingFirstSample=FALSE, so first sample is EMA-blended against the
// lift position. No ContactID reuse - see TRACK_RETAP_POLICY note above.
VOID
AmtContactBirthWithRetapSmoothing(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          RecentLiftX,
    _In_    USHORT          RecentLiftY,
    _In_    USHORT          slotHint
);

// Heuristic: is touch-down near a recent lift in time AND space?
// Time: within RETAP_WINDOW_100NS. Space: within RETAP_MAX_DISTANCE.
// Conservative - FALSE falls back to raw unsmoothed birth (always correct).
#define RETAP_WINDOW_100NS      (700LL * 10000LL)  // 700 ms
#define RETAP_MAX_DISTANCE      600                // normalized units

BOOLEAN
AmtContactIsRecentLiftNearby(
    _In_ LONGLONG LiftQpc,
    _In_ USHORT   LiftX,
    _In_ USHORT   LiftY,
    _In_ LONGLONG NowQpc,
    _In_ LONGLONG PerfFrequencyHz,
    _In_ USHORT   CandX,
    _In_ USHORT   CandY
);

// ACTIVE/GRACE -> FREE. Returns old ContactID/X/Y for lift-off report.
VOID
AmtContactKill(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// ACTIVE -> GRACE. Used when WasInGesture is TRUE at lift-off.
// Same-frame quarantine marker only - never re-binds ContactID.
VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// GRACE -> FREE. Called same frame after AmtContactEnterGrace.
// No report - lift-off already emitted by caller.
VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index);

// 2-pass deadzone evaluator. Pass 1: read-only check against current
// HystX/Y. Pass 2 (in AmtContactUpdate): commits HystX/Y first, then
// EMA blends OLD ReportX/Y toward new candidate.
BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY
);

// Per-frame ACTIVE contact update (Phase C). Deadzone + EMA (skipped on
// PendingFirstSample and first solo post-gesture). Writes ReportX/Y and
// LastSeenQpc/LastSlotHint (matching-hint maintenance).
VOID
AmtContactUpdate(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          rawX,
    _In_    USHORT          rawY,
    _In_    USHORT          slotHint,
    _In_    LONGLONG        nowQpc,
    _In_    BOOLEAN         aliveCountIsOne,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
);

#if DBG
// Debug-only pool invariant check: unique ContactIDs, no zero IDs on
// non-FREE entries, no flags set on FREE entries.
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);
#else
#define AmtContactPoolCheckInvariants(Pool) ((VOID)0)
#endif

EXTERN_C_END