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
//   TRACK_DEAD --(birth)--> TRACK_ACTIVE --(lift, untainted)--> TRACK_DEAD
//                                |
//                                |--(lift, gesture-tainted)--> TRACK_GRACE --(expire, same frame)--> TRACK_DEAD
//
// TRACK_DEAD          - slot is free. No ContactID assigned. Eligible
//                        for birth.
// TRACK_ACTIVE         - currently down. Has a live ContactID, smoothing
//                        baseline, hysteresis baseline, and tip-drop/
//                        gesture bookkeeping.
// TRACK_GRACE          - transitional quarantine state entered by a
//                        track whose ACTIVE lifetime included a
//                        multi-finger gesture (WasInGesture==TRUE) at
//                        the moment of lift-off. See
//                        TRACK_RETAP_POLICY below for what this state
//                        does and — just as importantly — does NOT do.
//
// A track's ContactID is assigned exactly once per ACTIVE lifetime, at
// birth (AmtTrackBirth) or recycle-into-new-identity (AmtTrackRecycle),
// and is NEVER reused while still possibly "warm" in Windows' internal
// contact tracking. See the NextContactId monotonic counter in
// DEVICE_CONTEXT. This is a hard, non-negotiable invariant — see
// TRACK_RETAP_POLICY immediately below for why, and what we do instead
// to get a clean, non-jumpy re-tap without violating it.
//
// ---------------------------------------------------------------------
// TRACK_RETAP_POLICY — why GRACE never re-binds a ContactID, and how a
// fast re-tap after a gesture is made to feel seamless WITHOUT reuse
// ---------------------------------------------------------------------
// A literal "retap continuation" — handing a NEW touch-down the SAME
// ContactID an already-lifted (TipSwitch=0) contact used — was
// considered and rejected. Windows' PTP digitizer stack tracks contacts
// BY ContactID; if a lifted ID reappears with TipSwitch=1 at a different
// position, Windows has no way to distinguish "this is a deliberate new
// press that happens to reuse a slot" from "the previous contact moved
// and we missed an intermediate report" — and PTP's own semantics say
// the latter. The visible result is exactly the cursor-teleport/jump
// class of bug this driver had been chasing: the pointer animates
// (snaps) from the old lift position toward the new touch-down instead
// of treating it as a fresh, independent press. So: ContactID reuse
// after a reported lift-off is OFF THE TABLE, permanently, regardless of
// how soon the re-tap happens.
//
// TRACK_GRACE exists for a DIFFERENT, narrower purpose: giving the
// frame-determinism invariant checker (AmtTrackCheckInvariants) and
// future diagnostics an explicit transitional state to assert against
// — "this lift-off happened at the end of a gesture" — instead of
// inferring it from a side boolean after the fact. Current policy is
// that GRACE is entered and immediately expired to TRACK_DEAD within
// the SAME frame (see Phase A in Interrupt.c): it never persists across
// a frame boundary, is never matched against by AmtMatchFrame, and a
// re-tap on that raw slot always gets a brand-new ContactID via
// AmtTrackBirth, same as any other fresh touch-down. There is no
// "held reservation" anywhere in this codebase.
//
// What actually fixes the visible jump on a fast post-gesture re-tap
// (the real bug behind task #2) is NOT ContactID reuse — it is making
// sure the FIRST REPORTED SAMPLE of the brand-new track is never
// smoothed against anything. AmtTrackBirth seeds ReportX/Y and HystX/Y
// directly to the new touch-down position and sets PendingFirstSample,
// which makes AmtTrackUpdate bypass the deadzone test and EMA blending
// entirely on that first sample (see AmtTrackCommitSample in Track.c).
// A brand-new ContactID reported at a brand-new position, with no
// smoothing pulling it toward anything old, is indistinguishable to
// Windows from any other ordinary finger-down — there is nothing for it
// to "correct" toward, because there is no continuity claim being made.
// The remaining historical jump source was a SEPARATE bug — a still-
// active SIBLING finger's first solo sample after the gesture partner
// lifted being mis-smoothed — fixed in AmtTrackCommitSample, see the
// FIX comment there and in Interrupt.c's Phase C.
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
//
// ---------------------------------------------------------------------
// L3 read-only contract (task #4)
// ---------------------------------------------------------------------
// AmtTrackUpdate (and everything it calls: AmtTrackEvaluateDeadzone,
// AmtTrackCommitSample) is the ONLY place TRACK state may be mutated
// during Phase C. The report-building code in Interrupt.c's Phase C
// loop (what task #4 calls "L3" / AmtPtpBuildReport-equivalent) reads
// the OutX/OutY values handed back by AmtTrackUpdate and
// Track->ContactID — it must never itself write SmoothedX/Y, HystX/Y,
// WasInGesture, or ReportedLastFrame (ReportedLastFrame is the sole,
// intentional exception: it is bookkeeping ABOUT the report having been
// produced, not logic state the next frame's L2 decisions depend on, and
// is documented as caller-owned in its field comment below). If a future
// change needs the report layer to influence tracking logic, that
// influence must flow IN to AmtTrackUpdate as a parameter (the way
// aliveCountIsOne already does), never as a back-channel mutation from
// the render/report step.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

typedef enum _TRACK_STATE
{
    TRACK_DEAD = 0,   // free slot, no identity
    TRACK_ACTIVE,     // finger down, identity live
    TRACK_GRACE,      // post-gesture lift-off quarantine marker; expires
                      // to TRACK_DEAD within the same frame it is
                      // entered (see TRACK_RETAP_POLICY above) — never
                      // matched against, never re-binds a ContactID.
} TRACK_STATE;

// One track per raw hardware slot index (0..PTP_MAX_CONTACT_POINTS-1).
// All fields here are owned EXCLUSIVELY by Track.c's lifecycle functions.
// Interrupt.c reads them but must route every mutation through
// AmtTrackBirth / AmtTrackRecycle / AmtTrackKill / AmtTrackUpdate (see
// the L3 read-only contract above for the one documented exception).
typedef struct _TRACK
{
    TRACK_STATE State;

    // Windows-facing identity. Valid only while State != TRACK_DEAD.
    // Assigned from DEVICE_CONTEXT.NextContactId; never reused while
    // "warm" (see Track.c AmtTrackAssignContactId comment, and
    // TRACK_RETAP_POLICY above).
    ULONG ContactID;

    // Reported (post deadzone + EMA) position, in normalized device units.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline (Track.c AmtTrackEvaluateDeadzone
    // reads this; AmtTrackCommitSample updates it — see
    // AmtTrackApplyDeadzone2Pass below for why these are split into
    // separate passes). Distinct from ReportX/Y, which is post-EMA.
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
    //
    // FIX (task #2 jump root cause): this flag must be consumed
    // (cleared) the FIRST time aliveCountIsOne==TRUE is actually
    // observed for this track, INCLUDING the very same frame a sibling
    // finger's lift-off drops the count from >=2 to 1 — not just on a
    // later frame once the deadzone happens to pass. See
    // AmtTrackCommitSample in Track.c for the consume-on-spend fix and
    // Interrupt.c Phase C for how aliveCountIsOne is computed from
    // THIS frame's post-Phase-A alive count, not a stale value.
    BOOLEAN WasInGesture;

    // TRUE for the single frame immediately following a state
    // transition into ACTIVE (birth or recycle) that has not yet had its
    // first AmtTrackUpdate() call. Lets AmtTrackUpdate() distinguish
    // "first sample of a brand new baseline" from "Nth sample of an
    // established baseline" without re-deriving it from ReportX==0 (0 is
    // a valid coordinate) or any other implicit signal. This is also the
    // mechanism that makes a post-gesture re-tap (TRACK_RETAP_POLICY
    // above) land cleanly: a brand-new track's first sample is never
    // smoothed, regardless of where the previous occupant of this slot
    // last reported.
    BOOLEAN PendingFirstSample;

    // Reported to the HID stack last frame? Drives Phase A lift-off
    // detection. Distinct from State==TRACK_ACTIVE: a track can be
    // ACTIVE but not yet have produced a report this session (impossible
    // today since birth and first report are the same frame, but kept
    // explicit rather than collapsed, since collapsing this into State
    // was exactly the kind of implicit-coupling this rewrite removes).
    // Owned by Interrupt.c's Phase C (the sole documented exception to
    // the L3 read-only contract above — see that section for why this
    // one field is different from WasInGesture/Hyst*/Smoothed*).
    BOOLEAN ReportedLastFrame;

} TRACK, *PTRACK;

// AmtTrackPoolInit - zero/DEAD-initialise every track in the pool.
// Call once at device creation and again at D0Entry (see Device.c) so a
// resume-from-sleep cannot inherit ContactIDs Windows might still
// consider warm - NextContactId is reseeded by the caller, this function
// only resets the TRACK structs themselves.
VOID
AmtTrackPoolInit(_Out_writes_(PTP_MAX_CONTACT_POINTS) PTRACK Tracks);

// AmtTrackBirth - TRACK_DEAD -> TRACK_ACTIVE. Assigns a fresh ContactID,
// initialises baseline to (x, y), clears all per-lifetime bookkeeping.
// This is the ONLY function that may write Track->ContactID for an
// otherwise-DEAD slot — see TRACK_RETAP_POLICY above for why a re-tap
// always lands here with a brand-new identity rather than ever reusing
// one.
// Precondition: Tracks[index].State == TRACK_DEAD (debug-asserted).
VOID
AmtTrackBirth(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  x,
    _In_    USHORT  y
);

// AmtTrackBirthWithRetapSmoothing - identical to AmtTrackBirth (still
// assigns a brand-new, never-before-used ContactID — see
// TRACK_RETAP_POLICY; this does NOT relax that invariant in any way),
// except the very first reported sample is smoothed against a caller-
// supplied "recent lift" position instead of being reported raw.
//
// FIX (task #2 — raw-snap-on-fast-retap, the actual remaining jump
// source): AmtTrackBirth's PendingFirstSample bypass is correct for an
// ordinary fresh touch-down (no prior position exists to blend toward,
// so there is nothing to smooth against). But for a deliberate fast
// re-tap in roughly the same spot shortly after a lift — exactly the
// "відвести курсор трохи, tap за ~0.6 sec" scenario in task #2 — the
// raw first sample can carry sensor settle noise (capacitive ramp-up on
// initial contact), which Windows' PTP stack renders as a visible
// cursor snap because nothing is smoothing the new contact's reported
// position. The fix is NOT to reuse the old ContactID (see
// TRACK_RETAP_POLICY) — it is to give the brand-new track's first
// sample a smoothing anchor when, and only when, the caller has
// determined (via AmtTrackIsRecentLiftNearby below) that this looks
// like a continuation gesture in the human sense, not the protocol
// sense. RecentLiftX/Y is what that anchor blends against, exactly once,
// on the first sample only — PendingFirstSample is still cleared
// afterward and every subsequent sample goes through the normal
// deadzone+EMA path against the now-established ReportX/Y.
VOID
AmtTrackBirthWithRetapSmoothing(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  x,
    _In_    USHORT  y,
    _In_    USHORT  RecentLiftX,
    _In_    USHORT  RecentLiftY
);

// AmtTrackIsRecentLiftNearby - decides whether a fresh touch-down at
// (x, y) on a given raw slot looks like a fast re-tap of the same
// physical finger that recently lifted from that slot, purely as a
// HEURISTIC for which smoothing anchor AmtTrackBirthWithRetapSmoothing
// should use. This has zero effect on ContactID assignment — it never
// has, and per TRACK_RETAP_POLICY in Track.h never will. Two
// independent conditions must both hold:
//   - time: NowQpc - LiftQpc must be within RETAP_WINDOW_100NS, and
//   - space: (x, y) must be within RETAP_MAX_DISTANCE of (LiftX, LiftY).
// Deliberately conservative on both axes: a FALSE here just means the
// ordinary (raw, unsmoothed) first-sample path runs, which was always
// correct for a genuinely new touch-down — this function only ever adds
// smoothing, it never removes correctness.
//
// FIX (units bug): LiftQpc/NowQpc are raw KeQueryPerformanceCounter
// ticks, NOT 100ns units — QueryPerformanceFrequency is documented as
// 10MHz on Windows 7+ in practice, but is not architecturally
// guaranteed to be exactly that on every platform/VM/firmware
// combination, and nothing else in this codebase assumes it without
// going through PerfFrequency (see the ticksPer100ns conversion in
// AmtPtpKeyboardNotifyCallback, Device.c, for the existing correct
// pattern this function now mirrors). PerfFrequencyHz must be the
// SAME DEVICE_CONTEXT.PerfFrequency.QuadPart value the caller already
// has cached from D0Entry — this function converts RETAP_WINDOW_100NS
// into QPC ticks internally rather than comparing raw tick counts
// against a 100ns-unit constant.
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

// AmtTrackRecycle - ACTIVE or GRACE -> DEAD, immediately followed by a
// fresh AmtTrackBirth at the same index. Exists as a single call (rather
// than "kill then birth" at the use site) so the never-reuse-a-warm-ID
// invariant and the full-state-reset invariant are enforced in one place.
// Returns the OLD ContactID/X/Y that were live before the recycle, via
// out-parameters, for the caller to use in a lift-off report - this is
// the ONLY supported way to read pre-transition state; do not read
// Tracks[index] fields after this call expecting old values.
// NOTE: not currently called anywhere in Interrupt.c (Phase A always
// goes through AmtTrackKill or AmtTrackEnterGrace+AmtTrackExpireGrace,
// and Phase B always goes through AmtTrackBirth on an already-DEAD
// slot — see the audit note in Track.c). Kept because it is the
// principled single-call primitive for "replace this track's identity
// in place" if a future caller needs it; it is NOT a hidden or
// alternate ContactID-reuse path — it still assigns a brand-new ID via
// AmtTrackBirth internally, same as every other path.
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
// this track's WasInGesture was TRUE at the moment of lift-off, so a
// near-simultaneous re-tap is recognisably "just came out of a gesture"
// for diagnostics/invariant-checking. Returns old ContactID/X/Y like
// AmtTrackKill. See TRACK_RETAP_POLICY above: GRACE is a same-frame
// transitional marker only, not a held reservation, and never causes a
// later touch-down to reuse this ContactID.
VOID
AmtTrackEnterGrace(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _In_    LONGLONG NowQpc,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
);

// AmtTrackExpireGrace - GRACE -> DEAD. Called in the same frame,
// immediately after AmtTrackEnterGrace, by Interrupt.c's Phase A (see
// TRACK_RETAP_POLICY above for why GRACE is not allowed to persist past
// the frame it was entered in). No report is emitted here — the
// lift-off report for this track was already produced by
// AmtTrackEnterGrace's caller, using the Old* out-parameters from that
// call.
VOID
AmtTrackExpireGrace(_Inout_ PTRACK Tracks, _In_ size_t index);

// AmtTrackApplyDeadzone2Pass - 2-pass deadzone evaluator.
//
// FIX (EMA <-> deadzone ordering conflict, task #9): the previous
// single-pass design called AmtApplyDeadzone (which MUTATES the
// hysteresis baseline as a side effect) and immediately fed its result
// into the EMA smoother, in the same statement evaluation. Whether the
// deadzone's baseline update was "seen" by that frame's smoothing input
// depended on argument evaluation order around the mutation, which is
// not load-bearing behaviour we want to depend on. Splitting into two
// explicit passes removes the ambiguity, and FIXES the resulting drift
// task #9 asked about:
//
//   Pass 1 (Evaluate, AmtTrackEvaluateDeadzone): compute whether the
//     candidate sample is outside its track's CURRENT (not-yet-updated)
//     deadzone baseline, WITHOUT mutating anything.
//   Pass 2 (Commit, AmtTrackCommitSample): for a sample that passed the
//     deadzone test, commit the new hysteresis baseline FIRST, THEN feed
//     the EMA smoother (against the track's still-OLD ReportX/Y, which
//     is exactly the EMA's job — blend old-report toward new-candidate).
//     A sample that failed the deadzone test gets the existing baseline
//     value and does NOT advance the EMA filter at all (no new
//     information).
//
// This fixes (not just documents) the drift task #9 flagged: of the
// three candidate orderings considered —
//   (a) deadzone -> ema             [previous, kept]
//   (b) ema -> deadzone
//   (c) disable ema while gesture-locked
// — (a) is correct PROVIDED the deadzone's baseline write and the EMA's
// input read happen in a fixed, well-defined order (baseline commits
// BEFORE the EMA reads the new candidate, but the EMA still blends
// against the OLD ReportX/Y, not the new HystX/Y). (b) is wrong: running
// EMA first means the deadzone would then be comparing a blended,
// already-smoothed value against the raw baseline, which is comparing
// two different coordinate spaces. (c) is unnecessary now that the
// gesture-tainted first-solo-sample case is handled by the dedicated
// skipEma branch in AmtTrackCommitSample — there is no remaining case
// where EMA needs to be globally disabled. The two-pass split below
// implements (a) with the ordering pinned down explicitly rather than
// left to evaluation order.
BOOLEAN
AmtTrackEvaluateDeadzone(
    _In_ const TRACK* Track,
    _In_ USHORT candX,
    _In_ USHORT candY
);

// AmtTrackUpdate - ACTIVE track regular per-frame update (Phase C).
// Performs: deadzone evaluate+commit (2-pass, see above), EMA smoothing
// (skipped on PendingFirstSample and on the first solo frame after a
// gesture - WasInGesture - to avoid blending against a stale baseline),
// tip-drop bookkeeping is the caller's responsibility (it depends on raw
// finger geometry Track.c does not see). Writes the reportable X/Y back
// into Track->ReportX/Y and returns them via out-parameters for
// convenience at the call site. This is the ONLY entry point through
// which Phase C may mutate a TRACK — see the L3 read-only contract
// above.
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
//   - No two non-DEAD tracks share a ContactID.
//   - No ACTIVE/GRACE track has ContactID == 0 (0 is reserved/unassigned).
//   - PendingFirstSample is never TRUE on a DEAD track.
//   - WasInGesture is never TRUE on a DEAD track.
//   - No track is ever observed in TRACK_GRACE by code outside Track.c/
//     Interrupt.c's Phase A (GRACE must not survive past the frame it
//     was entered in — see TRACK_RETAP_POLICY above). This file does not
//     assert that directly (it has no cross-frame memory), but
//     Interrupt.c calling AmtTrackExpireGrace unconditionally,
//     synchronously, right after AmtTrackEnterGrace in the same Phase A
//     iteration is what upholds it; see the audit note in Track.c.
// Called from Interrupt.c at well-defined phase boundaries in debug
// builds only; compiles to nothing in release.
VOID
AmtTrackCheckInvariants(_In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK* Tracks);
#else
#define AmtTrackCheckInvariants(Tracks) ((VOID)0)
#endif

EXTERN_C_END