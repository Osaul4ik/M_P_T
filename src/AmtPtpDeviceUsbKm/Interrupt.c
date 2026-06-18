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
// Number of consecutive frames a SlotActive contact is allowed to dip below
// the `tip` size threshold before it is treated as lifted.
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
    ctx->TipDropCount[i]  = 0;
}

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

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

    if (major <= 0 && minor <= 0)
        return PALM_NONE;

    if (major >= PALM_LARGE_MAJOR)
        return PALM_LARGE;

    if      (major > 260) score += 35;
    else if (major > 190) score += 15;
    else if (major > 130) score +=  8;

    if (minor > 0 && major > 120) {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

    if (major > 130) {
        INT xRange   = devInfo->x.max - devInfo->x.min;
        INT yRange   = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 28;
        INT edgePctY = yRange / 28;

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
        return;

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

        BOOLEAN largePalm = FALSE;
        BOOLEAN alive[PTP_MAX_CONTACT_POINTS];
        INT     normXi[PTP_MAX_CONTACT_POINTS];
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

            if (major <= 0 && minor <= 0) {
                // Unambiguous lift-off — reset debounce immediately.
                pCtx->TipDropCount[i] = 0;
                continue;
            }

            INT nx = (INT)AmtClampCoord(AmtRawToInteger(f->abs_x),
                                        pCtx->DeviceInfo->x.min,
                                        pCtx->DeviceInfo->x.max);

            // FIX BUG-2: clamp nyRaw to [0, y.max - y.min] to prevent
            // overflow when abs_y < y.min (hardware out-of-range reading).
            INT yRange = pCtx->DeviceInfo->y.max - pCtx->DeviceInfo->y.min;
            INT nyRaw  = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
            INT ny     = (nyRaw < 0) ? 0 : (nyRaw > yRange ? yRange : nyRaw);

            normXi[i] = nx;
            normYi[i] = ny;

            PALM_CLASS palm = AmtClassifyPalm(f, pCtx->DeviceInfo, nx, ny);

            if (palm == PALM_LARGE) {
                largePalm = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: LARGE palm (major=%d)", (ULONG64)i, major);

                // FIX BUG-5: reset TipDropCount for ALL slots when a large
                // palm is detected, including slots not yet visited by this
                // loop iteration, so future touches start with clean state.
                for (size_t j = 0; j < PTP_MAX_CONTACT_POINTS; j++) {
                    alive[j]             = FALSE;
                    pCtx->TipDropCount[j] = 0;
                }
                break;
            }

            if (palm == PALM_LOCAL) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: local palm (major=%d, score reject)",
                    (ULONG64)i, major);
                continue;
            }

            BOOLEAN tip = (major << 1) >= 200 || (minor << 1) >= 150;

            if (!tip) {
                if (pCtx->SlotActive[i] &&
                    pCtx->TipDropCount[i] < TIP_DROP_DEBOUNCE_FRAMES) {
                    pCtx->TipDropCount[i]++;
                    alive[i]  = TRUE;
                    normXi[i] = pCtx->SmoothedX[i];
                    normYi[i] = pCtx->SmoothedY[i];
                    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! slot %llu: tip below threshold, debounced (%u/%u)",
                        (ULONG64)i, pCtx->TipDropCount[i], TIP_DROP_DEBOUNCE_FRAMES);
                    continue;
                }

                pCtx->TipDropCount[i] = 0;
                continue;
            }

            pCtx->TipDropCount[i] = 0;
            alive[i] = TRUE;
        }

        if (largePalm) {
            pCtx->PalmDetected = TRUE;
        } else if (pCtx->PalmDetected) {
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
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    alive[i] = FALSE;
            }
        }

        // ----------------------------------------------------------------
        // Pass 2: emit lift-offs (Phase A) then active contacts (Phase B).
        //
        // FIX BUG-3: Phase A now only covers slots [0 .. raw_n-1].
        // Slots [raw_n .. PTP_MAX_CONTACT_POINTS-1] are handled exclusively
        // in the "silently dropped" block below.  Previously both Phase A
        // and the silently-dropped block could emit lift-off for the same
        // slot (alive[i] == FALSE for i >= raw_n in both paths), causing
        // double lift-off reports for every slot beyond raw_n.
        // ----------------------------------------------------------------
        UCHAR contactCount = 0;

        // Phase A: lift-off for contacts within raw_n that ended this frame.
        for (i = 0; i < raw_n; i++) {
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

                // FIX BUG-4: for debounce-alive slots normXi/normYi hold the
                // last-good smoothed position, not live hardware data, so
                // f->touch_minor from a sub-threshold frame is unreliable.
                // Use the previous Confidence value (SlotActive == TRUE means
                // at least one good frame was seen) rather than reading noisy
                // touch_minor.  For a real healthy contact the live value is
                // fine.
                if (pCtx->TipDropCount[i] > 0) {
                    // Debounce frame: hold last-good confidence (minor > 0).
                    Report.Contacts[contactCount].Confidence = 1;
                } else {
                    Report.Contacts[contactCount].Confidence =
                        (AmtRawToInteger(f->touch_minor) << 1) > 0 ? 1 : 0;
                }
                contactCount++;
                pCtx->SlotReportedLastFrame[i] = TRUE;
            }
        }

        // FIX BUG-1: lift-off for slots that hardware silently dropped
        // (raw_n shrank).  The slot state MUST be cleared unconditionally
        // regardless of whether there is room in the report buffer.  If we
        // skip AmtClearSlot / SlotReportedLastFrame reset when contactCount
        // hits the limit, the slot stays "reported" forever and generates a
        // stale lift-off on every subsequent frame.
        for (i = raw_n; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (!pCtx->SlotReportedLastFrame[i]) continue;

            if (contactCount < PTP_MAX_CONTACT_POINTS) {
                Report.Contacts[contactCount].ContactID  = (UCHAR)i;
                Report.Contacts[contactCount].TipSwitch  = 0;
                Report.Contacts[contactCount].Confidence = 1;
                Report.Contacts[contactCount].X          = pCtx->SmoothedX[i];
                Report.Contacts[contactCount].Y          = pCtx->SmoothedY[i];
                contactCount++;
            } else {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
                    "%!FUNC! slot %llu: silently-dropped lift-off lost (buffer full)",
                    (ULONG64)i);
            }

            // Always clear state even if we couldn't fit the report entry.
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