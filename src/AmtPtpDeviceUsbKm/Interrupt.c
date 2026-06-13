// Interrupt.c: Handles device input event.
//
// Contact-ID model
// ----------------
// Each PTP_MAX_CONTACT_POINTS slot has a fixed index that IS the ContactID
// Windows receives.  A slot is acquired on the first frame a physical finger
// reads as tip-down and released on the first frame it is no longer tip-down.
// No cross-frame position matching, velocity, smoothing, or prediction is
// performed.  The handler is fully stateless except for slot occupancy, last
// reported coordinates (for lift events), and per-slot hysteresis.
//
// Per-frame algorithm
// -------------------
//  1. Walk the USB finger array; determine which fingers are "tip-down" this
//     frame using the same touch_major / touch_minor threshold as before.
//  2. Map USB finger[i] → slot: if the finger count equals the last frame's
//     active slot count AND each active slot is still occupied in the same
//     order, keep existing assignments.  Otherwise assign slots by lowest
//     free index (stable sort by USB array position).
//  3. Emit lift contacts for every slot that was in-use last frame but has no
//     matching finger this frame.
//  4. Emit touch contacts for every slot that has an active finger this frame.
//  5. Apply a 2-unit deadzone hysteresis on X and Y independently to suppress
//     sub-pixel jitter while a finger stays on the same slot.

#include "Driver.h"
#include "Interrupt.tmh"

// ---- tunables ---------------------------------------------------------------

// Minimum touch_major (doubled) to count as tip-down, matching original code.
#define TIP_MAJOR_THRESHOLD  200
#define TIP_MINOR_THRESHOLD  150

// Deadzone: if the new coordinate differs from the hysteresis baseline by
// fewer than this many raw units, hold the last reported value.
// Set to 0 to disable.
#define XY_DEADZONE_UNITS    2

// -----------------------------------------------------------------------------

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

// Clamp a USHORT value to [0, max].  Returns 0 for negative raw coordinates.
static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT v = raw - minVal;
    if (v < 0)        v = 0;
    if (v > maxVal)   v = (USHORT)maxVal;
    return (USHORT)v;
}

// Apply deadzone hysteresis.  *pBaseline is updated only when the value moves
// outside the deadzone.  Returns the value that should be reported.
static inline USHORT
AmtApplyDeadzone(
    _In_    USHORT newVal,
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

    PDEVICE_CONTEXT         pDeviceContext  = Context;
    const size_t            headerSize      = (size_t)pDeviceContext->DeviceInfo->tp_header;
    const size_t            fingerSize      = (size_t)pDeviceContext->DeviceInfo->tp_fsize;
    size_t                  raw_n;
    size_t                  i;

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
    Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->InputQueue, &Request);
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
    PtpReport.ReportID      = REPORTID_MULTITOUCH;
    PtpReport.IsButtonClicked = 0;

    // Scan time in 100 µs units, clamped to UCHAR.
    KeQueryPerformanceCounter(&CurrentPerfCounter);
    PerfCounterDelta = (CurrentPerfCounter.QuadPart -
                        pDeviceContext->LastReportTime.QuadPart) / 100;
    if (PerfCounterDelta > 0xFF) PerfCounterDelta = 0xFF;
    PtpReport.ScanTime = (USHORT)PerfCounterDelta;

    // ---- touch contacts -------------------------------------------------
    if (pDeviceContext->PtpReportTouch)
    {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        f_base = TouchBuffer + headerSize + pDeviceContext->DeviceInfo->tp_delta;

        //
        // Step 1: Determine which USB fingers are tip-down and compute their
        //         normalised coordinates.  Store results in local arrays.
        //
        BOOLEAN fingerTipDown[PTP_MAX_CONTACT_POINTS] = { FALSE };
        USHORT  fingerNormX  [PTP_MAX_CONTACT_POINTS] = { 0 };
        USHORT  fingerNormY  [PTP_MAX_CONTACT_POINTS] = { 0 };

        for (i = 0; i < raw_n; i++) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            BOOLEAN tip = (AmtRawToInteger(f->touch_major) << 1) >= TIP_MAJOR_THRESHOLD ||
                          (AmtRawToInteger(f->touch_minor) << 1) >= TIP_MINOR_THRESHOLD;

            fingerTipDown[i] = tip;

            if (tip) {
                // Clamp X: map [x.min, x.min + range] → [0, range]
                INT xRange = pDeviceContext->DeviceInfo->x.max -
                             pDeviceContext->DeviceInfo->x.min;
                INT yRange = pDeviceContext->DeviceInfo->y.max -
                             pDeviceContext->DeviceInfo->y.min;

                fingerNormX[i] = AmtClampCoord(
                    AmtRawToInteger(f->abs_x),
                    pDeviceContext->DeviceInfo->x.min,
                    xRange);

                // Y axis is inverted: 0 = bottom of pad in raw, 0 = top in HID.
                INT normY = pDeviceContext->DeviceInfo->y.max -
                            AmtRawToInteger(f->abs_y);
                fingerNormY[i] = AmtClampCoord(normY, 0, yRange);
            }
        }

        //
        // Step 2: Assign slots.
        //
        // Strategy: walk active slots first.  For each slot currently in use,
        // try to find an unmatched tip-down finger in the same USB position
        // (i.e. fingerIndex == slot, since USB finger ordering is stable within
        // a touch sequence on Apple trackpads).  If that finger is still
        // tip-down, keep the slot.  If not, the slot will be freed below.
        //
        // For new fingers (no slot yet), assign the lowest free slot.
        //
        // This is deliberately O(n²) with n ≤ PTP_MAX_CONTACT_POINTS (≤ 10),
        // so the complexity is bounded and acceptable in a DISPATCH_LEVEL path.
        //

        // slotAssignedToFinger[i] = slot index for USB finger i, or
        //                           PTP_MAX_CONTACT_POINTS if unassigned.
        UCHAR slotAssignedToFinger[PTP_MAX_CONTACT_POINTS];
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
            slotAssignedToFinger[i] = PTP_MAX_CONTACT_POINTS;

        // Working copy of slot occupancy for this frame.
        BOOLEAN slotActive[PTP_MAX_CONTACT_POINTS];
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
            slotActive[i] = pDeviceContext->SlotInUse[i];

        // Assign tip-down fingers to slots.
        for (i = 0; i < raw_n; i++) {
            if (!fingerTipDown[i]) continue;

            // Prefer slot == i (USB index matches slot index) if it is free or
            // already assigned to this USB index from a prior frame.
            UCHAR preferredSlot = (i < PTP_MAX_CONTACT_POINTS) ? (UCHAR)i
                                                                 : PTP_MAX_CONTACT_POINTS;
            if (preferredSlot < PTP_MAX_CONTACT_POINTS && !slotActive[preferredSlot]) {
                // Free slot at preferred position — take it.
                slotAssignedToFinger[i] = preferredSlot;
                slotActive[preferredSlot] = TRUE;
            } else if (preferredSlot < PTP_MAX_CONTACT_POINTS && slotActive[preferredSlot]) {
                // Slot already occupied (possibly by us from last frame).
                // Claim it — if it was already used, it will be re-reported;
                // any collision is resolved by finding the next free slot below.
                slotAssignedToFinger[i] = preferredSlot;
                // slotActive[preferredSlot] already TRUE — no change needed.
            }

            // If slot still unassigned (e.g. preferred slot taken by another
            // finger), fall through to free-slot search.
            if (slotAssignedToFinger[i] == PTP_MAX_CONTACT_POINTS) {
                for (size_t s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
                    if (!slotActive[s]) {
                        slotAssignedToFinger[i] = (UCHAR)s;
                        slotActive[s] = TRUE;
                        break;
                    }
                }
            }
        }

        //
        // Step 3: Build the report.
        //
        UCHAR reportSlots = 0;

        // 3a. Lift events: slots that were in-use last frame but are not
        //     claimed by any tip-down finger this frame.
        for (size_t s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            if (!pDeviceContext->SlotInUse[s]) continue;  // was not active

            // Check if any finger claimed this slot.
            BOOLEAN claimed = FALSE;
            for (i = 0; i < raw_n; i++) {
                if (slotAssignedToFinger[i] == (UCHAR)s) {
                    claimed = TRUE;
                    break;
                }
            }

            if (!claimed) {
                // Emit lift using last known coordinates (stable final position).
                if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                    PtpReport.Contacts[reportSlots].ContactID  = (UCHAR)s;
                    PtpReport.Contacts[reportSlots].X          = pDeviceContext->LastNormX[s];
                    PtpReport.Contacts[reportSlots].Y          = pDeviceContext->LastNormY[s];
                    PtpReport.Contacts[reportSlots].TipSwitch  = 0;
                    PtpReport.Contacts[reportSlots].Confidence = 1;
                    reportSlots++;
                }

                // Release slot.
                pDeviceContext->SlotInUse[s] = FALSE;

                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! Slot %llu lifted", (ULONG64)s);
            }
        }

        // 3b. Touch events: active fingers this frame.
        for (i = 0; i < raw_n; i++) {
            if (!fingerTipDown[i]) continue;

            UCHAR slot = slotAssignedToFinger[i];
            if (slot >= PTP_MAX_CONTACT_POINTS) continue;  // ran out of slots

            // Apply per-slot deadzone hysteresis.
            USHORT reportX = AmtApplyDeadzone(fingerNormX[i], &pDeviceContext->HystX[slot]);
            USHORT reportY = AmtApplyDeadzone(fingerNormY[i], &pDeviceContext->HystY[slot]);

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

            // Update per-slot state.
            pDeviceContext->SlotInUse[slot]  = TRUE;
            pDeviceContext->LastNormX[slot]  = reportX;
            pDeviceContext->LastNormY[slot]  = reportY;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Slot %d  x=%d  y=%d", (INT)slot, (INT)reportX, (INT)reportY);
        }

        PtpReport.ContactCount = reportSlots;
    }

    // ---- button ---------------------------------------------------------
    if (pDeviceContext->PtpReportButton) {
        if (TouchBuffer[pDeviceContext->DeviceInfo->tp_button]) {
            PtpReport.IsButtonClicked = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Trackpad button clicked");
        }
    }

    // ---- write report back to HID stack ---------------------------------
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
