// Interrupt.c: Handles device input event.
//
// Contact-ID model
// ----------------
// Each PTP_MAX_CONTACT_POINTS slot is stable ContactID.
//
// Lifecycle:
// FREE → CONFIRMING → ACTIVE → PENDING_RELEASE → COOLDOWN → FREE

#include "Driver.h"
#include "Interrupt.tmh"

// ---- tunables ---------------------------------------------------------------
#define TIP_MAJOR_THRESHOLD  200
#define TIP_MINOR_THRESHOLD  150
#define TIP_CONFIRM_FRAMES   2
#define XY_DEADZONE_UNITS    2

#define SLOT_NONE  ((UCHAR)PTP_MAX_CONTACT_POINTS)
#define KEY_NONE   ((UCHAR)0xFF)

// -----------------------------------------------------------------------------

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT v = raw - minVal;
    if (v < 0) v = 0;
    if (v > maxVal) v = maxVal;
    return (USHORT)v;
}

static inline USHORT
AmtApplyDeadzone(_In_ USHORT newVal, _Inout_ USHORT* pBaseline)
{
#if XY_DEADZONE_UNITS > 0
    INT delta = (INT)newVal - (INT)(*pBaseline);
    if (delta < 0) delta = -delta;
    if (delta < XY_DEADZONE_UNITS) {
        return *pBaseline;
    }
#endif
    *pBaseline = newVal;
    return newVal;
}

// -----------------------------------------------------------------------------
// 🔥 FIX: helper reset (IMPORTANT)
static VOID
AmtPtpResetSlotCoordinates(_In_ PDEVICE_CONTEXT pCtx, _In_ UCHAR s)
{
    pCtx->LastNormX[s] = 0;
    pCtx->LastNormY[s] = 0;
    pCtx->HystX[s]     = 0;
    pCtx->HystY[s]     = 0;
}

// -----------------------------------------------------------------------------
// CONTINUOUS READER CONFIG
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext)
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

    if (transferLength == 0)
        return STATUS_UNKNOWN_REVISION;

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &contReaderConfig,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    contReaderConfig.EvtUsbTargetPipeReadersFailed =
        AmtPtpEvtUsbInterruptReadersFailed;

    WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe,
        &contReaderConfig);

    return STATUS_SUCCESS;
}

// -----------------------------------------------------------------------------
// INTERRUPT
VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE Pipe,
    _In_ WDFMEMORY Buffer,
    _In_ size_t NumBytesTransferred,
    _In_ WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pCtx = Context;

    const size_t headerSize = (size_t)pCtx->DeviceInfo->tp_header;
    const size_t fingerSize = (size_t)pCtx->DeviceInfo->tp_fsize;

    size_t raw_n, i, s;

    UCHAR* TouchBuffer;
    UCHAR* f_base;
    const struct TRACKPAD_FINGER* f;

    PTP_REPORT PtpReport;
    RtlZeroMemory(&PtpReport, sizeof(PtpReport));

    WDFREQUEST Request;
    WDFMEMORY RequestMemory;

    LARGE_INTEGER CurrentPerfCounter;
    LONGLONG PerfCounterDelta;

    // -------------------------------------------------------------------------
    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (!TouchBuffer) return;

    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request)))
        return;

    if (!NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &RequestMemory))) {
        WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
        return;
    }

    // -------------------------------------------------------------------------
    PtpReport.ReportID = REPORTID_MULTITOUCH;

    KeQueryPerformanceCounter(&CurrentPerfCounter);
    PerfCounterDelta =
        (CurrentPerfCounter.QuadPart - pCtx->LastReportTime.QuadPart) / 100;

    if (PerfCounterDelta > 0xFF) PerfCounterDelta = 0xFF;
    PtpReport.ScanTime = (USHORT)PerfCounterDelta;

    UCHAR reportSlots = 0;

    if (pCtx->PtpReportTouch)
    {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS)
            raw_n = PTP_MAX_CONTACT_POINTS;

        f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;

        // -------------------------
        BOOLEAN fingerTipDown[PTP_MAX_CONTACT_POINTS] = { 0 };
        USHORT fingerNormX[PTP_MAX_CONTACT_POINTS] = { 0 };
        USHORT fingerNormY[PTP_MAX_CONTACT_POINTS] = { 0 };
        UCHAR fingerKey[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
            fingerKey[i] = KEY_NONE;

        for (i = 0; i < raw_n; i++)
        {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * fingerSize);

            BOOLEAN tip =
                (AmtRawToInteger(f->touch_major) << 1) >= TIP_MAJOR_THRESHOLD ||
                (AmtRawToInteger(f->touch_minor) << 1) >= TIP_MINOR_THRESHOLD;

            fingerTipDown[i] = tip;

            if (tip)
            {
                INT xRange = pCtx->DeviceInfo->x.max - pCtx->DeviceInfo->x.min;
                INT yRange = pCtx->DeviceInfo->y.max - pCtx->DeviceInfo->y.min;

                fingerNormX[i] = AmtClampCoord(
                    AmtRawToInteger(f->abs_x),
                    pCtx->DeviceInfo->x.min,
                    xRange);

                INT normY = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
                fingerNormY[i] = AmtClampCoord(normY, 0, yRange);

                fingerKey[i] = (UCHAR)i;
            }
        }

        // 🔥 FIX BUILD WARNING (C4189 SAFETY)
        (void)fingerNormX;
        (void)fingerNormY;

        // -------------------------
        UCHAR slotForFinger[PTP_MAX_CONTACT_POINTS];
        UCHAR fingerForSlot[PTP_MAX_CONTACT_POINTS];

        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
            slotForFinger[i] = SLOT_NONE;

        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
            fingerForSlot[s] = SLOT_NONE;

        // -------------------------
        for (i = 0; i < raw_n; i++)
        {
            if (!fingerTipDown[i]) continue;

            for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
            {
                if ((pCtx->SlotInUse[s] || pCtx->SlotTipConfirmed[s] > 0) &&
                    pCtx->SlotFingerKey[s] == fingerKey[i] &&
                    fingerForSlot[s] == SLOT_NONE)
                {
                    slotForFinger[i] = (UCHAR)s;
                    fingerForSlot[s] = (UCHAR)i;
                    break;
                }
            }
        }

        // -------------------------
        for (i = 0; i < raw_n; i++)
        {
            if (!fingerTipDown[i]) continue;
            if (slotForFinger[i] != SLOT_NONE) continue;

            for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
            {
                if (!pCtx->SlotInUse[s] &&
                    !pCtx->SlotPendingRelease[s] &&
                    !pCtx->SlotCooldown[s] &&
                    pCtx->SlotTipConfirmed[s] == 0 &&
                    fingerForSlot[s] == SLOT_NONE)
                {
                    slotForFinger[i] = (UCHAR)s;
                    fingerForSlot[s] = (UCHAR)i;

                    pCtx->SlotFingerKey[s] = fingerKey[i];

                    // 🔥 FIX #2
                    AmtPtpResetSlotCoordinates(pCtx, (UCHAR)s);

                    break;
                }
            }
        }

        // -------------------------
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
        {
            if (fingerForSlot[s] != SLOT_NONE)
                continue;

            if (pCtx->SlotPendingRelease[s])
            {
                PtpReport.Contacts[reportSlots].ContactID = (UCHAR)s;
                PtpReport.Contacts[reportSlots].TipSwitch = 0;
                reportSlots++;

                pCtx->SlotPendingRelease[s] = FALSE;
                pCtx->SlotCooldown[s] = 2;

                continue;
            }

            if (pCtx->SlotInUse[s])
            {
                pCtx->SlotInUse[s] = FALSE;
                pCtx->SlotPendingRelease[s] = TRUE;
                pCtx->SlotFingerKey[s] = KEY_NONE;
                pCtx->SlotTipConfirmed[s] = 0;

                // 🔥 FIX #3
                AmtPtpResetSlotCoordinates(pCtx, (UCHAR)s);

                continue;
            }
        }

        // -------------------------
        for (i = 0; i < raw_n; i++)
        {
            if (!fingerTipDown[i]) continue;

            UCHAR slot = slotForFinger[i];
            if (slot >= PTP_MAX_CONTACT_POINTS) continue;

            if (pCtx->SlotTipConfirmed[slot] < TIP_CONFIRM_FRAMES)
                pCtx->SlotTipConfirmed[slot]++;

            if (!pCtx->SlotInUse[slot] &&
                pCtx->SlotTipConfirmed[slot] >= TIP_CONFIRM_FRAMES)
            {
                pCtx->SlotInUse[slot] = TRUE;
            }

            PtpReport.Contacts[reportSlots].ContactID = slot;
            PtpReport.Contacts[reportSlots].TipSwitch = 1;
            reportSlots++;

            pCtx->LastNormX[slot] = fingerNormX[i];
            pCtx->LastNormY[slot] = fingerNormY[i];
        }

        PtpReport.ContactCount = reportSlots;
    }

    // -------------------------------------------------------------------------
    pCtx->LastReportTime = CurrentPerfCounter;

    WdfMemoryCopyFromBuffer(RequestMemory, 0, &PtpReport, sizeof(PtpReport));
    WdfRequestSetInformation(Request, sizeof(PtpReport));
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

// -----------------------------------------------------------------------------
BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE Pipe,
    _In_ NTSTATUS Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);
    return TRUE;
}