// Track.h - Per-contact lifecycle FSM.
//
// Replaces the old parallel-array scheme with one TRACK struct per raw
// hardware slot, carrying an explicit state machine.
//
// ---------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------
//
//   TRACK_DEAD --(birth)--> TRACK_ACTIVE --(lift)--> TRACK_DEAD
//                                |
//                                |--(gesture lift)--> TRACK_GRACE --(expire)--> TRACK_DEAD
//
// TRACK_DEAD  - free slot, no identity.
// TRACK_ACTIVE - finger down, live ContactID and smoothing state.
// TRACK_GRACE - same-frame quarantine after gesture lift; never persists
//               past the frame it was entered (see TRACK_RETAP_POLICY).
//
// ContactID is NEVER reused while "warm" — monotonic counter via
// DEVICE_CONTEXT.NextContactId. Smooth, non-jumpy re-tap is achieved
// via PendingFirstSample bypass, NOT ContactID reuse.
//
// ---------------------------------------------------------------------
// Frame determinism rule
// ---------------------------------------------------------------------
// After AmtMatchFrame, no track may be mutated except through the
// ordered phase sequence: Phase A (lift) -> Phase B (birth) -> Phase C (update/report).
// Pre-transition state must be read BEFORE AmtTrackKill/Recycle.
//
// ---------------------------------------------------------------------
// L3 read-only contract
// ---------------------------------------------------------------------
// AmtTrackUpdate is the ONLY Phase C mutation path. Report code reads
// OutX/OutY and ContactID; never writes smoothing state directly
// (ReportedLastFrame is the sole documented exception).

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

typedef enum _TRACK_STATE
{
    TRACK_DEAD = 0,   // free slot, no identity
    TRACK_ACTIVE,     // finger down, identity live
    TRACK_GRACE,      // same-frame quarantine after gesture lift; never
                      // persists past the frame it was entered.
} TRACK_STATE;

// One track per raw slot. Mutated only by Track.c lifecycle functions.
typedef struct _TRACK
{
    TRACK_STATE State;

    // Windows-facing identity. Valid only while State != TRACK_DEAD.
    // Never reused while "warm" — see TRACK_RETAP_POLICY.
    ULONG ContactID;

    // Reported (post deadzone + EMA) position, in normalized device units.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline. Distinct from ReportX/Y (post-EMA).
    USHORT HystX;
    USHORT HystY;

    // Tip-size debounce counter.
    UCHAR TipDropCount;

    // TRUE if track was part of a >=2-finger frame during its ACTIVE
    // lifetime. Causes EMA skip on first solo frame (aliveCountIsOne).
    // Consumed on spend — see AmtTrackCommitSample.
    BOOLEAN WasInGesture;

    // TRUE on first frame after birth/recycle. Bypasses deadzone+EMA
    // on first sample. Makes post-gesture re-tap land cleanly.
    BOOLEAN PendingFirstSample;

    // Reported last frame? Drives Phase A lift-off detection.
    // Owned by Interrupt.c Phase C (sole L3 contract exception).
    BOOLEAN ReportedLastFrame;

} TRACK, *PTRACK;

// Zero/DEAD-initialise every track. Call at device creation and D0Entry.
VOID
AmtTrackPoolInit(_Out_writes_(PTP_MAX_CONTACT_POINTS) PTRACK Tracks);

// TRACK_DEAD -> TRACK_ACTIVE. Assigns fresh ContactID, seeds baseline.
// Precondition: State == TRACK_DEAD (debug-asserted).
VOID
AmtTrackBirth(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  x,
    _In_    USHORT  y
);

// Like AmtTrackBirth but seeds baseline to RecentLiftX/Y and leaves
// PendingFirstSample=FALSE, so first sample is EMA-blended against
// the lift position. No ContactID reuse (see TRACK_RETAP_POLICY).
VOID
AmtTrackBirthWithRetapSmoothing(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  RecentLiftX,
    _In_    USHORT  RecentLiftY
);

// Heuristic: is touch-down near a recent lift in time AND space?
// Time: within RETAP_WINDOW_100NS. Space: within RETAP_MAX_DISTANCE.
// Conservative — FALSE falls back to raw unsmoothed birth (always correct).
// Converts window to QPC ticks via PerfFrequencyHz (not assuming 10MHz).
#define RETAP_WINDOW_100NS      (700LL * 10000LL)  // 700 ms
#define RETAP_MAX_DISTANCE      600                // normalized units

BOOLEAN
AmtTrackIsRecentLiftNearby(
    _In_ LONGLONG LiftQpc,
    _In_ USHORT   LiftX,
    _In_ USHORT   LiftY,
    _In_ LONGLONG NowQpc,
    _In_ LONGLONG PerfFrequencyHz,
    _In_ USHORT   CandX,
    _In_ USHORT   CandY
);

// ACTIVE/GRACE -> DEAD + immediate re-birth. Single-call primitive for
// "replace identity in place". Returns old ContactID/X/Y for lift-off
// report. Currently unused by Interrupt.c (see audit note in Track.c).
VOID
AmtTrackRecycle(
    _Inout_  PTRACK  Tracks,
    _In_     size_t  index,
    _Inout_  ULONG*  NextContactId,
    _In_     USHORT  newX,
    _In_     USHORT  newY,
    _Out_    ULONG*  OldContactID,
    _Out_    USHORT* OldX,
    _Out_    USHORT* OldY
);

// ACTIVE/GRACE -> DEAD. No rebirth. Returns old ContactID/X/Y.
VOID
AmtTrackKill(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
);

// ACTIVE -> GRACE. Used when WasInGesture is TRUE at lift-off.
// Same-frame quarantine marker only — never re-binds ContactID.
VOID
AmtTrackEnterGrace(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _In_    LONGLONG NowQpc,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
);

// GRACE -> DEAD. Called same frame after AmtTrackEnterGrace.
// No report — lift-off already emitted by caller.
VOID
AmtTrackExpireGrace(_Inout_ PTRACK Tracks, _In_ size_t index);

// 2-pass deadzone evaluator. Pass 1 (Evaluate): read-only check against
// current HystX/Y. Pass 2 (Commit): write HystX/Y first, then EMA blends
// OLD ReportX/Y toward new candidate. Fixes ordering ambiguity (task #9).
BOOLEAN
AmtTrackEvaluateDeadzone(
    _In_ const TRACK* Track,
    _In_ USHORT candX,
    _In_ USHORT candY
);

// Per-frame ACTIVE track update (Phase C). Deadzone + EMA (skipped on
// PendingFirstSample and first solo post-gesture). Writes ReportX/Y.
// Only Phase C mutation entry point (L3 contract).
VOID
AmtTrackUpdate(
    _Inout_ PTRACK  Track,
    _In_    USHORT  rawX,
    _In_    USHORT  rawY,
    _In_    BOOLEAN aliveCountIsOne,
    _Out_   USHORT* OutX,
    _Out_   USHORT* OutY
);

#if DBG
// Debug-only pool invariant check: unique ContactIDs, no zero IDs on
// non-DEAD tracks, no flags set on DEAD tracks. Called at phase boundaries.
VOID
AmtTrackCheckInvariants(_In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK* Tracks);
#else
#define AmtTrackCheckInvariants(Tracks) ((VOID)0)
#endif

EXTERN_C_END