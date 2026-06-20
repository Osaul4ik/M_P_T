// Track.c - Track FSM implementation. See Track.h for the full design
// rationale (state machine, frame-determinism rule, why ContactID is
// monotonic and never reused while warm).

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
// other path in the codebase that may write Track->ContactID.
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
    // AmtTrackExpireGrace and the TRACK_RETAP_POLICY note in Track.h)
    // never actually re-binds a GRACE track to a later retap, so this
    // state is observable for exactly one instant before the caller
    // (Interrupt.c) immediately calls AmtTrackExpireGrace on it.
    //
    // FIX (closing the "retap continuation" question permanently, not
    // provisionally): a real continuation would mean handing the SAME
    // ContactID to a later touch-down after this lift-off has already
    // been reported with TipSwitch=0. That is in direct conflict with
    // the monotonic-NextContactId invariant in Device.h — "never reuse
    // an ID while it might still be warm in Windows' contact tracking" —
    // which exists specifically to kill the cursor-teleport bug this
    // entire rewrite was built to fix. Reusing an ID across a reported
    // lift, even a few milliseconds later, reopens exactly that bug
    // class: Windows would interpret the new touch-down as a
    // continuation of the just-removed contact and "correct" the
    // cursor toward the new position instead of treating it as a fresh
    // press. GRACE therefore stays a pure quarantine, permanently, not
    // a held reservation — it exists only so a diagnostics/invariant
    // pass has an explicit state to assert against instead of inferring
    // "this was a gesture lift" from a side boolean. Any future
    // continuation feature would need a DIFFERENT signal to Windows
    // (e.g. a position correlation hint outside ContactID reuse), not
    // a change to this function.
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
    // to count as real motion rather than sensor jitter.
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
        // New information: commit the fresh hysteresis baseline first.
        Track->HystX = candX;
        Track->HystY = candY;

        // Skip EMA blending entirely on:
        //   - the very first sample of a brand-new baseline
        //     (PendingFirstSample) — there is no prior ReportX/Y worth
        //     blending against, and
        //   - the first SOLO-finger sample immediately after this track
        //     was part of a multi-finger gesture (WasInGesture &&
        //     aliveCountIsOne) — blending here would pull the cursor
        //     toward the stale multi-finger-era position.
        BOOLEAN skipEma = Track->PendingFirstSample ||
                          (Track->WasInGesture && aliveCountIsOne);

        if (skipEma) {
            repX = candX;
            repY = candY;

            // FIX (one-shot taint consumed without firing): this used to
            // live UNCONDITIONALLY after the if/else below, keyed only on
            // (WasInGesture && aliveCountIsOne) — not on whether skipEma
            // actually ran. That meant a low-velocity first post-gesture
            // solo sample that FAILS the deadzone test (passedDeadzone ==
            // FALSE, handled in the sibling branch above, which just holds
            // ReportX/Y and never reaches here) would still have
            // WasInGesture cleared by the old unconditional check below,
            // having never actually received the skip-EMA treatment. The
            // NEXT frame — the first one to clear the deadzone — would
            // then blend via the normal EMA path against the still-stale
            // multi-finger-era ReportX/Y, reopening exactly the
            // post-gesture cursor-jump class this flag exists to prevent.
            // The taint must only be consumed at the moment it is
            // actually spent.
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