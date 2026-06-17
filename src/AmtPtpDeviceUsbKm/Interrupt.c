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

// Clamp a signed coordinate into [0, range] and return as USHORT.
// FIX: original code cast a potentially negative result directly to USHORT,
// producing huge wrap-around values for TYPE4 devices whose x.min is -6243.
static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT shifted = raw - minVal;
    if (shifted < 0)      shifted = 0;
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
    if (minor > 0 && major > 120)
    {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

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

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t   transferLength = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type)
    {
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
        DeviceContext->InterruptPipe,
        &contReaderConfig);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfUsbTargetPipeConfigContinuousReader failed %!STATUS!", status);
    }

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

    PDEVICE_CONTEXT pDeviceContext = Context;
    size_t          headerSize      = (unsigned int)pDeviceContext->DeviceInfo->tp_header;
    size_t          fingerprintSize = (unsigned int)pDeviceContext->DeviceInfo->tp_fsize;
    size_t          raw_n, i;
    UCHAR*          TouchBuffer     = NULL;
    const struct TRACKPAD_FINGER* f = NULL;

    LONGLONG        PerfCounterDelta;
    LARGE_INTEGER   CurrentPerfCounter;
    NTSTATUS        Status;
    PTP_REPORT      PtpReport;
    WDFREQUEST      Request;
    WDFMEMORY       RequestMemory;

    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerprintSize != 0)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Malformed input. Length = %llu", (ULONG64)NumBytesTransferred);
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! Failed to retrieve packet buffer");
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(pDeviceContext->InputQueue, &Request);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! No pending PTP request — frame discarded");
        return;
    }

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestRetrieveOutputMemory failed %!STATUS!", Status);
        WdfRequestComplete(Request, Status);
        return;
    }

    RtlZeroMemory(&PtpReport, sizeof(PTP_REPORT));
    PtpReport.ReportID        = REPORTID_MULTITOUCH;
    PtpReport.IsButtonClicked = 0;

    KeQueryPerformanceCounter(&CurrentPerfCounter);
    PerfCounterDelta = CurrentPerfCounter.QuadPart - pDeviceContext->LastReportTime.QuadPart;

    if (pDeviceContext->PerfFrequency.QuadPart > 0) {
        PerfCounterDelta = PerfCounterDelta * 10000LL /
                           pDeviceContext->PerfFrequency.QuadPart;
    } else {
        PerfCounterDelta /= 100LL;
    }

    if (PerfCounterDelta > 0xFFFF) PerfCounterDelta = 0xFFFF;
    PtpReport.ScanTime = (USHORT)PerfCounterDelta;
    pDeviceContext->LastReportTime = CurrentPerfCounter;

    // Typing suppression: send zero-contact report during suppression window.
    {
        LONGLONG suppressUntil = InterlockedCompareExchange64(
            &pDeviceContext->TypingSuppressUntil, 0, 0);

        if (suppressUntil > CurrentPerfCounter.QuadPart) {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! Typing suppression active");

            if (pDeviceContext->PtpReportButton &&
                TouchBuffer[pDeviceContext->DeviceInfo->tp_button]) {
                PtpReport.IsButtonClicked = TRUE;
            }

            PtpReport.ContactCount = 0;
            Status = WdfMemoryCopyFromBuffer(
                RequestMemory, 0, (PVOID)&PtpReport, sizeof(PTP_REPORT));
            WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
            WdfRequestComplete(Request, NT_SUCCESS(Status) ? STATUS_SUCCESS : Status);
            return;
        }
    }

    raw_n = (NumBytesTransferred - headerSize) / fingerprintSize;
    UCHAR* f_base = TouchBuffer + headerSize + pDeviceContext->DeviceInfo->tp_delta;

    if (pDeviceContext->PtpReportTouch)
    {
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        // FIX: was `<` (always false by construction); also guard bounds.
        if (raw_n * fingerprintSize > (NumBytesTransferred - headerSize)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Buffer size mismatch — aborting frame");
            WdfRequestComplete(Request, STATUS_DATA_ERROR);
            return;
        }

        BOOLEAN largePalmThisFrame = FALSE;

        // FIX: declare parallel arrays for all slots up-front and zero-init.
        // Original code left normXArr[i]/normYArr[i] uninitialised for slots
        // where `tip == FALSE`, then read them in Pass 2 via fingerAlive[i].
        BOOLEAN fingerAlive[PTP_MAX_CONTACT_POINTS];
        USHORT  normXArr[PTP_MAX_CONTACT_POINTS];
        USHORT  normYArr[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
            fingerAlive[i] = FALSE;
            normXArr[i]    = 0;
            normYArr[i]    = 0;
        }

        // Pass 1: palm classification.
        for (i = 0; i < raw_n; i++)
        {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerprintSize);

            BOOLEAN tip = (AmtRawToInteger(f->touch_major) << 1) >= 200 ||
                          (AmtRawToInteger(f->touch_minor) << 1) >= 150;
            if (!tip) continue;

            // FIX: use AmtClampCoord to prevent USHORT wrap-around when the
            // raw coordinate is below devInfo->x.min (e.g. TYPE4: min=-6243).
            USHORT nx = AmtClampCoord(AmtRawToInteger(f->abs_x),
                                      pDeviceContext->DeviceInfo->x.min,
                                      pDeviceContext->DeviceInfo->x.max);
            INT   nyRaw = pDeviceContext->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
            USHORT ny = (USHORT)(nyRaw > 0 ? nyRaw : 0);

            // Always store coords; needed by Pass 2 if this slot is alive.
            normXArr[i] = nx;
            normYArr[i] = ny;

            PALM_CLASS palm = AmtClassifyPalm(f, pDeviceContext->DeviceInfo, nx, ny);

            if (palm == PALM_LARGE) {
                largePalmThisFrame = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! finger %llu: LARGE palm — global suppress", (ULONG64)i);
                break;
            }

            if (palm == PALM_LOCAL) {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! finger %llu: per-contact palm reject", (ULONG64)i);
                continue;
            }

            // FIX: was missing bounds check; i is already < PTP_MAX_CONTACT_POINTS.
            fingerAlive[i] = TRUE;
        }

        if (largePalmThisFrame)
        {
            pDeviceContext->PalmDetected = TRUE;
            // fingerAlive[] already all FALSE from init above.
        }
        else if (pDeviceContext->PalmDetected)
        {
            BOOLEAN anyContact = FALSE;
            for (i = 0; i < raw_n; i++) {
                f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerprintSize);
                if (AmtRawToInteger(f->touch_major) > 0 ||
                    AmtRawToInteger(f->touch_minor) > 0) {
                    anyContact = TRUE;
                    break;
                }
            }
            if (!anyContact) {
                pDeviceContext->PalmDetected = FALSE;
                for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
                    pDeviceContext->SmoothedX[i]  = 0;
                    pDeviceContext->SmoothedY[i]  = 0;
                    pDeviceContext->HystX[i]      = 0;
                    pDeviceContext->HystY[i]      = 0;
                    pDeviceContext->SlotActive[i] = FALSE;
                }
            }
            // fingerAlive[] stays all-FALSE → contactCount == 0 below.
        }

        // Pass 2: build PTP contacts.
        UCHAR contactCount = 0;

        for (i = 0; i < raw_n; i++)
        {
            if (!fingerAlive[i]) {
                // Clear slot state for this finger.
                pDeviceContext->SmoothedX[i]  = 0;
                pDeviceContext->SmoothedY[i]  = 0;
                pDeviceContext->HystX[i]      = 0;
                pDeviceContext->HystY[i]      = 0;
                pDeviceContext->SlotActive[i] = FALSE;
                continue;
            }

            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerprintSize);

            USHORT nx = normXArr[i];
            USHORT ny = normYArr[i];

            // Seed smoothing buffers on first contact to avoid a jump from 0.
            if (!pDeviceContext->SlotActive[i]) {
                pDeviceContext->SmoothedX[i]  = nx;
                pDeviceContext->SmoothedY[i]  = ny;
                pDeviceContext->HystX[i]      = nx;
                pDeviceContext->HystY[i]      = ny;
                pDeviceContext->SlotActive[i] = TRUE;
            }

            USHORT dzX = AmtApplyDeadzone(nx, &pDeviceContext->HystX[i]);
            USHORT dzY = AmtApplyDeadzone(ny, &pDeviceContext->HystY[i]);

            USHORT reportX = AmtSmoothCoord(dzX, pDeviceContext->SmoothedX[i]);
            USHORT reportY = AmtSmoothCoord(dzY, pDeviceContext->SmoothedY[i]);
            pDeviceContext->SmoothedX[i] = reportX;
            pDeviceContext->SmoothedY[i] = reportY;

            if (contactCount < PTP_MAX_CONTACT_POINTS)
            {
                PtpReport.Contacts[contactCount].ContactID  = (UCHAR)i;
                PtpReport.Contacts[contactCount].X          = reportX;
                PtpReport.Contacts[contactCount].Y          = reportY;
                PtpReport.Contacts[contactCount].TipSwitch  = 1;
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
                "%!FUNC! Trackpad button clicked");
        }
    }

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&PtpReport, sizeof(PTP_REPORT));

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