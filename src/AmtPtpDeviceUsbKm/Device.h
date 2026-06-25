// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "ActiveContact.h"
#include "Gesture.h"
#include "PTPCore.h"

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

    // Previous frame's physical integrated-button state. Compared against
    // the current frame in PTPCore.c to detect the 0->1 click edge that
    // drives the forced-rebirth anti-jitter-snap workaround.
    BOOLEAN PrevButtonClicked;

    // Scan time
    LARGE_INTEGER LastReportTime;

    // Palm rejection - session-level latch (sticky "still palm-adjacent"
    // state), owned by PTPCore_ProcessFrame. Per-sample classification
    // lives in Palm.c.
    BOOLEAN PalmDetected;

    // Contact pool (PTPCore / ActiveContact). Pool POSITION is NOT
    // identity - ContactID is. See ActiveContact.h for the full
    // rationale (this replaces the old slot-indexed TRACK[] array).
    // ---------------------------------------------------------------
    ACTIVE_CONTACT ActiveContacts[MAX_CONTACTS];

    // Monotonic ContactID counter - never reuses an ID while "warm".
    // Every lift-off advances it; reseeded at D0Entry.
    ULONG   NextContactId;

    // GestureEngine session state (Gesture.h). ACTIVE_CONTACT.WasInGesture
    // is SET FROM this by PTPCore.c, never the reverse.
    GESTURE_SESSION GestureSession;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

    // Hot-path trace rate limiting - QPC of last verbose trace emission.
    LONGLONG LastHotPathTraceQpc;

    // Overflow lift-off queue - when PTPCore_ProcessFrame produces more
    // lift-offs than remaining PTP_CORE_FRAME capacity, deferred
    // entries are drained at the front of the next frame. See
    // AmtCoreEmitLift/AmtCoreDrainOverflow in PTPCore.c.
    // ---------------------------------------------------------------
    ULONG  OverflowContactID[PTP_MAX_CONTACT_POINTS];
    USHORT OverflowX[PTP_MAX_CONTACT_POINTS];
    USHORT OverflowY[PTP_MAX_CONTACT_POINTS];
    UCHAR  OverflowCount;

    // Recent-lift memory for retap smoothing (PTPCore.h /
    // RECENT_LIFT_RING). Deliberately NOT slot-indexed - see PTPCore.h
    // for why the old SlotLastLiftQpc/X/Y[PTP_MAX_CONTACT_POINTS]
    // arrays were a slot-as-identity mistake.
    // ---------------------------------------------------------------
    RECENT_LIFT_RING RecentLifts;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

#define POOL_TAG_PTP_CONTROL    'PTPC'

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