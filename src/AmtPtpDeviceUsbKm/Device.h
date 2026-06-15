/*++

Module Name:

    device.h

Abstract:

    Device context and function declarations.

    Threading model
    ---------------
    DISPATCH_LEVEL (USB continuous-reader callback, AmtPtpEvtUsbInterruptPipeReadComplete):
        - copies raw USB packet into RingBuffer
        - signals ProcEvent (KeSetEvent)
        - no HID request manipulation, no slot state access

    PASSIVE_LEVEL (ProcessingThread):
        - drains RingBuffer
        - runs slot state machine (InterruptTouch.c)
        - dequeues HID read requests from InputQueue and completes them

    All fields in DEVICE_CONTEXT that are shared between DISPATCH and
    PASSIVE are accessed ONLY by the processing thread except where
    explicitly noted (RingBuffer, ProcEvent).

Environment:

    Kernel-mode Driver Framework

--*/

#pragma once

#include "public.h"
#include "include/Hid.h"
#include "include/RingBuffer.h"

EXTERN_C_START

typedef struct _DEVICE_CONTEXT
{
    // ---------------------------------------------------------------
    // USB infrastructure
    // ---------------------------------------------------------------
    WDFUSBDEVICE        UsbDevice;
    WDFUSBPIPE          InterruptPipe;
    WDFUSBINTERFACE     UsbInterface;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    ULONG               UsbDeviceTraits;

    // ---------------------------------------------------------------
    // Device configuration (read-only after PrepareHardware)
    // ---------------------------------------------------------------
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN             IsWellspringModeOn;

    // ---------------------------------------------------------------
    // HID input queue (manual dispatch, power-unmanaged)
    // Read by processing thread at PASSIVE_LEVEL;
    // requests are forwarded into it from DISPATCH_LEVEL queue handler.
    // ---------------------------------------------------------------
    WDFQUEUE            InputQueue;

    // ---------------------------------------------------------------
    // PTP feature flags (written only from SetFeatures / PrepareHardware,
    // both at PASSIVE_LEVEL; read from processing thread — no locking
    // needed as they are stable during runtime).
    // ---------------------------------------------------------------
    BOOLEAN             PtpInputOn;
    BOOLEAN             PtpReportTouch;
    BOOLEAN             PtpReportButton;

    // ---------------------------------------------------------------
    // Scan-time reference (owned by processing thread only)
    // ---------------------------------------------------------------
    LARGE_INTEGER       LastReportTime;

    // Cached performance counter frequency (initialised once at D0Entry).
    LARGE_INTEGER       PerfCounterFreq;

    // ---------------------------------------------------------------
    // Ring buffer — single-producer (USB callback, DISPATCH_LEVEL),
    // single-consumer (processing thread, PASSIVE_LEVEL).
    // No spinlock required; see RingBuffer.h for the ordering protocol.
    // ---------------------------------------------------------------
    USB_PACKET_RING     RingBuffer;

    // ---------------------------------------------------------------
    // Processing thread control
    //   ProcEvent    — auto-reset kernel event; set by USB callback,
    //                  waited by processing thread.
    //   StopEvent    — manual-reset; set by ProcThreadStop() to ask
    //                  the thread to exit.
    //   ThreadObject — used by ProcThreadStop() to wait for exit.
    // ---------------------------------------------------------------
    KEVENT              ProcEvent;
    KEVENT              StopEvent;
    PVOID               ThreadObject;
    BOOLEAN             ThreadRunning;

    // ---------------------------------------------------------------
    // Slot-based contact tracking — owned exclusively by the
    // processing thread (PASSIVE_LEVEL).  No locking required.
    //
    // Lifecycle:
    //   FREE -> CONFIRMING -> ACTIVE -> PENDING_RELEASE -> COOLDOWN -> FREE
    //
    // Consolidated into one SLOT_STATE struct per contact point (see
    // include/Hid.h for the field layout and per-phase invariants).
    // AmtAssertSlotInvariants() in InterruptTouch.c validates these
    // invariants after every processed frame in debug builds.
    // ---------------------------------------------------------------
    SLOT_STATE          Slots[PTP_MAX_CONTACT_POINTS];

    // ---------------------------------------------------------------
    // Stable ContactID allocator
    // Monotonically incrementing counter for unique finger IDs.
    // Written at PASSIVE_LEVEL (processing thread only).
    // ---------------------------------------------------------------
    ULONG               NextContactID;

    // ---------------------------------------------------------------
    // Palm lock: when TRUE, ALL touch inputs are rejected
    // until no palm is detected for PALM_COOLDOWN_FRAMES consecutive
    // frames (hysteresis).  PalmCooldown is the remaining countdown.
    // Owned by processing thread (PASSIVE_LEVEL) only.
    // ---------------------------------------------------------------
    BOOLEAN             PalmDetected;
    UCHAR               PalmCooldown;

    // ---------------------------------------------------------------
    // Diagnostics (interlocked — safe from any level)
    // ---------------------------------------------------------------
    volatile LONG       DroppedPackets;   // incremented when ring is full
    volatile LONG       ProcessedPackets;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

//
// Pool tags
//
#define POOL_TAG_PTP_CONTROL    'PTPC'
#define POOL_TAG_PTP_THREAD     'PTPT'

// ---------------------------------------------------------------
// Device lifecycle
// ---------------------------------------------------------------
NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit);

EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY         AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          AmtPtpEvtDeviceD0Exit;

// ---------------------------------------------------------------
// USB helpers
// ---------------------------------------------------------------
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
    _In_ WDFDEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN IsWellspringModeOn);

// ---------------------------------------------------------------
// Interrupt / continuous reader
// ---------------------------------------------------------------
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext);

EVT_WDF_USB_READER_COMPLETION_ROUTINE AmtPtpEvtUsbInterruptPipeReadComplete;
EVT_WDF_USB_READERS_FAILED            AmtPtpEvtUsbInterruptReadersFailed;

// ---------------------------------------------------------------
// Touch frame processing (InterruptTouch.c — runs on processing thread)
// ---------------------------------------------------------------
VOID
AmtPtpProcessTouchFrame(
    _In_    PDEVICE_CONTEXT pCtx,
    _In_    UCHAR*          TouchBuffer,
    _In_    size_t          raw_n,
    _Inout_ PTP_REPORT*     PtpReport,
    _Inout_ UCHAR*          pReportSlots);

// ---------------------------------------------------------------
// HID
// ---------------------------------------------------------------
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetHidDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetDeviceAttribs(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetReportDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

NTSTATUS AmtPtpDispatchReadReportRequests(
    _In_  WDFDEVICE  Device,
    _In_  WDFREQUEST Request,
    _Out_ BOOLEAN*   Pending);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpReportFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpSetFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

// ---------------------------------------------------------------
// Slot reset helper (shared between Device.c and ProcessingThread.c)
// ---------------------------------------------------------------
VOID
AmtPtpResetSlotState(
    _In_ PDEVICE_CONTEXT pDeviceContext);

// ---------------------------------------------------------------
// Debug
// ---------------------------------------------------------------
PCHAR DbgDevicePowerString(_In_ WDF_POWER_DEVICE_STATE Type);
PCHAR DbgIoControlGetString(_In_ ULONG IoControlCode);

EXTERN_C_END