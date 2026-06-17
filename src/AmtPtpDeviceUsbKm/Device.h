/*++

Module Name:

    device.h

Abstract:

    This file contains the device definitions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "public.h"
#include <Hid.h>

EXTERN_C_START

//
// The device context performs the same job as
// a WDM device extension in the driver frameworks
//
typedef struct _DEVICE_CONTEXT
{
    // USB Things
    WDFUSBDEVICE    UsbDevice;
    WDFUSBPIPE      InterruptPipe;
    WDFUSBINTERFACE UsbInterface;
    WDFQUEUE        InputQueue;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    ULONG           UsbDeviceTraits;

    // Device Config
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN IsWellspringModeOn;

    // PTP Status
    BOOLEAN PtpInputOn;
    BOOLEAN PtpReportTouch;
    BOOLEAN PtpReportButton;

    // Timer
    LARGE_INTEGER LastReportTime;

    // Palm rejection
    BOOLEAN PalmDetected;

    // Per-finger smoothing (indexed by raw finger index 0..PTP_MAX_CONTACT_POINTS-1)
    USHORT  SmoothedX[PTP_MAX_CONTACT_POINTS];
    USHORT  SmoothedY[PTP_MAX_CONTACT_POINTS];
    USHORT  HystX[PTP_MAX_CONTACT_POINTS];
    USHORT  HystY[PTP_MAX_CONTACT_POINTS];
    BOOLEAN SlotActive[PTP_MAX_CONTACT_POINTS];

    //
    // Typing suppression:
    //
    // TypingSuppressUntil holds the QPC value (in 100-ns units, same as
    // KeQueryPerformanceCounter ticks * 1000000 / freq) after which touchpad
    // input is allowed again.  Written by the keyboard notification callback,
    // read (and discarded when expired) in the interrupt completion routine.
    // Declared volatile so the compiler does not cache across the callback
    // boundary; InterlockedExchange64 is used on 64-bit for atomicity.
    //
    volatile LONGLONG TypingSuppressUntil;  // QPC ticks, 0 = not suppressed

    //
    // Keyboard callback handle (registered in PrepareHardware,
    // unregistered in D0Exit / DeviceRemove).
    //
    PVOID KbdNotifyHandle;

    //
    // QPC frequency cached at D0Entry to avoid repeated KeQueryPerformanceFrequency calls.
    //
    LARGE_INTEGER PerfFrequency;

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
#define POOL_TAG_PTP_CONTROL    'PTPC'
#define POOL_TAG_KBD_NOTIFY     'KBDN'

//
// Typing suppression window: 500 ms expressed as 100-ns intervals.
//
#define TYPING_SUPPRESS_DURATION_100NS  (500LL * 10000LL)   // 5,000,000 x 100 ns = 500 ms

//
// Function to initialize the device's queues and callbacks
//
NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

//
// Function to select the device's USB configuration and get a WDFUSBDEVICE handle
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
// Keyboard notification (typing suppression)
//
VOID
AmtPtpRegisterKeyboardNotification(
    _In_ PDEVICE_CONTEXT DeviceContext
);

VOID
AmtPtpUnregisterKeyboardNotification(
    _In_ PDEVICE_CONTEXT DeviceContext
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
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ BOOLEAN*  Pending
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