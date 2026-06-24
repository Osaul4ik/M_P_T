// Gesture.c - GestureEngine session state. See Gesture.h for scope notes.

#include "Driver.h"
#include "Gesture.h"

VOID
AmtGestureSessionInit(_Out_ GESTURE_SESSION* Session)
{
    Session->Active = FALSE;
}

VOID
AmtGestureSessionUpdate(_Inout_ GESTURE_SESSION* Session, _In_ UCHAR AliveCount)
{
    if (AliveCount == 0) {
        Session->Active = FALSE;
    } else if (AliveCount >= 2) {
        Session->Active = TRUE;
    }
    // AliveCount == 1: unchanged (sticky) - preserves WasInGesture taint
    // through the transition from 2 fingers down to 1 finger remaining.
}

BOOLEAN
AmtGestureIsMultiFingerFrame(_In_ UCHAR AliveCount)
{
    return (AliveCount >= 2);
}