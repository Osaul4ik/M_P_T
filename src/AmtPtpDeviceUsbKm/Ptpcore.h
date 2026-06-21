// PTPCore.h - Layer contract for the PTP touch stack.
//
// Defines boundary types between layers:
//
//   USB/Wellspring driver (Interrupt.c)
//         |  RAW_FRAME (no state, no decisions)
//         v
//   InputAdapter (Input.c)              -- decode + normalize only
//         |  RAW_FRAME
//         v
//   PTPCore_ProcessFrame (PTPCore.c)    -- single orchestration entry point:
//         |                                ContactMatcher (Match.c) +
//         |                                StateMachine (ActiveContact.c) +
//         |                                palm session + gesture session
//         v
//   PTP_CORE_FRAME (stable ContactID[] with FSM phase)
//         v
//   Interrupt.c                         -- serializes into PTP_REPORT only
//
// Design notes:
//   1. RAW_CONTACT/RAW_FRAME use fixed-size array (kernel driver at
//      DISPATCH_LEVEL - no CRT, no heap). PTP_MAX_CONTACT_POINTS (5) is
//      a hard hardware limit.
//   2. Tip-size debounce is NOT in InputAdapter - it reads previous
//      contact state, so it's a Matcher concern (Match.c).
//   3. ContactMatcher (Match.c) is cost-based: squared spatial distance
//      with slot index as narrow tie-breaker only, never as identity key.
//   4. Orchestration lives in PTPCore.c, not Interrupt.c or scattered
//      into Match.c/ActiveContact.c.

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

// One reportable contact for the current frame. PTPCore's output contract.
// Interrupt.c reads this and nothing else - never touches ACTIVE_CONTACT.
typedef struct _PTP_CORE_CONTACT
{
    ULONG          ContactID;     // stable, monotonic, never reused while warm
    USHORT         X;
    USHORT         Y;
    CONTACT_PHASE  Phase;
    BOOLEAN        Confident;     // FALSE if position carried over (tip-drop)
    BOOLEAN        PalmSuspect;   // local-palm classification; suppressed
                                  // contacts don't appear in PTP_CORE_FRAME
} PTP_CORE_CONTACT, *PPTP_CORE_CONTACT;

typedef struct _PTP_CORE_FRAME
{
    LONGLONG          TimestampQpc;
    UCHAR             ContactCount;
    PTP_CORE_CONTACT  Contacts[PTP_MAX_CONTACT_POINTS];
    BOOLEAN           LargePalmBlanked;  // whole-pad palm event this frame
} PTP_CORE_FRAME, *PPTP_CORE_FRAME;

// ===========================================================================
// PTPCore_ProcessFrame - single entry point owning all frame orchestration:
// matching, lifecycle FSM, gesture/palm session bookkeeping.
// ===========================================================================

struct _DEVICE_CONTEXT; // fwd decl, defined in Device.h

VOID
PTPCore_ProcessFrame(
    _Inout_ struct _DEVICE_CONTEXT* DeviceContext,
    _In_    const RAW_FRAME*        RawFrame,
    _In_    LONGLONG                NowQpc,
    _Out_   PTP_CORE_FRAME*         OutResult
);

// FIX (Task 4.1): hot-path trace rate gate. Shared between Interrupt.c
// and PTPCore.c using the same DEVICE_CONTEXT.LastHotPathTraceQpc state.
BOOLEAN
AmtHotPathTraceGate(_Inout_ struct _DEVICE_CONTEXT* pCtx, _In_ LONGLONG NowQpc100ns);

// ===========================================================================
// Recent-lift memory for retap smoothing. Ring buffer (not slot-indexed)
// of "where/when did a contact last lift", matched by proximity at birth.
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

// Finds closest-in-time-and-space recent lift to (CandX, CandY).
// Returns FALSE if none qualifies (caller falls back to raw birth).
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