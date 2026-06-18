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

    // Tip-size debounce
    UCHAR   TipDropCount[PTP_MAX_CONTACT_POINTS];

    // Lift-off tracking
    BOOLEAN SlotReportedLastFrame[PTP_MAX_CONTACT_POINTS];

    // Gesture taint tracking
    BOOLEAN SlotWasInGesture[PTP_MAX_CONTACT_POINTS];

    //
    // FIX (cursor-jump after gesture + re-tap):
    //
    // Root cause: Windows PTP tracks contacts by ContactID across reports.
    // When a finger lifts and a new finger lands on the same raw hardware
    // slot, the driver was assigning ContactID = slot_index — so the new
    // tap got the same ContactID as the previous gesture contact.  Windows
    // interpreted this as the SAME physical finger reappearing and
    // "corrected" the cursor back to where that ContactID was last seen
    // (the gesture start/end position), producing the jump.
    //
    // Fix: maintain a logical ContactID per slot that is rotated (incremented
    // mod PTP_MAX_CONTACT_POINTS) every time a slot goes through a full
    // lift-off and is re-used for a new touch-down.  This guarantees that
    // a fresh finger always gets a ContactID that Windows has never seen
    // active at a different position, so no "correction" occurs.
    //
    // ContactIdForSlot[i] holds the ContactID currently assigned to raw
    // slot i.  It is bumped (mod PTP_MAX_CONTACT_POINTS*2 to stay in a
    // reasonable range and avoid wrapping to an ID that is currently live
    // on another slot) in AmtClearSlot, which is called on every genuine
    // lift-off.  The lift-off report itself still uses the OLD ContactID
    // (so Windows can close the contact cleanly); the NEW value takes
    // effect on the next touch-down.
    //
    UCHAR   ContactIdForSlot[PTP_MAX_CONTACT_POINTS];

    //
    // Typing suppression deadline in QPC ticks (0 = inactive).
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

//
// ContactID rotation pool size.  Must be > PTP_MAX_CONTACT_POINTS so that
// the rotated ID for one slot never collides with an ID currently live on
// another slot.  Using PTP_MAX_CONTACT_POINTS * 2 gives IDs 0..9 for 5
// max contacts.
//
#define CONTACT_ID_POOL  (PTP_MAX_CONTACT_POINTS * 2)

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