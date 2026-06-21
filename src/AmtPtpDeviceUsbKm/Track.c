// Track.c - Track FSM implementation. See Track.h for the full design
// rationale (state machine, frame-determinism rule, why ContactID is
// monotonic and never reused while warm — TRACK_RETAP_POLICY).

#include "Driver.h"
#include "Track.h"

// ---------------------------------------------------------------------
// Smoothing/deadzone tunables — moved here from the old Interrupt.c
// monolith, since smoothing is now exclusively a Track-owned concern
// (Interrupt.c no longer touches HystX/Y, ReportX/Y, or any baseline
// field directly — only through AmtTrackUpdate).
// ---------------------------------------------------------------------
#define XY_DEADZONE_UNITS    2
#define SMOOTHING_ALPHA_NUM  5
#define SMOOTHING_ALPHA_DEN  8

static inline USHORT
AmtTrackSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal)
{
    INT blended = ((INT)rawVal * SMOOTHING_ALPHA_NUM +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

// AmtTrackAssignContactId - hands the caller a brand-new ContactID that
// has never been used before by ANY track for the lifetime of the
// current device session (NextContactId is reseeded to 0 at every
// D0Entry — see AmtPtpEvtDeviceD0Entry in Device.c). Pre-increment means
// 0 is permanently reserved/unassigned, which is exactly what
// AmtTrackCheckInvariants asserts on for every non-DEAD track below.
//
// This is the single choke point through which every ACTIVE track gets
// its identity, called from exactly two places (AmtTrackBirth, and
// AmtTrackRecycle via its internal AmtTrackBirth call) — there is no
// other path in the codebase that may write Track->ContactID. See
// TRACK_RETAP_POLICY in Track.h: every re-tap, however fast, comes
// through here and gets a number that has never been seen before, full
// stop — there is no conditional reuse path anywhere below.
static inline ULONG
AmtTrackAssignContactId(_Inout_ ULONG* NextContactId)
{
    return ++(*NextContactId);
}

VOID
AmtTrackPoolInit(_Out_writes_(PTP_MAX_CONTACT_POINTS) PTRACK Tracks)
{
    // Zeroing is sufficient: TRACK_DEAD == 0, ContactID == 0 (reserved/
    // unassigned), and every BOOLEAN flag defaults to FALSE — exactly
    // the state AmtTrackCheckInvariants expects for a DEAD track.
    RtlZeroMemory(Tracks, sizeof(TRACK) * PTP_MAX_CONTACT_POINTS);
}

VOID
AmtTrackBirth(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  x,
    _In_    USHORT  y
)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_DEAD);
#endif

    t->State             = TRACK_ACTIVE;
    t->ContactID          = AmtTrackAssignContactId(NextContactId);
    t->ReportX            = x;
    t->ReportY            = y;
    t->HystX              = x;
    t->HystY              = y;
    t->TipDropCount       = 0;
    t->WasInGesture       = FALSE;
    t->PendingFirstSample = TRUE;
    t->ReportedLastFrame  = FALSE;
}

// FIX (task #2 — raw-snap-on-fast-retap): see the long comment on this
// function's declaration in Track.h. This is AmtTrackBirth plus exactly
// one extra thing: the baseline ReportX/Y/HystX/HystY is seeded to the
// NEW touch-down position (same as AmtTrackBirth — there is still no
// continuity claim made to Windows, still a brand-new ContactID), but
// PendingFirstSample is left FALSE instead of TRUE, so the very next
// AmtTrackUpdate call runs the NORMAL deadzone+EMA path rather than the
// raw bypass — and that EMA blends against ReportX/Y, which we seed here
// to the RECENT LIFT position rather than the new touch-down position.
// The net effect: the first reported sample is a smoothed blend between
// "where the finger lifted" and "where it just landed", instead of a
// raw, unsmoothed jump straight to the landing position. No ContactID
// is reused, no Windows-facing continuity is claimed — purely a local
// rendering smoothness choice.
VOID
AmtTrackBirthWithRetapSmoothing(
    _Inout_ PTRACK Tracks,
    _In_    size_t  index,
    _Inout_ ULONG*  NextContactId,
    _In_    USHORT  x,
    _In_    USHORT  y,
    _In_    USHORT  RecentLiftX,
    _In_    USHORT  RecentLiftY
)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_DEAD);
#endif

    t->State             = TRACK_ACTIVE;
    t->ContactID          = AmtTrackAssignContactId(NextContactId);
    t->ReportX            = RecentLiftX;
    t->ReportY            = RecentLiftY;
    t->HystX              = RecentLiftX;
    t->HystY              = RecentLiftY;
    t->TipDropCount       = 0;
    t->WasInGesture       = FALSE;
    // Deliberately FALSE, not TRUE — see the function-level comment
    // above. We WANT AmtTrackUpdate's normal deadzone+EMA path to run on
    // the first real sample, blending from RecentLiftX/Y (just seeded
    // above) toward the actual new touch-down coordinates handed to
    // AmtTrackUpdate by the caller.
    t->PendingFirstSample = FALSE;
    t->ReportedLastFrame  = FALSE;
}

BOOLEAN
AmtTrackIsRecentLiftNearby(
    _In_ LONGLONG LiftQpc,
    _In_ USHORT   LiftX,
    _In_ USHORT   LiftY,
    _In_ LONGLONG NowQpc,
    _In_ LONGLONG PerfFrequencyHz,
    _In_ USHORT   CandX,
    _In_ USHORT   CandY
)
{
    // LiftQpc == 0 is the documented "no recent lift recorded" sentinel
    // (see DEVICE_CONTEXT.SlotLastLiftQpc in Device.h) — QPC values are
    // never legitimately 0 once the device has entered D0.
    if (LiftQpc == 0)
        return FALSE;

    if (NowQpc < LiftQpc)
        return FALSE; // defensive: QPC must be monotonic; never trust a
                       // negative delta rather than risk a huge unsigned
                       // wrap if LONGLONG arithmetic assumptions change.

    // FIX (units bug): LiftQpc/NowQpc are raw QPC ticks. Converting the
    // FIXED window (RETAP_WINDOW_100NS, expressed in 100ns units) into
    // QPC ticks via PerfFrequencyHz — rather than comparing the raw tick
    // delta directly against a 100ns-unit constant — is required because
    // QueryPerformanceFrequency is not architecturally guaranteed to be
    // exactly 10,000,000 Hz on every platform; comparing raw ticks
    // against a 100ns-unit constant silently assumed that and would
    // make this window wrong (by the ratio of actual-Hz to 10MHz) on any
    // system where it doesn't hold. Mirrors the existing, correct
    // ticksPer100ns pattern in AmtPtpKeyboardNotifyCallback (Device.c).
    if (PerfFrequencyHz <= 0)
        return FALSE; // no usable clock — fail closed to the always-
                       // correct raw/unsmoothed birth path.

    LONGLONG deltaTicks       = NowQpc - LiftQpc;
    LONGLONG windowTicks      = (RETAP_WINDOW_100NS * PerfFrequencyHz) / 10000000LL;

    if (deltaTicks > windowTicks)
        return FALSE;

    INT dx = (INT)CandX - (INT)LiftX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)CandY - (INT)LiftY;
    if (dy < 0) dy = -dy;

    return (dx <= RETAP_MAX_DISTANCE) && (dy <= RETAP_MAX_DISTANCE);
}

// AUDIT NOTE (task #1, ContactID lifecycle): AmtTrackRecycle is NOT
// currently called from anywhere in Interrupt.c. Every Phase A
// transition uses either AmtTrackKill (untainted lift) or
// AmtTrackEnterGrace+AmtTrackExpireGrace (gesture-tainted lift), and
// every Phase B transition is AmtTrackBirth/AmtTrackBirthWithRetapSmoothing
// on a slot Phase A has already guaranteed is TRACK_DEAD this frame.
// This function is kept as the principled single-call "replace in
// place" primitive (see its Track.h doc comment) but is presently dead
// code from Interrupt.c's point of view. It is NOT a hidden reuse path:
// like every other path, it always calls AmtTrackBirth internally,
// which always calls AmtTrackAssignContactId — there is no branch in
// this file that hands out an old ContactID under any condition.
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
)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_ACTIVE || t->State == TRACK_GRACE);
#endif

    // Capture pre-transition state BEFORE anything is reset — this is
    // the only supported channel for it (see frame-determinism comment
    // in Track.h: never read Tracks[index] fields after this call
    // expecting old values).
    *OldContactID = t->ContactID;
    *OldX         = t->ReportX;
    *OldY         = t->ReportY;

    // Full reset to DEAD, then immediately re-birth at the same index.
    // Doing this as one call (rather than "kill then birth" at the call
    // site) guarantees no intermediate state can be observed/mutated by
    // anything else within the same frame, and that the new track never
    // inherits a stale baseline from the one it replaced.
    RtlZeroMemory(t, sizeof(TRACK));
    AmtTrackBirth(Tracks, index, NextContactId, newX, newY);
}

VOID
AmtTrackKill(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_ACTIVE || t->State == TRACK_GRACE);
#endif

    *OldContactID = t->ContactID;
    *OldX         = t->ReportX;
    *OldY         = t->ReportY;

    // FIX (task #1 invariant: "ніколи не перевикористовувати slot якщо
    // ReportedLastFrame=TRUE" without going through a reported lift
    // first): RtlZeroMemory below resets ReportedLastFrame to FALSE as
    // part of the full-struct clear, same as every other field. The
    // slot is only handed back to Phase B (AmtTrackBirth, which asserts
    // State==TRACK_DEAD) AFTER this completes, and AmtEmitLift in
    // Interrupt.c is always called with this function's Old* outputs
    // BEFORE Phase B can run for the same slot in the same frame (see
    // the Phase A/B/C ordering in Interrupt.c) — so a slot that was
    // ReportedLastFrame==TRUE always gets its TipSwitch=0 lift-off
    // queued before it can ever be reborn. There is no path that skips
    // straight from "reported as down" to "reported as a different
    // contact" without an intervening TipSwitch=0 frame.
    RtlZeroMemory(t, sizeof(TRACK));   // -> TRACK_DEAD, ContactID cleared
}

VOID
AmtTrackEnterGrace(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _In_    LONGLONG NowQpc,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_ACTIVE);
#endif

    *OldContactID = t->ContactID;
    *OldX         = t->ReportX;
    *OldY         = t->ReportY;

    // Quarantine rather than fully clear — current policy (see
    // AmtTrackExpireGrace and TRACK_RETAP_POLICY in Track.h) never
    // actually re-binds a GRACE track to a later retap, so this state is
    // observable for exactly one instant before the caller (Interrupt.c)
    // immediately calls AmtTrackExpireGrace on it.
    //
    // See TRACK_RETAP_POLICY in Track.h for the full reasoning (task
    // #5/#6): a real ContactID-reuse continuation across a reported
    // lift-off is permanently off the table — it reopens the exact
    // cursor-teleport bug this driver was built to fix, because Windows
    // would interpret a reused ID at a new position as a correction of
    // the OLD contact rather than a fresh press. GRACE therefore stays a
    // pure, same-frame quarantine marker, not a held reservation. The
    // actual fix for a smooth-feeling fast re-tap is the
    // PendingFirstSample bypass in AmtTrackUpdate/AmtTrackCommitSample —
    // a brand-new track's first sample is never smoothed against
    // anything, new ID or not.
    t->State = TRACK_GRACE;
    UNREFERENCED_PARAMETER(NowQpc);
}


VOID
AmtTrackExpireGrace(_Inout_ PTRACK Tracks, _In_ size_t index)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_GRACE);
#endif

    // No report is emitted here — the lift-off report for this track
    // was already produced by AmtTrackEnterGrace's caller, using the
    // Old* out-parameters from that call.
    RtlZeroMemory(t, sizeof(TRACK));   // -> TRACK_DEAD
}

BOOLEAN
AmtTrackEvaluateDeadzone(
    _In_ const TRACK* Track,
    _In_ USHORT candX,
    _In_ USHORT candY
)
{
#if XY_DEADZONE_UNITS > 0
    INT dx = (INT)candX - (INT)Track->HystX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)candY - (INT)Track->HystY;
    if (dy < 0) dy = -dy;

    // "Outside the deadzone" means at least one axis moved far enough
    // to count as real motion rather than sensor jitter. This is PASS 1
    // of the 2-pass evaluator (task #9) — read-only, compares against
    // the CURRENT (not-yet-updated-this-frame) HystX/Y baseline. See the
    // long ordering-correctness comment on
    // AmtTrackApplyDeadzone2Pass in Track.h.
    return (dx >= XY_DEADZONE_UNITS) || (dy >= XY_DEADZONE_UNITS);
#else
    UNREFERENCED_PARAMETER(Track);
    UNREFERENCED_PARAMETER(candX);
    UNREFERENCED_PARAMETER(candY);
    return TRUE;
#endif
}

// AmtTrackCommitSample - pass 2 of the 2-pass deadzone/EMA evaluator
// (see the long comment on AmtTrackApplyDeadzone2Pass in Track.h).
// Caller has already decided (via AmtTrackEvaluateDeadzone, or via the
// PendingFirstSample short-circuit in AmtTrackUpdate) whether this
// sample carries new information. This function commits the hysteresis
// baseline and EMA state accordingly and writes back the reportable
// position.
static inline VOID
AmtTrackCommitSample(
    _Inout_ PTRACK  Track,
    _In_    USHORT  candX,
    _In_    USHORT  candY,
    _In_    BOOLEAN passedDeadzone,
    _In_    BOOLEAN aliveCountIsOne,
    _Out_   USHORT* OutX,
    _Out_   USHORT* OutY
)
{
    USHORT repX, repY;

    if (!passedDeadzone) {
        // No new information this frame: hold the last reported
        // position. Do NOT advance the EMA filter — there is nothing to
        // blend toward.
        //
        // NOTE: WasInGesture is intentionally left untouched here. The
        // one-shot "skip EMA on first post-gesture solo frame" taint
        // must only be spent on the frame where it actually causes the
        // skip — see the FIX comment below, in the skipEma branch.
        repX = Track->ReportX;
        repY = Track->ReportY;
    } else {
        // New information: commit the fresh hysteresis baseline first,
        // BEFORE touching EMA (pass-2 ordering — see
        // AmtTrackApplyDeadzone2Pass in Track.h for why this order, and
        // specifically why the EMA call below still blends against the
        // OLD Track->ReportX/Y rather than the just-written HystX/Y:
        // HystX/Y is the jitter-rejection baseline, ReportX/Y is the
        // smoothed value actually shown to Windows, and EMA's whole job
        // is to blend the OLD shown value toward the NEW raw candidate).
        Track->HystX = candX;
        Track->HystY = candY;

        // Skip EMA blending entirely on:
        //   - the very first sample of a brand-new baseline
        //     (PendingFirstSample) — there is no prior ReportX/Y worth
        //     blending against, and this is also what makes a post-
        //     gesture re-tap land cleanly without ever needing to reuse
        //     a ContactID (see TRACK_RETAP_POLICY in Track.h), or
        //   - the first SOLO-finger sample immediately after this track
        //     was part of a multi-finger gesture (WasInGesture &&
        //     aliveCountIsOne) — blending here would pull the cursor
        //     toward the stale multi-finger-era position.
        BOOLEAN skipEma = Track->PendingFirstSample ||
                          (Track->WasInGesture && aliveCountIsOne);

        if (skipEma) {
            repX = candX;
            repY = candY;

            // FIX (task #2 — the actual remaining jump root cause): the
            // taint must be consumed at the moment it is actually spent,
            // and "spent" must be evaluated against THIS FRAME's
            // aliveCountIsOne — including the exact frame a gesture
            // partner finger lifts and drops the alive count from >=2 to
            // 1. The caller (Interrupt.c Phase C) computes
            // aliveCountIsOne from the alive count AFTER Phase A's
            // lift-offs have already been applied for this same frame,
            // not a value cached from before the lift — see the
            // aliveCount computation in Interrupt.c. That is what closes
            // the original gap: previously a one-frame-stale alive count
            // could let a tainted track take one more (unsmoothed-skip-
            // missing) EMA-blended frame against its stale multi-finger
            // baseline before the skip actually fired, which is exactly
            // the visible "tap after gesture -> jump" symptom task #2
            // described. With the count current as of THIS frame, the
            // skip fires on the correct frame, every time, and is
            // consumed here so it cannot fire again on a later frame
            // against an already-corrected baseline.
            if (Track->WasInGesture && aliveCountIsOne) {
                Track->WasInGesture = FALSE;
            }
        } else {
            repX = AmtTrackSmoothCoord(candX, Track->ReportX);
            repY = AmtTrackSmoothCoord(candY, Track->ReportY);
        }
    }

    Track->ReportX = repX;
    Track->ReportY = repY;
    Track->PendingFirstSample = FALSE;

    *OutX = repX;
    *OutY = repY;
}

VOID
AmtTrackUpdate(
    _Inout_ PTRACK  Track,
    _In_    USHORT  rawX,
    _In_    USHORT  rawY,
    _In_    BOOLEAN aliveCountIsOne,
    _Out_   USHORT* OutX,
    _Out_   USHORT* OutY
)
{
#if DBG
    NT_ASSERT(Track->State == TRACK_ACTIVE);
#endif

    BOOLEAN passed;

    if (Track->PendingFirstSample) {
        // First sample after birth/recycle: HystX/Y were seeded to this
        // same (x, y) at birth, so a deadzone comparison against itself
        // would trivially read as "inside the deadzone" (dx==dy==0) and
        // incorrectly classify the very first sample as "no new
        // information". Bypass the test and commit unconditionally.
        Track->HystX = rawX;
        Track->HystY = rawY;
        passed = TRUE;
    } else {
        passed = AmtTrackEvaluateDeadzone(Track, rawX, rawY);
    }

    AmtTrackCommitSample(Track, rawX, rawY, passed, aliveCountIsOne, OutX, OutY);
}

#if DBG
VOID
AmtTrackCheckInvariants(_In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK* Tracks)
{
    for (size_t i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        const TRACK* t = &Tracks[i];

        if (t->State == TRACK_DEAD) {
            NT_ASSERT(!t->PendingFirstSample);
            NT_ASSERT(!t->WasInGesture);
            NT_ASSERT(t->ContactID == 0);
            continue;
        }

        // ACTIVE or GRACE: must carry a real, non-reserved identity.
        NT_ASSERT(t->ContactID != 0);

        for (size_t j = i + 1; j < PTP_MAX_CONTACT_POINTS; j++) {
            const TRACK* u = &Tracks[j];
            if (u->State == TRACK_DEAD) continue;
            NT_ASSERT(t->ContactID != u->ContactID);
        }
    }
}
#endif