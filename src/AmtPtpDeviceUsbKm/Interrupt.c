// Interrupt.c: Handles device input event

#include "Driver.h"
#include "Interrupt.tmh"

#define XY_DEADZONE_UNITS   2
#define SMOOTHING_ALPHA_NUM 5
#define SMOOTHING_ALPHA_DEN 8
#define PALM_LARGE_MAJOR    460
#define PALM_SCORE_THRESH   55

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

// Clamp raw device coordinate into [0, max-min] range as USHORT.
// Prevents USHORT wrap-around for TYPE4 devices where x.min is negative.
static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT shifted = raw - minVal;
    if (shifted < 0)              shifted = 0;
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

// Reset all per-slot state for slot i. Centralised to avoid divergent paths.
static inline VOID
AmtClearSlot(_In_ PDEVICE_CONTEXT ctx, _In_ size_t i)
{
    ctx->SmoothedX[i]  = 0;
    ctx->SmoothedY[i]  = 0;
    ctx->HystX[i]      = 0;
    ctx->HystY[i]      = 0;
    ctx->SlotActive[i] = FALSE;
}

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

static PALM_CLASS
AmtClassifyPalm(
    _In_ const struct TRACKPAD_FINGER* f,
    _In_ const struct BCM5974_CONFIG*  devInfo,
    _In_ USHORT normX,
    _In_ USHORT normY)
{
    INT score = 0;

    if (AmtRawToInteger(f->touch_major) >= PALM_LARGE_MAJOR)
        return PALM_LARGE;

    INT major = AmtRawToInteger(f->touch_major);
    if      (major >= 380) score += 50;
    else if (major >  260) score += 35;
    else if (major >  190) score += 15;

    INT minor = AmtRawToInteger(f->touch_minor);
    if (minor > 0 && major > 120) {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

    if (major > 150) {
        INT xRange   = devInfo->x.max - devInfo->x.min;
        INT yRange   = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 28;
        INT edgePctY = yRange / 28;

        if (normX < (USHORT)edgePctX || normX > (USHORT)(xRange - edgePctX) ||
            normY < (USHORT)edgePctY || normY > (USHORT)(yRange - edgePctY))
            score += 8;
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

    PDEVICE_CONTEXT pCtx           = Context;
    size_t          headerSize     = (unsigned int)pCtx->DeviceInfo->tp_header;
    size_t          fingerSize     = (unsigned int)pCtx->DeviceInfo->tp_fsize;
    size_t          raw_n, i;
    UCHAR*          TouchBuffer    = NULL;
    const struct TRACKPAD_FINGER* f = NULL;

    LONGLONG        PerfDelta;
    LARGE_INTEGER   Now;
    NTSTATUS        Status;
    PTP_REPORT      Report;
    WDFREQUEST      Request;
    WDFMEMORY       RequestMemory;

    // Basic packet sanity.
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

    // Dequeue the pending HID read. Every path below must call WdfRequestComplete.
    Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request);
    if (!NT_SUCCESS(Status)) {
        // No consumer waiting — frame is discarded, no leak.
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! RetrieveOutputMemory failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    // Build report skeleton.
    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    // ScanTime in 100-µs units.
    KeQueryPerformanceCounter(&Now);
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;
    if (pCtx->PerfFrequency.QuadPart > 0)
        PerfDelta = PerfDelta * 10000LL / pCtx->PerfFrequency.QuadPart;
    else
        PerfDelta /= 100LL;
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    Report.ScanTime = (USHORT)PerfDelta;
    pCtx->LastReportTime = Now;

    //
    // Typing suppression: emit a zero-contact frame (but keep button state).
    // We must also emit lift-off for any contacts that were live last frame,
    // so Windows can finalize the gesture before suppression takes over.
    //
    {
        LONGLONG suppressUntil = InterlockedCompareExchange64(
            &pCtx->TypingSuppressUntil, 0, 0);

        if (suppressUntil > Now.QuadPart) {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! Typing suppression active");

            if (pCtx->PtpReportButton &&
                TouchBuffer[pCtx->DeviceInfo->tp_button])
                Report.IsButtonClicked = TRUE;

            //
            // FIX (inertia): emit lift-off for every slot that was live in
            // the previous frame so Windows finalises the gesture properly
            // before we start dropping input.
            //
            UCHAR liftCount = 0;
            for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
                if (!pCtx->SlotReportedLastFrame[i]) continue;
                Report.Contacts[liftCount].ContactID  = (UCHAR)i;
                Report.Contacts[liftCount].TipSwitch  = 0;  // lift-off
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

        // ----------------------------------------------------------------
        // Pass 1: classify fingers. All arrays are fully initialised here
        // so Pass 2 never reads uninitialised memory.
        // ----------------------------------------------------------------
        BOOLEAN largePalm = FALSE;
        BOOLEAN alive[PTP_MAX_CONTACT_POINTS];
        USHORT  normX[PTP_MAX_CONTACT_POINTS];
        USHORT  normY[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            alive[i] = FALSE;
            normX[i] = 0;
            normY[i] = 0;
        }

        for (i = 0; i < raw_n; i++) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            BOOLEAN tip = (AmtRawToInteger(f->touch_major) << 1) >= 200 ||
                          (AmtRawToInteger(f->touch_minor) << 1) >= 150;
            if (!tip) continue;

            // FIX: clamp to prevent USHORT wrap for negative raw coords.
            USHORT nx = AmtClampCoord(AmtRawToInteger(f->abs_x),
                                      pCtx->DeviceInfo->x.min,
                                      pCtx->DeviceInfo->x.max);
            INT nyRaw = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
            USHORT ny = (USHORT)(nyRaw > 0 ? nyRaw : 0);

            normX[i] = nx;
            normY[i] = ny;

            PALM_CLASS palm = AmtClassifyPalm(f, pCtx->DeviceInfo, nx, ny);
            if (palm == PALM_LARGE) {
                largePalm = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: LARGE palm", (ULONG64)i);
                break;
            }
            if (palm == PALM_LOCAL) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: local palm reject", (ULONG64)i);
                continue;
            }

            alive[i] = TRUE;
        }

        if (largePalm) {
            pCtx->PalmDetected = TRUE;
            // alive[] stays all-FALSE.
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
            }
            // While palm still present, alive[] stays all-FALSE →
            // lift-off logic below will close any open contacts.
        }

        // ----------------------------------------------------------------
        // Pass 2: build contacts.
        //
        // FIX (inertia + teleport): two-phase output.
        //
        // Phase A — lift-off: for every slot that was in the previous report
        //   with TipSwitch=1 but is NOT alive this frame, emit a final entry
        //   with TipSwitch=0 at the last known position.  This gives Windows
        //   PTP the clean finger-up event it needs to launch inertia.
        //
        // Phase B — active contacts: slots that are alive this frame get
        //   their normal smoothed position with TipSwitch=1.
        //
        // Ordering: lift-offs first, then live contacts.  ContactCount covers
        // both.  This matches the behaviour expected by HIDClass / PTP.
        //
        // FIX (teleport on gesture change): ContactID = slot index (i) is
        //   preserved across frames.  A slot that transitions alive→dead in
        //   one step is emitted with TipSwitch=0 (not silently dropped), so
        //   Windows never sees a ContactID appear or vanish without a matching
        //   down/up pair.
        // ----------------------------------------------------------------
        UCHAR contactCount = 0;

        // Phase A: lift-off for contacts that ended this frame.
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (!pCtx->SlotReportedLastFrame[i] || alive[i])
                continue;  // not previously reported, or still alive — skip

            if (contactCount >= PTP_MAX_CONTACT_POINTS) break;

            // Emit lift-off at last known smoothed position.
            Report.Contacts[contactCount].ContactID  = (UCHAR)i;
            Report.Contacts[contactCount].TipSwitch  = 0;
            Report.Contacts[contactCount].Confidence = 1;
            Report.Contacts[contactCount].X          = pCtx->SmoothedX[i];
            Report.Contacts[contactCount].Y          = pCtx->SmoothedY[i];
            contactCount++;

            // Mark slot closed.
            pCtx->SlotReportedLastFrame[i] = FALSE;
            AmtClearSlot(pCtx, i);

            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot %llu: lift-off", (ULONG64)i);
        }

        // Phase B: active contacts with smoothing.
        for (i = 0; i < raw_n; i++) {
            if (!alive[i]) continue;

            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            USHORT nx = normX[i];
            USHORT ny = normY[i];

            //
            // FIX (teleport on first touch):
            // Seed HystX/Y AND SmoothedX/Y from the actual first-frame
            // position so deadzone and EMA start from the correct baseline.
            // Previously HystX/Y could hold a stale value from the previous
            // touch on this slot, causing a deadzone "jump" on contact.
            //
            if (!pCtx->SlotActive[i]) {
                pCtx->SmoothedX[i]  = nx;
                pCtx->SmoothedY[i]  = ny;
                pCtx->HystX[i]      = nx;
                pCtx->HystY[i]      = ny;
                pCtx->SlotActive[i] = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot %llu: first touch x=%u y=%u", (ULONG64)i, nx, ny);
            }

            USHORT dzX    = AmtApplyDeadzone(nx, &pCtx->HystX[i]);
            USHORT dzY    = AmtApplyDeadzone(ny, &pCtx->HystY[i]);
            USHORT repX   = AmtSmoothCoord(dzX, pCtx->SmoothedX[i]);
            USHORT repY   = AmtSmoothCoord(dzY, pCtx->SmoothedY[i]);
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

        // Clear SlotReportedLastFrame for slots beyond raw_n that are no
        // longer mentioned by the hardware at all.
        for (i = raw_n; i < PTP_MAX_CONTACT_POINTS; i++) {
            if (pCtx->SlotReportedLastFrame[i]) {
                // Hardware dropped this slot without a tip=FALSE transition.
                // Emit lift-off if we still have room; clear the slot.
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