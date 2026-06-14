// Interrupt.c - USB interrupt pipe setup and the minimal read-complete callback.
//
// The callback does exactly three things at DISPATCH_LEVEL:
//   1. Validate packet geometry.
//   2. Copy raw bytes into the ring buffer (RingBufferWrite).
//   3. Signal the processing thread (ProcThreadSignal / KeSetEvent).
//
// All state-machine work, coordinate normalisation, hysteresis, and HID
// request completion happen in ProcessingThread.c at PASSIVE_LEVEL.

#include "Driver.h"
#include "Interrupt.tmh"
#include "include/ProcessingThread.h"

// ---- continuous-reader configuration ---------------------------------------

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG cfg;
    size_t   transferLength = 0;
    NTSTATUS status;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Entry");

    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1: transferLength = HEADER_TYPE1 + FSIZE_TYPE1 * MAX_FINGERS; break;
    case TYPE2: transferLength = HEADER_TYPE2 + FSIZE_TYPE2 * MAX_FINGERS; break;
    case TYPE3: transferLength = HEADER_TYPE3 + FSIZE_TYPE3 * MAX_FINGERS; break;
    case TYPE4: transferLength = HEADER_TYPE4 + FSIZE_TYPE4 * MAX_FINGERS; break;
    case TYPE5: transferLength = HEADER_TYPE5 + FSIZE_TYPE5 * MAX_FINGERS; break;
    }

    if (transferLength == 0) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! Unknown tp_type %d", DeviceContext->DeviceInfo->tp_type);
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    // Guard: the ring slot must be large enough for one full USB frame.
    if (transferLength > RING_PACKET_MAX_SIZE) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! transferLength=%llu exceeds RING_PACKET_MAX_SIZE=%d",
            (ULONG64)transferLength, RING_PACKET_MAX_SIZE);
        NT_ASSERTMSG("Increase RING_PACKET_MAX_SIZE", FALSE);
        status = STATUS_BUFFER_OVERFLOW;
        goto exit;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &cfg,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    cfg.EvtUsbTargetPipeReadersFailed = AmtPtpEvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe, &cfg);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
            "%!FUNC! WdfUsbTargetPipeConfigContinuousReader failed %!STATUS!", status);
    }

exit:
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit %!STATUS!", status);
    // Return SUCCESS: framework drops all future frames if this fails, which
    // is worse than a bad transfer-length guess.
    return NT_SUCCESS(status) ? STATUS_SUCCESS : status;
}

// ---- interrupt completion routine ------------------------------------------
//
// Called at DISPATCH_LEVEL by the framework continuous reader.
// MUST return as fast as possible.

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pCtx = (PDEVICE_CONTEXT)Context;
    UCHAR*          raw;
    BOOLEAN         written;

    // Empty frame — nothing to do.
    if (NumBytesTransferred == 0) {
        return;
    }

    // Quick geometry check: must be at least a header.
    if (NumBytesTransferred < (size_t)pCtx->DeviceInfo->tp_header) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER,
            "%!FUNC! Short packet len=%llu < header=%d — dropped",
            (ULONG64)NumBytesTransferred, pCtx->DeviceInfo->tp_header);
        return;
    }

    raw = (UCHAR*)WdfMemoryGetBuffer(Buffer, NULL);
    if (raw == NULL) {
        return;
    }

    // Attempt to enqueue.  If the ring is full the packet is dropped.
    written = RingBufferWrite(&pCtx->RingBuffer, raw, (ULONG)NumBytesTransferred);
    if (!written) {
        InterlockedIncrement(&pCtx->DroppedPackets);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
            "%!FUNC! Ring full — packet dropped (total dropped=%d)",
            pCtx->DroppedPackets);
        return;
    }

    // Wake the processing thread.
    ProcThreadSignal(pCtx);
}

// ---- reader failure callback -----------------------------------------------

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
        "%!FUNC! USB reader failed NTSTATUS=%!STATUS! USBD=0x%08x",
        Status, UsbdStatus);

    return TRUE;  // ask the framework to restart the reader
}
