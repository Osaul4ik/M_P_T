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
// via PendingFirstSample bypass, not ContactID reuse.
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

    // TRUE on first frame after birth. Bypasses deadzone+EMA.
    BOOLEAN PendingFirstSample;

    // Reported last frame? Drives Phase A lift-off detection.
    BOOLEAN ReportedLastFrame;

    // ---- Matching-hint fields. NOT identity. ----
    USHORT   LastSlotHint;    // hw slot matched to last frame; speeds up matching
    LONGLONG LastSeenQpc;     // QPC of last successful match; grace/retap timing

    // FIX (Task 4.2): frames alive since birth. Used by Phase A to hold
    // a too-fresh solo contact alive for MIN_CONTACT_LIFETIME_FRAMES so
    // Windows' gesture recognizer sees DOWN before UP. NOT identity,
    // NOT a matching hint.
    UCHAR FramesAlive;
} ACTIVE_CONTACT, *PACTIVE_CONTACT;

#define MAX_CONTACTS PTP_MAX_CONTACT_POINTS  // pool capacity, not slot count

// FIX (Task 4.2): min frames before Phase A can Kill an unmatched contact.
// Below this, re-reports as MOVE at last position for one more frame.
// 2 frames @ ~8ms = ~16ms, below human tap perception, enough for Windows
// gesture recognizer. Does NOT apply to WasInGesture/EnterGrace path.
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

// Like AmtContactBirth but seeds baseline to RecentLiftX/Y with
// PendingFirstSample=FALSE, so first sample EMA-blends against lift pos.
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
// PendingFirstSample and first solo post-gesture). Updates matching hints
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