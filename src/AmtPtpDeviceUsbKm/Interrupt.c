// Interrupt.c: Handles device input event.
//
// Contact-ID model
// ----------------
// Each PTP_MAX_CONTACT_POINTS slot has a fixed index that IS the ContactID
// Windows receives.  The slot lifecycle is:
//
//   FREE → CONFIRMING → ACTIVE → PENDING_RELEASE → COOLDOWN → FREE
//
//   FREE            No finger.  Available for new assignments after cooldown.
//   CONFIRMING      A tip-down was seen but not yet confirmed (debounce).
//                   SlotTipConfirmed counts consecutive tip-down frames.
//                   Contact is NOT yet reported to Windows in this state.
//   ACTIVE          Tip confirmed.  SlotInUse == TRUE.  Contact reported
//                   every frame while the finger stays down.
//   PENDING_RELEASE Tip went absent this frame.  SlotPendingRelease == TRUE.
//                   Lift event NOT yet emitted — wait one more frame to
//                   distinguish a true lift from a transient gap.
//   COOLDOWN        Lift event was emitted.  SlotCooldown == TRUE.
//                   Slot may not be reassigned this frame; cleared next frame.
//
// Slot assignment (USB-index independent)
// ----------------------------------------
// USB finger array index is treated as INPUT ORDER ONLY, not as identity.
// Identity is tracked via SlotFingerKey[], which stores a stable key derived
// from the order the finger first appeared.  On each frame:
//
//  1. For each tip-down USB finger, try to find an ACTIVE or CONFIRMING slot
//     whose SlotFingerKey matches.  If found, keep that slot.
//  2. If no existing slot matches, assign the lowest FREE (non-cooldown) slot
//     and store a new key.
//  3. Slots that are ACTIVE or CONFIRMING but received no match this frame
//     transition to PENDING_RELEASE.
//  4. Slots that were PENDING_RELEASE last frame and are still unmatched emit
//     a lift event and enter COOLDOWN.
//  5. COOLDOWN slots are cleared unconditionally at the start of each frame.
//
// Per-frame algorithm (summary)
// ------------------------------
//  Phase 0  Clear cooldown flags set last frame.
//  Phase 1  Walk USB finger array; compute tip-down + normalised coords.
//  Phase 2  Match tip-down fingers to existing slots by key; assign new
//           slots for unmatched fingers.
//  Phase 3  Process slots with no finger match:
//             CONFIRMING       → FREE (abort, no event emitted)
//             ACTIVE           → PENDING_RELEASE
//             PENDING_RELEASE  → emit lift, enter COOLDOWN
//  Phase 4  Build PTP_REPORT:
//             PENDING_RELEASE slots in state transition (see above) are
//             already handled in Phase 3 and will NOT appear in Phase 4.
//             ACTIVE slots (newly confirmed or continuing) emit touch contacts.
//  Phase 5  Update per-slot persistent state.

#include "Driver.h"
#include "Interrupt.tmh"

// ---- tunables ---------------------------------------------------------------

// Minimum touch_major (doubled) to count as tip-down.
#define TIP_MAJOR_THRESHOLD  200
#define TIP_MINOR_THRESHOLD  150

// Number of consecutive tip-down frames required before a slot becomes ACTIVE
// and is reported to Windows.  1 = report immediately on first seen frame
// (same as original).  2 = require two consecutive frames (debounce).
#define TIP_CONFIRM_FRAMES   2

// Deadzone: if the new coordinate differs from the hysteresis baseline by
// fewer than this many raw units, hold the last reported value.
// Set to 0 to disable.
#define XY_DEADZONE_UNITS    2

// Sentinel: "no slot" value for slot index fields.
#define SLOT_NONE  ((UCHAR)PTP_MAX_CONTACT_POINTS)

// Sentinel: "no key assigned" for SlotFingerKey[].
#define KEY_NONE   ((UCHAR)0xFF)

// -----------------------------------------------------------------------------

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

// Clamp a raw signed coordinate to [0, maxVal].
static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT v = raw - minVal;
    if (v < 0)      v = 0;
    if (v > maxVal) v = maxVal;
    return (USHORT)v;
}

// Apply deadzone hysteresis.  *pBaseline is updated only when the new value
// moves outside the deadzone band.  Returns the value to report.
static inline USHORT
AmtApplyDeadzone(
    _In_    USHORT  newVal,
    _Inout_ USHORT* pBaseline)
{
#if XY_DEADZONE_UNITS > 0
    INT delta = (INT)newVal - (INT)(*pBaseline);
    if (delta < 0) delta = -delta;
    if (delta < XY_DEADZONE_UNITS) {
        return *pBaseline;   // inside deadzone — hold previous
    }
#endif
    *pBaseline = newVal;
    return newVal;
}

// ---- continuous-reader configuration ----------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t transferLength = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1: transferLength = HEADER_TYPE1 + FSIZE_TYPE1 * MAX_FINGERS; break;
    case TYPE2: transferLength = HEADER_TYPE2 + FSIZE_TYPE2 * MAX_FINGERS; break;
    case TYPE3: transferLength = HEADER_TYPE3 + FSIZE_TYPE3 * MAX_FINGERS; break;
    case TYPE4: transferLength = HEADER_TYPE4 + FSIZE_TYPE4 * MAX_FINGERS; break;
    case TYPE5: transferLength = HEADER_TYPE5 + FSIZE_TYPE5 * MAX_FINGERS; break;
    }

    if (transferLength == 0) {
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &contReaderConfig,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    contReaderConfig.EvtUsbTargetPipeReadersFailed = AmtPtpEvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe,
        &contReaderConfig);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfUsbTargetPipeConfigContinuousReader failed %!STATUS!", status);
    }

exit:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    // Always return SUCCESS: the framework drops all frames if this returns
    // failure, which is worse than running with a bad transfer length.
    return STATUS_SUCCESS;
}

// ---- interrupt completion routine -------------------------------------------

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT         pCtx            = Context;
    const size_t            headerSize      = (size_t)pCtx->DeviceInfo->tp_header;
    const size_t            fingerSize      = (size_t)pCtx->DeviceInfo->tp_fsize;
    size_t                  raw_n;
    size_t                  i;
    size_t                  s;

    UCHAR*                  TouchBuffer;
    UCHAR*                  f_base;
    const struct TRACKPAD_FINGER* f;

    LARGE_INTEGER           CurrentPerfCounter;
    LONGLONG                PerfCounterDelta;
    NTSTATUS                Status;
    PTP_REPORT              PtpReport;

    WDFREQUEST              Request;
    WDFMEMORY               RequestMemory;

    // ---- basic packet validation ----------------------------------------
    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Malformed packet, length=%llu — dropped", NumBytesTransferred);
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! NULL TouchBuffer — dropped");
        return;
    }

    // ---- dequeue pending HID read request -------------------------------
    Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! No pending PTP request — dropped");
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestRetrieveOutputMemory failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    // ---- prepare report skeleton ----------------------------------------
    RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
    PtpReport.ReportID        = REPORTID_MULTITOUCH;
    PtpReport.IsButtonClicked = 0;

    // Scan time in 100 µs units, clamped to USHORT.
    KeQueryPerformanceCounter(&CurrentPerfCounter);
    PerfCounterDelta = (CurrentPerfCounter.QuadPart -
                        pCtx->LastReportTime.QuadPart) / 100;
    if (PerfCounterDelta > 0xFF) PerfCounterDelta = 0xFF;
    PtpReport.ScanTime = (USHORT)PerfCounterDelta;

    // ---- touch contacts -------------------------------------------------
    UCHAR reportSlots = 0;

    if (pCtx->PtpReportTouch)
    {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;

        // =================================================================
        // Phase 0: Decrement cooldown counters set in the previous frame.
        //
        // SlotCooldown is now a UCHAR counter, not a BOOLEAN.
        // A slot with SlotCooldown > 0 is BLOCKED from reassignment.
        // The counter is decremented HERE (start of frame) but a slot
        // is only truly FREE when it reaches 0 BEFORE Phase 2b runs.
        //
        // To guarantee the slot is blocked for at least one full frame,
        // the counter is set to 2 on release (Phase 3): the frame in
        // which the lift is emitted decrements it to 1 (still blocked),
        // and only on the NEXT frame does it reach 0 (FREE).
        //
        // Timeline (SlotCooldown values):
        //   Frame N  : lift emitted → set to 2
        //   Frame N+1: Phase 0 → 1  (still blocked in Phase 2b)
        //   Frame N+2: Phase 0 → 0  (FREE, assignable in Phase 2b)
        // =================================================================
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            if (pCtx->SlotCooldown[s] > 0) {
                pCtx->SlotCooldown[s]--;
                if (pCtx->SlotCooldown[s] == 0) {
                    pCtx->SlotFingerKey[s] = KEY_NONE;
                    // Wipe coordinate state so a future gesture on this slot
                    // cannot inherit the previous gesture's LastNormX/Y.
                    // LastNormX/Y are used for the lift-event position report;
                    // if they carry a stale value from an earlier gesture the
                    // HID stack sees a phantom position snap on the new touch.
                    pCtx->LastNormX[s] = 0;
                    pCtx->LastNormY[s] = 0;
                    pCtx->HystX[s]     = 0;
                    pCtx->HystY[s]     = 0;
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                        "%!FUNC! Slot %llu cooldown expired → FREE", (ULONG64)s);
                } else {
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                        "%!FUNC! Slot %llu cooldown remaining=%d", (ULONG64)s,
                        (INT)pCtx->SlotCooldown[s]);
                }
            }
        }

        // =================================================================
        // Phase 1: Walk USB finger array.
        //          Compute tip-down status and normalised coordinates.
        //          Assign a per-finger key: 0x00..0x09 in order of first
        //          appearance — stable within a touch sequence because Apple
        //          trackpads keep fingers in the same array position while
        //          they stay on the pad.  A fresh key is derived from the
        //          USB array index, but only used to FIND an existing slot,
        //          not to SELECT one.
        // =================================================================
        BOOLEAN fingerTipDown [PTP_MAX_CONTACT_POINTS] = { FALSE };
        USHORT  fingerNormX   [PTP_MAX_CONTACT_POINTS] = { 0 };
        USHORT  fingerNormY   [PTP_MAX_CONTACT_POINTS] = { 0 };
        UCHAR   fingerKey     [PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
            fingerKey[i] = KEY_NONE;

        for (i = 0; i < raw_n; i++) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            BOOLEAN tip =
                (AmtRawToInteger(f->touch_major) << 1) >= TIP_MAJOR_THRESHOLD ||
                (AmtRawToInteger(f->touch_minor) << 1) >= TIP_MINOR_THRESHOLD;

            fingerTipDown[i] = tip;

            if (tip) {
                INT xRange = pCtx->DeviceInfo->x.max - pCtx->DeviceInfo->x.min;
                INT yRange = pCtx->DeviceInfo->y.max - pCtx->DeviceInfo->y.min;

                fingerNormX[i] = AmtClampCoord(
                    AmtRawToInteger(f->abs_x),
                    pCtx->DeviceInfo->x.min,
                    xRange);

                // Y axis: 0 = bottom of pad in raw, 0 = top in HID.
                INT normY = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
                fingerNormY[i] = AmtClampCoord(normY, 0, yRange);

                // Finger key: USB array index cast to byte.
                // This is stable as long as the finger stays in the same
                // array position (which Apple trackpads guarantee).
                fingerKey[i] = (UCHAR)i;
            }
        }

        // =================================================================
        // Phase 2: Match tip-down fingers to slots.
        //
        // For each tip-down finger, search for a slot whose SlotFingerKey
        // matches.  A slot is a candidate if:
        //   • It is ACTIVE (SlotInUse) or CONFIRMING (SlotTipConfirmed > 0)
        //   • Its SlotFingerKey equals fingerKey[i]
        //
        // If no matching slot exists, assign the lowest FREE slot
        // (SlotInUse == FALSE && SlotPendingRelease == FALSE &&
        //  SlotCooldown == FALSE && SlotTipConfirmed == 0).
        //
        // slotForFinger[i] = slot index assigned to USB finger i, or SLOT_NONE.
        // fingerForSlot[s] = USB finger index matched to slot s, or SLOT_NONE.
        // =================================================================
        UCHAR slotForFinger[PTP_MAX_CONTACT_POINTS];
        UCHAR fingerForSlot[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) slotForFinger[i] = SLOT_NONE;
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) fingerForSlot[s] = SLOT_NONE;

        // 2a. Match by key.
        for (i = 0; i < raw_n; i++) {
            if (!fingerTipDown[i]) continue;

            UCHAR key = fingerKey[i];
            for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
                if ((pCtx->SlotInUse[s] || pCtx->SlotTipConfirmed[s] > 0) &&
                    pCtx->SlotFingerKey[s] == key &&
                    fingerForSlot[s] == SLOT_NONE)
                {
                    slotForFinger[i] = (UCHAR)s;
                    fingerForSlot[s] = (UCHAR)i;
                    break;
                }
            }
        }

        // 2b. Assign new slots for unmatched tip-down fingers.
        for (i = 0; i < raw_n; i++) {
            if (!fingerTipDown[i]) continue;
            if (slotForFinger[i] != SLOT_NONE) continue;  // already matched

            // Find lowest FREE slot (not in use, not pending release, not in
            // cooldown, not currently confirming another finger).
            for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
                if (!pCtx->SlotInUse[s] &&
                    !pCtx->SlotPendingRelease[s] &&
                    !pCtx->SlotCooldown[s] &&
                    pCtx->SlotTipConfirmed[s] == 0 &&
                    fingerForSlot[s] == SLOT_NONE)
                {
                    slotForFinger[i] = (UCHAR)s;
                    fingerForSlot[s] = (UCHAR)i;
                    pCtx->SlotFingerKey[s] = fingerKey[i];

                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                        "%!FUNC! Finger %llu → new slot %llu (key=%d)",
                        (ULONG64)i, (ULONG64)s, (INT)fingerKey[i]);
                    break;
                }
            }
        }

        // =================================================================
        // Phase 3: Advance slot state for slots that have NO finger match.
        //
        //   CONFIRMING (SlotTipConfirmed > 0, SlotInUse == FALSE)
        //     → reset tip counter and key; slot returns to FREE silently.
        //
        //   ACTIVE (SlotInUse == TRUE)
        //     → set SlotPendingRelease, clear SlotInUse.
        //       Lift event is NOT emitted yet — wait one frame.
        //
        //   PENDING_RELEASE (SlotPendingRelease == TRUE)
        //     → emit lift event now, clear pending flag, set cooldown.
        //
        // Note: a slot in PENDING_RELEASE can also receive a new finger match
        // (e.g. rapid re-touch).  In that case fingerForSlot[s] != SLOT_NONE
        // and we skip Phase 3 for that slot, resurrecting it as ACTIVE.
        // =================================================================
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {

            if (fingerForSlot[s] != SLOT_NONE) {
                // Slot has a live finger this frame — no release processing.
                // Note: a PENDING_RELEASE slot cannot reach here because:
                //   • Phase 2a: SlotFingerKey was cleared on ACTIVE→PENDING_RELEASE
                //   • Phase 2b: !SlotPendingRelease guard blocks assignment
                // Both paths are closed, so no resurrection is possible.
                continue;
            }

            // ---- PENDING_RELEASE → emit lift, enter COOLDOWN ----
            if (pCtx->SlotPendingRelease[s]) {
                if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                    PtpReport.Contacts[reportSlots].ContactID  = (UCHAR)s;
                    PtpReport.Contacts[reportSlots].X          = pCtx->LastNormX[s];
                    PtpReport.Contacts[reportSlots].Y          = pCtx->LastNormY[s];
                    PtpReport.Contacts[reportSlots].TipSwitch  = 0;
                    PtpReport.Contacts[reportSlots].Confidence = 1;
                    reportSlots++;
                }
                pCtx->SlotPendingRelease[s] = FALSE;
                pCtx->SlotCooldown[s]       = 2;   // blocks THIS frame's Phase 2b
                                                    // and the entire next frame;
                                                    // reaches 0 in frame N+2.
                // SlotFingerKey is cleared when cooldown reaches 0 in Phase 0.

                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! Slot %llu lift emitted → COOLDOWN", (ULONG64)s);
                continue;
            }

            // ---- ACTIVE → PENDING_RELEASE ----
            if (pCtx->SlotInUse[s]) {
                pCtx->SlotInUse[s]          = FALSE;
                pCtx->SlotPendingRelease[s] = TRUE;
                // Invalidate the finger key immediately so Phase 2a cannot
                // match a rapid re-tap (same USB array index) to this slot
                // while it is still draining through PENDING_RELEASE.
                // Leaving the key alive is the root cause of the identity
                // leakage: new tap → same key → Phase 2a match → slot
                // "resurrected" with stale LastNormX/HystX → position snap.
                pCtx->SlotFingerKey[s]    = KEY_NONE;
                // Reset tip counter — non-zero value here would make Phase 4
                // treat the very first frame of a new touch as already-
                // confirmed, bypassing the debounce and inheriting stale state.
                pCtx->SlotTipConfirmed[s] = 0;
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! Slot %llu ACTIVE → PENDING_RELEASE", (ULONG64)s);
                continue;
            }

            // ---- CONFIRMING (no match this frame) → FREE ----
            if (pCtx->SlotTipConfirmed[s] > 0) {
                pCtx->SlotTipConfirmed[s] = 0;
                pCtx->SlotFingerKey[s]    = KEY_NONE;
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! Slot %llu CONFIRMING aborted → FREE", (ULONG64)s);
                continue;
            }
        }

        // =================================================================
        // Phase 4: Build touch contact records for active fingers.
        //
        //   For each tip-down finger with an assigned slot:
        //     • Increment SlotTipConfirmed.
        //     • If SlotTipConfirmed reaches TIP_CONFIRM_FRAMES, mark the
        //       slot ACTIVE (SlotInUse = TRUE) and emit a touch contact.
        //     • If already ACTIVE (SlotInUse was TRUE before Phase 3 cleared
        //       it — but wait: Phase 3 only clears SlotInUse for unmatched
        //       slots, so matched ACTIVE slots still have SlotInUse == TRUE
        //       here), emit a touch contact directly.
        // =================================================================
        for (i = 0; i < raw_n; i++) {
            if (!fingerTipDown[i]) continue;

            UCHAR slot = slotForFinger[i];
            if (slot >= PTP_MAX_CONTACT_POINTS) continue;  // no free slot found

            // Advance tip confirmation counter.
            if (pCtx->SlotTipConfirmed[slot] < TIP_CONFIRM_FRAMES) {
                pCtx->SlotTipConfirmed[slot]++;
            }

            // Check if confirmed or already active.
            BOOLEAN alreadyActive    = pCtx->SlotInUse[slot];
            BOOLEAN justConfirmed    = (!alreadyActive &&
                                        pCtx->SlotTipConfirmed[slot] >= TIP_CONFIRM_FRAMES);

            if (!alreadyActive && !justConfirmed) {
                // Still in CONFIRMING state; do not emit yet.
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! Slot %d confirming (%d/%d)",
                    (INT)slot,
                    (INT)pCtx->SlotTipConfirmed[slot],
                    TIP_CONFIRM_FRAMES);
                continue;
            }

            if (justConfirmed) {
                pCtx->SlotInUse[slot] = TRUE;
                // Reset hysteresis baseline on activation so deadzone starts
                // from the first confirmed position.
                pCtx->HystX[slot] = fingerNormX[i];
                pCtx->HystY[slot] = fingerNormY[i];
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! Slot %d confirmed → ACTIVE", (INT)slot);
            }

            // Apply per-slot deadzone hysteresis.
            USHORT reportX = AmtApplyDeadzone(fingerNormX[i], &pCtx->HystX[slot]);
            USHORT reportY = AmtApplyDeadzone(fingerNormY[i], &pCtx->HystY[slot]);

            if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);
                BOOLEAN confidence = (AmtRawToInteger(f->touch_minor) << 1) > 0;

                PtpReport.Contacts[reportSlots].ContactID  = slot;
                PtpReport.Contacts[reportSlots].X          = reportX;
                PtpReport.Contacts[reportSlots].Y          = reportY;
                PtpReport.Contacts[reportSlots].TipSwitch  = 1;
                PtpReport.Contacts[reportSlots].Confidence = confidence ? 1 : 0;
                reportSlots++;
            }

            // =================================================================
            // Phase 5: Update persistent per-slot state.
            // =================================================================
            pCtx->LastNormX[slot] = reportX;
            pCtx->LastNormY[slot] = reportY;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Slot %d x=%d y=%d", (INT)slot, (INT)reportX, (INT)reportY);
        }

        PtpReport.ContactCount = reportSlots;
    }

    // ---- button ---------------------------------------------------------
    if (pCtx->PtpReportButton) {
        if (TouchBuffer[pCtx->DeviceInfo->tp_button]) {
            PtpReport.IsButtonClicked = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Trackpad button clicked");
        }
    }

    // ---- write report back to HID stack ---------------------------------
    // Update scan time baseline BEFORE completing the request.
    pCtx->LastReportTime = CurrentPerfCounter;

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&PtpReport, sizeof(PTP_REPORT));

    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, Status);
}

// ---- reader failure callback ------------------------------------------------

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);

    return TRUE;  // restart the reader
}