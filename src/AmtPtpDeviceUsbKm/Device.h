/*
 * Device.h - Device definitions.
 * Kernel-mode Driver Framework
 */

#include "public.h"
#include <Hid.h>

EXTERN_C_START

typedef struct _DEVICE_CONTEXT
{
    // USB
    WDFUSBDEVICE    UsbDevice;
    WDFUSBPIPE      InterruptPipe;
    WDFUSBINTERFACE UsbInterface;
    WDFQUEUE        InputQueue;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    ULONG           UsbDeviceTraits;

    // Device config
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN IsWellspringModeOn;

    // PTP state
    BOOLEAN PtpInputOn;
    BOOLEAN PtpReportTouch;
    BOOLEAN PtpReportButton;

    // Scan time
    LARGE_INTEGER LastReportTime;

    // Palm rejection
    BOOLEAN PalmDetected;

    // Per-slot smoothing (indexed by raw slot 0..PTP_MAX_CONTACT_POINTS-1)
    USHORT  SmoothedX[PTP_MAX_CONTACT_POINTS];
    USHORT  SmoothedY[PTP_MAX_CONTACT_POINTS];
    USHORT  HystX[PTP_MAX_CONTACT_POINTS];
    USHORT  HystY[PTP_MAX_CONTACT_POINTS];
    BOOLEAN SlotActive[PTP_MAX_CONTACT_POINTS];

    //
    // FIX (tip-size debounce / cursor-jump):
    // Counts consecutive frames where a slot that is SlotActive has a
    // nonzero touch_major/touch_minor but failed the `tip` size threshold.
    //
    UCHAR   TipDropCount[PTP_MAX_CONTACT_POINTS];

    //
    // FIX (inertia / lift-off):
    // Track which slots were included in the PREVIOUS HID report with
    // TipSwitch=1.
    //
    BOOLEAN SlotReportedLastFrame[PTP_MAX_CONTACT_POINTS];

    //
    // FIX (cursor-jump after multi-finger gesture):
    // Tracks the number of active contacts in the previous USB frame.
    // When that count drops from >=2 to 1 (or 0->1 after a multi-finger
    // lift), the surviving/new single finger is "tainted" by gesture state:
    // its SmoothedX/Y may reflect the last blended position of a finger
    // that was part of a scroll/swipe, not a deliberate cursor position.
    //
    // When a slot transitions from "was in a multi-finger frame" to being
    // the only active contact, we force-reinitialize its smoothing baseline
    // from the raw hardware position instead of blending against the stale
    // gesture position.  This eliminates the snap/jump on the first frame
    // after gesture end + tap.
    //
    // SlotWasInGesture[i] is set TRUE for every slot that was alive[i]=TRUE
    // in a frame where more than one slot was simultaneously alive.
    // It is cleared when the slot gets a fresh touch-down after a full
    // lift-off (SlotActive transitions FALSE->TRUE with no gesture taint).
    //
    BOOLEAN SlotWasInGesture[PTP_MAX_CONTACT_POINTS];

    //
    // Typing suppression deadline in QPC ticks (0 = inactive).
    // Written by keyboard callback, read in interrupt completion.
    //
    volatile LONGLONG TypingSuppressUntil;

    // Keyboard callback handle
    PVOID KbdNotifyHandle;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

#define POOL_TAG_PTP_CONTROL    'PTPC'
#define POOL_TAG_KBD_NOTIFY     'KBDN'

#define TYPING_SUPPRESS_DURATION_100NS  (500LL * 10000LL)

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY         AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          AmtPtpEvtDeviceD0Exit;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(_In_ WDFDEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext);

EVT_WDF_USB_READER_COMPLETION_ROUTINE AmtPtpEvtUsbInterruptPipeReadComplete;
EVT_WDF_USB_READERS_FAILED            AmtPtpEvtUsbInterruptReadersFailed;

PCHAR DbgDevicePowerString(_In_ WDF_POWER_DEVICE_STATE Type);
PCHAR DbgIoControlGetString(_In_ ULONG IoControlCode);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN IsWellspringModeOn
);

VOID AmtPtpRegisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext);
VOID AmtPtpUnregisterKeyboardNotification(_In_ PDEVICE_CONTEXT DeviceContext);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetHidDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetDeviceAttribs(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetReportDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

NTSTATUS AmtPtpDispatchReadReportRequests(
    _In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _Out_ BOOLEAN* Pending);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpReportFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpSetFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

EXTERN_C_END