// Gesture.h - GestureEngine session state.
//
// Extracted from the GestureSessionActive 2-edge FSM that previously
// lived inline in Interrupt.c. This is intentionally a SMALL extraction:
// the full "1 finger -> pointer, 2 fingers -> scroll/pinch, 3+ -> system
// gesture" interpretation described in the original task spec's Phase 7
// is Windows' job (the PTP HID report is the gesture contract with the
// OS) - this driver's only gesture-relevant responsibility is deciding
// which tracks get the WasInGesture taint, which is exactly the
// information GESTURE_SESSION carries. No tracking/FSM/identity logic
// here - matches the task spec's hard rule ("no tracking logic, no FSM
// changes, no identity modifications" for this layer).

#pragma once

#include "PTPCore.h"

EXTERN_C_START

typedef struct _GESTURE_SESSION
{
    BOOLEAN Active;  // TRUE once >=2 fingers have been down together
                     // this session; FALSE once all fingers lift.
                     // Unchanged on exactly 1 alive finger (sticky).
} GESTURE_SESSION;

VOID
AmtGestureSessionInit(_Out_ GESTURE_SESSION* Session);

// Two-edge update: aliveCount==0 -> FALSE, aliveCount>=2 -> TRUE,
// aliveCount==1 -> unchanged. Call once per frame with the alive count
// computed AFTER this frame's lift-offs are known (see Interrupt.c
// Phase A ordering note - staleness here previously caused a real bug,
// see Track.c AmtTrackCommitSample FIX comment).
VOID
AmtGestureSessionUpdate(_Inout_ GESTURE_SESSION* Session, _In_ UCHAR AliveCount);

// TRUE if a track born or alive during THIS frame should be tainted
// (i.e. >=2 fingers are down this exact frame, not just session-sticky).
// This is a per-frame question, distinct from Session->Active.
BOOLEAN
AmtGestureIsMultiFingerFrame(_In_ UCHAR AliveCount);

EXTERN_C_END