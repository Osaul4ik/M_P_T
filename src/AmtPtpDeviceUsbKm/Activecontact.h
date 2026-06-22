// ActiveContact.h - Contact lifecycle FSM, pool-slot addressed (not hw slot).
//
// Replaces Track.h/TRACK[PTP_MAX_CONTACT_POINTS] indexed by raw USB slot.
// Old scheme used hw slot as identity key - wrong: a finger lifting from
// slot 2 and a new touch landing on slot 0 next frame couldn't express
// "maybe related". Matching belongs in Match.c, not array indexing.
//
// Pool slots are an implementation detail - never exposed as identity.
// ContactID is the only identity. LastSlotHint is a matching OPTIMIZATION
// ONLY: removing it costs CPU, never correctness.
//
// ---------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------
//   FREE --(birth)--> ACTIVE --(lift)--> FREE
//                        |
//                        |--(gesture lift)--> GRACE --(expire)--> FREE
//
// ContactID never reused while warm (monotonic counter). Smooth re-tap
// via EMA baseline seeding in BirthWithRetapSmoothing, not ContactID reuse.
//
// ---------------------------------------------------------------------
// Frame determinism rule
// ---------------------------------------------------------------------
// After matching, contacts mutate only via Phase A (lift) -> Phase B
// (birth) -> Phase C (update/report). Ordering in PTPCore.c.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

typedef enum _CONTACT_STATE
{
    CONTACT_FREE = 0,  // free pool slot, no identity
    CONTACT_ACTIVE,    // finger down, identity live
    CONTACT_GRACE,     // same-frame quarantine after gesture lift
} CONTACT_STATE;

// One pool entry. Pool position != identity - ContactID is.
typedef struct _ACTIVE_CONTACT
{
    CONTACT_STATE State;

    // Windows-facing identity. Valid while State != CONTACT_FREE.
    // Never reused while warm.
    ULONG ContactID;

    // Reported (post deadzone + EMA) position, normalized device units.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline. Distinct from ReportX/Y (post-EMA).
    USHORT HystX;
    USHORT HystY;

    // Tip-size debounce counter.
    UCHAR TipDropCount;

    // TRUE if contact was in >=2-finger frame during ACTIVE lifetime.
    // Causes EMA skip on first solo frame (aliveCountIsOne).
    BOOLEAN WasInGesture;

    // TRUE on first frame after birth. Bypasses deadzone+EMA so first
    // DOWN report always carries the real finger position - UNLESS
    // RetapSeeded is also TRUE (see below), in which case the seeded
    // Hyst/Report baseline from BirthWithRetapSmoothing is preserved
    // and the deadzone+EMA path runs normally on this first sample.
    BOOLEAN PendingFirstSample;

    // BUG FIX: previously AmtContactUpdate() unconditionally overwrote
    // HystX/Y with the raw just-sampled position whenever
    // PendingFirstSample was TRUE, and AmtContactCommitSample()
    // unconditionally skipped EMA in that case too. That meant the
    // EMA-seed baseline written by AmtContactBirthWithRetapSmoothing
    // (RecentLiftX/Y) was clobbered by the very first AmtContactUpdate
    // call in the SAME frame, before it was ever read - making
    // AmtContactBirthWithRetapSmoothing behaviorally identical to a
    // plain AmtContactBirth. Retap smoothing never actually smoothed
    // anything.
    //
    // RetapSeeded distinguishes "first sample after a fresh birth"
    // (HystX/Y should be reset to the real finger position) from
    // "first sample after a retap-smoothed birth" (HystX/Y already
    // hold a deliberate seed and must be evaluated/blended normally,
    // not reset). Cleared together with PendingFirstSample once the
    // first sample has been committed.
    BOOLEAN RetapSeeded;

    // Reported last frame? Drives Phase A lift-off detection.
    BOOLEAN ReportedLastFrame;

    // ---- Matching-hint fields. NOT identity. ----
    USHORT   LastSlotHint;    // hw slot matched to last frame; speeds up matching
    LONGLONG LastSeenQpc;     // QPC of last successful match; grace/retap timing

    // Frames alive since birth. Used by Phase A to hold the last finger
    // of a gesture session alive for MIN_CONTACT_LIFETIME_FRAMES so
    // Windows' gesture recognizer sees DOWN before UP.
    // NOT used for solo tap deferral (Issue #2 fix).
    // NOT identity, NOT a matching hint.
    UCHAR FramesAlive;
} ACTIVE_CONTACT, *PACTIVE_CONTACT;

#define MAX_CONTACTS PTP_MAX_CONTACT_POINTS  // pool capacity, not slot count

// Min frames before Phase A defers kill on the last finger of a gesture
// session. Applied ONLY when WasInGesture=TRUE && aliveCount==0.
// Solo contacts are killed immediately regardless of FramesAlive.
// See PTPCore.c Phase A for the full gating logic.
#define MIN_CONTACT_LIFETIME_FRAMES 2

// Zero/FREE-initialise the whole pool. Call at device creation and D0Entry.
VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool);

// Finds a FREE pool entry. Returns index or MAX_CONTACTS if full.
size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);

// FREE -> ACTIVE. Assigns fresh ContactID, seeds baseline.
// Precondition: Pool[index].State == CONTACT_FREE.
VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
);

// Like AmtContactBirth but seeds HystX/Y and ReportX/Y to RecentLiftX/Y
// so EMA blends smoothly from the lift position on subsequent MOVE frames
// (cursor continuity on re-tap).
//
// Sets RetapSeeded=TRUE (see ACTIVE_CONTACT.RetapSeeded above) so the
// FIRST AmtContactUpdate call after this birth does NOT clobber the
// seeded HystX/Y with the raw touch-down position - it instead runs the
// normal deadzone+EMA path against the seeded baseline, which is what
// actually produces the intended smooth-cursor-continuity-on-retap
// behavior. (Previously RetapSeeded did not exist and the seed was
// overwritten before ever being used - see the FIX comment on
// RetapSeeded in ActiveContact.h.)
VOID
AmtContactBirthWithRetapSmoothing(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          RecentLiftX,
    _In_    USHORT          RecentLiftY,
    _In_    USHORT          slotHint
);

// Is touch-down near a recent lift in time (RETAP_WINDOW_100NS) AND space
// (RETAP_MAX_DISTANCE)? FALSE -> raw unsmoothed birth (always correct).
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

// ACTIVE -> GRACE. Used when WasInGesture at lift-off.
// Same-frame quarantine only - never re-binds ContactID.
VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// GRACE -> FREE. Called same frame after EnterGrace. No report emitted.
VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index);

// 2-pass deadzone evaluator. Pass 1: read-only check vs HystX/Y.
// Pass 2 (in AmtContactUpdate): commits HystX/Y, then EMA blends.
BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY
);

// Per-frame ACTIVE contact update (Phase C). Deadzone + EMA (skipped on
// PendingFirstSample for a plain birth, and on first solo post-gesture
// frame). For a retap-seeded birth (RetapSeeded==TRUE), the first sample
// runs deadzone+EMA normally against the seeded baseline instead of
// bypassing it - see ACTIVE_CONTACT.RetapSeeded. Updates matching hints
// and increments FramesAlive.
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
// Debug: unique ContactIDs, no zero IDs on non-FREE, no flags on FREE.
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);
#else
#define AmtContactPoolCheckInvariants(Pool) ((VOID)0)
#endif

EXTERN_C_END