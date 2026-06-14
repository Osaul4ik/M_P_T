/*
 * RingBuffer.h - Single-producer single-consumer lock-free ring buffer
 *                for raw USB touchpad packets.
 *
 * Producer: USB continuous-reader callback (DISPATCH_LEVEL)
 * Consumer: processing thread (PASSIVE_LEVEL)
 *
 * Capacity must be a power of two.  Each slot holds one raw USB frame
 * (up to RING_PACKET_MAX_SIZE bytes).  The actual byte length is stored
 * per-slot so the consumer knows how many bytes to inspect.
 *
 * Ordering:
 *   WriteIndex is written only by the producer after the payload copy.
 *   ReadIndex  is written only by the consumer after processing.
 *   Both are declared volatile; KeMemoryBarrier() is used instead of
 *   InterlockedXxx to avoid the full fence on the hot (producer) path.
 */

#pragma once

#include <ntddk.h>

// Maximum raw USB frame we will ever receive.
// TYPE4: HEADER_TYPE4 + MAX_FINGERS * FSIZE_TYPE4 = 46 + 16*30 = 526 bytes.
// Round up to a cache-friendly power of two.
#define RING_PACKET_MAX_SIZE    544

// Number of slots (must be a power of two, >= 2).
// 32 slots × 544 bytes = 17 KB — fits comfortably in non-paged pool per
// device; backpressure kicks in when the consumer is > 31 frames behind.
#define RING_CAPACITY           32
#define RING_MASK               (RING_CAPACITY - 1)

typedef struct _RING_SLOT {
    UCHAR   Data[RING_PACKET_MAX_SIZE];
    ULONG   Length;                     // valid bytes in Data
} RING_SLOT;

typedef struct _USB_PACKET_RING {
    RING_SLOT   Slots[RING_CAPACITY];

    // Indices are 32-bit; wrap-around is safe due to masking.
    volatile ULONG  WriteIndex;         // owned by producer
    volatile ULONG  ReadIndex;          // owned by consumer
} USB_PACKET_RING;

//
// Initialize ring to empty state.  Call once from PrepareHardware at
// PASSIVE_LEVEL before starting the USB reader or the processing thread.
//
static inline VOID
RingBufferInit(
    _Out_ USB_PACKET_RING* Ring)
{
    RtlZeroMemory(Ring, sizeof(*Ring));
}

//
// Returns the number of slots currently occupied (readable).
//
static inline ULONG
RingBufferCount(
    _In_ const USB_PACKET_RING* Ring)
{
    return Ring->WriteIndex - Ring->ReadIndex;
}

//
// Returns TRUE if the ring has no unread slots.
//
static inline BOOLEAN
RingBufferIsEmpty(
    _In_ const USB_PACKET_RING* Ring)
{
    return Ring->WriteIndex == Ring->ReadIndex;
}

//
// Returns TRUE if all slots are occupied.
//
static inline BOOLEAN
RingBufferIsFull(
    _In_ const USB_PACKET_RING* Ring)
{
    return (Ring->WriteIndex - Ring->ReadIndex) >= RING_CAPACITY;
}

//
// Producer: copy up to RING_PACKET_MAX_SIZE bytes into the next write slot.
// Returns TRUE on success, FALSE if the ring is full (packet dropped).
//
// May be called at DISPATCH_LEVEL.
//
static inline BOOLEAN
RingBufferWrite(
    _Inout_ USB_PACKET_RING*    Ring,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG                  Length)
{
    ULONG wi;
    RING_SLOT* slot;

    if (RingBufferIsFull(Ring)) {
        return FALSE;
    }

    if (Length > RING_PACKET_MAX_SIZE) {
        Length = RING_PACKET_MAX_SIZE;
    }

    wi   = Ring->WriteIndex & RING_MASK;
    slot = &Ring->Slots[wi];

    RtlCopyMemory(slot->Data, Data, Length);
    slot->Length = Length;

    // Store-store barrier: ensure payload is visible before WriteIndex bump.
    KeMemoryBarrier();
    Ring->WriteIndex++;

    return TRUE;
}

//
// Consumer: obtain a pointer to the next readable slot (zero-copy read).
// Returns NULL if the ring is empty.  Caller must call RingBufferConsumed()
// after processing to release the slot.
//
// Must be called at PASSIVE_LEVEL (processing thread).
//
static inline RING_SLOT*
RingBufferPeek(
    _In_ USB_PACKET_RING* Ring)
{
    if (RingBufferIsEmpty(Ring)) {
        return NULL;
    }
    // Load-load barrier: ensure we read WriteIndex before slot data.
    KeMemoryBarrier();
    return &Ring->Slots[Ring->ReadIndex & RING_MASK];
}

//
// Consumer: advance ReadIndex after processing the peeked slot.
//
static inline VOID
RingBufferConsumed(
    _Inout_ USB_PACKET_RING* Ring)
{
    KeMemoryBarrier();
    Ring->ReadIndex++;
}
