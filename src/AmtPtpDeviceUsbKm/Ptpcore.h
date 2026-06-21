// PTPCore.h - Layer contract for the PTP touch stack.
//
// This header defines the boundary types that flow between layers:
//
//   USB/Wellspring driver (Interrupt.c)
//         |  RAW_FRAME (no state, no decisions)
//         v
//   InputAdapter (Input.c)              -- decode + normalize only
//         |  RAW_FRAME
//         v
//   PTPCore_ProcessFrame (PTPCore.c)    -- single orchestration entry point:
//         |                                ContactMatcher (Match.c, cost-based,
//         |                                identity independent of hw slot) +
//         |                                StateMachine (ActiveContact.c) +
//         |                                palm session + gesture session
//         v
//   PTP_CORE_FRAME (stable ContactID[] with FSM phase)
//         v
//   Interrupt.c                         -- serializes into PTP_REPORT only
//
// Design notes / deliberate deviations from a literal slot-array port
// (see audit/04_correction_plan.md for the correction history - an
// earlier version of this refactor still indexed the contact pool by
// hardware slot, which was wrong and has been replaced):
//
//   1. RAW_CONTACT/RAW_FRAME use a fixed-size array, not std::vector.
//      This is a kernel-mode C driver running at DISPATCH_LEVEL inside
//      a USB read completion routine - no CRT, no heap allocation on
//      the hot path. PTP_MAX_CONTACT_POINTS (5) is a hard physical
//      limit of the hardware, so a fixed array loses nothing.
//
//   2. Tip-size debounce is NOT part of InputAdapter. Tip-drop requires
//      reading previous contact state - it is a StateMachine/Matcher
//      concern (see Match.c AmtMatchBuildCandidates), not parsing.
//      InputAdapter only decodes geometry; it never reads contact state.
//
//   3. ContactMatcher (Match.c) is a real cost-based matcher: every
//      RawFrame contact is matched against every ACTIVE_CONTACT pool
//      entry by squared spatial distance, with hardware slot index used
//      ONLY as a narrow tie-breaker (see MATCH_TIE_EPSILON_SQ in
//      Match.c), never as the correspondence key itself. A contact that
//      moves to a different hardware slot between frames is still
//      matched correctly by position.
//
//   4. Orchestration (which Match.c/ActiveContact.c primitive to call,
//      and in what order, for all contacts in one frame) lives in
//      PTPCore.c, not in Interrupt.c and not scattered into Match.c/
//      ActiveContact.c. Interrupt.c calls PTPCore_ProcessFrame exactly
//      once per USB completion and does nothing else with frame
//      semantics - see PTPCore.c.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

// ===========================================================================
// Layer 0: Raw input (InputAdapter output). No state, no decisions.
// ===========================================================================

typedef struct _RAW_CONTACT
{
    USHORT SlotIndex;   // raw hardware slot index (0..PTP_MAX_CONTACT_POINTS-1)
    USHORT X;            // normalized device units (post AmtClampCoord)
    USHORT Y;
    USHORT Major;        // touch_major, raw
    USHORT Minor;        // touch_minor, raw
    UCHAR  Origin;        // firmware origin field; 0 == identity break signal
} RAW_CONTACT, *PRAW_CONTACT;

typedef struct _RAW_FRAME
{
    LONGLONG    TimestampQpc;
    UCHAR       ContactCount;
    RAW_CONTACT Contacts[PTP_MAX_CONTACT_POINTS];
} RAW_FRAME, *PRAW_FRAME;

// ===========================================================================
// Layer 1: PTPCore output. Stable contact identities + FSM phase.
// ===========================================================================

typedef enum _CONTACT_PHASE
{
    CONTACT_PHASE_NONE = 0,  // no contact this frame
    CONTACT_PHASE_DOWN,      // born this frame (FREE -> ACTIVE)
    CONTACT_PHASE_MOVE,      // continuing (ACTIVE, updated)
    CONTACT_PHASE_UP,        // lifted this frame (-> FREE or GRACE)
} CONTACT_PHASE;

// One reportable contact for the current frame. This is PTPCore's
// output contract - Interrupt.c reads this and nothing else; it never
// touches ACTIVE_CONTACT directly.
typedef struct _PTP_CORE_CONTACT
{
    ULONG          ContactID;     // stable, monotonic, never reused while warm
    USHORT         X;
    USHORT         Y;
    CONTACT_PHASE  Phase;
    BOOLEAN        Confident;     // FALSE if position carried over (tip-drop debounce)
    BOOLEAN        PalmSuspect;   // local-palm classification (Palm.c scoring);
                                  // suppression itself is applied inside
                                  // PTPCore_ProcessFrame before this struct
                                  // is populated - a suppressed contact simply
                                  // does not appear in PTP_CORE_FRAME.Contacts[]
} PTP_CORE_CONTACT, *PPTP_CORE_CONTACT;

typedef struct _PTP_CORE_FRAME
{
    LONGLONG          TimestampQpc;
    UCHAR             ContactCount;
    PTP_CORE_CONTACT  Contacts[PTP_MAX_CONTACT_POINTS];
    BOOLEAN           LargePalmBlanked;  // whole-pad palm event this frame
} PTP_CORE_FRAME, *PPTP_CORE_FRAME;

// ===========================================================================
// PTPCore_ProcessFrame - the single entry point that owns all frame
// orchestration: contact matching, lifecycle FSM transitions, gesture
// session bookkeeping, palm session bookkeeping. Interrupt.c calls this
// exactly once per USB completion and does nothing else with frame
// semantics - see PTPCore.c and audit/04_correction_plan.md.
// ===========================================================================

struct _DEVICE_CONTEXT; // fwd decl, defined in Device.h

VOID
PTPCore_ProcessFrame(
    _Inout_ struct _DEVICE_CONTEXT* DeviceContext,
    _In_    const RAW_FRAME*        RawFrame,
    _In_    LONGLONG                NowQpc,
    _Out_   PTP_CORE_FRAME*         OutResult
);

// FIX (Task 4.1, instrumentation): hot-path trace rate gate. Originally
// a static helper duplicated only in Interrupt.c; promoted to a shared
// declaration here so PTPCore.c can use the SAME rate-limit state
// (DEVICE_CONTEXT.LastHotPathTraceQpc) for its own diagnostic trace
// instead of either duplicating the function or flooding the log with
// an independent gate. Defined in Interrupt.c (no behavior change there
// beyond dropping `static`).
BOOLEAN
AmtHotPathTraceGate(_Inout_ struct _DEVICE_CONTEXT* pCtx, _In_ LONGLONG NowQpc100ns);

// ===========================================================================
// Recent-lift memory for retap smoothing. Deliberately NOT slot-indexed
// (the old DEVICE_CONTEXT.SlotLastLiftQpc/X/Y[PTP_MAX_CONTACT_POINTS]
// arrays were keyed by hardware slot, which is the same slot-as-identity
// mistake this refactor removes everywhere else - see
// audit/04_correction_plan.md). A small ring buffer of "where/when did
// a contact last lift" entries, matched by proximity at birth time, not
// by slot.
// ===========================================================================

#define RECENT_LIFT_CAPACITY PTP_MAX_CONTACT_POINTS

typedef struct _RECENT_LIFT
{
    BOOLEAN  Valid;
    LONGLONG LiftQpc;
    USHORT   X;
    USHORT   Y;
} RECENT_LIFT;

typedef struct _RECENT_LIFT_RING
{
    RECENT_LIFT Entries[RECENT_LIFT_CAPACITY];
    UCHAR       NextWriteIndex; // round-robin
} RECENT_LIFT_RING;

VOID
AmtRecentLiftRecord(
    _Inout_ RECENT_LIFT_RING* Ring,
    _In_    LONGLONG          NowQpc,
    _In_    USHORT            X,
    _In_    USHORT            Y
);

// Finds the closest-in-time-and-space recent lift to (CandX, CandY) and
// reports it via Out*. Returns FALSE if none qualifies (caller falls
// back to raw unsmoothed birth - always correct).
BOOLEAN
AmtRecentLiftFindNearby(
    _In_  const RECENT_LIFT_RING* Ring,
    _In_  LONGLONG                NowQpc,
    _In_  LONGLONG                PerfFrequencyHz,
    _In_  USHORT                  CandX,
    _In_  USHORT                  CandY,
    _Out_ USHORT*                 OutX,
    _Out_ USHORT*                 OutY
);

EXTERN_C_END