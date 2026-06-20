// Track.h - Unified per-contact lifecycle (Track FSM).
//
// Replaces the old scheme of N parallel arrays
// (SmoothedX/SmoothedY/HystX/HystY/SlotActive/TipDropCount/
//  SlotReportedLastFrame/SlotWasInGesture/ContactIdForSlot) indexed by
// raw hardware slot index, with one struct per logical track, indexed by
// the SAME raw slot index but carrying an explicit state machine.
//
// ---------------------------------------------------------------------
// Why this exists
// ---------------------------------------------------------------------
// The previous design conflated three different things under a single
// raw array index `i`:
//   1. The hardware's RAW reporting slot for this frame (0..raw_n-1).
//   2. A piece of "is there a finger here" state (SlotActive[i]).
//   3. A Windows-facing identity (ContactIdForSlot[i]).
//
// Raw slot index is NOT a stable finger identity across frames (the
// firmware compacts its array when a sibling finger lifts — see the
// `origin == 0` handling that used to live directly in Interrupt.c).
// Folding birth/recycle/update logic into a single linear pass over the
// raw array, with implicit state spread across 8 arrays, made it very
// easy to introduce 1-frame ordering bugs: a slot could be re-used
// (recycled with a new ContactID) and then immediately re-bound to a
// stale baseline in the SAME frame, or a lift-off could be emitted AFTER
// a same-index touch-down had already mutated the shared arrays for that
// index, corrupting the lift-off report.
//
// The TRACK FSM below makes every transition explicit and confines all
// per-contact state to one struct so a single track's lifecycle can be
// reasoned about (and asserted) independently of every other track.
//
// ---------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------
//
//   DEAD --(birth)--> ACTIVE --(lift, taint==FALSE)--> DEAD
//                        |
//                        |--(lift, taint==TRUE)--> GRACE --(timeout)--> DEAD
//                                                      |
//                                                      `--(retap match)--> ACTIVE (continuation)
//
// DEAD   - slot is free. No ContactID assigned. Eligible for birth.
// ACTIVE - currently down. Has a live ContactID, smoothing baseline,
//          hysteresis baseline, and tip-drop/gesture bookkeeping.
// GRACE  - just lifted off from a multi-finger gesture. Retains its old
//          ContactID OUT of the matching pool (never reused) but retains
//          its last known position + a short deadline so that an
//          immediate re-tap in roughly the same place can be classified
//          as deliberate gesture-continuation input by the caller if it
//          wants that (current policy: GRACE never re-binds — see
//          TRACK_RETAP_POLICY note in Track.c. The state still exists
//          because (a) it is the principled place to encode that policy
//          decision rather than an ad-hoc bool, and (b) it gives the
//          frame-determinism invariant checker a state to assert against
//          instead of inferring "was this a gesture" from a side array).
//
// A track's ContactID is assigned exactly once per ACTIVE lifetime, at
// birth (TrackBirth) or recycle-into-new-identity (TrackRecycle), and is
// NEVER reused while still possibly "warm" in Windows' internal contact
// tracking. See the NextContactId monotonic counter in DEVICE_CONTEXT.
//
// ---------------------------------------------------------------------
// Frame determinism rule
// ---------------------------------------------------------------------
// Once AmtMatchFrame() has produced a binding (raw index -> track)
// for a given frame, NO function may further mutate ANY track's
// state/identity for that same raw index during the same frame except
// through the ordered phase sequence:
//
//     Phase A (lift, no exceptions) -> Phase B (recycle/birth) -> Phase C (update/report)
//
// In particular a track must never be killed/recycled and then
// re-touched by Phase B/C logic that still believes it is looking at the
// pre-recycle baseline. AmtTrackKill()/AmtTrackRecycle() always fully
// reset baseline fields as part of the transition specifically so a
// caller cannot accidentally read stale smoothing state after a
// transition - if you need the pre-transition X/Y for a lift-off report,
// read it BEFORE calling AmtTrackKill()/AmtTrackRecycle(), never after.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

typedef enum _TRACK_STATE
{
    TRACK_DEAD = 0,   // free slot, no identity
    TRACK_ACTIVE,     // finger down, identity live
    TRACK_GRACE,      // just lifted from a gesture; quarantined, not matchable
} TRACK_STATE;

// One track per raw hardware slot index (0..PTP_MAX_CONTACT_POINTS-1).
// All fields here are owned EXCLUSIVELY by Track.c's lifecycle functions.
// Interrupt.c reads them but must route every mutation through
// AmtTrackBirth / AmtTrackRecycle / AmtTrackKill / AmtTrackUpdate.
typedef struct _TRACK
{
    TRACK_STATE State;

    // Windows-facing identity. Valid only while State != TRACK_DEAD.
    // Assigned from DEVICE_CONTEXT.NextContactId; never reused while
    // "warm" (see Track.c AmtTrackAssignContactId comment).
    ULONG ContactID;

    // Reported (post deadzone + EMA) position, in normalized device units.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline (Track.c AmtTrackApplyDeadzone uses
    // and updates this; distinct from ReportX/Y, which is post-EMA).
    USHORT HystX;
    USHORT HystY;

    // Tip-size debounce: consecutive frames a borderline-small contact
    // has been kept alive on its last good position.
    UCHAR TipDropCount;

    // TRUE if this track was part of a >=2-finger frame at any point
    // during its current ACTIVE lifetime and has not yet had its first
    // post-gesture single-finger report. Distinct from the SESSION-level
    // gesture flag in DEVICE_CONTEXT (see GestureSessionActive there) —
    // this is per-track, persists exactly across one ACTIVE lifetime,
    // and answers "should THIS track skip EMA blending on its next solo
    // update", not "is a gesture happening right now across the pad".
    BOOLEAN WasInGesture;

    // TRUE for the single frame immediately following a state
    // transition into ACTIVE (birth or recycle) that has not yet had its
    // first AmtTrackUpdate() call. Lets AmtTrackUpdate() distinguish
    // "first sample of a brand new baseline" from "Nth sample of an
    // established baseline" without re-deriving it from ReportX==0 (0 is
    // a valid coordinate) or any other implicit signal.
    BOOLEAN PendingFirstSample;

    // Reported to the HID stack last frame? Drives Phase A lift-off
    // detection. Distinct from State==TRACK_ACTIVE: a track can be
    // ACTIVE but not yet have produced a report this session (impossible
    // today since birth and first report are the same frame, but kept
    // explicit rather than collapsed, since collapsing this into State
    // was exactly the kind of implicit-coupling this rewrite removes).
    BOOLEAN ReportedLastFrame;

} TRACK, *PTRACK;

// AmtTrackPoolInit - zero/DEAD-initialise every track in the pool.
// Call once at device creation and again at D0Entry (see Device.c) so a
// resume-from-sleep cannot inherit ContactIDs Windows might still
// consider warm - NextContactId is reseeded by the caller, this function
// only resets the TRACK structs themselves.
VOID
AmtTrackPoolInit(_Out_writes_(PTP_MAX_CONTACT_POINTS) PTRACK Tracks);

// AmtTrackBirth - DEAD -> ACTIVE. Assigns a fresh ContactID, initialises
// baseline to (x, y), clears all per-lifetime bookkeeping.
// Precondition: Tracks[index].State == TRACK_DEAD (debug-asserted).
VOID
AmtTrackBirth(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  x,
    _In_    USHORT  y
);

// AmtTrackRecycle - ACTIVE or GRACE -> DEAD, immediately followed by a
// fresh AmtTrackBirth at the same index. Exists as a single call (rather
// than "kill then birth" at the use site) so the never-reuse-a-warm-ID
// invariant and the full-state-reset invariant are enforced in one place.
// Returns the OLD ContactID/X/Y that were live before the recycle, via
// out-parameters, for the caller to use in a lift-off report - this is
// the ONLY supported way to read pre-transition state; do not read
// Tracks[index] fields after this call expecting old values.
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

// AmtTrackKill - ACTIVE or GRACE -> DEAD. No rebirth. Returns the OLD
// ContactID/X/Y for the lift-off report, same contract as
// AmtTrackRecycle regarding stale-read prevention.
VOID
AmtTrackKill(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
);

// AmtTrackEnterGrace - ACTIVE -> GRACE. Used instead of AmtTrackKill when
// SessionGestureActive was TRUE for this track's lifetime, so a
// near-simultaneous re-tap is recognisably "just came out of a gesture"
// for diagnostics/invariant-checking, even though current policy (see
// Track.c) treats GRACE as non-matchable and lets it expire to DEAD like
// any other quarantine. Returns old ContactID/X/Y like AmtTrackKill.
VOID
AmtTrackEnterGrace(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _In_    LONGLONG NowQpc,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
);

// AmtTrackExpireGrace - GRACE -> DEAD once a track's grace deadline has
// passed. Caller (Interrupt.c) supplies NowQpc and the per-context grace
// deadline array; pure bookkeeping, no report is emitted for this
// transition (the lift-off report was already emitted by
// AmtTrackEnterGrace's caller at the moment of the gesture-lift).
VOID
AmtTrackExpireGrace(_Inout_ PTRACK Tracks, _In_ size_t index);

// AmtTrackApplyDeadzone2Pass - 2-pass deadzone evaluator.
//
// FIX (EMA <-> deadzone ordering conflict): the previous single-pass
// design called AmtApplyDeadzone (which MUTATES the hysteresis baseline
// as a side effect) and immediately fed its result into the EMA
// smoother, in the same statement evaluation. Whether the deadzone's
// baseline update was "seen" by that frame's smoothing input depended on
// argument evaluation order around the mutation, which is not load-bearing
// behaviour we want to depend on. Splitting into two explicit passes
// removes the ambiguity:
//   Pass 1 (Evaluate): compute whether each candidate sample is outside
//     its track's deadzone, WITHOUT mutating anything.
//   Pass 2 (Apply): for samples that passed the deadzone test, commit the
//     new hysteresis baseline AND feed the EMA smoother. Samples that
//     failed the deadzone test get the existing baseline value and do
//     NOT advance the EMA filter at all (since there is, by definition,
//     no new information).
// Pass 1 is AmtTrackEvaluateDeadzone (below); pass 2 is performed inside
// AmtTrackUpdate, which calls it and then conditionally calls
// AmtTrackCommitSample.
BOOLEAN
AmtTrackEvaluateDeadzone(
    _In_ const TRACK* Track,
    _In_ USHORT candX,
    _In_ USHORT candY
);

// AmtTrackUpdate - ACTIVE track regular per-frame update (Phase C).
// Performs: deadzone evaluate+apply (2-pass, see above), EMA smoothing
// (skipped on PendingFirstSample and on the first solo frame after a
// gesture - WasInGesture - to avoid blending against a stale baseline),
// tip-drop bookkeeping is the caller's responsibility (it depends on raw
// finger geometry Track.c does not see). Writes the reportable X/Y back
// into Track->ReportX/Y and returns them via out-parameters for
// convenience at the call site.
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
// AmtTrackCheckInvariants - debug-only assertion pass over the whole
// pool. Checks (see also the per-function comments above for the
// individual rules):
//   - No two DEAD-distinct tracks share a ContactID.
//   - No ACTIVE/GRACE track has ContactID == 0 (0 is reserved/unassigned).
//   - PendingFirstSample is never TRUE on a DEAD track.
//   - WasInGesture is never TRUE on a DEAD track.
// Called from Interrupt.c at well-defined phase boundaries in debug
// builds only; compiles to nothing in release.
VOID
AmtTrackCheckInvariants(_In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK* Tracks);
#else
#define AmtTrackCheckInvariants(Tracks) ((VOID)0)
#endif

EXTERN_C_END