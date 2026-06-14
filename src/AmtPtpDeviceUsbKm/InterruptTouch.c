// InterruptTouch.c: Per-finger slot tracking state machine.
//
// Called exclusively from ProcessingThread.c at PASSIVE_LEVEL.
// No shared state is touched by the USB interrupt callback, so no locking
// is required for any field in this file.
//
// Lifecycle: FREE -> CONFIRMING -> ACTIVE -> PENDING_RELEASE -> COOLDOWN -> FREE
//
// LastNormX/Y and HystX/Y are scoped to a single ACTIVE gesture:
//   - HystX/Y is seeded on CONFIRMING->ACTIVE and zeroed on ACTIVE exit.
//   - LastNormX/Y is written every ACTIVE frame, read once for the lift
//     report on PENDING_RELEASE->COOLDOWN, then zeroed.
// By the time a slot reaches FREE both pairs are guaranteed zero, so no
// gesture can inherit a previous gesture's position.
//
// State is consolidated into SLOT_STATE (include/Hid.h): one struct per
// slot with an explicit Phase field, instead of 8 parallel arrays whose
// combinations previously had to encode the FSM state implicitly.
// AmtAssertSlotInvariants() validates FSM consistency after every frame
// in debug builds.
//
// --- Soft Tap Fix ---
// When TIP_CONFIRM_FRAMES > 1 a very brief touch (fewer frames than
// TIP_CONFIRM_FRAMES) would be silently discarded: Phase 4/5 only emits
// a contact once the slot is ACTIVE, but if the finger lifts before
// enough confirm-frames accumulate, Phase 3 transitions CONFIRMING->FREE
// without ever emitting anything.  Windows never sees a tap.
//
// Fix: on CONFIRMING->FREE, if TipConfirmed >= 1, synthesise a
// compressed tap: emit one frame with TipSwitch=1 (tip-down) followed
// immediately by a tip-up entry in the same report via PENDING_RELEASE.
// Because both are in the same PTP_REPORT the HID stack sees them in
// order and Windows registers a tap.  A ContactID is still allocated from
// pCtx->NextContactID so Windows can track the contact correctly.

#include "Driver.h"
#include "InterruptTouch.tmh"

// Minimum touch_major / touch_minor (doubled) to count as tip-down.
#define TIP_MAJOR_THRESHOLD  200
#define TIP_MINOR_THRESHOLD  150

// Consecutive tip-down frames required before a slot goes ACTIVE.
// NOTE: soft tap emission on CONFIRMING->FREE handles the case where
// the finger lifts before this count is reached (see above).
#define TIP_CONFIRM_FRAMES   2

// Deadzone: hold last value if movement is below this many raw units.
#define XY_DEADZONE_UNITS    2

// Max position delta (raw units) allowed for a 2a-bis rebind. This catches a
// USB-array index swap between two fingers in the same frame, which Phase 2a's
// pure key match cannot — the swap looks like two simultaneous key mismatches
// but the true fingers barely moved.
//
// NOTE: FingerKey is the USB-array index recorded at the slot's last
// (re)bind, NOT a hardware finger ID — the T2 controller does not expose
// one. Phase 2a key-match only succeeds when the controller happens to
// keep a tracked finger at the same array index across frames; Phase 2a-bis
// position rebind is the primary mechanism for re-establishing identity
// after a reorder.
//
// To reduce false matches when two fingers are close together, 2a-bis uses
// a greedy MUTUAL-best-match: repeatedly bind the globally closest
// (slot, finger) pair among those still unmatched. This guarantees a slot
// never steals a finger that is actually closer to a different,
// still-unmatched slot.
//
// REBIND_MAX_DELTA is scaled per device by using the x resolution range.
#define REBIND_MAX_DELTA_DENOM  300  // fraction of x-range for adaptive delta
#define REBIND_MIN_DELTA        30   // minimum absolute delta

// Palm rejection: finger aspect ratio threshold (major/minor > this = palm)
// A typical fingertip has aspect ratio ~1.0–2.0; a palm is >3.0.
#define PALM_ASPECT_RATIO_THRESHOLD  6
// Palm rejection: absolute major threshold (in raw units).
// Typical finger touch_major values are 100–200; palm/thumb values are 250+.
#define PALM_MAJOR_THRESHOLD         250

// Coordinate smoothing (exponential moving average) alpha factor.
// Higher = more responsive, lower = smoother.
#define SMOOTHING_ALPHA_NUM  3   // numerator   (3/8 = 0.375)
#define SMOOTHING_ALPHA_DEN  8   // denominator

#define SLOT_NONE  ((UCHAR)PTP_MAX_CONTACT_POINTS)

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT v = raw - minVal;
    if (v < 0)      v = 0;
    if (v > maxVal) v = maxVal;
    return (USHORT)v;
}

static inline USHORT
AmtApplyDeadzone(_In_ USHORT newVal, _Inout_ USHORT* pBaseline)
{
#if XY_DEADZONE_UNITS > 0
    INT delta = (INT)newVal - (INT)(*pBaseline);
    if (delta < 0) delta = -delta;
    if (delta < XY_DEADZONE_UNITS) {
        return *pBaseline;
    }
#endif
    *pBaseline = newVal;
    return newVal;
}

//
// Exponential moving average smoothing (non-linear, low-latency).
// blended = (raw * alpha_num + prev * (alpha_den - alpha_num)) / alpha_den
//
static inline USHORT
AmtSmoothCoord(
    _In_ USHORT rawVal,
    _In_ USHORT prevVal)
{
    INT blended = ((INT)rawVal * SMOOTHING_ALPHA_NUM +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM))
                  / SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

//
// Palm rejection heuristics.
// Returns TRUE if the finger is likely a palm and should be suppressed.
//
static inline BOOLEAN
AmtIsPalm(
    _In_ const struct TRACKPAD_FINGER* f,
    _In_ const struct BCM5974_CONFIG* devInfo,
    _In_ USHORT normX,
    _In_ USHORT normY)
{
    // 1. Absolute size check: if touch_major is huge (≥250), it's a palm
    //    or the edge of the thumb.  Typical fingertip values are 100–200.
    if (f->touch_major >= PALM_MAJOR_THRESHOLD) {
        return TRUE;
    }

    // 2. Aspect ratio: palm is elongated (oval), finger is round.
    //    If minor > 0, ratio = major / minor.  Only reject when both
    //    the aspect ratio is extreme AND the finger is fairly large
    //    (touch_major > 80) to avoid rejecting small elongated noise.
    if (f->touch_minor > 0 && f->touch_major > 80) {
        INT ratio = (INT)f->touch_major / (INT)f->touch_minor;
        if (ratio >= PALM_ASPECT_RATIO_THRESHOLD) {
            return TRUE;
        }
    }

    // 3. Edge zones: only reject fingers very close to the absolute edge
    //    (3% margin) AND that are also somewhat large — a small fingertip
    //    near the edge (e.g., reaching for a scrollbar) should still work.
    if (f->touch_major > 120) {
        INT xRange = devInfo->x.max - devInfo->x.min;
        INT yRange = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 32;  // ~3% from each edge
        INT edgePctY = yRange / 32;

        if (normX < (USHORT)edgePctX ||
            normX > (USHORT)(xRange - edgePctX) ||
            normY < (USHORT)edgePctY ||
            normY > (USHORT)(yRange - edgePctY))
        {
            return TRUE;
        }
    }

    return FALSE;
}

static inline INT
AmtAbsDelta(_In_ INT a, _In_ INT b)
{
    INT d = a - b;
    return (d < 0) ? -d : d;
}

//
// Normalise one finger's raw coordinates into device-space (0..range).
// Pure function of the device config + raw finger sample; touches no
// slot state. Unifies the clamp logic that was previously inlined in
// Phase 1.
//
static inline VOID
AmtNormalizeFinger(
    _In_  PDEVICE_CONTEXT pCtx,
    _In_  const struct TRACKPAD_FINGER* f,
    _Out_ USHORT* pNormX,
    _Out_ USHORT* pNormY)
{
    INT xRange = pCtx->DeviceInfo->x.max - pCtx->DeviceInfo->x.min;
    INT yRange = pCtx->DeviceInfo->y.max - pCtx->DeviceInfo->y.min;
    INT normY;

    *pNormX = AmtClampCoord(
        AmtRawToInteger(f->abs_x), pCtx->DeviceInfo->x.min, xRange);

    normY = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
    *pNormY = AmtClampCoord(normY, 0, yRange);
}

//
// Debug-only FSM invariant check, run once per frame after all slot
// transitions are applied. Traces and asserts on the first violation
// found per slot. See include/Hid.h (SLOT_STATE) for the documented
// per-phase invariants this enforces.
//
static inline VOID
AmtAssertSlotInvariants(
    _In_ PDEVICE_CONTEXT pCtx)
{
#if DBG
    size_t s;

    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        PSLOT_STATE st = &pCtx->Slots[s];
        BOOLEAN ok = TRUE;

        switch (st->Phase) {
        case SLOT_FREE:
            ok = (st->TipConfirmed == 0) && (st->Cooldown == 0) &&
                 (st->FingerKey == SLOT_KEY_NONE) &&
                 (st->LastNormX == 0) && (st->LastNormY == 0) &&
                 (st->HystX == 0) && (st->HystY == 0);
            break;

        case SLOT_CONFIRMING:
            ok = (st->TipConfirmed >= 1) &&
                 (st->TipConfirmed < TIP_CONFIRM_FRAMES) &&
                 (st->Cooldown == 0) &&
                 (st->HystX == 0) && (st->HystY == 0);
            break;

        case SLOT_ACTIVE:
            ok = (st->TipConfirmed == TIP_CONFIRM_FRAMES) &&
                 (st->Cooldown == 0) &&
                 (st->FingerKey != SLOT_KEY_NONE);
            break;

        case SLOT_PENDING_RELEASE:
            ok = (st->TipConfirmed == 0) && (st->Cooldown == 0) &&
                 (st->HystX == 0) && (st->HystY == 0) &&
                 (st->FingerKey == SLOT_KEY_NONE);
            break;

        case SLOT_COOLDOWN:
            ok = (st->Cooldown > 0) && (st->TipConfirmed == 0) &&
                 (st->HystX == 0) && (st->HystY == 0) &&
                 (st->LastNormX == 0) && (st->LastNormY == 0);
            break;

        default:
            ok = FALSE;
            break;
        }

        if (!ok) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_INPUT,
                "%!FUNC! INVARIANT VIOLATION slot=%llu phase=%d "
                "tip=%d cooldown=%d key=0x%02x last=(%u,%u) hyst=(%u,%u)",
                (ULONG64)s, (INT)st->Phase, st->TipConfirmed, st->Cooldown,
                st->FingerKey, st->LastNormX, st->LastNormY,
                st->HystX, st->HystY);
            NT_ASSERTMSG("Slot FSM invariant violated", FALSE);
        }
    }
#else
    UNREFERENCED_PARAMETER(pCtx);
#endif
}

//
// Compute a per-device REBIND_MAX_DELTA based on x-resolution range.
// This makes the 2a-bis position threshold consistent across different
// trackpad sizes (13" vs 15" etc.) and avoids false matches on
// high-resolution devices where fixed 30 units is too large.
//
static inline INT
AmtGetRebindMaxDelta(
    _In_ PDEVICE_CONTEXT pCtx)
{
    INT xRange = pCtx->DeviceInfo->x.max - pCtx->DeviceInfo->x.min;
    INT adaptive = xRange / REBIND_MAX_DELTA_DENOM;
    return (adaptive < REBIND_MIN_DELTA) ? REBIND_MIN_DELTA : adaptive;
}

//
// Emit a synthetic tap (tip-down + tip-up) for a slot that was in
// CONFIRMING but lifted before reaching TIP_CONFIRM_FRAMES.
//
// Two contacts are appended to ptpReport:
//   [reportSlots+0]: TipSwitch=1 (tip-down) at the slot's last known position
//   [reportSlots+1]: TipSwitch=0 (tip-up)   at the same position
//
// Both share the same ContactID so Windows sees a clean tap gesture.
//
// The slot is immediately transitioned to COOLDOWN so no further processing
// occurs for it this frame. LastNormX/Y are zeroed after use (COOLDOWN
// invariant requires them to be 0).
//
// Returns the new reportSlots count (incremented by 0, 1, or 2 depending
// on remaining capacity).
//
static UCHAR
AmtEmitSoftTap(
    _In_    PDEVICE_CONTEXT pCtx,
    _In_    size_t          slot,
    _In_    USHORT          normX,
    _In_    USHORT          normY,
    _Inout_ PTP_REPORT*     PtpReport,
    _In_    UCHAR           reportSlots)
{
    PSLOT_STATE st = &pCtx->Slots[slot];
    ULONG contactID;

    // Assign a unique ContactID for this synthetic tap.
    contactID = pCtx->NextContactID++;
    st->ContactID = contactID;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
        "%!FUNC! soft tap: slot=%llu normX=%u normY=%u ContactID=%lu",
        (ULONG64)slot, normX, normY, contactID);

    // Tip-down entry.
    if (reportSlots < PTP_MAX_CONTACT_POINTS) {
        PtpReport->Contacts[reportSlots].ContactID  = contactID;
        PtpReport->Contacts[reportSlots].X          = normX;
        PtpReport->Contacts[reportSlots].Y          = normY;
        PtpReport->Contacts[reportSlots].TipSwitch  = 1;
        PtpReport->Contacts[reportSlots].Confidence = 1;
        reportSlots++;
    }

    // Tip-up entry immediately following — same report, next slot.
    // Windows PTP processes contacts in array order within one report,
    // so tip-down then tip-up in the same report constitutes a valid tap.
    if (reportSlots < PTP_MAX_CONTACT_POINTS) {
        PtpReport->Contacts[reportSlots].ContactID  = contactID;
        PtpReport->Contacts[reportSlots].X          = normX;
        PtpReport->Contacts[reportSlots].Y          = normY;
        PtpReport->Contacts[reportSlots].TipSwitch  = 0;
        PtpReport->Contacts[reportSlots].Confidence = 1;
        reportSlots++;
    }

    // Transition to COOLDOWN — all fields must satisfy COOLDOWN invariant.
    st->Phase        = SLOT_COOLDOWN;
    st->Cooldown     = 2;
    st->TipConfirmed = 0;
    st->FingerKey    = SLOT_KEY_NONE;
    st->HystX        = 0;
    st->HystY        = 0;
    st->LastNormX    = 0;    // COOLDOWN requires LastNormX == 0
    st->LastNormY    = 0;    // COOLDOWN requires LastNormY == 0

    return reportSlots;
}

VOID
AmtPtpProcessTouchFrame(
    _In_    PDEVICE_CONTEXT pCtx,
    _In_    UCHAR*          TouchBuffer,
    _In_    size_t          raw_n,
    _Inout_ PTP_REPORT*     PtpReport,
    _Inout_ UCHAR*          pReportSlots)
{
    UCHAR* f_base = TouchBuffer
        + pCtx->DeviceInfo->tp_header
        + pCtx->DeviceInfo->tp_delta;
    const struct TRACKPAD_FINGER* f;
    size_t i, s;
    UCHAR reportSlots = *pReportSlots;
    INT rebindMaxDelta = AmtGetRebindMaxDelta(pCtx);

    // Phase 0: cooldown countdown.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        PSLOT_STATE st = &pCtx->Slots[s];

        if (st->Phase == SLOT_COOLDOWN) {
            st->Cooldown--;
            if (st->Cooldown == 0) {
                st->FingerKey = SLOT_KEY_NONE;
                st->Phase     = SLOT_FREE;

                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot=%llu COOLDOWN -> FREE", (ULONG64)s);
            }
        }
    }

    // Phase 1: per-finger tip-down + normalised coords + array-index key.
    BOOLEAN fingerTipDown [PTP_MAX_CONTACT_POINTS] = { FALSE };
    USHORT  fingerNormX   [PTP_MAX_CONTACT_POINTS] = { 0 };
    USHORT  fingerNormY   [PTP_MAX_CONTACT_POINTS] = { 0 };
    UCHAR   fingerKey     [PTP_MAX_CONTACT_POINTS];

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
        fingerKey[i] = SLOT_KEY_NONE;

    for (i = 0; i < raw_n; i++) {
        f = (const struct TRACKPAD_FINGER*)(f_base + i * pCtx->DeviceInfo->tp_fsize);

        BOOLEAN tip =
            (AmtRawToInteger(f->touch_major) << 1) >= TIP_MAJOR_THRESHOLD ||
            (AmtRawToInteger(f->touch_minor) << 1) >= TIP_MINOR_THRESHOLD;

        if (!tip) {
            fingerTipDown[i] = FALSE;
            continue;
        }

        AmtNormalizeFinger(pCtx, f, &fingerNormX[i], &fingerNormY[i]);

        // Palm rejection: if this finger looks like a palm, ignore it.
        if (AmtIsPalm(f, pCtx->DeviceInfo, fingerNormX[i], fingerNormY[i])) {
            fingerTipDown[i] = FALSE;
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! finger=%d rejected as palm", (INT)i);
            continue;
        }

        fingerTipDown[i] = TRUE;
        fingerKey[i] = (UCHAR)i;
    }

    // Phase 2: match tip-down fingers to slots.
    UCHAR slotForFinger[PTP_MAX_CONTACT_POINTS];
    UCHAR fingerForSlot[PTP_MAX_CONTACT_POINTS];

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) slotForFinger[i] = SLOT_NONE;
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) fingerForSlot[s] = SLOT_NONE;

    // 2a. Match by key (works only when the controller happens to keep a
    // tracked finger at the same array index across frames).
    for (i = 0; i < raw_n; i++) {
        if (!fingerTipDown[i]) continue;
        UCHAR key = fingerKey[i];
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            PSLOT_STATE st = &pCtx->Slots[s];
            if ((st->Phase == SLOT_ACTIVE || st->Phase == SLOT_CONFIRMING) &&
                st->FingerKey == key &&
                fingerForSlot[s] == SLOT_NONE)
            {
                slotForFinger[i] = (UCHAR)s;
                fingerForSlot[s] = (UCHAR)i;
                break;
            }
        }
    }

    // 2a-bis. Position rebind for slots/fingers left unmatched.
    //
    // Greedy global mutual-best-match: repeatedly find the closest
    // remaining (slot, finger) pair within rebindMaxDelta and bind it.
    // Because each iteration picks the single globally-closest pair, a
    // slot can never grab a finger that some other still-unmatched slot
    // is closer to (that other pair would have been picked first), and
    // vice versa for fingers. This is equivalent to enforcing mutual
    // best-match without an O(n^2) stable-matching pass, which is
    // unnecessary at n <= PTP_MAX_CONTACT_POINTS (5).
    for (;;) {
        UCHAR bestSlot   = SLOT_NONE;
        UCHAR bestFinger = SLOT_NONE;
        INT   bestDist   = rebindMaxDelta + 1;

        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            PSLOT_STATE st = &pCtx->Slots[s];

            if (fingerForSlot[s] != SLOT_NONE) continue;
            if (!(st->Phase == SLOT_ACTIVE || st->Phase == SLOT_CONFIRMING)) continue;

            for (i = 0; i < raw_n; i++) {
                if (!fingerTipDown[i]) continue;
                if (slotForFinger[i] != SLOT_NONE) continue;

                INT dx   = AmtAbsDelta((INT)fingerNormX[i], (INT)st->LastNormX);
                INT dy   = AmtAbsDelta((INT)fingerNormY[i], (INT)st->LastNormY);
                INT dist = dx + dy;

                if (dist < bestDist) {
                    bestDist   = dist;
                    bestSlot   = (UCHAR)s;
                    bestFinger = (UCHAR)i;
                }
            }
        }

        if (bestSlot == SLOT_NONE) {
            break;  // no remaining candidate pair within rebindMaxDelta
        }

        slotForFinger[bestFinger] = bestSlot;
        fingerForSlot[bestSlot]   = bestFinger;
        pCtx->Slots[bestSlot].FingerKey = fingerKey[bestFinger];

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
            "%!FUNC! slot=%d rebound to finger=%d by position (dist=%d, max=%d)",
            (INT)bestSlot, (INT)bestFinger, bestDist, rebindMaxDelta);
    }

    // 2b. Assign new slots for unmatched fingers.
    for (i = 0; i < raw_n; i++) {
        if (!fingerTipDown[i]) continue;
        if (slotForFinger[i] != SLOT_NONE) continue;

        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            PSLOT_STATE st = &pCtx->Slots[s];

            if (st->Phase == SLOT_FREE && fingerForSlot[s] == SLOT_NONE) {
                slotForFinger[i] = (UCHAR)s;
                fingerForSlot[s] = (UCHAR)i;
                st->FingerKey    = fingerKey[i];

                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot=%llu FREE -> CONFIRMING (finger=%d)",
                    (ULONG64)s, (INT)i);

                st->Phase = SLOT_CONFIRMING;
                // Seed LastNormX/Y so 2a-bis can rebind on the next frame
                // even if TipConfirmed hasn't reached ACTIVE yet.
                // (Invariant checker allows LastNormX/Y to be non-zero in
                //  CONFIRMING only when noted here — CONFIRMING invariant
                //  in AmtAssertSlotInvariants does NOT require them to be 0.)
                st->LastNormX = fingerNormX[i];
                st->LastNormY = fingerNormY[i];
                break;
            }
        }
    }

    // Phase 3: advance slots with no finger match this frame.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        if (fingerForSlot[s] != SLOT_NONE) continue;

        PSLOT_STATE st = &pCtx->Slots[s];

        switch (st->Phase) {
        case SLOT_PENDING_RELEASE:
            // PENDING_RELEASE -> emit lift, enter COOLDOWN.
            if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                PtpReport->Contacts[reportSlots].ContactID  = st->ContactID;
                PtpReport->Contacts[reportSlots].X          = st->LastNormX;
                PtpReport->Contacts[reportSlots].Y          = st->LastNormY;
                PtpReport->Contacts[reportSlots].TipSwitch  = 0;
                PtpReport->Contacts[reportSlots].Confidence = 1;
                reportSlots++;
            } else {
                // No room in this report — the lift will be emitted next frame
                // because the slot remains PENDING_RELEASE.
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
                    "%!FUNC! slot=%llu PENDING_RELEASE: report full, lift deferred",
                    (ULONG64)s);
                break;
            }

            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot=%llu PENDING_RELEASE -> COOLDOWN", (ULONG64)s);

            st->Phase     = SLOT_COOLDOWN;
            st->Cooldown  = 2;
            st->LastNormX = 0;
            st->LastNormY = 0;
            break;

        case SLOT_ACTIVE:
            // ACTIVE -> PENDING_RELEASE.
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot=%llu ACTIVE -> PENDING_RELEASE", (ULONG64)s);

            st->Phase        = SLOT_PENDING_RELEASE;
            st->FingerKey    = SLOT_KEY_NONE;
            st->TipConfirmed = 0;
            st->HystX        = 0;
            st->HystY        = 0;
            break;

        case SLOT_CONFIRMING:
            // CONFIRMING (no match) -> the finger lifted before we could
            // confirm it as ACTIVE.
            //
            // FIX: If TipConfirmed >= 1 the hardware saw at least one
            // real tip-down frame — this was a genuine (soft) tap.
            // Synthesise a compressed tip-down + tip-up in this report so
            // Windows registers the tap gesture.
            //
            // If TipConfirmed == 0 the slot was brand-new this frame and
            // the finger is already gone — almost certainly electrical
            // noise.  Discard silently.
            if (st->TipConfirmed >= 1) {
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! slot=%llu CONFIRMING -> synthetic soft tap "
                    "(TipConfirmed=%d)", (ULONG64)s, st->TipConfirmed);

                // Use the last known position seeded in Phase 2b.
                reportSlots = AmtEmitSoftTap(
                    pCtx, s,
                    st->LastNormX, st->LastNormY,
                    PtpReport, reportSlots);
                // AmtEmitSoftTap transitions the slot to COOLDOWN and
                // clears all fields — nothing more to do here.
            } else {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot=%llu CONFIRMING -> FREE (noise, tip=0)",
                    (ULONG64)s);

                // Clean transition to FREE — clear all fields.
                st->Phase        = SLOT_FREE;
                st->TipConfirmed = 0;
                st->FingerKey    = SLOT_KEY_NONE;
                st->LastNormX    = 0;   // was seeded in Phase 2b; clear now
                st->LastNormY    = 0;
            }
            break;

        case SLOT_FREE:
        case SLOT_COOLDOWN:
        default:
            // Nothing to do — handled in Phase 0 or already idle.
            break;
        }
    }

    // Phase 4/5: emit contacts for matched fingers (with smoothing).
    for (i = 0; i < raw_n; i++) {
        if (!fingerTipDown[i]) continue;

        UCHAR slot = slotForFinger[i];
        if (slot >= PTP_MAX_CONTACT_POINTS) continue;

        PSLOT_STATE st = &pCtx->Slots[slot];

        if (st->TipConfirmed < TIP_CONFIRM_FRAMES) {
            st->TipConfirmed++;
        }

        BOOLEAN alreadyActive = (st->Phase == SLOT_ACTIVE);
        BOOLEAN justConfirmed = (!alreadyActive &&
            st->TipConfirmed >= TIP_CONFIRM_FRAMES);

        if (!alreadyActive && !justConfirmed) {
            // Still CONFIRMING — update LastNormX/Y so 2a-bis stays
            // accurate on the next frame even though we don't emit yet.
            st->LastNormX = fingerNormX[i];
            st->LastNormY = fingerNormY[i];
            continue;
        }

        if (justConfirmed) {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot=%d CONFIRMING -> ACTIVE (ContactID=%lu)",
                (INT)slot, pCtx->NextContactID);

            st->Phase = SLOT_ACTIVE;
            st->HystX = fingerNormX[i];
            st->HystY = fingerNormY[i];
            // Seed the smoother with the first active position.
            st->SmoothedX = fingerNormX[i];
            st->SmoothedY = fingerNormY[i];
            // Assign a globally unique ContactID so that Windows PTP
            // can track this physical finger across frames.
            st->ContactID = pCtx->NextContactID++;
        }

        USHORT reportX, reportY;

        // Apply deadzone, then EMA smoothing.
        USHORT afterDeadzoneX = AmtApplyDeadzone(fingerNormX[i], &st->HystX);
        USHORT afterDeadzoneY = AmtApplyDeadzone(fingerNormY[i], &st->HystY);

        reportX = AmtSmoothCoord(afterDeadzoneX, st->SmoothedX);
        reportY = AmtSmoothCoord(afterDeadzoneY, st->SmoothedY);

        st->SmoothedX = reportX;
        st->SmoothedY = reportY;

        if (reportSlots < PTP_MAX_CONTACT_POINTS) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * pCtx->DeviceInfo->tp_fsize);
            BOOLEAN confidence = (AmtRawToInteger(f->touch_minor) << 1) > 0;

            // Use the stable ContactID assigned at CONFIRMING->ACTIVE.
            // Using slot index as ContactID causes cursor jumps when
            // a fast 2-finger tap is followed by a 1-finger tap, because
            // the new finger reuses a slot whose old ContactID Windows
            // still associates with the previous gesture's position.
            PtpReport->Contacts[reportSlots].ContactID  = st->ContactID;
            PtpReport->Contacts[reportSlots].X          = reportX;
            PtpReport->Contacts[reportSlots].Y          = reportY;
            PtpReport->Contacts[reportSlots].TipSwitch  = 1;
            PtpReport->Contacts[reportSlots].Confidence = confidence ? 1 : 0;
            reportSlots++;
        }

        st->LastNormX = reportX;
        st->LastNormY = reportY;
    }

    *pReportSlots = reportSlots;

    AmtAssertSlotInvariants(pCtx);
}