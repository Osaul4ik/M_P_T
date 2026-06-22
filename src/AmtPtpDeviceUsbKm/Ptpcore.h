// PTPCore.h - Layer contract for the PTP touch stack.
// Interrupt.c -> Input.c -> PTPCore.c (Match.c + ActiveContact.c + Gesture.c) -> PTP_REPORT.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

// Raw input (InputAdapter output). No state, no decisions.

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

// PTPCore output. Stable contact identities + FSM phase.

typedef enum _CONTACT_PHASE
{
    CONTACT_PHASE_NONE = 0,  // no contact this frame
    CONTACT_PHASE_DOWN,      // born this frame (FREE -> ACTIVE)
    CONTACT_PHASE_MOVE,      // continuing (ACTIVE, updated)
    CONTACT_PHASE_UP,        // lifted this frame (-> FREE or GRACE)
} CONTACT_PHASE;

// PTPCore output contract. Interrupt.c reads only this.
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

// PTPCore_ProcessFrame - single frame-orchestration entry point.

struct _DEVICE_CONTEXT; // fwd decl, defined in Device.h

VOID
PTPCore_ProcessFrame(
    _Inout_ struct _DEVICE_CONTEXT* DeviceContext,
    _In_    const RAW_FRAME*        RawFrame,
    _In_    LONGLONG                NowQpc,
    _Out_   PTP_CORE_FRAME*         OutResult
);

// Hot-path trace rate gate. Shared between Interrupt.c and PTPCore.c.
BOOLEAN
AmtHotPathTraceGate(_Inout_ struct _DEVICE_CONTEXT* pCtx, _In_ LONGLONG NowQpc100ns);

// Recent-lift ring buffer for retap smoothing.

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

// Find closest lift to (CandX, CandY). FALSE -> raw birth.
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