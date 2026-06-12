/*++
Module Name:
    device.h
Abstract:
    This file contains the device definitions.
Environment:
    Kernel-mode Driver Framework
--*/
#include "public.h"
#ifndef PTP_MAX_CONTACT_POINTS
#define PTP_MAX_CONTACT_POINTS 5
#endif
EXTERN_C_START
typedef struct _SPI_TRACKPAD_INFO {
	USHORT VendorId;
	USHORT ProductId;
	SHORT XMin;
	SHORT XMax;
	SHORT YMin;
	SHORT YMax;
} SPI_TRACKPAD_INFO, *PSPI_TRACKPAD_INFO;
typedef enum _REPORT_TYPE {
	PrecisionTouchpad = 0,
	Touchscreen = 1,
	InvalidReportType = 0x7fffffff,
} REPORT_TYPE;
typedef enum _PTP_AAPL_DEVICE_POWER_STATUS {
	D3 = 0,
	D0ActiveAndConfigured = 1,
	D0ActiveAndUnconfigured = 2
} PTP_AAPL_DEVICE_POWER_STATUS;
typedef struct _DEVICE_CONTEXT
{
	// IO content
	WDFDEVICE	SpiDevice;
	WDFIOTARGET SpiTrackpadIoTarget;
	// FIX (sync): DeviceStatus is written from PnP callbacks (PASSIVE_LEVEL)
	// and read from the completion routine (DISPATCH_LEVEL).  Declared LONG so
	// it can be updated with InterlockedExchange / read with
	// InterlockedCompareExchange for safe cross-IRQL visibility.
	// Cast to/from PTP_AAPL_DEVICE_POWER_STATUS at call sites.
	volatile LONG DeviceStatus;
	WDFQUEUE	HidQueue;
	// SPI device metadata
	USHORT HidVendorID;
	USHORT HidProductID;
	USHORT HidVersionNumber;
	SPI_TRACKPAD_INFO TrackpadInfo;
	REPORT_TYPE ReportType;
	// Windows PTP mode flags (set via SET_FEATURE from Windows).
	BOOLEAN PtpInputOn;
	BOOLEAN PtpReportTouch;
	BOOLEAN PtpReportButton;
	// FIX (sync): LastReportTime is written at DISPATCH_LEVEL inside the
	// completion routine and read from the same routine (single pre-allocated
	// request guarantees no concurrent completion).  Stored as ULONGLONG
	// (unsigned) to avoid signed/unsigned subtraction hazards; the previous
	// LARGE_INTEGER union forced a cast to LONGLONG which could produce
	// spurious negatives on wrap or corruption.
	volatile ULONGLONG LastReportTime;
	WDFTIMER PowerOnRecoveryTimer;
	// Performance counter frequency, cached once at D0Entry.
	// KeQueryPerformanceFrequency returns a hardware constant;
	// caching avoids a kernel call on every SPI completion (~125-250 Hz).
	LONGLONG PerformanceFrequency;
	// Precomputed coordinate ranges, cached at D0Entry.
	// XRange = XMax - XMin, YRange = YMax - YMin (both positive).
	// Avoids repeated signed arithmetic in the hot completion path.
	USHORT XRange;
	USHORT YRange;
	// Previous hardware finger count (clamped to PTP_MAX_CONTACT_POINTS).
	// Kept separate from PrevReportedCount — used only as a raw snapshot
	// of what the hardware reported.  Do NOT use for lift-frame logic or
	// ScanTime cap; use PrevReportedCount / PrevReportedMask instead.
	// Access is safe: only written/read inside AmtPtpRequestCompletionRoutine,
	// which is serialized by the single pre-allocated request.
	UINT8 PrevAdjustedCount;
	// Number of contacts actually emitted to the host in the previous report
	// (excludes palm-suppressed and lift-frame slots).
	// Used for the first-frame ScanTime cap condition.
	UINT8 PrevReportedCount;
	// Bitmask of ContactIDs emitted with TipSwitch=1 in the previous report.
	// Bit N set  =>  ContactID N was live in the last HID report.
	// Used to detect which contacts disappeared so we can emit TipSwitch=0
	// lift frames, preventing ghost contacts / cursor-jump on re-touch.
	// Invariant: only bits 0..(PTP_MAX_CONTACT_POINTS-1) are ever set.
	UINT8 PrevReportedMask;
	// Per-slot palm state, re-evaluated each frame.
	BOOLEAN SlotIsPalm[PTP_MAX_CONTACT_POINTS];
	// Read buffer lookaside list.
	WDFLOOKASIDE HidReadBufferLookaside;
	// Pre-allocated SPI read request and output memory, reused every frame
	// via WdfRequestReuse to avoid per-frame kernel object alloc/free.
	// Allocated once in AmtPtpDeviceSpiKmCreateDevice, never freed until
	// device removal (lifetime = Device object).
	// Invariant: only one outstanding request at a time — completion routine
	// re-issues it after each frame.  All PrevXxx fields above are safe
	// without a spinlock solely because of this single-request invariant.
	WDFREQUEST  SpiHidReadRequest;
	WDFMEMORY   SpiHidReadOutputMemory;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)
typedef struct _WORKER_REQUEST_CONTEXT {
	PDEVICE_CONTEXT DeviceContext;
	WDFMEMORY RequestMemory;
} WORKER_REQUEST_CONTEXT, *PWORKER_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WORKER_REQUEST_CONTEXT, WorkerRequestGetContext)

// Helpers for atomic DeviceStatus access.
// Writers (PnP callbacks, timer): use DEVICE_STATUS_WRITE.
// Readers (completion routine at DISPATCH_LEVEL): use DEVICE_STATUS_READ.
#define DEVICE_STATUS_WRITE(ctx, val) \
	InterlockedExchange(&(ctx)->DeviceStatus, (LONG)(val))
#define DEVICE_STATUS_READ(ctx) \
	((PTP_AAPL_DEVICE_POWER_STATUS)InterlockedCompareExchange( \
		&(ctx)->DeviceStatus, 0, 0))

NTSTATUS
AmtPtpDeviceSpiKmCreateDevice(
	_Inout_ PWDFDEVICE_INIT DeviceInit
);
EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpEvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT AmtPtpEvtDeviceD0Exit;
NTSTATUS
AmtPtpEvtDeviceSelfManagedIoInitOrRestart(
	_In_ WDFDEVICE Device
);
PCHAR
DbgDevicePowerString(
	_In_ WDF_POWER_DEVICE_STATE Type
);
NTSTATUS
AmtPtpSpiSetState(
	_In_ WDFDEVICE Device,
	_In_ BOOLEAN DesiredState
);
void AmtPtpPowerRecoveryTimerCallback(
	WDFTIMER Timer
);
// EvtIoStop handler registered on HidQueue.
// Required to allow KMDF to drain the queue during power transitions and
// surprise removal — without it the framework cannot guarantee all requests
// are complete before the device powers down, risking DRIVER_POWER_STATE_FAILURE (0x9F).
EVT_WDF_IO_QUEUE_IO_STOP AmtPtpEvtIoStop;
EXTERN_C_END
