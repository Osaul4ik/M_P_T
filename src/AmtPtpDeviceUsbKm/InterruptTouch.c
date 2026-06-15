// InterruptTouch.c: Per-finger slot tracking state machine.
//
// Called exclusively from ProcessingThread.c at PASSIVE_LEVEL.
// No shared state is touched by the USB interrupt callback, so no locking
// is required for any field in this file.
//
// Lifecycle:
//   FREE -> CONFIRMING -> ACTIVE -> PENDING_RELEASE -> COOLDOWN -> FREE
//
//   CONFIRMING (lift before ACTIVE):
//     -> SOFT_PENDING_UP (frame N  : emits TipSwitch=1)
//     -> COOLDOWN        (frame N+1: emits TipSwitch=0)
//     -> FREE
//
// PTP snapshot semantics
// ----------------------
// Each PTP_REPORT is a SNAPSHOT of contacts active in that instant.
// TipSwitch transitions MUST span two separate frames — never in one report.
// ContactCount = number of contacts with TipSwitch=1 in the snapshot.
//
// Soft-tap fix
// ------------
// When a finger lifts before TIP_CONFIRM_FRAMES are accumulated, Phase 3
// transitions CONFIRMING -> SOFT_PENDING_UP with TipSwitch=1 emitted in
// frame N.  Frame N+1 (Phase 3 again) sees SOFT_PENDING_UP, emits
// TipSwitch=0, and moves to COOLDOWN.  Windows sees two distinct frames:
// contact-down then contact-up — a clean tap gesture with no ghost contacts.
//
// LastNormX/Y and HystX/Y are scoped to a single ACTIVE gesture:
//   - HystX/Y is seeded on CONFIRMING->ACTIVE and zeroed on ACTIVE exit.
//   - LastNormX/Y is written every ACTIVE frame, read once for the lift
//     report on PENDING_RELEASE->COOLDOWN, then zeroed.
// By the time a slot reaches FREE both pairs are guaranteed zero, so no
// gesture can inherit a previous gesture's position.

#include "Driver.h"
#include "InterruptTouch.tmh"

// Minimum touch_major / touch_minor (doubled) to count as tip-down.
#define TIP_MAJOR_THRESHOLD  200
#define TIP_MINOR_THRESHOLD  150

// Consecutive tip-down frames required before a slot goes ACTIVE.
#define TIP_CONFIRM_FRAMES   2

// Deadzone: hold last value if movement is below this many raw units.
#define XY_DEADZONE_UNITS    1

// Max position delta (raw units) for 2a-bis rebind; adaptive, see below.
#define REBIND_MAX_DELTA_DENOM  300
#define REBIND_MIN_DELTA        30

// Palm rejection thresholds.
#define PALM_ASPECT_RATIO_THRESHOLD  6
#define PALM_MAJOR_THRESHOLD         350

// Coordinate smoothing (EMA) alpha.
#define SMOOTHING_ALPHA_NUM  3
#define SMOOTHING_ALPHA_DEN  8

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

static inline BOOLEAN
AmtIsPalm(
    _In_ const struct TRACKPAD_FINGER* f,
    _In_ const struct BCM5974_CONFIG* devInfo,
    _In_ USHORT normX,
    _In_ USHORT normY)
{
    INT score = 0;

    if (f->touch_major >= PALM_MAJOR_THRESHOLD) {
        score += 40;
    } else if (f->touch_major > 180) {
        score += 20;
    }

    if (f->touch_minor > 0 && f->touch_major > 80) {
        INT ratio = (INT)f->touch_major * 100 / (INT)f->touch_minor;
        if (ratio > 1100) score += 15;
        else if (ratio > 800) score += 8;
    }

    if (f->touch_major > 140) {
        INT xRange = devInfo->x.max - devInfo->x.min;
        INT yRange = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 32;
        INT edgePctY = yRange / 32;

        if (normX < edgePctX ||
            normX > (xRange - edgePctX) ||
            normY < edgePctY ||
            normY > (yRange - edgePctY))
        {
            score += 3;
        }
    }

    return (score >= 75);
}

static inline INT
AmtAbsDelta(_In_ INT a, _In_ INT b)
{
    INT d = a - b;
    return (d < 0) ? -d : d;
}

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

// ---------------------------------------------------------------------------
// Debug-only FSM invariant check.
// Extended to cover SLOT_SOFT_PENDING_UP.
// ---------------------------------------------------------------------------
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
            // LastNormX/Y may be non-zero (seeded in Phase 2b).
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

        case SLOT_SOFT_PENDING_UP:
            // Waiting to emit TipSwitch=0 next frame.
            // LastNormX/Y hold the tap position (non-zero is expected).
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

static inline INT
AmtGetRebindMaxDelta(
    _In_ PDEVICE_CONTEXT pCtx)
{
    INT xRange = pCtx->DeviceInfo->x.max - pCtx->DeviceInfo->x.min;
    INT adaptive = xRange / REBIND_MAX_DELTA_DENOM;
    return (adaptive < REBIND_MIN_DELTA) ? REBIND_MIN_DELTA : adaptive;
}

// ---------------------------------------------------------------------------
// AmtEmitSoftTap
//
// Called from Phase 3 when a CONFIRMING slot lifts before reaching
// TIP_CONFIRM_FRAMES (TipConfirmed >= 1 means at least one real frame).
//
// This function handles FRAME N of the two-frame soft-tap sequence:
//   - Emits one TipSwitch=1 contact entry (contact appears).
//   - Transitions slot to SLOT_SOFT_PENDING_UP.
//   - Preserves LastNormX/Y for the next frame's TipSwitch=0 emission.
//
// Frame N+1 is handled in Phase 3 under the SLOT_SOFT_PENDING_UP case.
//
// Returns updated reportSlots count.
//
// ContactCount rule: this entry IS counted (TipSwitch=1 = active contact).
// ---------------------------------------------------------------------------
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

    contactID = pCtx->NextContactID++;
    st->ContactID = contactID;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
        "%!FUNC! soft tap frame N: slot=%llu normX=%u normY=%u ContactID=%lu",
        (ULONG64)slot, normX, normY, contactID);

    // Emit TipSwitch=1 — contact appears this frame.
    if (reportSlots < PTP_MAX_CONTACT_POINTS) {
        PtpReport->Contacts[reportSlots].ContactID  = contactID;
        PtpReport->Contacts[reportSlots].X          = normX;
        PtpReport->Contacts[reportSlots].Y          = normY;
        PtpReport->Contacts[reportSlots].TipSwitch  = 1;
        PtpReport->Contacts[reportSlots].Confidence = 1;
        reportSlots++;
    }

    // Transition to SOFT_PENDING_UP — TipSwitch=0 will be emitted next frame.
    // LastNormX/Y carry the position forward for that emission.
    st->Phase        = SLOT_SOFT_PENDING_UP;
    st->TipConfirmed = 0;
    st->FingerKey    = SLOT_KEY_NONE;
    st->HystX        = 0;
    st->HystY        = 0;
    st->LastNormX    = normX;   // preserved for frame N+1 tip-up
    st->LastNormY    = normY;

    return reportSlots;
}

// ---------------------------------------------------------------------------
// AmtPtpProcessTouchFrame
//
// Produces one PTP_REPORT snapshot from a raw USB frame.
// Called exclusively from ProcessingThread.c at PASSIVE_LEVEL.
//
// ContactCount = number of TipSwitch=1 entries in this snapshot.
// TipSwitch=0 entries (lifts) are included in the Contacts array so
// Windows can match them to their ContactID, but they do NOT increment
// the ContactCount per PTP specification.
// ---------------------------------------------------------------------------
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

    // reportSlots  : total entries written to PtpReport->Contacts
    // activeTipCount: entries with TipSwitch=1 (= ContactCount)
    UCHAR reportSlots  = *pReportSlots;
    UCHAR activeTipCount = 0;   // will become ContactCount

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

    // 2a. Key match.
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

    // 2a-bis. Position rebind (greedy global mutual-best-match).
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
            break;
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
                // Seed position for 2a-bis and soft-tap emission.
                st->LastNormX = fingerNormX[i];
                st->LastNormY = fingerNormY[i];
                break;
            }
        }
    }

    // Phase 3: advance slots that have no finger match this frame.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        if (fingerForSlot[s] != SLOT_NONE) continue;

        PSLOT_STATE st = &pCtx->Slots[s];

        switch (st->Phase) {

        // ------------------------------------------------------------------
        // SOFT_PENDING_UP: frame N+1 of synthetic soft tap.
        // Emit TipSwitch=0 (contact disappears) then -> COOLDOWN.
        // This entry is a lift; it does NOT increment activeTipCount.
        // ------------------------------------------------------------------
        case SLOT_SOFT_PENDING_UP:
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! slot=%llu SOFT_PENDING_UP -> COOLDOWN (soft tap frame N+1, ContactID=%lu)",
                (ULONG64)s, st->ContactID);

            if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                PtpReport->Contacts[reportSlots].ContactID  = st->ContactID;
                PtpReport->Contacts[reportSlots].X          = st->LastNormX;
                PtpReport->Contacts[reportSlots].Y          = st->LastNormY;
                PtpReport->Contacts[reportSlots].TipSwitch  = 0;
                PtpReport->Contacts[reportSlots].Confidence = 1;
                reportSlots++;
                // TipSwitch=0 → lift → NOT counted in activeTipCount
            } else {
                // No room — stay in SOFT_PENDING_UP, retry next frame.
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
                    "%!FUNC! slot=%llu SOFT_PENDING_UP: report full, tip-up deferred",
                    (ULONG64)s);
                break;
            }

            st->Phase     = SLOT_COOLDOWN;
            st->Cooldown  = 2;
            st->LastNormX = 0;
            st->LastNormY = 0;
            break;
        }

        // ------------------------------------------------------------------
        // PENDING_RELEASE: normal finger lift.
        // Emit TipSwitch=0 then -> COOLDOWN.
        // Lift entry does NOT increment activeTipCount.
        // ------------------------------------------------------------------
        case SLOT_PENDING_RELEASE:
        {
            if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                PtpReport->Contacts[reportSlots].ContactID  = st->ContactID;
                PtpReport->Contacts[reportSlots].X          = st->LastNormX;
                PtpReport->Contacts[reportSlots].Y          = st->LastNormY;
                PtpReport->Contacts[reportSlots].TipSwitch  = 0;
                PtpReport->Contacts[reportSlots].Confidence = 1;
                reportSlots++;
                // TipSwitch=0 → NOT counted in activeTipCount
            } else {
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
        }

        // ------------------------------------------------------------------
        // ACTIVE: finger left surface — begin release sequence.
        // No emission this frame; lift appears in the NEXT frame when the
        // slot is seen in PENDING_RELEASE.
        // ------------------------------------------------------------------
        case SLOT_ACTIVE:
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot=%llu ACTIVE -> PENDING_RELEASE", (ULONG64)s);

            st->Phase        = SLOT_PENDING_RELEASE;
            st->FingerKey    = SLOT_KEY_NONE;
            st->TipConfirmed = 0;
            st->HystX        = 0;
            st->HystY        = 0;
            break;
        }

        // ------------------------------------------------------------------
        // CONFIRMING (no match): finger lifted before reaching ACTIVE.
        //
        //   TipConfirmed >= 1 → genuine soft tap → frame N: emit TipSwitch=1,
        //                        slot -> SOFT_PENDING_UP.
        //                        Frame N+1 (above) emits TipSwitch=0.
        //
        //   TipConfirmed == 0 → brand-new slot, finger already gone →
        //                        almost certainly noise → discard silently.
        // ------------------------------------------------------------------
        case SLOT_CONFIRMING:
        {
            if (st->TipConfirmed >= 1) {
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! slot=%llu CONFIRMING -> SOFT_PENDING_UP "
                    "(soft tap frame N, TipConfirmed=%d)",
                    (ULONG64)s, st->TipConfirmed);

                // AmtEmitSoftTap emits TipSwitch=1 and moves to SOFT_PENDING_UP.
                // This IS an active contact this frame → count it.
                reportSlots = AmtEmitSoftTap(
                    pCtx, s,
                    st->LastNormX, st->LastNormY,
                    PtpReport, reportSlots);
                activeTipCount++;   // TipSwitch=1 entry
            } else {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! slot=%llu CONFIRMING -> FREE (noise, tip=0)",
                    (ULONG64)s);

                st->Phase        = SLOT_FREE;
                st->TipConfirmed = 0;
                st->FingerKey    = SLOT_KEY_NONE;
                st->LastNormX    = 0;
                st->LastNormY    = 0;
            }
            break;
        }

        case SLOT_FREE:
        case SLOT_COOLDOWN:
        default:
            break;
        }
    }

    // Phase 4/5: emit contacts for matched (active) fingers.
    // Every entry here has TipSwitch=1 — count each in activeTipCount.
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
            // Still CONFIRMING — update position for 2a-bis.
            st->LastNormX = fingerNormX[i];
            st->LastNormY = fingerNormY[i];
            continue;
        }

        if (justConfirmed) {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                "%!FUNC! slot=%d CONFIRMING -> ACTIVE (ContactID=%lu)",
                (INT)slot, pCtx->NextContactID);

            st->Phase     = SLOT_ACTIVE;
            st->HystX     = fingerNormX[i];
            st->HystY     = fingerNormY[i];
            st->SmoothedX = fingerNormX[i];
            st->SmoothedY = fingerNormY[i];
            st->ContactID = pCtx->NextContactID++;
        }

        USHORT reportX, reportY;

        USHORT afterDeadzoneX = AmtApplyDeadzone(fingerNormX[i], &st->HystX);
        USHORT afterDeadzoneY = AmtApplyDeadzone(fingerNormY[i], &st->HystY);

        reportX = AmtSmoothCoord(afterDeadzoneX, st->SmoothedX);
        reportY = AmtSmoothCoord(afterDeadzoneY, st->SmoothedY);

        st->SmoothedX = reportX;
        st->SmoothedY = reportY;

        if (reportSlots < PTP_MAX_CONTACT_POINTS) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * pCtx->DeviceInfo->tp_fsize);
            BOOLEAN confidence = (AmtRawToInteger(f->touch_minor) << 1) > 0;

            PtpReport->Contacts[reportSlots].ContactID  = st->ContactID;
            PtpReport->Contacts[reportSlots].X          = reportX;
            PtpReport->Contacts[reportSlots].Y          = reportY;
            PtpReport->Contacts[reportSlots].TipSwitch  = 1;
            PtpReport->Contacts[reportSlots].Confidence = confidence ? 1 : 0;
            reportSlots++;
            activeTipCount++;   // TipSwitch=1 → active contact
        }

        st->LastNormX = reportX;
        st->LastNormY = reportY;
    }

    // ContactCount = number of contacts with TipSwitch=1 in this snapshot.
    // Lift entries (TipSwitch=0) are present in Contacts[] so Windows can
    // match them by ContactID, but they are not "active" contacts.
    PtpReport->ContactCount = activeTipCount;

    // pReportSlots carries the total slot count back to the caller so it
    // can pass the correct value to WdfMemoryCopyFromBuffer.
    *pReportSlots = reportSlots;

    AmtAssertSlotInvariants(pCtx);
}