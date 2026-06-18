// Device.h - Device definitions. Kernel-mode Driver Framework

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

    // FIX (cursor-jump after gesture + re-tap):
    // Windows PTP tracks contacts by ContactID. A re-used slot with the same
    // ContactID caused cursor correction. Fix: rotate ContactID per slot on
    // lift-off so a fresh finger gets an ID Windows has never seen before.
    // ContactIdForSlot[i] is bumped (mod CONTACT_ID_POOL) in AmtClearSlot.
    // The lift-off report uses the old ID; the new one takes effect on next
    // touch-down.
    UCHAR   ContactIdForSlot[PTP_MAX_CONTACT_POINTS];

    // Typing suppression deadline in QPC ticks (0 = inactive).
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

// ContactID rotation pool size (> PTP_MAX_CONTACT_POINTS to avoid collisions).
// PTP_MAX_CONTACT_POINTS * 2 = 10 IDs for 5 max contacts.
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