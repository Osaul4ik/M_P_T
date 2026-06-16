// Interrupt.c: Handles device input event

#include "Driver.h"
#include "Interrupt.tmh"

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

// Deadzone: ignore movement smaller than this many raw units.
#define XY_DEADZONE_UNITS   2

// EMA smoothing: newVal * NUM/DEN + prevVal * (DEN-NUM)/DEN
// 5/8 = 62.5% toward new value each frame — responsive but smooth.
#define SMOOTHING_ALPHA_NUM 5
#define SMOOTHING_ALPHA_DEN 8

// Palm thresholds (touch_major raw units, range ~0-2048)
#define PALM_LARGE_MAJOR    460     // flat resting palm → suppress ALL input
#define PALM_SCORE_THRESH   55      // per-contact score threshold → drop THIS finger only

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

// Apply deadzone: if new value is within XY_DEADZONE_UNITS of baseline, hold
// the baseline. Otherwise update baseline and return new value.
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

// EMA low-pass filter.
static inline USHORT
AmtSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal)
{
    INT blended = ((INT)rawVal * SMOOTHING_ALPHA_NUM +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

// Palm classification result.
typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

// Two-tier palm classifier (mirrors macOS behaviour):
//   PALM_LARGE  → flat resting hand, suppress everything.
//   PALM_LOCAL  → thumb / palm edge, reject only this contact.
//   PALM_NONE   → normal finger, track it.
static PALM_CLASS
AmtClassifyPalm(
    _In_ const struct TRACKPAD_FINGER* f,
    _In_ const struct BCM5974_CONFIG*  devInfo,
    _In_ USHORT normX,
    _In_ USHORT normY)
{
    INT score = 0;

    // Flat palm: freeze the whole pad until all fingers lift.
    if (AmtRawToInteger(f->touch_major) >= PALM_LARGE_MAJOR)
        return PALM_LARGE;

    // Size score.
    INT major = AmtRawToInteger(f->touch_major);
    if      (major >= 380) score += 50;
    else if (major >  260) score += 35;
    else if (major >  190) score += 15;

    // Aspect ratio score.
    INT minor = AmtRawToInteger(f->touch_minor);
    if (minor > 0 && major > 120)
    {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

    // Edge proximity bonus (contacts near the border are more likely palms).
    if (major > 150)
    {
        INT xRange   = devInfo->x.max - devInfo->x.min;
        INT yRange   = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 28;
        INT edgePctY = yRange / 28;

        if (normX < edgePctX || normX > (xRange - edgePctX) ||
            normY < edgePctY || normY > (yRange - edgePctY))
        {
            score += 8;
        }
    }

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}

// ---------------------------------------------------------------------------
// AmtPtpConfigContReaderForInterruptEndPoint
// ---------------------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t transferLength = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type)
    {
    case TYPE1: transferLength = HEADER_TYPE1 + FSIZE_TYPE1 * MAX_FINGERS; break;
    case TYPE2: transferLength = HEADER_TYPE2 + FSIZE_TYPE2 * MAX_FINGERS; break;
    case TYPE3: transferLength = HEADER_TYPE3 + FSIZE_TYPE3 * MAX_FINGERS; break;
    case TYPE4: transferLength = HEADER_TYPE4 + FSIZE_TYPE4 * MAX_FINGERS; break;
    case TYPE5: transferLength = HEADER_TYPE5 + FSIZE_TYPE5 * MAX_FINGERS; break;
    }

    if (transferLength <= 0) {
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
            "%!FUNC! AmtPtpConfigContReaderForInterruptEndPoint failed with Status code %!STATUS!", status);
        goto exit;
    }

exit:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// AmtPtpEvtUsbInterruptPipeReadComplete
// ---------------------------------------------------------------------------

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pDeviceContext = Context;
    size_t headerSize      = (unsigned int)pDeviceContext->DeviceInfo->tp_header;
    size_t fingerprintSize = (unsigned int)pDeviceContext->DeviceInfo->tp_fsize;
    size_t raw_n, i;
    UCHAR* TouchBuffer = NULL;
    const struct TRACKPAD_FINGER* f = NULL;

    LONGLONG       PerfCounterDelta;
    LARGE_INTEGER  CurrentPerfCounter;
    NTSTATUS       Status;
    PTP_REPORT     PtpReport;
    WDFREQUEST     Request;
    WDFMEMORY      RequestMemory;

    // Basic sanity check on packet length.
    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerprintSize != 0)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Malformed input received. Length = %llu", NumBytesTransferred);
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Failed to retrieve packet");
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->InputQueue, &Request);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! No pending PTP request. Disposed");
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestRetrieveOutputMemory failed %!STATUS!", Status);
        return;
    }

    // Prepare report skeleton.
    RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
    PtpReport.ReportID       = REPORTID_MULTITOUCH;
    PtpReport.IsButtonClicked = 0;

    raw_n  = (NumBytesTransferred - headerSize) / fingerprintSize;
    UCHAR* f_base = TouchBuffer + headerSize + pDeviceContext->DeviceInfo->tp_delta;

    KeQueryPerformanceCounter(&CurrentPerfCounter);
    PerfCounterDelta = (CurrentPerfCounter.QuadPart -
                        pDeviceContext->LastReportTime.QuadPart) / 100;
    if (PerfCounterDelta > 0xFF) PerfCounterDelta = 0xFF;
    PtpReport.ScanTime = (USHORT)PerfCounterDelta;

    if (pDeviceContext->PtpReportTouch)
    {
        if (raw_n >= PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        if (raw_n * fingerprintSize < (NumBytesTransferred - headerSize)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Buffer may have a problem");
            WdfRequestComplete(Request, STATUS_DATA_ERROR);
            return;
        }

        // ----------------------------------------------------------------
        // Pass 1: palm classification
        //
        // Check for a LARGE flat palm first — if found, suppress everything
        // and hold the lock until ALL contacts physically lift.
        // ----------------------------------------------------------------
        BOOLEAN largePalmThisFrame = FALSE;
        BOOLEAN fingerAlive[PTP_MAX_CONTACT_POINTS];
        USHORT  normXArr[PTP_MAX_CONTACT_POINTS];
        USHORT  normYArr[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < raw_n; i++) fingerAlive[i] = FALSE;

        for (i = 0; i < raw_n; i++)
        {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerprintSize);

            // Tip-down test (same threshold as original code).
            BOOLEAN tip = (AmtRawToInteger(f->touch_major) << 1) >= 200 ||
                          (AmtRawToInteger(f->touch_minor) << 1) >= 150;
            if (!tip) continue;

            // Normalise coordinates (Y is flipped, same as original).
            USHORT nx = (USHORT)((AmtRawToInteger(f->abs_x) - pDeviceContext->DeviceInfo->x.min) > 0
                        ? (AmtRawToInteger(f->abs_x) - pDeviceContext->DeviceInfo->x.min) : 0);
            INT   nyRaw = pDeviceContext->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
            USHORT ny = (USHORT)(nyRaw > 0 ? nyRaw : 0);
            normXArr[i] = nx;
            normYArr[i] = ny;

            PALM_CLASS palm = AmtClassifyPalm(f, pDeviceContext->DeviceInfo, nx, ny);

            if (palm == PALM_LARGE) {
                largePalmThisFrame = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! finger %llu: LARGE palm — global suppress", (ULONG64)i);
                break;  // no need to check further
            }

            if (palm == PALM_LOCAL) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! finger %llu: per-contact palm reject", (ULONG64)i);
                continue;   // drop only this finger
            }

            fingerAlive[i] = TRUE;
        }

        // Palm-lock gate.
        if (largePalmThisFrame)
        {
            pDeviceContext->PalmDetected = TRUE;
            for (i = 0; i < raw_n; i++) fingerAlive[i] = FALSE;
        }
        else if (pDeviceContext->PalmDetected)
        {
            // Release only when the surface is completely clear.
            BOOLEAN anyContact = FALSE;
            for (i = 0; i < raw_n; i++) {
                f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerprintSize);
                if (AmtRawToInteger(f->touch_major) > 0 || AmtRawToInteger(f->touch_minor) > 0) {
                    anyContact = TRUE;
                    break;
                }
            }
            if (!anyContact) {
                pDeviceContext->PalmDetected = FALSE;
                // Reset smoothing for all slots so there's no cursor jump.
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
                    pDeviceContext->SmoothedX[i] = 0;
                    pDeviceContext->SmoothedY[i] = 0;
                    pDeviceContext->HystX[i]     = 0;
                    pDeviceContext->HystY[i]     = 0;
                    pDeviceContext->SlotActive[i] = FALSE;
                }
            }
            else {
                for (i = 0; i < raw_n; i++) fingerAlive[i] = FALSE;
            }
        }

        // ----------------------------------------------------------------
        // Pass 2: build PTP contacts with deadzone + EMA smoothing.
        // ----------------------------------------------------------------
        UCHAR contactCount = 0;

        for (i = 0; i < raw_n; i++)
        {
            if (!fingerAlive[i]) {
                // Finger left or was rejected — clear its smoothing state.
                if (i < PTP_MAX_CONTACT_POINTS) {
                    pDeviceContext->SmoothedX[i]  = 0;
                    pDeviceContext->SmoothedY[i]  = 0;
                    pDeviceContext->HystX[i]      = 0;
                    pDeviceContext->HystY[i]      = 0;
                    pDeviceContext->SlotActive[i] = FALSE;
                }
                continue;
            }

            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerprintSize);

            USHORT nx = normXArr[i];
            USHORT ny = normYArr[i];

            // Seed smoothing on first contact frame to avoid jump from 0.
            if (!pDeviceContext->SlotActive[i]) {
                pDeviceContext->SmoothedX[i]  = nx;
                pDeviceContext->SmoothedY[i]  = ny;
                pDeviceContext->HystX[i]      = nx;
                pDeviceContext->HystY[i]      = ny;
                pDeviceContext->SlotActive[i] = TRUE;
            }

            // Deadzone.
            USHORT dzX = AmtApplyDeadzone(nx, &pDeviceContext->HystX[i]);
            USHORT dzY = AmtApplyDeadzone(ny, &pDeviceContext->HystY[i]);

            // EMA smoothing.
            USHORT reportX = AmtSmoothCoord(dzX, pDeviceContext->SmoothedX[i]);
            USHORT reportY = AmtSmoothCoord(dzY, pDeviceContext->SmoothedY[i]);
            pDeviceContext->SmoothedX[i] = reportX;
            pDeviceContext->SmoothedY[i] = reportY;

            if (contactCount < PTP_MAX_CONTACT_POINTS)
            {
                PtpReport.Contacts[contactCount].ContactID = (UCHAR)i;
                PtpReport.Contacts[contactCount].X         = reportX;
                PtpReport.Contacts[contactCount].Y         = reportY;
                PtpReport.Contacts[contactCount].TipSwitch = 1;
                PtpReport.Contacts[contactCount].Confidence =
                    (AmtRawToInteger(f->touch_minor) << 1) > 0 ? 1 : 0;
                contactCount++;
            }
        }

        PtpReport.ContactCount = contactCount;
    }

    if (pDeviceContext->PtpReportButton)
    {
        if (TouchBuffer[pDeviceContext->DeviceInfo->tp_button]) {
            PtpReport.IsButtonClicked = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC!: Trackpad button clicked");
        }
    }

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&PtpReport, sizeof(PTP_REPORT));

    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer failed %!STATUS!", Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, Status);
}

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE    Pipe,
    _In_ NTSTATUS      Status,
    _In_ USBD_STATUS   UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);
    return TRUE;
}