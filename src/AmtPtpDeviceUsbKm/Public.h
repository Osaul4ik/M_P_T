// Public.h - Shared declarations for driver and user apps.
//
// This header is included by BOTH the kernel-mode driver and any user-mode
// configuration application (e.g. AmtPtpConfigGui). Keep it free of
// kernel-only or user-only types - only PODs that are safe to marshal
// across the DeviceIoControl boundary belong here.

#pragma once

// ============================================================================
// Palm-rejection runtime configuration - shared wire format.
// ============================================================================

typedef struct _AMT_PALM_CONFIG
{
    ULONG StructVersion;   // AMT_PALM_CONFIG_VERSION - bump on layout change

    // Edge-zone size, in PERMILLE (parts-per-1000, i.e. 0.1%) of the pad's
    // usable width/height. A birth (fresh finger) landing inside this zone
    // is hard-rejected as palm. See Palm.c / AmtPalmInEdgeZone.
    ULONG EdgePermilleTop;
    ULONG EdgePermilleLeft;
    ULONG EdgePermilleRight;
    ULONG EdgePermilleBottom;

    // Contact-shape thresholds (raw sensor units - same scale as the
    // Major/Minor axis values reported by the trackpad firmware).
    ULONG PalmLargeMajor;   // major >= this -> candidate for instant PALM_LARGE
    ULONG PalmLargeRatio;   // major/minor*100 >= this (with PalmLargeMajor) -> PALM_LARGE
    ULONG PalmScoreThresh;  // soft-score threshold -> PALM_LOCAL
    ULONG PalmMinMajor;     // below this AND PalmMinMinor -> never palm (early out)
    ULONG PalmMinMinor;

    ULONG Reserved[4];      // future growth, keep struct size stable
} AMT_PALM_CONFIG, *PAMT_PALM_CONFIG;

#define AMT_PALM_CONFIG_VERSION 1

// Compiled-in defaults - identical to the previous hardcoded values in
// Palm.c, so a fresh install / registry-less first boot behaves exactly
// like the driver did before this config became runtime-tunable.
#define AMT_PALM_CONFIG_DEFAULT_INIT                                        \
{                                                                            \
    /* StructVersion       */ AMT_PALM_CONFIG_VERSION,                      \
    /* EdgePermilleTop     */ 0,                                           \
    /* EdgePermilleLeft    */ 70,                                          \
    /* EdgePermilleRight   */ 70,                                          \
    /* EdgePermilleBottom  */ 110,                                          \
    /* PalmLargeMajor      */ 380,                                          \
    /* PalmLargeRatio      */ 180,                                          \
    /* PalmScoreThresh     */ 55,                                           \
    /* PalmMinMajor        */ 80,                                           \
    /* PalmMinMinor        */ 40,                                           \
    /* Reserved            */ { 0, 0, 0, 0 }                                \
}

// Sane clamp range for the GUI/driver to enforce on every field except
// StructVersion/Reserved - keeps a fat-fingered value from disabling palm
// rejection outright or rejecting every touch as palm.
#define AMT_PALM_EDGE_PERMILLE_MAX   400   // 40% - beyond this, "edge zone" eats the whole pad
#define AMT_PALM_MAJOR_MAX          2000
#define AMT_PALM_RATIO_MAX          1000
#define AMT_PALM_SCORE_MAX           200


// ============================================================================
// Pointer runtime configuration - Force Tap (force touch) tuning.
// ============================================================================

typedef struct _AMT_POINTER_CONFIG
{
    ULONG StructVersion;   // AMT_POINTER_CONFIG_VERSION - bump on layout change
    ULONG ForceTapThreshold;
    ULONG ForceTapAction;
    ULONG ForceTouchEnabled;
    ULONG RequirePressureToActivate;
    // When enabled on Force Touch devices, pressure must remain positive on
    // every frame. If pressure drops to zero, the candidate is treated as
    // absent for that frame (continuous pressure rejection).
    ULONG RequirePressureContinuously;

    // Cursor motion tuning. Percent values use 100 as the current/default
    // behavior. CursorSmoothingPercent: 0 = raw, 100 = strongest smoothing.
    ULONG CursorSmoothingPercent;
    ULONG CursorSpeedPercent;
    ULONG CursorDeadzone;
    ULONG CursorDeadzoneSlow;
    ULONG CursorDeadzoneFast;
    ULONG CursorSlowVelocity;
    ULONG CursorFastVelocity;
    ULONG SmoothingAlphaDen;
    ULONG SmoothingAlphaNumSlow;

    // Reject small contacts on trackpads without Force Touch. A contact is
    // initially rejected while Major < 80 AND Minor < 60; once it reaches
    // Major >= 80 OR Minor >= 60 it stays accepted until that contact is
    // lifted. Ignored on Force Touch-capable devices.
    ULONG SmallContactRejectionEnabled;

    // When enabled together with SmallContactRejectionEnabled on a
    // non-Force-Touch device, the Major/Minor gate is applied continuously:
    // every frame requires Major >= SmallContactMajorThreshold AND
    // Minor >= AMT_SMALL_CONTACT_MINOR_THRESHOLD (Match.c).
    ULONG SmallContactRejectionStrict;

    // GUI-tunable "M" (Major axis) cutoff used by the gate above. Replaces
    // the previously hardcoded 50. Minor keeps its fixed 30 floor - only
    // Major is exposed as a slider.
    ULONG SmallContactMajorThreshold;

    // Same small-contact size gate as SmallContactRejectionEnabled/Strict
    // above, but for Force-Touch-capable devices (where it is normally
    // ignored - see SupportsForceTouch branch in AmtMatchBuildCandidates,
    // Match.c). When both Enabled and Strict are set, every frame requires
    // Major >= ForceTouchSmallContactMajorThreshold AND
    // Minor >= AMT_SMALL_CONTACT_MINOR_THRESHOLD, same continuous
    // (every-frame, not just birth) semantics as the non-Force-Touch gate.
    ULONG ForceTouchSmallContactRejectionEnabled;
    ULONG ForceTouchSmallContactRejectionStrict;
    ULONG ForceTouchSmallContactMajorThreshold;

    // Software Force Touch emulation for trackpads with no hardware
    // pressure channel (DEVICE_CONTEXT::SupportsForceTouch == FALSE - see
    // that field's comment in Device.h). Meaningless (ignored by the
    // driver) on real Force Touch hardware, exactly like
    // SmallContactRejection* above is ignored once real pressure is
    // available - see PTPCore_ProcessFrame in Ptpcore.c.
    //
    // Reuses the SAME CLICK_ARBITRATION_STATE machine as hardware Force
    // Touch: a mechanical Hard Tap (button down) starts PENDING, and instead
    // of a pressure threshold this path resolves PENDING -> FORCE_TOUCH once
    // ForceTouchEmulationHoldMs has elapsed while the press is held - see
    // Ptpcore.c for the resolution logic.
    ULONG ForceTouchEmulationEnabled;
    ULONG ForceTouchEmulationAction;    // one of AMT_POINTER_ACTION_*
    ULONG ForceTouchEmulationHoldMs;    // hold duration to trigger, in ms

    // Drag-cancel distance: how far a press may drift from where it started
    // before Force Touch is cancelled and the press falls back to an
    // ordinary click/drag. Raw sensor units (device-dependent, NOT mm/px -
    // see PTPCore_ProcessFrame's anchor/lockout comments in Ptpcore.c for
    // why an exact mm figure isn't available at this layer).
    //
    // Two independent values because the two paths get here differently:
    // hardware Force Touch reaches this distance during the ~90ms pressure
    // ramp-up, while emulation can be waiting on it for up to 2 seconds -
    // same mechanism, very different dwell time, so worth tuning
    // separately.
    ULONG ForceTapDragLockoutDistance;              // hardware (pressure) path
    ULONG ForceTouchEmulationDragLockoutDistance;   // emulation (hold-timer) path
} AMT_POINTER_CONFIG, *PAMT_POINTER_CONFIG;

#define AMT_POINTER_CONFIG_VERSION 10

#define AMT_POINTER_SMOOTH_MIN       0
#define AMT_POINTER_SMOOTH_MAX       100
#define AMT_POINTER_SPEED_MIN        50
#define AMT_POINTER_SPEED_MAX        200
#define AMT_POINTER_DEADZONE_MIN     0
#define AMT_POINTER_DEADZONE_MAX     8
#define AMT_POINTER_SLOW_VEL_MIN     20
#define AMT_POINTER_SLOW_VEL_MAX     300
#define AMT_POINTER_FAST_VEL_MIN     200
#define AMT_POINTER_FAST_VEL_MAX     2000
#define AMT_POINTER_ALPHA_DEN_MIN    1
#define AMT_POINTER_ALPHA_DEN_MAX    16
#define AMT_POINTER_ALPHA_SLOW_MIN  1
#define AMT_POINTER_ALPHA_SLOW_MAX 16

// Small-contact size gate ("M" = Major axis). Range for both
// SmallContactMajorThreshold (non-Force-Touch) and
// ForceTouchSmallContactMajorThreshold (Force-Touch) sliders. Minor keeps
// its fixed floor below - only Major is GUI-tunable.
#define AMT_POINTER_SMALL_CONTACT_MAJOR_MIN   10
#define AMT_POINTER_SMALL_CONTACT_MAJOR_MAX  150
#define AMT_SMALL_CONTACT_MINOR_THRESHOLD     30

// ForceTapAction values.
#define AMT_POINTER_ACTION_CONTEXT_MENU 0   // synthetic right-click (Button2)
#define AMT_POINTER_ACTION_MIDDLE_CLICK 1   // synthetic middle-click (Button3)
#define AMT_POINTER_ACTION_DOUBLE_CLICK 2   // synthetic double left-click (Button1 x2, e.g. "open")

// Compiled-in defaults - tuned factory values for the current Wellspring PTP
// experience. CONTEXT_MENU remains the default Force Tap action so a
// previous always-right-click behavior, so a fresh install / registry-less
// first boot behaves exactly like the driver did before this became
// runtime-tunable.
#define AMT_POINTER_CONFIG_DEFAULT_INIT                                    \
{                                                                            \
    AMT_POINTER_CONFIG_VERSION,                                             \
    265,                                                                    \
    AMT_POINTER_ACTION_CONTEXT_MENU,                                        \
    1,                                                                      \
    1,                                                                      \
    0,                                                                      \
    0,                                                                      \
    100,                                                                    \
    1,                                                                      \
    4,                                                                      \
    0,                                                                      \
    110,                                                                    \
    905,                                                                    \
    8,                                                                      \
    3,                                                                      \
    1,                                                                      \
    0,                                                                      \
    /* SmallContactMajorThreshold             */ 50,                       \
    /* ForceTouchSmallContactRejectionEnabled */ 0,                        \
    /* ForceTouchSmallContactRejectionStrict  */ 0,                        \
    /* ForceTouchSmallContactMajorThreshold   */ 50,                       \
    1,                                                                      \
    AMT_POINTER_ACTION_CONTEXT_MENU,                                        \
    300,                                                                    \
    110,                                                                    \
    110                                                                      \
}

// Sane clamp range - raw pressure realistically spans ~0-300, so keep the
// threshold well inside that instead of letting it go degenerate (0 would
// fire on any touch; a huge value would make Force Tap unreachable).
#define AMT_POINTER_THRESHOLD_MIN   200
#define AMT_POINTER_THRESHOLD_MAX   400
#define AMT_POINTER_ACTION_MAX        2   // highest valid AMT_POINTER_ACTION_* value

// Force Touch emulation hold-duration range/step, in milliseconds. The GUI
// slider is expected to only ever produce values on this 50ms grid; the
// driver clamps AND re-quantizes anything it receives (SET IOCTL or
// registry) onto the same grid so a hand-crafted or corrupt value can't
// land between steps.
#define AMT_POINTER_FORCE_TOUCH_EMULATION_HOLD_MS_MIN   200
#define AMT_POINTER_FORCE_TOUCH_EMULATION_HOLD_MS_MAX  2000
#define AMT_POINTER_FORCE_TOUCH_EMULATION_HOLD_MS_STEP   50

// Drag-cancel distance range/step, in raw sensor units. Default is 60 on
// both paths (tighter than the old hardcoded 160 FORCE_TOUCH_DRAG_LOCKOUT_
// DISTANCE #define this replaces - see Ptpcore.c). 10-unit grid for the
// hand-crafted/corrupt-value re-quantization reason as the hold-ms grid
// above.
#define AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_MIN    40
#define AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_MAX   400
#define AMT_POINTER_FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE_STEP   10


// ============================================================================
// Live touch monitor
// ============================================================================

#define AMT_LIVE_FRAME_VERSION 3
#define AMT_LIVE_MAX_CONTACTS 5

typedef struct _AMT_LIVE_CONTACT
{
    ULONG ContactID;
    USHORT X;              // normalized X: 0 .. (XMax - XMin)
    USHORT Y;              // normalized Y: 0 .. (YMax - YMin)
    ULONG Phase;           // CONTACT_PHASE_* value from PTPCore.h
    UCHAR Confident;
    UCHAR PalmSuspect;
    USHORT Reserved;
    SHORT RawX;            // exact raw USB abs_x
    SHORT RawY;            // exact raw USB abs_y
    USHORT Major;          // touch_major, raw sensor units (nearest-raw match)
    USHORT Minor;          // touch_minor, raw sensor units (nearest-raw match)
    USHORT Pressure;       // raw pressure/force value from the matched raw contact
    USHORT Orientation;    // raw Apple orientation (16384 = point, otherwise 15-bit angle)
} AMT_LIVE_CONTACT, *PAMT_LIVE_CONTACT;

typedef struct _AMT_LIVE_FRAME
{
    ULONG StructVersion;
    ULONG Sequence;
    LONGLONG TimestampQpc;

    UCHAR ContactCount;
    UCHAR RawContactCount;
    UCHAR LargePalmBlanked;
    UCHAR ButtonDown;

    UCHAR ForceTouchClick;
    UCHAR ButtonClickReport;
    USHORT Reserved0;

    AMT_LIVE_CONTACT Contacts[AMT_LIVE_MAX_CONTACTS];
} AMT_LIVE_FRAME, *PAMT_LIVE_FRAME;

// ============================================================================
// Custom IOCTLs for AmtPtpConfigGui <-> driver communication.
// ============================================================================

#define AMT_PTP_IOCTL_INDEX 0x900

#define IOCTL_AMT_PTP_GET_PALM_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 0, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_AMT_PTP_SET_PALM_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 1, METHOD_BUFFERED, FILE_WRITE_DATA)

// Returns the pad's usable sensor range (BCM5974_CONFIG x/y min/max) so the
// GUI can draw the edge zones and a to-scale finger ellipse without
// guessing at hardware geometry. Read-only.
#define IOCTL_AMT_PTP_GET_PAD_GEOMETRY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 2, METHOD_BUFFERED, FILE_READ_DATA)

// Resets AMT_PALM_CONFIG (in memory and in the registry) to
// AMT_PALM_CONFIG_DEFAULT_INIT. No input/output buffer.
#define IOCTL_AMT_PTP_RESET_PALM_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 3, METHOD_BUFFERED, FILE_WRITE_DATA)

#define IOCTL_AMT_PTP_SET_LIVE_ENABLED \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 4, METHOD_BUFFERED, FILE_WRITE_DATA)

#define IOCTL_AMT_PTP_GET_LIVE_FRAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 5, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_AMT_PTP_GET_POINTER_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 6, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_AMT_PTP_SET_POINTER_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 7, METHOD_BUFFERED, FILE_WRITE_DATA)

// Resets AMT_POINTER_CONFIG (in memory and in the registry) to
// AMT_POINTER_CONFIG_DEFAULT_INIT. No input/output buffer.
#define IOCTL_AMT_PTP_RESET_POINTER_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 8, METHOD_BUFFERED, FILE_WRITE_DATA)

#define IOCTL_AMT_PTP_GET_SCROLL_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 9, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_AMT_PTP_SET_SCROLL_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 10, METHOD_BUFFERED, FILE_WRITE_DATA)

#define IOCTL_AMT_PTP_RESET_SCROLL_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 11, METHOD_BUFFERED, FILE_WRITE_DATA)

// Vendor/product id plus whether the connected trackpad's packet format
// actually carries a pressure channel (TYPE4/TYPE5 - see
// DEVICE_CONTEXT::SupportsForceTouch in Device.h/AppleDefinition.h). The GUI
// uses this to hide Force Touch settings entirely on hardware that has no
// pressure sensor (e.g. pre-2015 MacBook Air / WELLSPRING8) instead of
// showing controls the driver would just ignore. Read-only, no input buffer.
#define IOCTL_AMT_PTP_GET_DEVICE_INFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 12, METHOD_BUFFERED, FILE_READ_DATA)

typedef struct _AMT_DEVICE_INFO
{
    ULONG  StructVersion;
    USHORT VendorId;
    USHORT ProductId;
    UCHAR  SupportsForceTouch;
    UCHAR  Reserved0;
    UCHAR  Reserved1;
    UCHAR  Reserved2;
} AMT_DEVICE_INFO, *PAMT_DEVICE_INFO;

#define AMT_DEVICE_INFO_VERSION 1

// Runtime debug-trace switch (DEVICE_CONTEXT::TraceDebugEnabled - see
// Trace.h/Trace.c in the driver). Simple ULONG (0/1) in and out, same
// "raw ULONG buffer" shape as IOCTL_AMT_PTP_SET_LIVE_ENABLED - there is no
// dedicated config struct because there is only one field. Unlike
// LiveEnabled this is NOT per-file/per-owner: any GUI instance may read or
// flip it, and it also persists to the registry (DebugMode under this
// device's Device Parameters key) so it survives a reboot/replug, matching
// how PalmConfig/PointerConfig/ScrollConfig persist.
#define IOCTL_AMT_PTP_GET_DEBUG_MODE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 13, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_AMT_PTP_SET_DEBUG_MODE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 14, METHOD_BUFFERED, FILE_WRITE_DATA)

typedef struct _AMT_SCROLL_CONFIG
{
    ULONG StructVersion;
    ULONG SpeedPercent;
    ULONG FastSpeedPercent;
    ULONG SmoothingPercent;
    ULONG Deadzone;
    ULONG FastVelocity;
    ULONG ScaleNum;
    ULONG ScaleDen;
    ULONG ScaleNumFast;
    ULONG ScaleDenFast;
    ULONG Reserved[2];
} AMT_SCROLL_CONFIG, *PAMT_SCROLL_CONFIG;

#define AMT_SCROLL_CONFIG_VERSION 2
#define AMT_SCROLL_SPEED_MIN          20
#define AMT_SCROLL_SPEED_MAX          200
#define AMT_SCROLL_FAST_SPEED_MIN     20
#define AMT_SCROLL_FAST_SPEED_MAX     250
#define AMT_SCROLL_SMOOTH_MIN         0
#define AMT_SCROLL_SMOOTH_MAX         100
#define AMT_SCROLL_DEADZONE_MIN       0
#define AMT_SCROLL_DEADZONE_MAX       8
#define AMT_SCROLL_FAST_VEL_MIN       500
#define AMT_SCROLL_FAST_VEL_MAX       4000
#define AMT_SCROLL_SCALE_NUM_MIN       1
#define AMT_SCROLL_SCALE_NUM_MAX     400
#define AMT_SCROLL_SCALE_DEN_MIN       1
#define AMT_SCROLL_SCALE_DEN_MAX     400
#define AMT_SCROLL_CONFIG_DEFAULT_INIT \
{ \
    AMT_SCROLL_CONFIG_VERSION, \
    60, \
    100, \
    0, \
    1, \
    1600, \
    6, \
    10, \
    108, \
    100, \
    { 0, 0 } \
}

typedef struct _AMT_PAD_GEOMETRY
{
    ULONG StructVersion;
    LONG  XMin;
    LONG  XMax;
    LONG  YMin;
    LONG  YMax;
} AMT_PAD_GEOMETRY, *PAMT_PAD_GEOMETRY;

#define AMT_PAD_GEOMETRY_VERSION 1