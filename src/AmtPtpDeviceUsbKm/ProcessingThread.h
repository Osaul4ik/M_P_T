/*
 * ProcessingThread.h - Declarations for the dedicated PASSIVE_LEVEL
 *                      processing thread that owns the slot state machine.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>

// Forward declaration — full type is in Device.h.
typedef struct _DEVICE_CONTEXT DEVICE_CONTEXT, *PDEVICE_CONTEXT;

/*
 * Start the processing thread.  Called from D0Entry after the USB
 * interrupt pipe is started.
 *
 * IRQL: PASSIVE_LEVEL
 */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ProcThreadStart(
    _In_ PDEVICE_CONTEXT pCtx);

/*
 * Signal the processing thread to drain its queue and exit.
 * Waits for the thread object before returning.
 * Called from D0Exit.
 *
 * IRQL: PASSIVE_LEVEL
 */
_IRQL_requires_(PASSIVE_LEVEL)
VOID
ProcThreadStop(
    _In_ PDEVICE_CONTEXT pCtx);

/*
 * Wake the processing thread.  Called by the USB interrupt callback
 * after writing a packet to the ring buffer.
 *
 * Safe to call at DISPATCH_LEVEL.
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
ProcThreadSignal(
    _In_ PDEVICE_CONTEXT pCtx);
