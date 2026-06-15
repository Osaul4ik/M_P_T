/*
 * ProcessingThread.c - Dedicated PASSIVE_LEVEL processing thread.
 *
 * Responsibilities:
 *   - Drain USB_PACKET_RING produced by the USB interrupt callback.
 *   - Run the slot state machine (AmtPtpProcessTouchFrame).
 *   - Dequeue pending HID read requests from InputQueue and complete them.
 *
 * Threading model:
 *   The thread is created in D0Entry (after events are initialised in
 *   CreateDevice) and destroyed in D0Exit (after the USB reader is stopped).
 *   Because no USB packets can arrive after WdfIoTargetStop returns, the
 *   ring is guaranteed to be stable once the thread observes StopEvent.
 *
 * Backpressure:
 *   If there is no pending HID request when the thread finishes building a
 *   report, the report is held in pCtx->PendingReport and the thread parks
 *   on ProcEvent again.  The next USB packet (which may be a no-op empty
 *   frame) will wake the thread and the held report is delivered first.
 *   This means at most one report worth of backpressure before the HID
 *   stack is considered starved; further packets are not discarded but the
 *   ring WILL fill if the HID stack stops reading.
 *
 * ContactCount
 *   ContactCount in the PTP_REPORT is set exclusively by
 *   AmtPtpProcessTouchFrame (InterruptTouch.c). It reflects only contacts
 *   with TipSwitch=1 in the snapshot (PTP spec §5.3). This file must NOT
 *   overwrite ContactCount after the call returns.
 */

#include "driver.h"
#include "ProcessingThread.tmh"
#include "include/ProcessingThread.h"

// Forward declaration of the thread body.
static VOID
ProcThreadBody(
    _In_ PVOID Context);

// ---- public API ------------------------------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ProcThreadStart(
    _In_ PDEVICE_CONTEXT pCtx)
{
    NTSTATUS    status;
    HANDLE      threadHandle;
    OBJECT_ATTRIBUTES objAttr;

    PAGED_CODE();

    if (pCtx->ThreadRunning) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! Thread already running — skip");
        return STATUS_SUCCESS;
    }

    // Ensure ProcEvent is non-signalled so the thread parks on first wait.
    KeClearEvent(&pCtx->ProcEvent);
    KeClearEvent(&pCtx->StopEvent);

    InitializeObjectAttributes(&objAttr, NULL,
        OBJ_KERNEL_HANDLE, NULL, NULL);

    status = PsCreateSystemThread(
        &threadHandle,
        THREAD_ALL_ACCESS,
        &objAttr,
        NULL,           // ProcessHandle — NULL means kernel context
        NULL,
        ProcThreadBody,
        pCtx);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! PsCreateSystemThread failed %!STATUS!", status);
        return status;
    }

    // Get a kernel object pointer so we can wait for the thread on exit.
    status = ObReferenceObjectByHandle(
        threadHandle,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        &pCtx->ThreadObject,
        NULL);

    ZwClose(threadHandle);  // handle no longer needed

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! ObReferenceObjectByHandle failed %!STATUS!", status);
        KeSetEvent(&pCtx->StopEvent, IO_NO_INCREMENT, FALSE);
        pCtx->ThreadObject  = NULL;
        pCtx->ThreadRunning = FALSE;
        return status;
    }

    pCtx->ThreadRunning = TRUE;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Thread started");
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
ProcThreadStop(
    _In_ PDEVICE_CONTEXT pCtx)
{
    PAGED_CODE();

    if (!pCtx->ThreadRunning) {
        return;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! Signalling thread to stop");

    KeSetEvent(&pCtx->StopEvent, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&pCtx->ProcEvent, IO_NO_INCREMENT, FALSE);

    if (pCtx->ThreadObject != NULL) {
        KeWaitForSingleObject(
            pCtx->ThreadObject,
            Executive,
            KernelMode,
            FALSE,
            NULL);

        ObDereferenceObject(pCtx->ThreadObject);
        pCtx->ThreadObject = NULL;
    }

    pCtx->ThreadRunning = FALSE;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Thread stopped");
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
ProcThreadSignal(
    _In_ PDEVICE_CONTEXT pCtx)
{
    KeSetEvent(&pCtx->ProcEvent, IO_DISK_INCREMENT, FALSE);
}

// ---- thread body -----------------------------------------------------------

/*
 * Build and deliver one HID PTP report from a raw USB packet.
 *
 * Returns TRUE  if the report was delivered to a waiting HID request.
 * Returns FALSE if no HID request was pending (caller should retry later).
 *
 * ContactCount is set by AmtPtpProcessTouchFrame and must NOT be
 * overwritten here — it already reflects the PTP snapshot rule
 * (TipSwitch=1 contacts only).
 */
static BOOLEAN
ProcThreadBuildAndDeliver(
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ UCHAR*          TouchBuffer,
    _In_ ULONG           NumBytes)
{
    const size_t headerSize = (size_t)pCtx->DeviceInfo->tp_header;
    const size_t fingerSize = (size_t)pCtx->DeviceInfo->tp_fsize;
    size_t       raw_n;

    PTP_REPORT   ptpReport;
    UCHAR        reportSlots = 0;   // total Contacts[] entries written
                                    // (used only for the copy; ContactCount
                                    //  is managed inside ProcessTouchFrame)

    WDFREQUEST   request;
    WDFMEMORY    requestMemory;
    NTSTATUS     status;
    LARGE_INTEGER now;

    // Validate packet geometry.
    if ((size_t)NumBytes < headerSize ||
        ((size_t)NumBytes - headerSize) % fingerSize != 0)
    {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! Malformed packet len=%lu header=%llu fsize=%llu — dropped",
            NumBytes, (ULONG64)headerSize, (ULONG64)fingerSize);
        return TRUE;  // consume but don't deliver
    }

    // Dequeue a pending HID read request.
    status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &request);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_DRIVER,
            "%!FUNC! No pending HID request");
        return FALSE;
    }

    status = WdfRequestRetrieveOutputMemory(request, &requestMemory);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! RetrieveOutputMemory failed %!STATUS!", status);
        WdfRequestComplete(request, status);
        return TRUE;
    }

    // Build report skeleton.
    RtlZeroMemory(&ptpReport, sizeof(PTP_REPORT));
    ptpReport.ReportID        = REPORTID_MULTITOUCH;
    ptpReport.IsButtonClicked = 0;

    // Scan time in 100 µs units (0.1 ms per unit, per PTP spec).
    //
    // The PerformanceCounter frequency is cached once at D0Entry
    // in pCtx->PerfCounterFreq.  Convert ticks to 100 µs:
    //   scan_time = (now - last) * 10000 / freq
    //
    // Derivation: 1 s = freq ticks, 1 scan unit = 100 µs = 1e-4 s.
    //   delta_ticks / freq = delta_s
    //   delta_s / 1e-4    = delta_s * 10000 = scan_time_units
    //   So: delta_ticks * 10000 / freq = scan_time_units.
    KeQueryPerformanceCounter(&now);
    LONGLONG delta = (now.QuadPart - pCtx->LastReportTime.QuadPart);
    if (delta < 0) delta = 0;
    delta = (delta * 10000) / pCtx->PerfCounterFreq.QuadPart;
    if (delta > 0xFFFF) delta = 0xFFFF;
    ptpReport.ScanTime = (USHORT)delta;

    // Touch contacts.
    if (pCtx->PtpReportTouch) {
        raw_n = ((size_t)NumBytes - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        // AmtPtpProcessTouchFrame:
        //   - writes contact entries to ptpReport.Contacts[]
        //   - sets ptpReport.ContactCount = number of TipSwitch=1 entries
        //   - updates *reportSlots with the total entries written
        AmtPtpProcessTouchFrame(pCtx, TouchBuffer, raw_n, &ptpReport, &reportSlots);

        // DO NOT overwrite ptpReport.ContactCount here.
        // It is already set correctly inside AmtPtpProcessTouchFrame.
    } else {
        ptpReport.ContactCount = 0;
    }

    // Button.
    if (pCtx->PtpReportButton) {
        if (TouchBuffer[pCtx->DeviceInfo->tp_button]) {
            ptpReport.IsButtonClicked = TRUE;
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Button clicked");
        }
    }

    pCtx->LastReportTime = now;

    // Copy the full fixed-size report to the HID request buffer.
    // We always send sizeof(PTP_REPORT) regardless of how many contacts
    // are active — the HID descriptor declares a fixed-length report and
    // unused Contacts[] slots are zero-filled by RtlZeroMemory above.
    status = WdfMemoryCopyFromBuffer(
        requestMemory, 0,
        (PVOID)&ptpReport, sizeof(PTP_REPORT));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfMemoryCopyFromBuffer failed %!STATUS!", status);
        WdfRequestComplete(request, status);
        return TRUE;
    }

    WdfRequestSetInformation(request, sizeof(PTP_REPORT));
    WdfRequestComplete(request, STATUS_SUCCESS);

    InterlockedIncrement(&pCtx->ProcessedPackets);
    return TRUE;
}

static VOID
ProcThreadBody(
    _In_ PVOID Context)
{
    PDEVICE_CONTEXT pCtx = (PDEVICE_CONTEXT)Context;
    PVOID           waitObjects[2];
    NTSTATUS        waitStatus;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! Processing thread started");

    waitObjects[0] = &pCtx->ProcEvent;
    waitObjects[1] = &pCtx->StopEvent;

    for (;;) {
        waitStatus = KeWaitForMultipleObjects(
            2,
            waitObjects,
            WaitAny,
            Executive,
            KernelMode,
            FALSE,
            NULL,
            NULL);

        // Exit signal.
        if (waitStatus == STATUS_WAIT_1 ||
            KeReadStateEvent(&pCtx->StopEvent))
        {
            RING_SLOT* slot;
            while ((slot = RingBufferPeek(&pCtx->RingBuffer)) != NULL) {
                if (!ProcThreadBuildAndDeliver(pCtx, slot->Data, slot->Length)) {
                    while ((slot = RingBufferPeek(&pCtx->RingBuffer)) != NULL) {
                        InterlockedIncrement(&pCtx->DroppedPackets);
                        RingBufferConsumed(&pCtx->RingBuffer);
                    }
                    break;
                }
                RingBufferConsumed(&pCtx->RingBuffer);
            }
            break;
        }

        // STATUS_WAIT_0 — drain all pending slots.
        RING_SLOT* slot;
        while ((slot = RingBufferPeek(&pCtx->RingBuffer)) != NULL &&
               !KeReadStateEvent(&pCtx->StopEvent))
        {
            BOOLEAN delivered = ProcThreadBuildAndDeliver(
                pCtx, slot->Data, slot->Length);
            if (!delivered) {
                break;
            }
            RingBufferConsumed(&pCtx->RingBuffer);
        }
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER,
        "%!FUNC! Processing thread exiting (dropped=%d processed=%d)",
        pCtx->DroppedPackets, pCtx->ProcessedPackets);

    PsTerminateSystemThread(STATUS_SUCCESS);
}