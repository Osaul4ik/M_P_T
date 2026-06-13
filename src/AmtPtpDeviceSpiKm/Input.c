#include "driver.h"
#include "Input.tmh"

VOID
AmtPtpSpiInputRoutineWorker(WDFDEVICE Device, WDFREQUEST PtpRequest)
{
    NTSTATUS        Status;
    PDEVICE_CONTEXT pCtx = DeviceGetContext(Device);
    Status = WdfRequestForwardToIoQueue(PtpRequest, pCtx->HidQueue);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestForwardToIoQueue %!STATUS!", Status);
        WdfRequestComplete(PtpRequest, Status);
    }
}

VOID
AmtPtpSpiInputIssueRequest(WDFDEVICE Device)
{
    NTSTATUS             Status;
    PDEVICE_CONTEXT      pCtx = DeviceGetContext(Device);
    WDF_REQUEST_REUSE_PARAMS ReuseParams;

    WDF_REQUEST_REUSE_PARAMS_INIT(&ReuseParams, WDF_REQUEST_REUSE_NO_FLAGS,
                                  STATUS_SUCCESS);
    Status = WdfRequestReuse(pCtx->SpiHidReadRequest, &ReuseParams);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! WdfRequestReuse %!STATUS!", Status);
        return;
    }
    if (!WdfRequestSend(pCtx->SpiHidReadRequest,
                        pCtx->SpiTrackpadIoTarget, NULL)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! WdfRequestSend %!STATUS!",
            WdfRequestGetStatus(pCtx->SpiHidReadRequest));
    }
}

// Saturating normalize: [XMin..XMax] → [0..XRange], clamped.
FORCEINLINE USHORT NormalizeX(_In_ const DEVICE_CONTEXT* c, _In_ SHORT x)
{
    LONG n = (LONG)x - (LONG)c->TrackpadInfo.XMin;
    if (n <= 0)                  return 0;
    if (n >= (LONG)c->XRange)    return c->XRange;
    return (USHORT)n;
}

// Apple Y grows downward; PTP Y grows upward → invert.
FORCEINLINE USHORT NormalizeY(_In_ const DEVICE_CONTEXT* c, _In_ SHORT y)
{
    LONG n = (LONG)c->TrackpadInfo.YMax - (LONG)y;
    if (n <= 0)                  return 0;
    if (n >= (LONG)c->YRange)    return c->YRange;
    return (USHORT)n;
}

// ─────────────────────────────────────────────────────────────────────────────
// AmtPtpRequestCompletionRoutine — DISPATCH_LEVEL, serialised by single-request
// invariant (only one SPI read outstanding at a time).
//
// Two-pass design:
//   Pass A (match + transition):  O(H²·S) matching, O(S) state updates.
//   Pass B (report build):        O(S) single scan, fills PTP_REPORT.
// ─────────────────────────────────────────────────────────────────────────────
VOID
AmtPtpRequestCompletionRoutine(
    WDFREQUEST                     SpiRequest,
    WDFIOTARGET                    Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params,
    WDFCONTEXT                     Context)
{
    NTSTATUS                Status;
    PDEVICE_CONTEXT         pCtx;
    PSPI_TRACKPAD_PACKET    pPkt;
    SIZE_T                  BufLen;
    LONG                    SpiLen;
    UINT8                   HwCount;

    WDFREQUEST              PtpRequest;
    PTP_REPORT*             pReport;   // direct pointer — avoids extra WDF copy
    SIZE_T                  OutLen;
    UINT8                   ReportSlots;

    ULONGLONG               Now;
    ULONGLONG               Delta100us;
    UINT8                   i, s;

    // Pass-A temporaries:
    //   hw_to_slot[i] = slot assigned to HW finger i (SLOT_NONE if unmatched).
    //   slot_to_hw[s] = HW finger index matched to slot s (SLOT_NONE if none).
    UINT8  hw_to_slot[SPI_TRACKPAD_MAX_FINGERS];
    UINT8  slot_to_hw[PTP_MAX_CONTACT_POINTS];
    SHORT  HwRawX[SPI_TRACKPAD_MAX_FINGERS];
    SHORT  HwRawY[SPI_TRACKPAD_MAX_FINGERS];

    UNREFERENCED_PARAMETER(Target);

    pCtx = ((PWORKER_REQUEST_CONTEXT)Context)->DeviceContext;

    // ── Step 1: dequeue PTP request ───────────────────────────────────────
    Status = WdfIoQueueRetrieveNextRequest(pCtx->HidQueue, &PtpRequest);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! No pending PTP request (%!STATUS!)", Status);
        goto cleanup;
    }

    // ── Mode check ────────────────────────────────────────────────────────
    if (!pCtx->PtpInputOn || !pCtx->PtpReportTouch) {
        // Retrieve output buffer directly — avoids WdfMemoryCopyFromBuffer DDI
        // overhead for this trivial zero-report path.
        Status = WdfRequestRetrieveOutputBuffer(
                     PtpRequest, sizeof(PTP_REPORT), (PVOID*)&pReport, &OutLen);
        if (NT_SUCCESS(Status)) {
            RtlZeroMemory(pReport, sizeof(PTP_REPORT));
            pReport->ReportID = REPORTID_MULTITOUCH;
            WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));
        }
        goto exit;
    }

    // ── Step 2: validate SPI packet ───────────────────────────────────────
    SpiLen = (LONG)WdfRequestGetInformation(SpiRequest);
    BufLen = 0;
    pPkt   = (PSPI_TRACKPAD_PACKET)WdfMemoryGetBuffer(
                 Params->Parameters.Ioctl.Output.Buffer, &BufLen);

    if (SpiLen > (LONG)BufLen) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! SpiLen %d > BufLen %Iu", SpiLen, BufLen);
        Status = STATUS_BUFFER_OVERFLOW; goto exit;
    }
    if (SpiLen < 46) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! Header truncated %d < 46", SpiLen);
        Status = STATUS_DEVICE_DATA_ERROR; goto exit;
    }
    if (pPkt->NumOfFingers > SPI_TRACKPAD_MAX_FINGERS) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! NumOfFingers %d > %d",
            pPkt->NumOfFingers, SPI_TRACKPAD_MAX_FINGERS);
        Status = STATUS_DEVICE_DATA_ERROR; goto exit;
    }
    {
        LONG MinLen = 46L + (LONG)pPkt->NumOfFingers
                          * (LONG)sizeof(SPI_TRACKPAD_FINGER);
        if (SpiLen < MinLen) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Packet truncated %d < %d (%d fingers)",
                SpiLen, MinLen, pPkt->NumOfFingers);
            Status = STATUS_DEVICE_DATA_ERROR; goto exit;
        }
    }

    HwCount = (pPkt->NumOfFingers > PTP_MAX_CONTACT_POINTS)
              ? PTP_MAX_CONTACT_POINTS : (UINT8)pPkt->NumOfFingers;

    // ── Step 3: ScanTime ─────────────────────────────────────────────────
    Now        = KeQueryInterruptTime();
    Delta100us = (Now - pCtx->LastReportTime) / 1000ULL;
    if (Delta100us > 0xFFFFULL) Delta100us = 0xFFFF;
    if (pCtx->Contacts.LiveCount == 0 && HwCount > 0 &&
        Delta100us > IDLE_SCANTIME_THRESHOLD) {
        Delta100us = FIRST_FRAME_SCANTIME_CAP;
    }
    pCtx->LastReportTime = Now;

    // ── PASS A: match + state transitions ────────────────────────────────
    //
    // A1. Collect raw HW coordinates (avoid repeated field access).
    for (i = 0; i < HwCount; i++) {
        HwRawX[i] = pPkt->Fingers[i].X;
        HwRawY[i] = pPkt->Fingers[i].Y;
    }

    // A2. Pre-fill slot_to_hw with SLOT_NONE.
    RtlFillMemory(slot_to_hw, sizeof(slot_to_hw), SLOT_NONE);

    // A3. Optimal bipartite matching: HW fingers ↔ Live slots.
    //     Uses raw SPI coordinates on both sides — no DC bias.
    AmtPtpMatchFingers(&pCtx->Contacts, HwCount,
                       HwRawX, HwRawY, hw_to_slot, slot_to_hw);

    // A4. Allocate new slots for unmatched HW fingers.
    for (i = 0; i < HwCount; i++) {
        if (hw_to_slot[i] != SLOT_NONE) continue; // already matched

        s = AmtPtpAllocateSlot(&pCtx->Contacts);
        if (s == SLOT_NONE) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_HID_INPUT,
                "%!FUNC! Table full, dropping finger %d", i);
            continue;
        }
        pCtx->Contacts.Slots[s].State   = SlotLive;
        pCtx->Contacts.Slots[s].IsPalm  =
            (pPkt->Fingers[i].TouchMajor >= PALM_MAJOR_THRESHOLD &&
             pPkt->Fingers[i].TouchMinor >= PALM_MINOR_THRESHOLD);
        pCtx->Contacts.Slots[s].LastRawX  = HwRawX[i];
        pCtx->Contacts.Slots[s].LastRawY  = HwRawY[i];
        pCtx->Contacts.Slots[s].LastNormX = NormalizeX(pCtx, HwRawX[i]);
        pCtx->Contacts.Slots[s].LastNormY = NormalizeY(pCtx, HwRawY[i]);

        hw_to_slot[i]  = s;
        slot_to_hw[s]  = i;

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
            "%!FUNC! New slot %d %s", s,
            pCtx->Contacts.Slots[s].IsPalm ? "(palm)" : "");
    }

    // A5. Single pass over all slots: update Live, transition to Lifting/Empty.
    pCtx->Contacts.LiveCount = 0;
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        CONTACT_SLOT* sl = &pCtx->Contacts.Slots[s];

        if (sl->State == SlotLifting) {
            // Already emitted TipSwitch=0 last frame — free the slot.
            sl->State  = SlotEmpty;
            sl->IsPalm = FALSE;
            continue;
        }

        if (sl->State != SlotLive) continue; // SlotEmpty: nothing to do

        if (slot_to_hw[s] == SLOT_NONE) {
            // No HW finger matched this slot → finger disappeared.
            sl->State = SlotLifting;
        } else {
            // Matched. Check pressure.
            UINT8 hw = slot_to_hw[s];
            if (pPkt->Fingers[hw].Pressure < 1) {
                // Pressure=0 transitional frame (finger releasing).
                sl->State = SlotLifting;
            } else {
                // Still live: update position.
                sl->LastRawX  = HwRawX[hw];
                sl->LastRawY  = HwRawY[hw];
                if (!sl->IsPalm) {
                    sl->LastNormX = NormalizeX(pCtx, HwRawX[hw]);
                    sl->LastNormY = NormalizeY(pCtx, HwRawY[hw]);
                }
                pCtx->Contacts.LiveCount++;
            }
        }
    }

    // ── PASS B: build PTP_REPORT ──────────────────────────────────────────
    //
    // Single pass. Live non-palm → TipSwitch=1. Lifting non-palm → TipSwitch=0
    // at LastNormX/Y. Palm lifting → silent Empty (no HID entry).
    //
    // Direct output-buffer pointer avoids WdfMemoryCopyFromBuffer DDI overhead
    // (~100 cycles) for this 30-byte copy.
    Status = WdfRequestRetrieveOutputBuffer(
                 PtpRequest, sizeof(PTP_REPORT), (PVOID*)&pReport, &OutLen);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestRetrieveOutputBuffer %!STATUS!", Status);
        goto exit;
    }

    RtlZeroMemory(pReport, sizeof(PTP_REPORT));
    ReportSlots = 0;

    for (s = 0; s < PTP_MAX_CONTACT_POINTS && ReportSlots < PTP_MAX_CONTACT_POINTS; s++) {
        CONTACT_SLOT* sl = &pCtx->Contacts.Slots[s];

        if (sl->State == SlotLive && !sl->IsPalm) {
            pReport->Contacts[ReportSlots].ContactID  = s;
            pReport->Contacts[ReportSlots].TipSwitch  = 1;
            pReport->Contacts[ReportSlots].Confidence = 1;
            pReport->Contacts[ReportSlots].X          = sl->LastNormX;
            pReport->Contacts[ReportSlots].Y          = sl->LastNormY;
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
                "%!FUNC! ID=%d LIVE X=%u Y=%u", s, sl->LastNormX, sl->LastNormY);
            ReportSlots++;

        } else if (sl->State == SlotLifting) {
            if (sl->IsPalm) {
                // Palm: no TipSwitch=1 was ever sent, no lift needed.
                sl->State  = SlotEmpty;
                sl->IsPalm = FALSE;
            } else {
                // CRITICAL: use LastNormX/Y — NOT (0,0).
                // Windows updates its internal "last position" for this
                // ContactID even from TipSwitch=0 frames.  Sending (0,0)
                // anchors gesture termination at trackpad origin, causing
                // cursor snap-back on the next touch sequence.
                pReport->Contacts[ReportSlots].ContactID  = s;
                pReport->Contacts[ReportSlots].TipSwitch  = 0;
                pReport->Contacts[ReportSlots].Confidence = 0;
                pReport->Contacts[ReportSlots].X          = sl->LastNormX;
                pReport->Contacts[ReportSlots].Y          = sl->LastNormY;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
                    "%!FUNC! ID=%d LIFT X=%u Y=%u", s, sl->LastNormX, sl->LastNormY);
                ReportSlots++;
            }
        }
    }

    pReport->ReportID        = REPORTID_MULTITOUCH;
    pReport->ContactCount    = ReportSlots;
    pReport->IsButtonClicked = pPkt->ClickOccurred;
    pReport->ScanTime        = (USHORT)Delta100us;

    WdfRequestSetInformation(PtpRequest, sizeof(PTP_REPORT));

exit:
    WdfRequestComplete(PtpRequest, Status);

cleanup:
    if (DEVICE_STATUS_READ(pCtx) == D0ActiveAndConfigured) {
        AmtPtpSpiInputIssueRequest(pCtx->SpiDevice);
    }
}
