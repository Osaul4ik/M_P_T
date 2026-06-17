// Interrupt.c: Handles device input event

#include "Driver.h"
#include "Interrupt.tmh"

#define XY_DEADZONE_UNITS   2
#define SMOOTHING_ALPHA_NUM 5
#define SMOOTHING_ALPHA_DEN 8

// touch_major threshold for a LARGE (flat) palm — entire pad suppressed.
#define PALM_LARGE_MAJOR    380
// Per-contact score threshold for PALM_LOCAL (edge/thumb rejection).
#define PALM_SCORE_THRESH   45

//
// FIX (cursor-jump, tip-size debounce):
// Number of consecutive frames a SlotActive contact is allowed to dip below
// the `tip` size threshold (major/minor too small) while still being
// nonzero, before it is actually treated as lifted.  T2 touch_major/minor
// readings are noisy near the lower bound -- a single low sample no longer
// kills the slot outright.  Kept small (not a real "hold" state) so a true
// quick lift-and-retouch within 1-2 frames still lifts promptly; this only
// absorbs single-frame measurement noise, not real lift-off.
//
#define TIP_DROP_DEBOUNCE_FRAMES 2

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

// Clamp raw device coordinate into [0, max-min] and return as USHORT.
static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT shifted = raw - minVal;
    if (shifted < 0)               shifted = 0;
    if (shifted > maxVal - minVal) shifted = maxVal - minVal;
    return (USHORT)shifted;
}

static inline USHORT
AmtApplyDeadzone(_In_ USHORT newVal, _Inout_ USHORT* pBaseline)
{
#if XY_DEADZONE_UNITS > 0
    INT delta = (INT)newVal - (INT)(*pBaseline);
    if (delta < 0) delta = -delta;
    if (delta < XY_DEADZONE_UNITS)
        return *pBaseline;
#endif
    *pBaseline = newVal;
    return newVal;
}

static inline USHORT
AmtSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal)
{
    INT blended = ((INT)rawVal * SMOOTHING_ALPHA_NUM +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

// Reset all per-slot state for slot i.
static inline VOID
AmtClearSlot(_In_ PDEVICE_CONTEXT ctx, _In_ size_t i)
{
    ctx->SmoothedX[i]     = 0;
    ctx->SmoothedY[i]     = 0;
    ctx->HystX[i]         = 0;
    ctx->HystY[i]         = 0;
    ctx->SlotActive[i]    = FALSE;
    ctx->TipDropCount[i]  = 0;   // FIX: clear debounce counter with the slot
}

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

//
// AmtClassifyPalm
//
// Two-tier palm classifier:
//   PALM_LARGE  — flat resting hand; suppress all input until lifted.
//   PALM_LOCAL  — thumb / palm edge; reject only this contact.
//   PALM_NONE   — normal finger; track it.
//
// FIX 1: The `tip` pre-filter in Pass 1 used to run BEFORE this function,
//   which meant contacts with touch_major < 100 were skipped entirely and
//   never classified as palms.  A palm resting flat can produce contacts
//   with lower touch_major values because the contact area is spread across
//   multiple hardware sensor segments.  Classification is now called
//   unconditionally for any contact with touch_major > 0.
//
// FIX 2: PALM_LARGE_MAJOR was 460 and PALM_SCORE_THRESH was 55.  A contact
//   with major=459 scored only 50 (major>=380 → +50) and therefore fell
//   through as PALM_NONE despite being clearly palm-sized.  The gap between
//   the LARGE threshold and the score path left a blind spot.  Fixed by:
//     - lowering PALM_LARGE_MAJOR to 380 so the score path handles 260-379,
//     - lowering PALM_SCORE_THRESH to 45 so major>=260 (+35) + any secondary
//       signal (aspect ratio or edge) tips it over.
//
// FIX 3: Edge proximity cast to USHORT was incorrect.  normX and normY are
//   already in [0, range] but edgePctX/edgePctY are INT.  Comparing INT
//   against USHORT via explicit cast hid potential sign-extension bugs and
//   made the condition unreadable.  All comparisons are now done as INT.
//
static PALM_CLASS
AmtClassifyPalm(
    _In_ const struct TRACKPAD_FINGER* f,
    _In_ const struct BCM5974_CONFIG*  devInfo,
    _In_ INT normX,
    _In_ INT normY)
{
    INT major = AmtRawToInteger(f->touch_major);
    INT minor = AmtRawToInteger(f->touch_minor);
    INT score = 0;

    // Zero contact — no palm.
    if (major <= 0 && minor <= 0)
        return PALM_NONE;

    // Flat resting hand: immediately suppress everything.
    if (major >= PALM_LARGE_MAJOR)
        return PALM_LARGE;

    // Size score (covers 260–379 after PALM_LARGE_MAJOR change).
    if      (major > 260) score += 35;
    else if (major > 190) score += 15;
    else if (major > 130) score +=  8;

    // Aspect ratio: a very elongated touch is likely a thumb/palm edge.
    if (minor > 0 && major > 120) {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

    // Edge proximity: contacts near the border are more likely palms.
    if (major > 130) {
        INT xRange   = devInfo->x.max - devInfo->x.min;
        INT yRange   = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 28;
        INT edgePctY = yRange / 28;

        // FIX 3: pure INT comparison, no USHORT cast.
        if (normX < edgePctX || normX > (xRange - edgePctX) ||
            normY < edgePctY || normY > (yRange - edgePctY))
            score += 10;
    }

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t   transferLength = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1: transferLength = HEADER_TYPE1 + FSIZE_TYPE1 * MAX_FINGERS; break;
    case TYPE2: transferLength = HEADER_TYPE2 + FSIZE_TYPE2 * MAX_FINGERS; break;
    case TYPE3: transferLength = HEADER_TYPE3 + FSIZE_TYPE3 * MAX_FINGERS; break;
    case TYPE4: transferLength = HEADER_TYPE4 + FSIZE_TYPE4 * MAX_FINGERS; break;
    case TYPE5: transferLength = HEADER_TYPE5 + FSIZE_TYPE5 * MAX_FINGERS; break;
    default:
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! Unknown tp_type %d", DeviceContext->DeviceInfo->tp_type);
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
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
        DeviceContext->InterruptPipe, &contReaderConfig);

    if (!NT_SUCCESS(status))
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfUsbTargetPipeConfigContinuousReader failed %!STATUS!", status);

exit:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return status;
}

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pCtx       = Context;
    size_t          headerSize = (unsigned int)pCtx->DeviceInfo->tp_header;
    size_t          fingerSize = (unsigned int)pCtx->DeviceInfo->tp_fsize;
    size_t          raw_n, i;
    UCHAR*          TouchBuffer = NULL;
    const struct TRACKPAD_FINGER* f = NULL;

    LONGLONG      PerfDelta;
    LARGE_INTEGER Now;
    NTSTATUS      Status;
    PTP_REPORT    Report;
    WDFREQUEST    Request;
    WDFMEMORY     RequestMemory;

    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Malformed packet, len=%llu", (ULONG64)NumBytesTransferred);
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! NULL buffer");
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request);
    if (!NT_SUCCESS(Status))
        return;  // No consumer — frame discarded, no leak.

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! RetrieveOutputMemory failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    KeQueryPerformanceCounter(&Now);
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;
    if (pCtx->PerfFrequency.QuadPart > 0)
        PerfDelta = PerfDelta * 10000LL / pCtx->PerfFrequency.QuadPart;
    else
        PerfDelta /= 100LL;
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    Report.ScanTime = (USHORT)PerfDelta;
    pCtx->LastReportTime = Now;

    // Typing suppression.
    {
        LONGLONG suppressUntil = InterlockedCompareExchange64(
            &pCtx->TypingSuppressUntil, 0, 0);

        if (suppressUntil > Now.QuadPart) {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! Typing suppression active");

            if (pCtx->PtpReportButton &&
                TouchBuffer[pCtx->DeviceInfo->tp_button])
                Report.IsButtonClicked = TRUE;

            UCHAR liftCount = 0;
            for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
                if (!pCtx->SlotReportedLastFrame[i]) continue;
                Report.Contacts[liftCount].ContactID  = (UCHAR)i;
                Report.Contacts[liftCount].TipSwitch  = 0;
                Report.Contacts[liftCount].Confidence = 1;
                Report.Contacts[liftCount].X          = pCtx->SmoothedX[i];
                Report.Contacts[liftCount].Y          = pCtx->SmoothedY[i];
                liftCount++;
                pCtx->SlotReportedLastFrame[i] = FALSE;
                AmtClearSlot(pCtx, i);
            }
            Report.ContactCount = liftCount;

            Status = WdfMemoryCopyFromBuffer(
                RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
            WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
            WdfRequestComplete(Request, NT_SUCCESS(Status) ? STATUS_SUCCESS : Status);
            return;
        }
    }

    raw_n = (NumBytesTransferred - headerSize) / fingerSize;
    UCHAR* f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;

    if (pCtx->PtpReportTouch)
    {
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        if (raw_n * fingerSize > (NumBytesTransferred - headerSize)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Buffer size mismatch");
            WdfRequestComplete(Request, STATUS_DATA_ERROR);
            return;
        }

        //
        // NOTE (pre-existing architectural limitation, not patched here):
        // `i` below is the raw USB record index for this frame, and is also
        // used directly as the persistent slot/ContactID for SmoothedX/Y,
        // HystX/Y, SlotActive, TipDropCount, SlotReportedLastFrame.  If the
        // hardware drops a finger's record so raw_n shrinks (e.g. 3 fingers
        // down -> one lifts -> raw_n goes 3 -> 2), the record that used to
        // be at index 2 is no longer at index 2; whatever finger is now at
        // index 1 inherits index 1's old smoothing baseline/hysteresis even
        // though it may be a physically different contact.  This is a
        // genuine identity-leak source of cursor snap, but fixing it
        // requires real slot matching (nearest-position assignment or a
        // persistent ContactID scheme, as already implemented for the SPI
        // variant's FSM) rather than a local patch, since it changes how
        // ContactID is assigned for the whole frame. Flagging explicitly
        // rather than silently reshuffling index semantics here.
        //

        // ----------------------------------------------------------------
        // Pass 1: classify all contacts.
        //
        // FIX: removed the `tip` pre-filter that guarded the AmtClassifyPalm
        // call.  The filter required touch_major >= 100, which excluded some
        // palm contacts from ever reaching the classifier.  Classification is
        // now done for every contact with any nonzero touch extent.
        // Contacts that are neither tip nor palm (touch_major == 0) are simply
        // left with alive[i] = FALSE, which is the same outcome as before.
        // ----------------------------------------------------------------
        BOOLEAN largePalm = FALSE;
        BOOLEAN alive[PTP_MAX_CONTACT_POINTS];
        INT     normXi[PTP_MAX_CONTACT_POINTS];  // INT to avoid USHORT cast bugs
        INT     normYi[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            alive[i]  = FALSE;
            normXi[i] = 0;
            normYi[i] = 0;
        }

        for (i = 0; i < raw_n; i++) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            INT major = AmtRawToInteger(f->touch_major);
            INT minor = AmtRawToInteger(f->touch_minor);

            // Skip completely silent slots (hardware placeholder rows).
            if (major <= 0 && minor <= 0) {
                // FIX: a slot going fully silent (0/0) is unambiguous
                // lift-off, not measurement noise -- reset its debounce
                // counter immediately so a future re-touch starts clean.
                pCtx->TipDropCount[i] = 0;
                continue;
            }

            INT nx = (INT)AmtClampCoord(AmtRawToInteger(f->abs_x),
                                        pCtx->DeviceInfo->x.min,
                                        pCtx->DeviceInfo->x.max);
            INT nyRaw = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
            INT ny    = (nyRaw > 0) ? nyRaw : 0;

            normXi[i] = nx;
            normYi[i] = ny;

            PALM_CLASS palm = AmtClassifyPalm(f, pCtx->DeviceInfo, nx, ny);

            if (palm == PALM_LARGE) {
                largePalm = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: LARGE palm (major=%d)", (ULONG64)i, major);
                // FIX: clear alive[] for ALL slots immediately so no contact
                // leaks through to Phase B while palm lock is active.
                for (size_t j = 0; j < PTP_MAX_CONTACT_POINTS; j++)
                    alive[j] = FALSE;
                break;
            }

            if (palm == PALM_LOCAL) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: local palm (major=%d, score reject)",
                    (ULONG64)i, major);
                continue;
            }

            // Require a minimum tip size to count as an intentional contact.
            // This replaces the old `tip` pre-filter but runs AFTER palm
            // classification so palms are never confused with small fingers.
            BOOLEAN tip = (major << 1) >= 200 || (minor << 1) >= 150;

            if (!tip) {
                //
                // FIX (cursor-jump): tip-size debounce.
                //
                // Previously, a single frame where major/minor dipped below
                // threshold (common sensor noise on T2 while a finger is
                // firmly down) caused `alive[i] = FALSE` here.  Phase A then
                // emitted an immediate lift-off for this slot, and the next
                // frame -- seeing the contact again -- re-armed it via
                // `!pCtx->SlotActive[i]`, resetting SmoothedX/Y to the new
                // raw position with NO smoothing applied.  That hard reset
                // is what produced the visible cursor jump/snap during
                // sustained contact.
                //
                // Fix: if this slot was already SlotActive (a real, ongoing
                // contact), tolerate up to TIP_DROP_DEBOUNCE_FRAMES
                // consecutive sub-threshold frames before declaring lift-off.
                // While in the debounce window we keep the slot alive and
                // simply hold position (no panic reset), so Phase B will
                // continue smoothing from the last known-good coordinate.
                // If the slot was NOT yet active (a brand-new low-confidence
                // touch), there is no smoothing state to protect, so it is
                // treated as not-yet-a-tip immediately, same as before.
                //
                if (pCtx->SlotActive[i] &&
                    pCtx->TipDropCount[i] < TIP_DROP_DEBOUNCE_FRAMES) {
                    pCtx->TipDropCount[i]++;
                    alive[i]  = TRUE;
                    // Reuse last known-good normalized position so Phase B's
                    // deadzone/smoothing sees a stable input instead of a
                    // noisy low-confidence sample.
                    normXi[i] = pCtx->SmoothedX[i];
                    normYi[i] = pCtx->SmoothedY[i];
                    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! slot %llu: tip below threshold, debounced (%u/%u)",
                        (ULONG64)i, pCtx->TipDropCount[i], TIP_DROP_DEBOUNCE_FRAMES);
                    continue;
                }

                // Debounce window exhausted (or never active) — genuine
                // non-tip / lift-off.
                pCtx->TipDropCount[i] = 0;
                continue;
            }

            // Healthy tip-sized contact — clear any accumulated debounce.
            pCtx->TipDropCount[i] = 0;
            alive[i] = TRUE;
        }

        if (largePalm) {
            pCtx->PalmDetected = TRUE;
        } else if (pCtx->PalmDetected) {
            //
            // Stay locked until ALL contacts are physically lifted.
            //
            // Previously the lock was released as soon as the large-palm
            // contact itself disappeared (major < PALM_LARGE_MAJOR).  That
            // meant a user could: 1) rest a palm on the pad (lock engaged),
            // 2) place another finger, 3) lift the palm — and the pad would
            // immediately unlock and process that other finger, causing an
            // unwanted cursor jump.
            //
            // Fix: while palm-locked, check for ANY nonzero contact in the
            // raw USB frame.  Only when every contact has lifted do we
            // release the lock and allow tracking again.
            //
            // IMPORTANT: alive[] was already populated by the classification
            // loop above.  If the lock remains active (anyContact == TRUE),
            // we MUST force alive[] to all-FALSE here, otherwise Phase B
            // will process those contacts and the cursor will move despite
            // the palm lock.
            //
            BOOLEAN anyContact = FALSE;
            for (i = 0; i < raw_n; i++) {
                f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);
                INT major = AmtRawToInteger(f->touch_major);
                INT minor = AmtRawToInteger(f->touch_minor);
                if (major > 0 || minor > 0) {
                    anyContact = TRUE;
                    break;
                }
            }
            if (!anyContact) {
                pCtx->PalmDetected = FALSE;
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    AmtClearSlot(pCtx, i);
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! Palm released — resuming");
            } else {
                // Lock still active — suppress all contacts that the
                // classification loop may have marked as alive.
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    alive[i] = FALSE;
            }
        }

        // ----------------------------------------------------------------
        // Pass 2: emit lift-offs (Phase A) then active contacts (Phase B).
        // ----------------------------------------------------------------
        UCHAR contactCount = 0;

        // Phase A: lift-off for contacts that ended this frame.
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (!pCtx->SlotReportedLastFrame[i] || alive[i])
                continue;

            if (contactCount >= PTP_MAX_CONTACT_POINTS) break;

            Report.Contacts[contactCount].ContactID  = (UCHAR)i;
            Report.Contacts[contactCount].TipSwitch  = 0;
            Report.Contacts[contactCount].Confidence = 1;
            Report.Contacts[contactCount].X          = pCtx->SmoothedX[i];
            Report.Contacts[contactCount].Y          = pCtx->SmoothedY[i];
            contactCount++;

            pCtx->SlotReportedLastFrame[i] = FALSE;
            AmtClearSlot(pCtx, i);

            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot %llu: lift-off", (ULONG64)i);
        }

        // Phase B: active contacts with smoothing.
        for (i = 0; i < raw_n; i++) {
            if (!alive[i]) continue;

            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            USHORT nx = (USHORT)normXi[i];
            USHORT ny = (USHORT)normYi[i];

            if (!pCtx->SlotActive[i]) {
                pCtx->SmoothedX[i]  = nx;
                pCtx->SmoothedY[i]  = ny;
                pCtx->HystX[i]      = nx;
                pCtx->HystY[i]      = ny;
                pCtx->SlotActive[i] = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: first touch x=%u y=%u", (ULONG64)i, nx, ny);
            }

            USHORT dzX  = AmtApplyDeadzone(nx, &pCtx->HystX[i]);
            USHORT dzY  = AmtApplyDeadzone(ny, &pCtx->HystY[i]);
            USHORT repX = AmtSmoothCoord(dzX, pCtx->SmoothedX[i]);
            USHORT repY = AmtSmoothCoord(dzY, pCtx->SmoothedY[i]);
            pCtx->SmoothedX[i] = repX;
            pCtx->SmoothedY[i] = repY;

            if (contactCount < PTP_MAX_CONTACT_POINTS) {
                Report.Contacts[contactCount].ContactID  = (UCHAR)i;
                Report.Contacts[contactCount].X          = repX;
                Report.Contacts[contactCount].Y          = repY;
                Report.Contacts[contactCount].TipSwitch  = 1;
                Report.Contacts[contactCount].Confidence =
                    (AmtRawToInteger(f->touch_minor) << 1) > 0 ? 1 : 0;
                contactCount++;
                pCtx->SlotReportedLastFrame[i] = TRUE;
            }
        }

        // Lift-off for slots that hardware silently dropped (raw_n shrank).
        for (i = raw_n; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (!pCtx->SlotReportedLastFrame[i]) continue;
            if (contactCount < PTP_MAX_CONTACT_POINTS) {
                Report.Contacts[contactCount].ContactID  = (UCHAR)i;
                Report.Contacts[contactCount].TipSwitch  = 0;
                Report.Contacts[contactCount].Confidence = 1;
                Report.Contacts[contactCount].X          = pCtx->SmoothedX[i];
                Report.Contacts[contactCount].Y          = pCtx->SmoothedY[i];
                contactCount++;
            }
            pCtx->SlotReportedLastFrame[i] = FALSE;
            AmtClearSlot(pCtx, i);
        }

        Report.ContactCount = contactCount;
    }

    if (pCtx->PtpReportButton && TouchBuffer[pCtx->DeviceInfo->tp_button]) {
        Report.IsButtonClicked = TRUE;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT, "%!FUNC! Button clicked");
    }

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);
    return TRUE;
}