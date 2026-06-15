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
#define TIP_MAJOR_THRESHOLD 200
#define TIP_MINOR_THRESHOLD 150

// Consecutive tip-down frames required before a slot goes ACTIVE.
#define TIP_CONFIRM_FRAMES 2

// Deadzone: hold last value if movement is below this many raw units.
#define XY_DEADZONE_UNITS 2

// Max position delta (raw units) for 2a-bis rebind; adaptive, see below.
#define REBIND_MAX_DELTA_DENOM 300
#define REBIND_MIN_DELTA 30

// Palm rejection: unused threshold constants (kept as documentation
// for the scoring function thresholds).
#define PALM_ASPECT_RATIO_THRESHOLD 6
#define PALM_MAJOR_THRESHOLD 300

// Coordinate smoothing (EMA) alpha.
#define SMOOTHING_ALPHA_NUM 3
#define SMOOTHING_ALPHA_DEN 8

#define SLOT_NONE ((UCHAR)PTP_MAX_CONTACT_POINTS)

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT v = raw - minVal;
    if (v < 0)
        v = 0;
    if (v > maxVal)
        v = maxVal;
    return (USHORT)v;
}

static inline USHORT
AmtApplyDeadzone(_In_ USHORT newVal, _Inout_ USHORT *pBaseline)
{
#if XY_DEADZONE_UNITS > 0
    INT delta = (INT)newVal - (INT)(*pBaseline);
    if (delta < 0)
        delta = -delta;
    if (delta < XY_DEADZONE_UNITS)
    {
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
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

static inline BOOLEAN
AmtIsPalm(
    _In_ const struct TRACKPAD_FINGER *f,
    _In_ const struct BCM5974_CONFIG *devInfo,
    _In_ USHORT normX,
    _In_ USHORT normY)
{
    INT score = 0;

    if (f->touch_major >= 380)
    {
        score += 45; // майже гарантована долоня
    }
    else if (f->touch_major > 260)
    {
        score += 25; // великий палець / частина долоні
    }
    else if (f->touch_major > 190)
    {
        score += 10; // середній палець (майже не чіпаємо)
    }

    if (f->touch_minor > 0 && f->touch_major > 120)
    {
        INT ratio = (INT)f->touch_major * 100 / (INT)f->touch_minor;

        if (ratio > 1200)
            score += 18; // плоска долоня
        else if (ratio > 900)
            score += 10;
        else if (ratio > 650)
            score += 4;
    }

    if (f->touch_major > 220)
    {
        INT xRange = devInfo->x.max - devInfo->x.min;
        INT yRange = devInfo->y.max - devInfo->y.min;

        INT edgePctX = xRange / 28; // трохи ширше (було 32)
        INT edgePctY = yRange / 28;

        if (normX < edgePctX ||
            normX > (xRange - edgePctX) ||
            normY < edgePctY ||
            normY > (yRange - edgePctY))
        {
            score += 6;
        }
    }

    return (score >= 60);
}

static inline INT
AmtAbsDelta(_In_ INT a, _In_ INT b)
{
    INT d = a - b;
    return (d < 0) ? -d : d;
}

static inline VOID
AmtNormalizeFinger(
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ const struct TRACKPAD_FINGER *f,
    _Out_ USHORT *pNormX,
    _Out_ USHORT *pNormY)
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

    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
    {
        PSLOT_STATE st = &pCtx->Slots[s];
        BOOLEAN ok = TRUE;

        switch (st->Phase)
        {
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

        if (!ok)
        {
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
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ size_t slot,
    _In_ USHORT normX,
    _In_ USHORT normY,
    _Inout_ PTP_REPORT *PtpReport,
    _In_ UCHAR reportSlots)
{
    PSLOT_STATE st = &pCtx->Slots[slot];
    ULONG contactID;

    contactID = pCtx->NextContactID++;
    st->ContactID = contactID;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! soft tap frame N: slot=%llu normX=%u normY=%u ContactID=%lu",
                (ULONG64)slot, normX, normY, contactID);

    // Emit TipSwitch=1 — contact appears this frame.
    if (reportSlots < PTP_MAX_CONTACT_POINTS)
    {
        PtpReport->Contacts[reportSlots].ContactID = contactID;
        PtpReport->Contacts[reportSlots].X = normX;
        PtpReport->Contacts[reportSlots].Y = normY;
        PtpReport->Contacts[reportSlots].TipSwitch = 1;
        PtpReport->Contacts[reportSlots].Confidence = 1;
        reportSlots++;
    }

    // Transition to SOFT_PENDING_UP — TipSwitch=0 will be emitted next frame.
    // LastNormX/Y carry the position forward for that emission.
    st->Phase = SLOT_SOFT_PENDING_UP;
    st->TipConfirmed = 0;
    st->FingerKey = SLOT_KEY_NONE;
    st->HystX = 0;
    st->HystY = 0;
    st->LastNormX = normX; // preserved for frame N+1 tip-up
    st->LastNormY = normY;

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
VOID AmtPtpProcessTouchFrame(
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ UCHAR *TouchBuffer,
    _In_ size_t raw_n,
    _Inout_ PTP_REPORT *PtpReport,
    _Inout_ UCHAR *pReportSlots)
{
    UCHAR *f_base = TouchBuffer + pCtx->DeviceInfo->tp_header + pCtx->DeviceInfo->tp_delta;
    const struct TRACKPAD_FINGER *f;
    size_t i, s;

    // reportSlots: total entries written to PtpReport->Contacts[].
    //
    // ContactCount = reportSlots (ALL entries, including TipSwitch=0 lifts).
    //
    // The PTP/HID spec says ContactCount tells Windows how many Contacts[]
    // entries to read.  Windows iterates Contacts[0..ContactCount-1] and uses
    // the per-entry TipSwitch bit to determine up/down state.
    // If ContactCount < actual entries written, trailing entries are silently
    // dropped — causing ghosts (lifts never delivered) and missed active
    // contacts (scroll stops, single-tap not recognised).
    //
    // Ordering rule: emit TipSwitch=1 (active) entries BEFORE TipSwitch=0
    // (lift) entries within a single frame.  This ensures that even if a
    // future reader caps at an arbitrary ContactCount it sees active contacts
    // first.  The ordering is enforced structurally: Phase 4/5 (active) runs
    // before the lift-emission pass (Phase 3b).
    UCHAR reportSlots = *pReportSlots;
    UCHAR liftSlots = 0;                         // TipSwitch=0 entries staged below
    PTP_CONTACT liftBuf[PTP_MAX_CONTACT_POINTS]; // lift staging buffer
    RtlZeroMemory(liftBuf, sizeof(liftBuf));

    INT rebindMaxDelta = AmtGetRebindMaxDelta(pCtx);

    // Phase 0: cooldown countdown.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
    {
        PSLOT_STATE st = &pCtx->Slots[s];

        if (st->Phase == SLOT_COOLDOWN)
        {
            st->Cooldown--;
            if (st->Cooldown == 0)
            {
                st->FingerKey = SLOT_KEY_NONE;
                st->Phase = SLOT_FREE;

                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                            "%!FUNC! slot=%llu COOLDOWN -> FREE", (ULONG64)s);
            }
        }
    }

    // Phase 1: per-finger tip-down + normalised coords + array-index key.
    //
    // Palm-lock logic:
    //   - If ANY finger is classified as palm, ALL fingers are rejected
    //     (fingerTipDown[i] = FALSE for every i) and pCtx->PalmDetected is set.
    //   - In the NEXT frame after the last palm lifts, input is still blocked
    //     (drain frame) so existing slot state machines can properly sequence
    //     PENDING_RELEASE->COOLDOWN->FREE.  After that frame PalmDetected is
    //     cleared and normal processing resumes.
    //   - This prevents false touches from non-palm fingers while the user is
    //     resting a palm on the trackpad (e.g., while typing).
    BOOLEAN fingerTipDown[PTP_MAX_CONTACT_POINTS] = {FALSE};
    USHORT fingerNormX[PTP_MAX_CONTACT_POINTS] = {0};
    USHORT fingerNormY[PTP_MAX_CONTACT_POINTS] = {0};
    UCHAR fingerKey[PTP_MAX_CONTACT_POINTS];
    BOOLEAN anyPalmThisFrame = FALSE;

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
        fingerKey[i] = SLOT_KEY_NONE;

    // First pass: classify all fingers.
    for (i = 0; i < raw_n; i++)
    {
        f = (const struct TRACKPAD_FINGER *)(f_base + i * pCtx->DeviceInfo->tp_fsize);

        BOOLEAN tip =
            (AmtRawToInteger(f->touch_major) << 1) >= TIP_MAJOR_THRESHOLD ||
            (AmtRawToInteger(f->touch_minor) << 1) >= TIP_MINOR_THRESHOLD;

        if (!tip)
        {
            fingerTipDown[i] = FALSE;
            continue;
        }

        AmtNormalizeFinger(pCtx, f, &fingerNormX[i], &fingerNormY[i]);

        if (AmtIsPalm(f, pCtx->DeviceInfo, fingerNormX[i], fingerNormY[i]))
        {
            anyPalmThisFrame = TRUE;
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! finger=%d rejected as palm", (INT)i);
            continue;
        }

        fingerTipDown[i] = TRUE;
        fingerKey[i] = (UCHAR)i;
    }

    // Palm-lock gate: once a palm is detected, ALL touch input is
    // suppressed until ALL fingers have been lifted from the surface.
    //
    // Rationale: when a palm is present, the controller's finger data
    // is unreliable — the partially-lifted palm may fail the per-frame
    // scoring classifier but still have a valid tip-down footprint on
    // the sensor.  If we unblock while any contact remains, that contact
    // can inject false gestures (cursor jumps, phantom scrolls, taps).
    //
    // Therefore the lock is released only when a frame arrives with
    // ZERO tip-down contacts — proving that the user has physically
    // removed ALL fingers (including the palm) from the surface.
    if (anyPalmThisFrame)
    {
        pCtx->PalmDetected = TRUE;
        for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
        {
            fingerTipDown[i] = FALSE;
        }
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                    "%!FUNC! Palm detected — all input suppressed");
    }
    else if (pCtx->PalmDetected)
    {
        // Check if ALL physical contacts have been completely lifted
        // from the surface.  We use a raw presence test (any non-zero
        // touch_major or touch_minor) rather than the tip-down threshold,
        // because a partially-lifted palm may still have a light footprint
        // on the sensor below TIP_MAJOR/MINOR_THRESHOLD but should still
        // block input — the user has not yet fully removed the palm.
        BOOLEAN anyContactRemaining = FALSE;
        for (i = 0; i < raw_n; i++)
        {
            f = (const struct TRACKPAD_FINGER *)(f_base + i * pCtx->DeviceInfo->tp_fsize);
            if (AmtRawToInteger(f->touch_major) > 0 ||
                AmtRawToInteger(f->touch_minor) > 0)
            {
                anyContactRemaining = TRUE;
                break;
            }
        }

        if (!anyContactRemaining)
        {
            // All clear: palm and all fingers are gone.  Release lock.
            pCtx->PalmDetected = FALSE;
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! All contacts lifted — palm lock released");
            // fingerTipDown values from the per-finger pass are used as-is.
        }
        else
        {
            // Still has contact on the pad — keep blocking.
            for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
            {
                fingerTipDown[i] = FALSE;
            }
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! Contacts still present — input suppressed");
        }
    }
    // else: normal operation — PalmDetected is FALSE and no palm this frame.
    // fingerTipDown values from the per-finger pass are used as-is.

    // Phase 2: match tip-down fingers to slots.
    UCHAR slotForFinger[PTP_MAX_CONTACT_POINTS];
    UCHAR fingerForSlot[PTP_MAX_CONTACT_POINTS];

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
        slotForFinger[i] = SLOT_NONE;
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
        fingerForSlot[s] = SLOT_NONE;

    // 2a. Key match.
    for (i = 0; i < raw_n; i++)
    {
        if (!fingerTipDown[i])
            continue;
        UCHAR key = fingerKey[i];
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
        {
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
    for (;;)
    {
        UCHAR bestSlot = SLOT_NONE;
        UCHAR bestFinger = SLOT_NONE;
        INT bestDist = rebindMaxDelta + 1;

        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
        {
            PSLOT_STATE st = &pCtx->Slots[s];

            if (fingerForSlot[s] != SLOT_NONE)
                continue;
            if (!(st->Phase == SLOT_ACTIVE || st->Phase == SLOT_CONFIRMING))
                continue;

            for (i = 0; i < raw_n; i++)
            {
                if (!fingerTipDown[i])
                    continue;
                if (slotForFinger[i] != SLOT_NONE)
                    continue;

                INT dx = AmtAbsDelta((INT)fingerNormX[i], (INT)st->LastNormX);
                INT dy = AmtAbsDelta((INT)fingerNormY[i], (INT)st->LastNormY);
                INT dist = dx + dy;

                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestSlot = (UCHAR)s;
                    bestFinger = (UCHAR)i;
                }
            }
        }

        if (bestSlot == SLOT_NONE)
        {
            break;
        }

        slotForFinger[bestFinger] = bestSlot;
        fingerForSlot[bestSlot] = bestFinger;
        pCtx->Slots[bestSlot].FingerKey = fingerKey[bestFinger];

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                    "%!FUNC! slot=%d rebound to finger=%d by position (dist=%d, max=%d)",
                    (INT)bestSlot, (INT)bestFinger, bestDist, rebindMaxDelta);
    }

    // 2b. Assign new slots for unmatched fingers.
    for (i = 0; i < raw_n; i++)
    {
        if (!fingerTipDown[i])
            continue;
        if (slotForFinger[i] != SLOT_NONE)
            continue;

        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
        {
            PSLOT_STATE st = &pCtx->Slots[s];

            if (st->Phase == SLOT_FREE && fingerForSlot[s] == SLOT_NONE)
            {
                slotForFinger[i] = (UCHAR)s;
                fingerForSlot[s] = (UCHAR)i;
                st->FingerKey = fingerKey[i];

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

    // Phase 3a: state transitions for unmatched slots (no lift emission yet).
    // Lift entries are collected into liftBuf and appended AFTER active
    // contacts in Phase 3b, so that Windows always sees TipSwitch=1 entries
    // first regardless of ContactCount.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++)
    {
        if (fingerForSlot[s] != SLOT_NONE)
            continue;

        PSLOT_STATE st = &pCtx->Slots[s];

        switch (st->Phase)
        {

        // ------------------------------------------------------------------
        // SOFT_PENDING_UP: frame N+1 of synthetic soft tap.
        // Stage TipSwitch=0 into liftBuf; append after active contacts.
        // SLOT_SOFT_PENDING_UP slots must NOT be matched by Phase 2a/2a-bis
        // (FingerKey==SLOT_KEY_NONE, Phase not ACTIVE/CONFIRMING — already
        // excluded from matching loops).
        // ------------------------------------------------------------------
        case SLOT_SOFT_PENDING_UP:
        {
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                        "%!FUNC! slot=%llu SOFT_PENDING_UP stage lift (ContactID=%lu)",
                        (ULONG64)s, st->ContactID);

            if (liftSlots < PTP_MAX_CONTACT_POINTS)
            {
                liftBuf[liftSlots].ContactID = st->ContactID;
                liftBuf[liftSlots].X = st->LastNormX;
                liftBuf[liftSlots].Y = st->LastNormY;
                liftBuf[liftSlots].TipSwitch = 0;
                liftBuf[liftSlots].Confidence = 1;
                liftSlots++;
            }
            else
            {
                // Staging buffer full — defer to next frame.
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
                            "%!FUNC! slot=%llu SOFT_PENDING_UP: lift buffer full, deferred",
                            (ULONG64)s);
                break;
            }

            st->Phase = SLOT_COOLDOWN;
            st->Cooldown = 2;
            st->LastNormX = 0;
            st->LastNormY = 0;
            break;
        }

        // ------------------------------------------------------------------
        // PENDING_RELEASE: normal finger lift.
        // Stage TipSwitch=0 into liftBuf; append after active contacts.
        // ------------------------------------------------------------------
        case SLOT_PENDING_RELEASE:
        {
            if (liftSlots < PTP_MAX_CONTACT_POINTS)
            {
                liftBuf[liftSlots].ContactID = st->ContactID;
                liftBuf[liftSlots].X = st->LastNormX;
                liftBuf[liftSlots].Y = st->LastNormY;
                liftBuf[liftSlots].TipSwitch = 0;
                liftBuf[liftSlots].Confidence = 1;
                liftSlots++;
            }
            else
            {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_INPUT,
                            "%!FUNC! slot=%llu PENDING_RELEASE: lift buffer full, deferred",
                            (ULONG64)s);
                break;
            }

            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! slot=%llu PENDING_RELEASE -> COOLDOWN", (ULONG64)s);

            st->Phase = SLOT_COOLDOWN;
            st->Cooldown = 2;
            st->LastNormX = 0;
            st->LastNormY = 0;
            break;
        }

        // ------------------------------------------------------------------
        // ACTIVE: finger left surface — begin release sequence.
        // No emission this frame; lift appears in the NEXT frame when the
        // slot is seen in PENDING_RELEASE (and staged into liftBuf there).
        // ------------------------------------------------------------------
        case SLOT_ACTIVE:
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! slot=%llu ACTIVE -> PENDING_RELEASE", (ULONG64)s);

            st->Phase = SLOT_PENDING_RELEASE;
            st->FingerKey = SLOT_KEY_NONE;
            st->TipConfirmed = 0;
            st->HystX = 0;
            st->HystY = 0;
            break;
        }

        // ------------------------------------------------------------------
        // CONFIRMING (no match): finger lifted before reaching ACTIVE.
        //
        //   TipConfirmed >= 1 → genuine soft tap.
        //     AmtEmitSoftTap writes TipSwitch=1 directly into Contacts[]
        //     (active contact, frame N).  Slot -> SOFT_PENDING_UP.
        //     Frame N+1 (above) stages TipSwitch=0 into liftBuf.
        //
        //   TipConfirmed == 0 → brand-new slot, finger already gone →
        //     almost certainly noise → discard silently, no emission.
        // ------------------------------------------------------------------
        case SLOT_CONFIRMING:
        {
            if (st->TipConfirmed >= 1)
            {
                TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                            "%!FUNC! slot=%llu CONFIRMING -> SOFT_PENDING_UP "
                            "(soft tap frame N, TipConfirmed=%d)",
                            (ULONG64)s, st->TipConfirmed);

                // Emits TipSwitch=1 directly to Contacts[reportSlots].
                // Soft-tap down is an active contact — goes BEFORE lifts.
                reportSlots = AmtEmitSoftTap(
                    pCtx, s,
                    st->LastNormX, st->LastNormY,
                    PtpReport, reportSlots);
            }
            else
            {
                TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                            "%!FUNC! slot=%llu CONFIRMING -> FREE (noise, TipConfirmed=0)",
                            (ULONG64)s);

                st->Phase = SLOT_FREE;
                st->TipConfirmed = 0;
                st->FingerKey = SLOT_KEY_NONE;
                st->LastNormX = 0;
                st->LastNormY = 0;
            }
            break;
        }

        case SLOT_FREE:
        case SLOT_COOLDOWN:
        default:
            break;
        }
    }

    // Phase 4/5: emit active (TipSwitch=1) contacts for matched fingers.
    // These are written BEFORE lift entries (Phase 3b below) so Windows
    // sees active contacts at lower Contacts[] indices.
    for (i = 0; i < raw_n; i++)
    {
        if (!fingerTipDown[i])
            continue;

        UCHAR slot = slotForFinger[i];
        if (slot >= PTP_MAX_CONTACT_POINTS)
            continue;

        PSLOT_STATE st = &pCtx->Slots[slot];

        if (st->TipConfirmed < TIP_CONFIRM_FRAMES)
        {
            st->TipConfirmed++;
        }

        BOOLEAN alreadyActive = (st->Phase == SLOT_ACTIVE);
        BOOLEAN justConfirmed = (!alreadyActive &&
                                 st->TipConfirmed >= TIP_CONFIRM_FRAMES);

        if (!alreadyActive && !justConfirmed)
        {
            // Still CONFIRMING — update position for 2a-bis, no emission.
            st->LastNormX = fingerNormX[i];
            st->LastNormY = fingerNormY[i];
            continue;
        }

        if (justConfirmed)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_INPUT,
                        "%!FUNC! slot=%d CONFIRMING -> ACTIVE (ContactID=%lu)",
                        (INT)slot, pCtx->NextContactID);

            st->Phase = SLOT_ACTIVE;
            st->HystX = fingerNormX[i];
            st->HystY = fingerNormY[i];
            st->SmoothedX = fingerNormX[i];
            st->SmoothedY = fingerNormY[i];
            st->ContactID = pCtx->NextContactID++;
        }

        USHORT afterDeadzoneX = AmtApplyDeadzone(fingerNormX[i], &st->HystX);
        USHORT afterDeadzoneY = AmtApplyDeadzone(fingerNormY[i], &st->HystY);

        USHORT reportX = AmtSmoothCoord(afterDeadzoneX, st->SmoothedX);
        USHORT reportY = AmtSmoothCoord(afterDeadzoneY, st->SmoothedY);

        st->SmoothedX = reportX;
        st->SmoothedY = reportY;

        if (reportSlots < PTP_MAX_CONTACT_POINTS)
        {
            f = (const struct TRACKPAD_FINGER *)(f_base + i * pCtx->DeviceInfo->tp_fsize);
            BOOLEAN confidence = (AmtRawToInteger(f->touch_minor) << 1) > 0;

            PtpReport->Contacts[reportSlots].ContactID = st->ContactID;
            PtpReport->Contacts[reportSlots].X = reportX;
            PtpReport->Contacts[reportSlots].Y = reportY;
            PtpReport->Contacts[reportSlots].TipSwitch = 1;
            PtpReport->Contacts[reportSlots].Confidence = confidence ? 1 : 0;
            reportSlots++;
        }

        st->LastNormX = reportX;
        st->LastNormY = reportY;
    }

    // Phase 3b: append staged lift entries (TipSwitch=0) after active contacts.
    // Ordering guarantee: active contacts (TipSwitch=1) already written above;
    // lift entries follow so Windows processes them in the correct order when
    // it iterates Contacts[0..ContactCount-1].
    for (UCHAR li = 0; li < liftSlots && reportSlots < PTP_MAX_CONTACT_POINTS; li++)
    {
        PtpReport->Contacts[reportSlots] = liftBuf[li];
        reportSlots++;
    }

    // ContactCount = total entries written (TipSwitch=1 AND TipSwitch=0).
    // Windows uses this as the loop bound for Contacts[].  Missing lifts
    // (ContactCount too low) leave ghost contacts; missing actives cause
    // dropped fingers and gestures that stop prematurely.
    PtpReport->ContactCount = reportSlots;

    // Return total entries to caller (used for WdfMemoryCopyFromBuffer sizing).
    *pReportSlots = reportSlots;

    AmtAssertSlotInvariants(pCtx);
}