/*++
Module Name:
    device.h
Abstract:
    Device context and exported function declarations for the Apple SPI PTP driver.
Environment:
    Kernel-mode Driver Framework
--*/

#pragma once
#include "public.h"
#include "ContactTracking.h"

#ifndef PTP_MAX_CONTACT_POINTS
#define PTP_MAX_CONTACT_POINTS 5
#endif

EXTERN_C_START

// ── Hardware descriptor ───────────────────────────────────────────────────

typedef struct _SPI_TRACKPAD_INFO {
    USHORT VendorId;
    USHORT ProductId;
    SHORT  XMin;
    SHORT  XMax;
    SHORT  YMin;
    SHORT  YMax;
} SPI_TRACKPAD_INFO, *PSPI_TRACKPAD_INFO;

// ── Enumerations ──────────────────────────────────────────────────────────

typedef enum _REPORT_TYPE {
    PrecisionTouchpad  = 0,
    Touchscreen        = 1,
    InvalidReportType  = 0x7fffffff,
} REPORT_TYPE;

// Written from PnP callbacks (PASSIVE_LEVEL); read from completion routine
// (DISPATCH_LEVEL).  Stored as LONG so InterlockedExchange/
// InterlockedCompareExchange can be used for cross-IRQL visibility.
typedef enum _PTP_AAPL_DEVICE_POWER_STATUS {
    D3                    = 0,
    D0ActiveAndConfigured = 1,
    D0ActiveAndUnconfigured = 2,
} PTP_AAPL_DEVICE_POWER_STATUS;

// ── Device context ────────────────────────────────────────────────────────

typedef struct _DEVICE_CONTEXT {

    //
    // ── WDF objects ──────────────────────────────────────────────────────
    //
    WDFDEVICE   SpiDevice;
    WDFIOTARGET SpiTrackpadIoTarget;
    WDFQUEUE    HidQueue;
    WDFTIMER    PowerOnRecoveryTimer;

    //
    // ── Power state ───────────────────────────────────────────────────────
    // volatile LONG so InterlockedExchange / InterlockedCompareExchange give
    // acquire/release semantics across PASSIVE and DISPATCH levels.
    //
    volatile LONG DeviceStatus;

    //
    // ── Device metadata (set once in PrepareHardware, read-only after) ────
    //
    USHORT           HidVendorID;
    USHORT           HidProductID;
    USHORT           HidVersionNumber;
    SPI_TRACKPAD_INFO TrackpadInfo;
    REPORT_TYPE      ReportType;

    //
    // ── Precomputed coordinate ranges (set in SelfManagedIoInit) ─────────
    // XRange = XMax - XMin, YRange = YMax - YMin.
    // Avoids repeated signed subtraction in the 125-250 Hz hot path.
    //
    USHORT XRange;
    USHORT YRange;

    //
    // ── Windows PTP mode flags ────────────────────────────────────────────
    // Written from SET_FEATURE (PASSIVE_LEVEL via Queue.c).
    // Read from completion routine (DISPATCH_LEVEL).
    // BOOLEAN is a single byte: load/store is atomic on x64/ARM64.
    // A one-frame race (stale read in completion) is benign.
    //
    BOOLEAN PtpInputOn;
    BOOLEAN PtpReportTouch;
    BOOLEAN PtpReportButton;

    //
    // ── ScanTime tracking ─────────────────────────────────────────────────
    // KeQueryInterruptTime() → 100 ns units.  Divide by 1000 for 100 µs PTP
    // ScanTime units.  Stored as ULONGLONG (unsigned) to avoid signed-
    // subtraction hazard when wrapping.
    // Access: only inside AmtPtpRequestCompletionRoutine (serialised by the
    // single-request invariant — see SpiHidReadRequest comment below).
    //
    ULONGLONG LastReportTime;

    //
    // ── Contact tracking table ────────────────────────────────────────────
    // Implements the persistent slot / proximity-match architecture.
    // See ContactTracking.h for full design documentation.
    // Access: only inside AmtPtpRequestCompletionRoutine (serialised).
    //
    CONTACT_TABLE Contacts;

    //
    // ── Pre-allocated SPI read pipeline ──────────────────────────────────
    // Created once in AmtPtpDeviceSpiKmCreateDevice; lifetime = Device object.
    //
    // INVARIANT: at most one SPI read request is outstanding at any time.
    // The completion routine re-issues it at the very end.  This invariant
    // serialises ALL access to LastReportTime and Contacts without a spinlock.
    //
    WDFLOOKASIDE HidReadBufferLookaside;
    WDFREQUEST   SpiHidReadRequest;
    WDFMEMORY    SpiHidReadOutputMemory;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// ── Request worker context ────────────────────────────────────────────────

typedef struct _WORKER_REQUEST_CONTEXT {
    PDEVICE_CONTEXT DeviceContext;
    WDFMEMORY       RequestMemory;
} WORKER_REQUEST_CONTEXT, *PWORKER_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WORKER_REQUEST_CONTEXT, WorkerRequestGetContext)

// ── Atomic DeviceStatus helpers ───────────────────────────────────────────
// Use these everywhere — never read/write DeviceStatus directly.

#define DEVICE_STATUS_WRITE(ctx, val) \
    InterlockedExchange(&(ctx)->DeviceStatus, (LONG)(val))

#define DEVICE_STATUS_READ(ctx) \
    ((PTP_AAPL_DEVICE_POWER_STATUS)InterlockedCompareExchange( \
        &(ctx)->DeviceStatus, 0, 0))

// ── Palm rejection thresholds ─────────────────────────────────────────────
// Apple SPI raw units (~0.01 mm each).
// A palm has BOTH axes >= threshold simultaneously.
// A finger at an angle has large Major but small Minor → must not be rejected.
// 3500 units ≈ 35 mm per axis.  A palm covers 60-80 mm; a finger tip 8-12 mm.
#define PALM_MAJOR_THRESHOLD 3500
#define PALM_MINOR_THRESHOLD 3500

// ── ScanTime caps ─────────────────────────────────────────────────────────
// Units: 100 µs per HID PTP spec.
// IDLE_THRESHOLD: if delta > 500ms treat as first frame of new gesture.
// FIRST_FRAME_CAP: clamp first-frame delta to 8ms (typical 125 Hz period).
#define IDLE_SCANTIME_THRESHOLD  5000u   // 500 ms
#define FIRST_FRAME_SCANTIME_CAP   80u   //   8 ms

// ── Function declarations ─────────────────────────────────────────────────

NTSTATUS
AmtPtpDeviceSpiKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
);

EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpEvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY         AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          AmtPtpEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_STOP        AmtPtpEvtIoStop;

NTSTATUS
AmtPtpEvtDeviceSelfManagedIoInitOrRestart(
    _In_ WDFDEVICE Device
);

NTSTATUS
AmtPtpSpiSetState(
    _In_ WDFDEVICE Device,
    _In_ BOOLEAN   DesiredState
);

PCHAR
DbgDevicePowerString(
    _In_ WDF_POWER_DEVICE_STATE Type
);

VOID
AmtPtpPowerRecoveryTimerCallback(
    WDFTIMER Timer
);

EXTERN_C_END
