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

    // ---------------------------------------------------------------
    // Track pool — replaces the previous 8 parallel arrays
    // (SmoothedX/Y, HystX/Y, SlotActive, TipDropCount,
    //  SlotReportedLastFrame, SlotWasInGesture, ContactIdForSlot) with
    // one struct per logical contact, indexed by raw hardware slot
    // index. See Track.h for the full state-machine writeup and the
    // frame-determinism rule governing how Interrupt.c may mutate these.
    // ---------------------------------------------------------------
    TRACK Tracks[PTP_MAX_CONTACT_POINTS];

    // FIX (cursor-jump after gesture + re-tap), revised — see the long
    // comment on AmtTrackAssignContactId() in Track.c for the full story.
    //
    // Windows PTP tracks contacts by ContactID. A re-used slot that
    // presents the same ContactID it used at a previous, different
    // position causes Windows to interpret the new touch-down as a
    // continuation of the old contact and "correct" the cursor toward the
    // new position instead of treating it as a fresh press — visible as a
    // jump.
    //
    // NextContactId is a single monotonic counter shared by every track:
    // every lift-off (AmtTrackKill/AmtTrackRecycle) advances it and hands
    // the track an ID that has never been used by ANY track for the
    // lifetime of the device session (reseeded at D0Entry — see
    // AmtPtpEvtDeviceD0Entry in Device.c). Must stay ULONG to match
    // PTP_CONTACT.ContactID (include/Hid.h) — truncating to a smaller
    // type would silently wrap the counter and reopen the same
    // "still warm" collision this fix exists to prevent.
    ULONG   NextContactId;

    // ---------------------------------------------------------------
    // Session-level gesture flag — DISTINCT from TRACK.WasInGesture.
    //
    // FIX (WasInGesture session vs per-track split): the previous design
    // had exactly one "was this a gesture" bit per slot
    // (SlotWasInGesture[i]), used for two different questions:
    //   (1) "should THIS finger skip EMA blending on its next solo
    //        update" — genuinely per-track, persists across exactly one
    //        ACTIVE lifetime.
    //   (2) implicitly, "is a multi-finger gesture in progress on the pad
    //        right now" — a SESSION-level fact about the current frame,
    //        previously re-derived every frame from aliveCount>=2 rather
    //        than being tracked as its own piece of state.
    // Conflating these made the gesture-quarantine logic harder to
    // reason about, since "was in a gesture" had to be inferred per-slot
    // instead of asked once per frame. GestureSessionActive answers (2)
    // directly; TRACK.WasInGesture continues to answer (1) and is SET
    // FROM GestureSessionActive transitions in Interrupt.c, never the
    // other way around.
    BOOLEAN GestureSessionActive;

    // Typing suppression deadline in QPC ticks (0 = inactive).
    volatile LONGLONG TypingSuppressUntil;

    // Keyboard callback handle
    PVOID KbdNotifyHandle;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

    // Rate limiting for TraceEvents in the hot (per-frame) interrupt
    // path — see TRACE_HOT_PATH_MIN_INTERVAL_100NS in Interrupt.c. Holds
    // the QPC value of the last hot-path verbose trace emission; frames
    // arriving before the interval elapses skip the TraceEvents call
    // entirely rather than relying on the WPP session-level filter,
    // since argument marshalling/formatting cost is paid before that
    // filter is consulted.
    LONGLONG LastHotPathTraceQpc;

    // ---------------------------------------------------------------
    // FIX (starvation case): a PTP_REPORT carries at most
    // PTP_MAX_CONTACT_POINTS contacts. Phase A can in principle produce
    // more lift-off events in a single frame than remaining report
    // capacity (defensive handling for a future revision that changes
    // raw_n's bound without a matching report-capacity change — cannot
    // currently happen since raw_n is clamped to PTP_MAX_CONTACT_POINTS
    // before Phase A runs, but the fallback is real rather than a stub).
    // Rather than silently dropping a lift-off in that scenario — which
    // would leave Windows believing a contact is still down — overflow
    // lift-offs are queued here and drained into the FRONT of the next
    // frame's report, ahead of that frame's own contacts, guaranteeing
    // bounded (one extra frame) delivery latency instead of permanent
    // loss. See AmtEmitLift / AmtDrainOverflow in Interrupt.c.
    // ---------------------------------------------------------------
    ULONG  OverflowContactID[PTP_MAX_CONTACT_POINTS];
    USHORT OverflowX[PTP_MAX_CONTACT_POINTS];
    USHORT OverflowY[PTP_MAX_CONTACT_POINTS];
    UCHAR  OverflowCount;

    // ---------------------------------------------------------------
    // FIX (task #2 — raw-snap-on-fast-retap) — per-slot "recent lift"
    // memory, used ONLY to seed a smoothing anchor for
    // AmtTrackBirthWithRetapSmoothing (see Track.h/Track.c). Captured
    // from AmtTrackKill/AmtTrackEnterGrace's Old* out-parameters at the
    // moment a track is lifted, indexed by the SAME raw slot index the
    // track occupied. Deliberately NOT part of TRACK — TRACK is fully
    // zeroed on every kill/grace-expire transition (see the frame-
    // determinism rule in Track.h), and this memory must specifically
    // SURVIVE that zeroing so a later birth on the same slot can still
    // see where the previous occupant lifted from.
    //
    // This is position-only memory. It never feeds ContactID assignment
    // and never causes a re-tap to claim continuity with the lifted
    // contact — see TRACK_RETAP_POLICY in Track.h. SlotLastLiftQpc==0
    // is the "no recent lift" sentinel (see AmtTrackIsRecentLiftNearby),
    // and is the natural state after AmtTrackPoolInit/D0Entry zero the
    // whole DEVICE_CONTEXT — no separate reset code is required for it.
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