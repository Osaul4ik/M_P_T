// Interrupt.c: Handles device input event

#include "Driver.h"
#include "Interrupt.tmh"

#define XY_DEADZONE_UNITS   2
#define SMOOTHING_ALPHA_NUM 5
#define SMOOTHING_ALPHA_DEN 8

#define PALM_LARGE_MAJOR    380
#define PALM_SCORE_THRESH   45
#define TIP_DROP_DEBOUNCE_FRAMES 2
#define TIP_DROP_MAX_REPOSITION_DELTA 300

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

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

//
// AmtClearSlot — reset all per-slot state on lift-off.
//
// FIX (cursor-jump after gesture + re-tap):
// Rotate ContactIdForSlot BEFORE clearing so the lift-off report (emitted
// by the caller just before AmtClearSlot) still carries the OLD ContactID,
// and the NEXT touch-down on this slot gets a fresh ID that Windows has
// never seen at a different position.
//
static inline VOID
AmtClearSlot(_In_ PDEVICE_CONTEXT ctx, _In_ size_t i)
{
    // Rotate ContactID so next touch-down on this slot gets a new ID.
    // (CONTACT_ID_POOL is PTP_MAX_CONTACT_POINTS*2 = 10, giving IDs 0-9)
    ctx->ContactIdForSlot[i] = (UCHAR)((ctx->ContactIdForSlot[i] + PTP_MAX_CONTACT_POINTS)
                                        % CONTACT_ID_POOL);

    ctx->SmoothedX[i]        = 0;
    ctx->SmoothedY[i]        = 0;
    ctx->HystX[i]            = 0;
    ctx->HystY[i]            = 0;
    ctx->SlotActive[i]       = FALSE;
    ctx->TipDropCount[i]     = 0;
    ctx->SlotWasInGesture[i] = FALSE;
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
                // Use the CURRENT ContactID for the lift-off report;
                // AmtClearSlot will rotate it for the next touch-down.
                Report.Contacts[liftCount].ContactID  = pCtx->ContactIdForSlot[i];
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
                pCtx->TipDropCount[i] = 0;
                continue;
            }

            INT nx = (INT)AmtClampCoord(AmtRawToInteger(f->abs_x),
                                        pCtx->DeviceInfo->x.min,
                                        pCtx->DeviceInfo->x.max);

            INT yRange = pCtx->DeviceInfo->y.max - pCtx->DeviceInfo->y.min;
            INT nyRaw  = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
            INT ny     = (nyRaw < 0) ? 0 : (nyRaw > yRange ? yRange : nyRaw);

            normXi[i] = nx;
            normYi[i] = ny;

            PALM_CLASS palm = AmtClassifyPalm(f, pCtx->DeviceInfo, nx, ny);

            if (palm == PALM_LARGE) {
                largePalm = TRUE;
                for (size_t j = 0; j < PTP_MAX_CONTACT_POINTS; j++) {
                    alive[j]              = FALSE;
                    pCtx->TipDropCount[j] = 0;
                }
                break;
            }

            if (palm == PALM_LOCAL) continue;

            BOOLEAN tip = (major << 1) >= 200 || (minor << 1) >= 150;

            if (!tip) {
                INT dxAbs = nx - (INT)pCtx->SmoothedX[i];
                if (dxAbs < 0) dxAbs = -dxAbs;
                INT dyAbs = ny - (INT)pCtx->SmoothedY[i];
                if (dyAbs < 0) dyAbs = -dyAbs;

                BOOLEAN samePositionAsBefore =
                    (dxAbs <= TIP_DROP_MAX_REPOSITION_DELTA) &&
                    (dyAbs <= TIP_DROP_MAX_REPOSITION_DELTA);

                if (pCtx->SlotActive[i] && samePositionAsBefore &&
                    pCtx->TipDropCount[i] < TIP_DROP_DEBOUNCE_FRAMES) {
                    pCtx->TipDropCount[i]++;
                    alive[i]  = TRUE;
                    normXi[i] = pCtx->SmoothedX[i];
                    normYi[i] = pCtx->SmoothedY[i];
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
                if (AmtRawToInteger(f->touch_major) > 0 ||
                    AmtRawToInteger(f->touch_minor) > 0) {
                    anyContact = TRUE;
                    break;
                }
            }
            if (!anyContact) {
                pCtx->PalmDetected = FALSE;
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    AmtClearSlot(pCtx, i);
            } else {
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
                    alive[i] = FALSE;
            }
        }

        // Count alive slots to detect gesture frames.
        UCHAR aliveCount = 0;
        for (i = 0; i < raw_n; i++) {
            if (alive[i]) aliveCount++;
        }

        // Mark gesture taint on all slots alive in a multi-finger frame.
        if (aliveCount >= 2) {
            for (i = 0; i < raw_n; i++) {
                if (alive[i])
                    pCtx->SlotWasInGesture[i] = TRUE;
            }
        }

        UCHAR contactCount = 0;

        // Phase A: lift-off for contacts that ended this frame.
        for (i = 0; i < raw_n; i++) {
            if (!pCtx->SlotReportedLastFrame[i] || alive[i])
                continue;
            if (contactCount >= PTP_MAX_CONTACT_POINTS) break;

            // Emit lift-off with the CURRENT ContactID before rotating it.
            Report.Contacts[contactCount].ContactID  = pCtx->ContactIdForSlot[i];
            Report.Contacts[contactCount].TipSwitch  = 0;
            Report.Contacts[contactCount].Confidence = 1;
            Report.Contacts[contactCount].X          = pCtx->SmoothedX[i];
            Report.Contacts[contactCount].Y          = pCtx->SmoothedY[i];
            contactCount++;

            pCtx->SlotReportedLastFrame[i] = FALSE;
            AmtClearSlot(pCtx, i);  // rotates ContactIdForSlot[i]

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
                // Fresh touch-down: initialise smoothing baseline.
                // ContactIdForSlot[i] was already rotated by AmtClearSlot
                // on the preceding lift-off, so this new contact gets a
                // fresh ID that Windows has never seen at another position.
                if (pCtx->SlotWasInGesture[i]) {
                    pCtx->SlotWasInGesture[i] = FALSE;
                    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! slot %llu: gesture-tainted re-use, "
                        "new ContactID=%u x=%u y=%u",
                        (ULONG64)i, pCtx->ContactIdForSlot[i], nx, ny);
                }
                pCtx->SmoothedX[i]  = nx;
                pCtx->SmoothedY[i]  = ny;
                pCtx->HystX[i]      = nx;
                pCtx->HystY[i]      = ny;
                pCtx->SlotActive[i] = TRUE;
            }

            USHORT dzX = AmtApplyDeadzone(nx, &pCtx->HystX[i]);
            USHORT dzY = AmtApplyDeadzone(ny, &pCtx->HystY[i]);

            // First single-finger frame after a gesture: skip smoothing to
            // avoid blending against the stale gesture position.
            USHORT repX, repY;
            if (pCtx->SlotWasInGesture[i] && aliveCount == 1) {
                repX = dzX;
                repY = dzY;
                pCtx->SmoothedX[i]        = repX;
                pCtx->SmoothedY[i]        = repY;
                pCtx->SlotWasInGesture[i] = FALSE;
            } else {
                repX = AmtSmoothCoord(dzX, pCtx->SmoothedX[i]);
                repY = AmtSmoothCoord(dzY, pCtx->SmoothedY[i]);
                pCtx->SmoothedX[i] = repX;
                pCtx->SmoothedY[i] = repY;
            }

            if (contactCount < PTP_MAX_CONTACT_POINTS) {
                Report.Contacts[contactCount].ContactID  = pCtx->ContactIdForSlot[i];
                Report.Contacts[contactCount].X          = repX;
                Report.Contacts[contactCount].Y          = repY;
                Report.Contacts[contactCount].TipSwitch  = 1;
                Report.Contacts[contactCount].Confidence =
                    (pCtx->TipDropCount[i] > 0)
                        ? 1
                        : ((AmtRawToInteger(f->touch_minor) << 1) > 0 ? 1 : 0);
                contactCount++;
                pCtx->SlotReportedLastFrame[i] = TRUE;
            }
        }

        // Lift-off for slots hardware silently dropped (raw_n shrank).
        for (i = raw_n; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (!pCtx->SlotReportedLastFrame[i]) continue;

            if (contactCount < PTP_MAX_CONTACT_POINTS) {
                Report.Contacts[contactCount].ContactID  = pCtx->ContactIdForSlot[i];
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