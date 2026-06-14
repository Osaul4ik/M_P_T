/*++

Module Name:

    device.h

Abstract:

    This file contains the device definitions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "public.h"
#include "include/Hid.h"

EXTERN_C_START

//
// The device context performs the same job as
// a WDM device extension in the driver frameworks
//

typedef struct _DEVICE_CONTEXT
{
    // USB Things
    WDFUSBDEVICE        UsbDevice;
    WDFUSBPIPE          InterruptPipe;
    WDFUSBINTERFACE     UsbInterface;
    WDFQUEUE            InputQueue;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    ULONG               UsbDeviceTraits;

    // Device Config
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN             IsWellspringModeOn;

    // PTP Status
    BOOLEAN             PtpInputOn;
    BOOLEAN             PtpReportTouch;
    BOOLEAN             PtpReportButton;

    // Timer
    LARGE_INTEGER       LastReportTime;

    //
    // Slot-based contact tracking
    // ---------------------------------------------------------------
    // Each slot index IS the ContactID reported to Windows.
    //
    // Lifecycle:
    //   COOLDOWN → FREE → CONFIRMING → ACTIVE → PENDING_RELEASE → COOLDOWN
    //
    // SlotInUse         - slot is ACTIVE: finger confirmed, being reported.
    // SlotPendingRelease- finger disappeared this frame; hold for 1 frame
    //                     before actually freeing the slot.
    // SlotCooldown      - slot was just released; block reassignment for
    //                     1 frame to prevent flicker-based reuse.
    // SlotTipConfirmed  - counts consecutive tip-down frames for this slot
    //                     (debounce: must reach TIP_CONFIRM_FRAMES before
    //                     SlotInUse is set and the contact is emitted).
    // SlotFingerKey     - a stable identity key for the physical finger
    //                     occupying this slot, used to re-bind across frames
    //                     without relying on USB array index.
    //                     Currently encoded as: (UCHAR)firstSeenUsbIndex.
    //                     Set on first confirmation, cleared on release.
    //
    BOOLEAN             SlotInUse[PTP_MAX_CONTACT_POINTS];
    BOOLEAN             SlotPendingRelease[PTP_MAX_CONTACT_POINTS];
    UCHAR               SlotCooldown[PTP_MAX_CONTACT_POINTS];     // >0 = blocked; set to 2 on lift, decremented each frame
    UCHAR               SlotTipConfirmed[PTP_MAX_CONTACT_POINTS]; // consecutive tip-down frame counter
    UCHAR               SlotFingerKey[PTP_MAX_CONTACT_POINTS];    // finger identity (USB array index at first touch)

    // Last reported normalised coordinates — used for lift-event report so
    // the final position is stable. Scoped to a single ACTIVE gesture: see
    // InterruptTouch.c header comment for the write/read/zero lifecycle.
    USHORT              LastNormX[PTP_MAX_CONTACT_POINTS];
    USHORT              LastNormY[PTP_MAX_CONTACT_POINTS];

    // Per-slot hysteresis: suppress sub-deadzone jitter while a finger stays
    // down.  No prediction or smoothing is applied.
    USHORT              HystX[PTP_MAX_CONTACT_POINTS];
    USHORT              HystY[PTP_MAX_CONTACT_POINTS];

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

//
// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

//
// Pool tags
//
#define POOL_TAG_PTP_CONTROL 'PTPC'

//
// Function to initialize the device's queues and callbacks
//
NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

//
// Function to select the device's USB configuration and get a WDFUSBDEVICE
// handle
//
EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY         AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          AmtPtpEvtDeviceD0Exit;

//
// Function to select interrupt interface
//
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
    _In_ WDFDEVICE Device
);

//
// Function to configure interrupt
//
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext
);

//
// Functions to serve interrupt
//
EVT_WDF_USB_READER_COMPLETION_ROUTINE AmtPtpEvtUsbInterruptPipeReadComplete;
EVT_WDF_USB_READERS_FAILED            AmtPtpEvtUsbInterruptReadersFailed;

//
// Touch frame processing (slot state machine, InterruptTouch.c)
//
VOID
AmtPtpProcessTouchFrame(
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ UCHAR* TouchBuffer,
    _In_ size_t raw_n,
    _Inout_ PTP_REPORT* PtpReport,
    _Inout_ UCHAR* pReportSlots
);

//
// Debug utilities
//
PCHAR
DbgDevicePowerString(
    _In_ WDF_POWER_DEVICE_STATE Type
);

PCHAR
DbgIoControlGetString(
    _In_ ULONG IoControlCode
);

//
// Device functions
//
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN IsWellspringModeOn
);

//
// HID routines
//
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetHidDescriptor(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetDeviceAttribs(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetReportDescriptor(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

NTSTATUS
AmtPtpDispatchReadReportRequests(
    _In_  WDFDEVICE  Device,
    _In_  WDFREQUEST Request,
    _Out_ BOOLEAN*   Pending
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpReportFeatures(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetFeatures(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request
);

EXTERN_C_END