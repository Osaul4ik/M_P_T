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
        // Thread will still run; we just can't wait for it.
        // Signal stop immediately so it exits cleanly.
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

    // Signal stop.  Also kick ProcEvent so the thread wakes from its wait.
    KeSetEvent(&pCtx->StopEvent, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&pCtx->ProcEvent, IO_NO_INCREMENT, FALSE);

    if (pCtx->ThreadObject != NULL) {
        KeWaitForSingleObject(
            pCtx->ThreadObject,
            Executive,
            KernelMode,
            FALSE,
            NULL);              // wait indefinitely

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
    // IO_NETWORK_INCREMENT (2) gives a small boost; avoids the
    // full THREAD_PRIORITY_NORMAL boost from IO_NO_INCREMENT while still
    // waking the thread promptly.
    KeSetEvent(&pCtx->ProcEvent, IO_DISK_INCREMENT, FALSE);
}

// ---- thread body -----------------------------------------------------------

/*
 * Build and deliver one HID PTP report from a raw USB packet.
 *
 * Returns TRUE  if the report was delivered to a waiting HID request.
 * Returns FALSE if no HID request was pending (caller should retry later).
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
    UCHAR        reportSlots = 0;

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
        // No request available — caller will retry on next wake.
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

    // Scan time in 100 µs units.
    KeQueryPerformanceCounter(&now);
    LONGLONG delta = (now.QuadPart - pCtx->LastReportTime.QuadPart) / 100;
    if (delta > 0xFFFF) delta = 0xFFFF;
    ptpReport.ScanTime = (USHORT)delta;

    // Touch contacts.
    if (pCtx->PtpReportTouch) {
        raw_n = ((size_t)NumBytes - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;
        AmtPtpProcessTouchFrame(pCtx, TouchBuffer, raw_n, &ptpReport, &reportSlots);
        ptpReport.ActualCount   = reportSlots;   // how many slots carry valid data
        ptpReport.ContactCount  = reportSlots;
    } else {
        ptpReport.ActualCount   = 0;
        ptpReport.ContactCount  = 0;
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

    // Copy report to the HID request buffer.
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

    // We wait on two objects simultaneously:
    //   [0] ProcEvent — new USB packet available
    //   [1] StopEvent — time to exit
    waitObjects[0] = &pCtx->ProcEvent;
    waitObjects[1] = &pCtx->StopEvent;

    for (;;) {
        // Wait for either a new packet or a stop request.
        waitStatus = KeWaitForMultipleObjects(
            2,
            waitObjects,
            WaitAny,
            Executive,
            KernelMode,
            FALSE,
            NULL,   // no timeout
            NULL);

        // Exit signal.
        if (waitStatus == STATUS_WAIT_1 ||
            KeReadStateEvent(&pCtx->StopEvent))
        {
            // Drain ring before exiting so no packet is silently lost
            // when the ring still has data right at shutdown.
            // If no HID request is pending, just discard the slot rather than
            // busy-wait — otherwise ProcThreadStop would deadlock waiting for
            // this thread.
            RING_SLOT* slot;
            while ((slot = RingBufferPeek(&pCtx->RingBuffer)) != NULL) {
                if (!ProcThreadBuildAndDeliver(pCtx, slot->Data, slot->Length)) {
                    // No HID request — discard the remaining bits so we can exit.
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

    // STATUS_WAIT_0 — ProcEvent fired.  Drain all pending slots.
    // Also check StopEvent inside the loop: if both events were set,
    // KeWaitForMultipleObjects returns STATUS_WAIT_0 (ProcEvent has
    // lower index), so we MUST poll StopEvent here to avoid a
    // busy-spin on the ring when shutdown is requested.
    RING_SLOT* slot;
    while ((slot = RingBufferPeek(&pCtx->RingBuffer)) != NULL &&
           !KeReadStateEvent(&pCtx->StopEvent))
    {
        BOOLEAN delivered = ProcThreadBuildAndDeliver(
            pCtx, slot->Data, slot->Length);
        if (!delivered) {
            // No HID request available.
            // Leave this slot in the ring — we'll retry when the next
            // packet arrives (which re-signals ProcEvent) or when the
            // HID stack posts a new read request and the driver calls
            // ProcThreadSignal from AmtPtpDispatchReadReportRequests.
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
