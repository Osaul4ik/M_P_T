/*++
Module Name:
    device.h
Abstract:
    This file contains the device definitions.
Environment:
    Kernel-mode Driver Framework
--*/
#include "public.h"
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
	PTP_AAPL_DEVICE_POWER_STATUS DeviceStatus;
	WDFQUEUE	HidQueue;
	// SPI device metadata
	USHORT HidVendorID;
	USHORT HidProductID;
	USHORT HidVersionNumber;
	SPI_TRACKPAD_INFO TrackpadInfo;
	REPORT_TYPE ReportType;
	// Windows PTP context
	BOOLEAN PtpInputOn;
	BOOLEAN PtpReportTouch;
	BOOLEAN PtpReportButton;
	// Timer
	LARGE_INTEGER LastReportTime;
	WDFTIMER PowerOnRecoveryTimer;
	// Performance counter frequency, cached at D0Entry.
	LONGLONG PerformanceFrequency;
	// Precomputed coordinate ranges, cached at D0Entry.
	USHORT XRange;
	USHORT YRange;
	// Finger tracking state for stable ContactID assignment.
	// Apple SPI packs active fingers densely with no slot ID.
	// We match fingers between frames by OriginalX/Y (touchdown coordinates)
	// and assign a rolling ContactID that stays stable for the finger lifetime.
	SHORT  PrevOriginalX[PTP_MAX_CONTACT_POINTS];
	SHORT  PrevOriginalY[PTP_MAX_CONTACT_POINTS];
	UINT8  SlotContactID[PTP_MAX_CONTACT_POINTS];
	UINT8  NextContactID;
	// List of buffers
	WDFLOOKASIDE HidReadBufferLookaside;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)
typedef struct _WORKER_REQUEST_CONTEXT {
	PDEVICE_CONTEXT DeviceContext;
	WDFMEMORY RequestMemory;
} WORKER_REQUEST_CONTEXT, *PWORKER_REQUEST_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(WORKER_REQUEST_CONTEXT, WorkerRequestGetContext)
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
EXTERN_C_END
