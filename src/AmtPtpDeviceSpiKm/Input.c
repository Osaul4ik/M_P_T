/*++
Module Name:
    Input.c
Abstract:
    SPI read pipeline and PTP report generation for the Apple T2 touchpad.
    Contact tracking uses the persistent slot / proximity-match architecture
    defined in ContactTracking.h.
Environment:
    Kernel-mode Driver Framework, DISPATCH_LEVEL in completion routine.
--*/

#include "driver.h"
#include "Input.tmh"

// ─────────────────────────────────────────────────────────────────────────────
// AmtPtpSpiInputRoutineWorker
//
// Called by Queue.c when Windows sends IOCTL_HID_READ_REPORT.
// Forwards the request to HidQueue (manual, not power-managed).
// Does NOT issue the SPI read here — that happens at the end of the
// completion routine once the device is fully configured.
// ─────────────────────────────────────────────────────────────────────────────
VOID
AmtPtpSpiInputRoutineWorker(
    WDFDEVICE   Device,
    WDFREQUEST  PtpRequest
)
{
    NTSTATUS        Status;
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(Device);

    Status = WdfRequestForwardToIoQueue(PtpRequest, pDeviceContext->HidQueue);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestForwardToIoQueue failed %!STATUS!", Status);
        WdfRequestComplete(PtpRequest, Status);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AmtPtpSpiInputIssueRequest
//
// Reuse the pre-allocated SPI read request and send it to the lower driver.
// Must only be called when DeviceStatus == D0ActiveAndConfigured.
// ─────────────────────────────────────────────────────────────────────────────
VOID
AmtPtpSpiInputIssueRequest(
    WDFDEVICE Device
)
{
    NTSTATUS             Status;
    PDEVICE_CONTEXT      pDeviceContext = DeviceGetContext(Device);
    WDF_REQUEST_REUSE_PARAMS ReuseParams;
    BOOLEAN              Sent;

    WDF_REQUEST_REUSE_PARAMS_INIT(&ReuseParams, WDF_REQUEST_REUSE_NO_FLAGS,
                                  STATUS_SUCCESS);
    Status = WdfRequestReuse(pDeviceContext->SpiHidReadRequest, &ReuseParams);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! WdfRequestReuse failed %!STATUS!", Status);
        return;
    }

    Sent = WdfRequestSend(pDeviceContext->SpiHidReadRequest,
                          pDeviceContext->SpiTrackpadIoTarget,
                          NULL);
    if (!Sent) {
        Status = WdfRequestGetStatus(pDeviceContext->SpiHidReadRequest);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
            "%!FUNC! WdfRequestSend failed %!STATUS!", Status);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// NormalizeX / NormalizeY
//
// Map a raw SPI coordinate to a PTP logical coordinate with saturating clamp.
// Uses LONG arithmetic to avoid signed SHORT overflow before comparison.
// ─────────────────────────────────────────────────────────────────────────────

FORCEINLINE USHORT
NormalizeX(
    _In_ const DEVICE_CONTEXT* pCtx,
    _In_ SHORT                 RawX
)
{
    LONG N = (LONG)RawX - (LONG)pCtx->TrackpadInfo.XMin;
    if (N <= 0)                         return 0;
    if (N >= (LONG)pCtx->XRange)        return pCtx->XRange;
    return (USHORT)N;
}

FORCEINLINE USHORT
NormalizeY(
    _In_ const DEVICE_CONTEXT* pCtx,
    _In_ SHORT                 RawY
)
{
    // Apple Y increases downward; PTP Y increases upward.
    LONG N = (LONG)pCtx->TrackpadInfo.YMax - (LONG)RawY;
    if (N <= 0)                         return 0;
    if (N >= (LONG)pCtx->YRange)        return pCtx->YRange;
    return (USHORT)N;
}

// ─────────────────────────────────────────────────────────────────────────────
// AmtPtpRequestCompletionRoutine
//
// Runs at DISPATCH_LEVEL when the lower SPI driver completes a read.
// Serialised by the single-pre-allocated-request invariant:
//   only one invocation is outstanding at any time.
//
// Algorithm
// ─────────
//  1. Retrieve pending PTP request from HidQueue.
//  2. Validate SPI packet.
//  3. Compute ScanTime.
//  4. Match hardware fingers to contact slots (proximity algorithm).
//  5. Transition slot states (Empty→Live, Live→Lifting, Lifting→Empty).
//  6. Build PTP_REPORT:
//     a. For each Live slot: TipSwitch=1, X/Y from current frame.
//     b. For each Lifting slot: TipSwitch=0, X/Y from LAST LIVE position.
//     c. Palm slots: suppressed (no HID entry while palm, silent lift on drop).
//  7. Complete PTP request.
//  8. Re-issue SPI read.
// ─────────────────────────────────────────────────────────────────────────────
VOID
AmtPtpRequestCompletionRoutine(
    WDFREQUEST                  SpiRequest,
    WDFIOTARGET                 Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params,
    WDFCONTEXT                  Context
)
{
    NTSTATUS                 Status;
    PWORKER_REQUEST_CONTEXT  pReqCtx;
    PDEVICE_CONTEXT          pCtx;

    // ── SPI packet ────────────────────────────────────────────────────────
    LONG                     SpiLen;
    SIZE_T                   BufLen;
    PSPI_TRACKPAD_PACKET     pPkt;
    UINT8                    HwCount;    // hardware finger count (clamped)

    // ── PTP report ────────────────────────────────────────────────────────
    WDFREQUEST               PtpRequest;
    WDFMEMORY                PtpMemory;
    PTP_REPORT               Report;
    UINT8                    ReportSlots; // total slots written (live + lifting)

    // ── Timing ───────────────────────────────────────────────────────────
    ULONGLONG                Now;
    ULONGLONG                Delta100us;

    // ── Matching temporaries ──────────────────────────────────────────────
    UINT8                    i, j;
    // matched[i] = slot index that hardware finger i was matched/allocated to,
    // or PTP_MAX_CONTACT_POINTS if the finger was dropped (table full, palm).
    UINT8                    matched[SPI_TRACKPAD_MAX_FINGERS];
    // touched[s] = TRUE if slot s was matched to a hardware finger this frame.
    BOOLEAN                  touched[PTP_MAX_CONTACT_POINTS];

    UNREFERENCED_PARAMETER(Target);

    pReqCtx = (PWORKER_REQUEST_CONTEXT)Context;
    pCtx    = pReqCtx->DeviceContext;

    // ── Step 1: retrieve pending PTP request ─────────────────────────────
    Status = WdfIoQueueRetrieveNextRequest(pCtx->HidQueue, &PtpRequest);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
            "%!FUNC! No pending PTP request (%!STATUS!), skipping", Status);
        goto cleanup;
    }

    // ── Mode check: if PTP input is disabled, send empty report ──────────
    if (!pCtx->PtpInputOn || !pCtx->PtpReportTouch) {
        RtlZeroMemory(&Report, sizeof(Report));
        Report.ReportID = REPORTID_MULTITOUCH;
        Status = WdfRequestRetrieveOutputMemory(PtpRequest, &PtpMemory);
        if (NT_SUCCESS(Status)) {
            WdfMemoryCopyFromBuffer(PtpMemory, 0, &Report, sizeof(Report));
            WdfRequestSetInformation(PtpRequest, sizeof(Report));
        }
        goto exit;
    }

    // ── Step 2: validate SPI packet ──────────────────────────────────────
    SpiLen = (LONG)WdfRequestGetInformation(SpiRequest);
    BufLen = 0;
    pPkt   = (PSPI_TRACKPAD_PACKET)WdfMemoryGetBuffer(
                 Params->Parameters.Ioctl.Output.Buffer, &BufLen);

    if (SpiLen > (LONG)BufLen) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! SpiLen %d > BufLen %Iu", SpiLen, BufLen);
        Status = STATUS_BUFFER_OVERFLOW;
        goto exit;
    }
    if (SpiLen < 46) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! Header truncated: %d < 46", SpiLen);
        Status = STATUS_DEVICE_DATA_ERROR;
        goto exit;
    }
    if (pPkt->NumOfFingers > SPI_TRACKPAD_MAX_FINGERS) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! NumOfFingers %d > max %d",
            pPkt->NumOfFingers, SPI_TRACKPAD_MAX_FINGERS);
        Status = STATUS_DEVICE_DATA_ERROR;
        goto exit;
    }
    {
        LONG MinLen = 46 + (LONG)pPkt->NumOfFingers * (LONG)sizeof(SPI_TRACKPAD_FINGER);
        if (SpiLen < MinLen) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
                "%!FUNC! Packet truncated: %d < %d (%d fingers)",
                SpiLen, MinLen, pPkt->NumOfFingers);
            Status = STATUS_DEVICE_DATA_ERROR;
            goto exit;
        }
    }

    HwCount = (pPkt->NumOfFingers > PTP_MAX_CONTACT_POINTS)
              ? PTP_MAX_CONTACT_POINTS : pPkt->NumOfFingers;

    // ── Step 3: ScanTime ─────────────────────────────────────────────────
    Now         = KeQueryInterruptTime();
    Delta100us  = (Now - pCtx->LastReportTime) / 1000ULL;
    if (Delta100us > 0xFFFFULL) Delta100us = 0xFFFF;

    // First-frame cap: if nothing was live before and fingers appear now,
    // the delta since last report reflects idle time (possibly seconds).
    // Cap to one 125 Hz period to give the gesture engine a sane start.
    if (pCtx->Contacts.LiveCount == 0 && HwCount > 0) {
        if (Delta100us > IDLE_SCANTIME_THRESHOLD) {
            Delta100us = FIRST_FRAME_SCANTIME_CAP;
        }
    }
    pCtx->LastReportTime = Now;

    // ── Step 4: proximity matching ────────────────────────────────────────
    //
    // For each hardware finger, find the closest Live slot within
    // CONTACT_MATCH_THRESHOLD_SQ.  Use a greedy O(H×S) algorithm:
    // with max 5 slots and max 5 hardware fingers this is at most 25
    // comparisons per frame — negligible at DISPATCH_LEVEL.
    //
    // matched[i] = slot assigned to hardware finger i.
    // touched[s] = slot s has been claimed by a hardware finger.
    RtlFillMemory(matched, sizeof(matched), PTP_MAX_CONTACT_POINTS);
    RtlZeroMemory(touched, sizeof(touched));

    for (i = 0; i < HwCount; i++) {
        SHORT HwX = pPkt->Fingers[i].X;
        SHORT HwY = pPkt->Fingers[i].Y;
        UINT8 slot;

        // Try to match to an existing Live slot.
        slot = AmtPtpFindMatchingSlot(&pCtx->Contacts, HwX, HwY);

        if (slot == PTP_MAX_CONTACT_POINTS) {
            // No Live slot close enough → new contact, allocate a slot.
            slot = AmtPtpAllocateSlot(&pCtx->Contacts);
            if (slot == PTP_MAX_CONTACT_POINTS) {
                // Table full.  Drop this finger (no ID available).
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_HID_INPUT,
                    "%!FUNC! Contact table full, dropping finger %d", i);
                continue;
            }
            // Initialise the new slot.
            pCtx->Contacts.Slots[slot].State  = SlotLive;
            pCtx->Contacts.Slots[slot].IsPalm = FALSE;
            pCtx->Contacts.Slots[slot].LastX  = NormalizeX(pCtx, HwX);
            pCtx->Contacts.Slots[slot].LastY  = NormalizeY(pCtx, HwY);

            // ── Palm classification (sticky, evaluated once at touchdown) ─
            //
            // Evaluated ONLY when the slot transitions Empty→Live.
            // Once classified as palm, IsPalm stays TRUE until SlotEmpty.
            // This prevents a mid-gesture reclassification from emitting a
            // TipSwitch=1 report for a contact that had no prior touchdown.
            if (pPkt->Fingers[i].TouchMajor >= PALM_MAJOR_THRESHOLD &&
                pPkt->Fingers[i].TouchMinor >= PALM_MINOR_THRESHOLD)
            {
                pCtx->Contacts.Slots[slot].IsPalm = TRUE;
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
                    "%!FUNC! Slot %d: new contact classified as palm "
                    "(Maj=%d Min=%d)", slot,
                    pPkt->Fingers[i].TouchMajor,
                    pPkt->Fingers[i].TouchMinor);
            }
        }

        matched[i] = slot;
        touched[slot] = TRUE;
    }

    // ── Step 5: state transitions ─────────────────────────────────────────
    //
    // Slots not touched this frame transition Live→Lifting.
    // Lifting slots from the previous frame transition Lifting→Empty.
    //
    // We also handle the case where the hardware delivers a Pressure=0 frame
    // while the finger is still reported (NumOfFingers > 0):
    //   Pressure=0 → TipSwitch=0 → treat as Lifting immediately.
    //
    // First: advance existing Lifting slots to Empty.
    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        if (pCtx->Contacts.Slots[i].State == SlotLifting) {
            pCtx->Contacts.Slots[i].State  = SlotEmpty;
            pCtx->Contacts.Slots[i].IsPalm = FALSE;
            // LastX/Y can be left stale — slot is Empty, nothing reads them.
        }
    }

    // Update LiveCount: count how many live slots were matched this frame
    // AND whose hardware finger has Pressure>=1.
    // Also transition unmatched Live slots to Lifting.
    pCtx->Contacts.LiveCount = 0;
    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        if (pCtx->Contacts.Slots[i].State != SlotLive) continue;

        if (touched[i]) {
            // Find the hardware finger that was matched to this slot.
            BOOLEAN HasPressure = FALSE;
            for (j = 0; j < HwCount; j++) {
                if (matched[j] == i) {
                    HasPressure = (pPkt->Fingers[j].Pressure >= 1);
                    // Update last known position while finger is still live.
                    if (HasPressure && !pCtx->Contacts.Slots[i].IsPalm) {
                        pCtx->Contacts.Slots[i].LastX =
                            NormalizeX(pCtx, pPkt->Fingers[j].X);
                        pCtx->Contacts.Slots[i].LastY =
                            NormalizeY(pCtx, pPkt->Fingers[j].Y);
                    }
                    break;
                }
            }
            if (!HasPressure) {
                // Hardware says Pressure=0: finger lifting, transition now.
                pCtx->Contacts.Slots[i].State = SlotLifting;
            } else {
                pCtx->Contacts.LiveCount++;
            }
        } else {
            // Slot was live but has no matching hardware finger: finger gone.
            pCtx->Contacts.Slots[i].State = SlotLifting;
        }
    }

    // ── Step 6: build PTP_REPORT ──────────────────────────────────────────
    RtlZeroMemory(&Report, sizeof(Report));
    ReportSlots = 0;

    // 6a. Live non-palm slots → TipSwitch=1, Confidence=1.
    for (i = 0; i < PTP_MAX_CONTACT_POINTS && ReportSlots < PTP_MAX_CONTACT_POINTS; i++) {
        if (pCtx->Contacts.Slots[i].State != SlotLive)  continue;
        if (pCtx->Contacts.Slots[i].IsPalm)             continue;

        Report.Contacts[ReportSlots].ContactID  = i;
        Report.Contacts[ReportSlots].TipSwitch  = 1;
        Report.Contacts[ReportSlots].Confidence = 1;
        Report.Contacts[ReportSlots].X          = pCtx->Contacts.Slots[i].LastX;
        Report.Contacts[ReportSlots].Y          = pCtx->Contacts.Slots[i].LastY;

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
            "%!FUNC! ID=%d LIVE X=%d Y=%d",
            i, pCtx->Contacts.Slots[i].LastX, pCtx->Contacts.Slots[i].LastY);

        ReportSlots++;
    }

    // 6b. Lifting non-palm slots → TipSwitch=0 at LAST LIVE X/Y.
    //
    // *** CRITICAL: use LastX/LastY, NOT (0,0). ***
    //
    // Windows PTP updates its internal "last position" for a ContactID from
    // every report it receives, including TipSwitch=0 frames.  Sending (0,0)
    // moves the contact's termination point to the trackpad origin.  On the
    // next gesture start the gesture engine interpolates from (0,0), causing
    // the cursor to snap toward the top-left corner.  This was the primary
    // cause of the cursor jump observed after repeated tap sequences.
    for (i = 0; i < PTP_MAX_CONTACT_POINTS && ReportSlots < PTP_MAX_CONTACT_POINTS; i++) {
        if (pCtx->Contacts.Slots[i].State != SlotLifting) continue;
        if (pCtx->Contacts.Slots[i].IsPalm) {
            // Palm never sent a TipSwitch=1 → no lift needed.
            // Transition directly to Empty (done next frame in step 5 above,
            // but accelerate it here to free the slot sooner).
            pCtx->Contacts.Slots[i].State  = SlotEmpty;
            pCtx->Contacts.Slots[i].IsPalm = FALSE;
            continue;
        }

        Report.Contacts[ReportSlots].ContactID  = i;
        Report.Contacts[ReportSlots].TipSwitch  = 0;
        Report.Contacts[ReportSlots].Confidence = 0;
        Report.Contacts[ReportSlots].X          = pCtx->Contacts.Slots[i].LastX;
        Report.Contacts[ReportSlots].Y          = pCtx->Contacts.Slots[i].LastY;

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_HID_INPUT,
            "%!FUNC! ID=%d LIFT at X=%d Y=%d",
            i, pCtx->Contacts.Slots[i].LastX, pCtx->Contacts.Slots[i].LastY);

        ReportSlots++;
    }

    Report.ReportID        = REPORTID_MULTITOUCH;
    Report.ContactCount    = ReportSlots;
    Report.IsButtonClicked = pPkt->ClickOccurred;
    Report.ScanTime        = (USHORT)Delta100us;

    // ── Step 7: send PTP report ───────────────────────────────────────────
    Status = WdfRequestRetrieveOutputMemory(PtpRequest, &PtpMemory);
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfRequestRetrieveOutputMemory %!STATUS!", Status);
        goto exit;
    }

    Status = WdfMemoryCopyFromBuffer(PtpMemory, 0, &Report, sizeof(Report));
    if (!NT_SUCCESS(Status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer %!STATUS!", Status);
        goto exit;
    }

    WdfRequestSetInformation(PtpRequest, sizeof(Report));

exit:
    WdfRequestComplete(PtpRequest, Status);

cleanup:
    // ── Step 8: re-issue SPI read ─────────────────────────────────────────
    // InterlockedCompareExchange gives acquire semantics; guaranteed to see
    // the D3 write from D0Exit if it has happened.
    if (DEVICE_STATUS_READ(pCtx) == D0ActiveAndConfigured) {
        AmtPtpSpiInputIssueRequest(pCtx->SpiDevice);
    }
}
