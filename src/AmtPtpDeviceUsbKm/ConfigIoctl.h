// ConfigIoctl.h - AmtPtpConfigGui <-> driver custom IOCTL surface.
//
// Implements the IOCTL_AMT_PTP_* control codes declared in Public.h:
// GET/SET palm-rejection config, GET pad geometry, RESET palm config to
// defaults, and the equivalent GET/SET/RESET trio for AMT_POINTER_CONFIG (Force Tap + cursor tuning), and
// the equivalent GET/SET/RESET trio for AMT_SCROLL_CONFIG.
// Declarations for the handler functions live in Device.h (next to the
// other AmtPtpGet*/AmtPtpSet* HID handlers they sit beside in Queue.c).
//
// IMPORTANT: this header intentionally does NOT include "Driver.h" (or
// anything that pulls in Device.h/Queue.h). Neither Driver.h nor Device.h
// has an include guard / #pragma once in this codebase - every .c file in
// the project is expected to include Driver.h exactly once. ConfigIoctl.c
// already does that before including this header, so pulling Driver.h in
// again here would double-include Device.h in the same translation unit
// and redefine every type/function it declares (enums, DEVICE_CONTEXT,
// DeviceGetContext, the AmtAbsDelta/AmtDistSq/AmtRawToSignedInt inline
// helpers, etc. - exactly the C2011/C2084 errors this fix addresses).
// This header only needs EXTERN_C_START/END and L"..." string literals,
// neither of which requires any driver header.

#pragma once

EXTERN_C_START

// Registry value names used to persist AMT_PALM_CONFIG fields, one REG_DWORD
// each, under the device's "Device Parameters" software key. Kept as plain
// DWORDs (not one binary blob) so the values are individually visible/
// editable in regedit for field debugging, matching how Elan/Synaptics OEM
// panels typically expose their tunables under Device Parameters too.
#define AMT_REG_VALUE_EDGE_TOP     L"PalmEdgePermilleTop"
#define AMT_REG_VALUE_EDGE_LEFT    L"PalmEdgePermilleLeft"
#define AMT_REG_VALUE_EDGE_RIGHT   L"PalmEdgePermilleRight"
#define AMT_REG_VALUE_EDGE_BOTTOM  L"PalmEdgePermilleBottom"
#define AMT_REG_VALUE_LARGE_MAJOR  L"PalmLargeMajor"
#define AMT_REG_VALUE_LARGE_RATIO  L"PalmLargeRatio"
#define AMT_REG_VALUE_SCORE_THRESH L"PalmScoreThresh"
#define AMT_REG_VALUE_MIN_MAJOR    L"PalmMinMajor"
#define AMT_REG_VALUE_MIN_MINOR    L"PalmMinMinor"

// Registry value names for AMT_POINTER_CONFIG, same "one REG_DWORD each"
// convention as the palm values above.
#define AMT_REG_VALUE_FORCETAP_THRESHOLD L"PointerForceTapThreshold"
#define AMT_REG_VALUE_FORCETAP_ACTION    L"PointerForceTapAction"
#define AMT_REG_VALUE_FORCETOUCH_ENABLED L"PointerForceTouchEnabled"
#define AMT_REG_VALUE_REQUIRE_PRESSURE   L"PointerRequirePressureToActivate"
#define AMT_REG_VALUE_REQUIRE_PRESSURE_CONTINUOUS L"PointerRequirePressureContinuously"
#define AMT_REG_VALUE_CURSOR_SMOOTH      L"PointerCursorSmoothing"
#define AMT_REG_VALUE_CURSOR_SPEED       L"PointerCursorSpeed"
#define AMT_REG_VALUE_CURSOR_DEADZONE    L"PointerCursorDeadzone"
#define AMT_REG_VALUE_CURSOR_DEADZONE_SLOW L"PointerCursorDeadzoneSlow"
#define AMT_REG_VALUE_CURSOR_DEADZONE_FAST L"PointerCursorDeadzoneFast"
#define AMT_REG_VALUE_CURSOR_SLOW_VEL    L"PointerCursorSlowVelocity"
#define AMT_REG_VALUE_CURSOR_FAST_VEL    L"PointerCursorFastVelocity"
#define AMT_REG_VALUE_CURSOR_ALPHA_DEN   L"PointerSmoothingAlphaDen"
#define AMT_REG_VALUE_CURSOR_ALPHA_SLOW  L"PointerSmoothingAlphaNumSlow"
#define AMT_REG_VALUE_SMALL_CONTACT_REJECTION L"PointerSmallContactRejectionEnabled"
#define AMT_REG_VALUE_SMALL_CONTACT_REJECTION_STRICT L"PointerSmallContactRejectionStrict"
#define AMT_REG_VALUE_SMALL_CONTACT_BIRTH_MAJOR_THRESHOLD L"PointerSmallContactBirthMajorThreshold"
#define AMT_REG_VALUE_SMALL_CONTACT_MAJOR_THRESHOLD L"PointerSmallContactMajorThreshold"
#define AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_REJECTION L"PointerForceTouchSmallContactRejectionEnabled"
#define AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_REJECTION_STRICT L"PointerForceTouchSmallContactRejectionStrict"
#define AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_BIRTH_MAJOR_THRESHOLD L"PointerForceTouchSmallContactBirthMajorThreshold"
#define AMT_REG_VALUE_FORCETOUCH_SMALL_CONTACT_MAJOR_THRESHOLD L"PointerForceTouchSmallContactMajorThreshold"
#define AMT_REG_VALUE_FORCETOUCH_EMULATION_ENABLED L"PointerForceTouchEmulationEnabled"
#define AMT_REG_VALUE_FORCETOUCH_EMULATION_ACTION  L"PointerForceTouchEmulationAction"
#define AMT_REG_VALUE_FORCETOUCH_EMULATION_HOLD_MS L"PointerForceTouchEmulationHoldMs"
#define AMT_REG_VALUE_FORCE_TAP_DRAG_LOCKOUT_DISTANCE           L"PointerForceTapDragLockoutDistance"
#define AMT_REG_VALUE_FORCETOUCH_EMULATION_DRAG_LOCKOUT_DISTANCE L"PointerForceTouchEmulationDragLockoutDistance"

#define AMT_REG_VALUE_SCROLL_SPEED       L"ScrollSpeed"
#define AMT_REG_VALUE_SCROLL_FAST_SPEED  L"ScrollFastSpeed"
#define AMT_REG_VALUE_SCROLL_SMOOTH      L"ScrollSmoothing"
#define AMT_REG_VALUE_SCROLL_DEADZONE    L"ScrollDeadzone"
#define AMT_REG_VALUE_SCROLL_FAST_VEL    L"ScrollFastVelocity"
#define AMT_REG_VALUE_SCROLL_SCALE_NUM   L"ScrollScaleNum"
#define AMT_REG_VALUE_SCROLL_SCALE_DEN   L"ScrollScaleDen"
#define AMT_REG_VALUE_SCROLL_SCALE_NUM_FAST L"ScrollScaleNumFast"
#define AMT_REG_VALUE_SCROLL_SCALE_DEN_FAST L"ScrollScaleDenFast"

EXTERN_C_END