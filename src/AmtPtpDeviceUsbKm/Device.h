// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "Track.h"

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

    // Track pool — one TRACK per raw slot. See Track.h for FSM docs.
    // ---------------------------------------------------------------
    TRACK Tracks[PTP_MAX_CONTACT_POINTS];

    // Monotonic ContactID counter — never reuses an ID while "warm".
    // Every lift-off advances it; reseeded at D0Entry. ULONG to match
    // PTP_CONTACT.ContactID.
    ULONG   NextContactId;

    // Session-level gesture flag — distinct from per-track WasInGesture.
    // TRUE when >=2 fingers are down. TRACK.WasInGesture is SET FROM
    // this, never the reverse.
    BOOLEAN GestureSessionActive;

    // Typing suppression deadline in QPC ticks (0 = inactive).
    volatile LONGLONG TypingSuppressUntil;

    // Keyboard callback handle
    PVOID KbdNotifyHandle;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

    // Hot-path trace rate limiting — QPC of last verbose trace emission.
    // See TRACE_HOT_PATH_MIN_INTERVAL_100NS in Interrupt.c.
    LONGLONG LastHotPathTraceQpc;

    // Overflow lift-off queue — when Phase A produces more lift-offs
    // than remaining report capacity, deferred entries are drained at
    // the front of the next frame. See AmtEmitLift/AmtDrainOverflow.
    // ---------------------------------------------------------------
    ULONG  OverflowContactID[PTP_MAX_CONTACT_POINTS];
    USHORT OverflowX[PTP_MAX_CONTACT_POINTS];
    USHORT OverflowY[PTP_MAX_CONTACT_POINTS];
    UCHAR  OverflowCount;

    // Per-slot "recent lift" memory — smoothing anchor for fast re-tap
    // (task #2). Survives TRACK zeroing on kill. Position-only, never
    // feeds ContactID. SlotLastLiftQpc==0 = no recent lift sentinel.
    // ---------------------------------------------------------------
    LONGLONG SlotLastLiftQpc[PTP_MAX_CONTACT_POINTS];
    USHORT   SlotLastLiftX[PTP_MAX_CONTACT_POINTS];
    USHORT   SlotLastLiftY[PTP_MAX_CONTACT_POINTS];

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