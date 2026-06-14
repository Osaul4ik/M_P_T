// InterruptTouch.c: Per-finger slot tracking state machine.
//
// Lifecycle: FREE -> CONFIRMING -> ACTIVE -> PENDING_RELEASE -> COOLDOWN -> FREE
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
#define XY_DEADZONE_UNITS    2

// Max position delta (raw units) allowed for a 2a-bis rebind. This catches a
// USB-array index swap between two fingers in the same frame, which Phase 2a's
// pure key match cannot — the swap looks like two simultaneous key mismatches
// but the true fingers barely moved.
#define REBIND_MAX_DELTA     30

#define SLOT_NONE  ((UCHAR)PTP_MAX_CONTACT_POINTS)
#define KEY_NONE   ((UCHAR)0xFF)

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

static inline INT
AmtAbsDelta(_In_ INT a, _In_ INT b)
{
    INT d = a - b;
    return (d < 0) ? -d : d;
}

VOID
AmtPtpProcessTouchFrame(
    _In_ PDEVICE_CONTEXT pCtx,
    _In_ UCHAR* TouchBuffer,
    _In_ size_t raw_n,
    _Inout_ PTP_REPORT* PtpReport,
    _Inout_ UCHAR* pReportSlots)
{
    UCHAR* f_base = TouchBuffer + pCtx->DeviceInfo->tp_header + pCtx->DeviceInfo->tp_delta;
    const struct TRACKPAD_FINGER* f;
    size_t i, s;
    UCHAR reportSlots = *pReportSlots;

    // Phase 0: cooldown countdown. LastNormX/Y and HystX/Y are already zero
    // by this point (cleared in Phase 3 the frame the gesture ended), so only
    // SlotFingerKey needs clearing here.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        if (pCtx->SlotCooldown[s] > 0) {
            pCtx->SlotCooldown[s]--;
            if (pCtx->SlotCooldown[s] == 0) {
                pCtx->SlotFingerKey[s] = KEY_NONE;
            }
        }
    }

    // Phase 1: per-finger tip-down + normalised coords + array-index key.
    BOOLEAN fingerTipDown [PTP_MAX_CONTACT_POINTS] = { FALSE };
    USHORT  fingerNormX   [PTP_MAX_CONTACT_POINTS] = { 0 };
    USHORT  fingerNormY   [PTP_MAX_CONTACT_POINTS] = { 0 };
    UCHAR   fingerKey     [PTP_MAX_CONTACT_POINTS];

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++)
        fingerKey[i] = KEY_NONE;

    for (i = 0; i < raw_n; i++) {
        f = (const struct TRACKPAD_FINGER*)(f_base + i * pCtx->DeviceInfo->tp_fsize);

        BOOLEAN tip =
            (AmtRawToInteger(f->touch_major) << 1) >= TIP_MAJOR_THRESHOLD ||
            (AmtRawToInteger(f->touch_minor) << 1) >= TIP_MINOR_THRESHOLD;

        fingerTipDown[i] = tip;
        if (!tip) continue;

        INT xRange = pCtx->DeviceInfo->x.max - pCtx->DeviceInfo->x.min;
        INT yRange = pCtx->DeviceInfo->y.max - pCtx->DeviceInfo->y.min;

        fingerNormX[i] = AmtClampCoord(AmtRawToInteger(f->abs_x), pCtx->DeviceInfo->x.min, xRange);

        INT normY = pCtx->DeviceInfo->y.max - AmtRawToInteger(f->abs_y);
        fingerNormY[i] = AmtClampCoord(normY, 0, yRange);

        fingerKey[i] = (UCHAR)i;
    }

    // Phase 2: match tip-down fingers to slots.
    UCHAR slotForFinger[PTP_MAX_CONTACT_POINTS];
    UCHAR fingerForSlot[PTP_MAX_CONTACT_POINTS];

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) slotForFinger[i] = SLOT_NONE;
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) fingerForSlot[s] = SLOT_NONE;

    // 2a. Match by key.
    for (i = 0; i < raw_n; i++) {
        if (!fingerTipDown[i]) continue;

        UCHAR key = fingerKey[i];
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            if ((pCtx->SlotInUse[s] || pCtx->SlotTipConfirmed[s] > 0) &&
                pCtx->SlotFingerKey[s] == key &&
                fingerForSlot[s] == SLOT_NONE)
            {
                slotForFinger[i] = (UCHAR)s;
                fingerForSlot[s] = (UCHAR)i;
                break;
            }
        }
    }

    // 2a-bis. Rebind by position for slots/fingers left unmatched by 2a.
    // Handles a USB-array index swap between two simultaneously-held
    // fingers: each finger's key no longer matches its old slot, but its
    // position is still close to that slot's last known position.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
        if (fingerForSlot[s] != SLOT_NONE) continue;
        if (!(pCtx->SlotInUse[s] || pCtx->SlotTipConfirmed[s] > 0)) continue;

        UCHAR  bestFinger = SLOT_NONE;
        INT    bestDist   = REBIND_MAX_DELTA + 1;

        for (i = 0; i < raw_n; i++) {
            if (!fingerTipDown[i]) continue;
            if (slotForFinger[i] != SLOT_NONE) continue;

            INT dx = AmtAbsDelta((INT)fingerNormX[i], (INT)pCtx->LastNormX[s]);
            INT dy = AmtAbsDelta((INT)fingerNormY[i], (INT)pCtx->LastNormY[s]);
            INT dist = dx + dy;

            if (dist < bestDist) {
                bestDist   = dist;
                bestFinger = (UCHAR)i;
            }
        }

        if (bestFinger != SLOT_NONE) {
            slotForFinger[bestFinger] = (UCHAR)s;
            fingerForSlot[s]          = bestFinger;
            pCtx->SlotFingerKey[s]    = fingerKey[bestFinger];

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INPUT,
                "%!FUNC! Slot %llu rebound to finger %d by position (dist=%d)",
                (ULONG64)s, (INT)bestFinger, bestDist);
        }
    }

    // 2b. Assign new slots for fingers still unmatched.
    for (i = 0; i < raw_n; i++) {
        if (!fingerTipDown[i]) continue;
        if (slotForFinger[i] != SLOT_NONE) continue;

        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            if (!pCtx->SlotInUse[s] &&
                !pCtx->SlotPendingRelease[s] &&
                !pCtx->SlotCooldown[s] &&
                pCtx->SlotTipConfirmed[s] == 0 &&
                fingerForSlot[s] == SLOT_NONE)
            {
                slotForFinger[i] = (UCHAR)s;
                fingerForSlot[s] = (UCHAR)i;
                pCtx->SlotFingerKey[s] = fingerKey[i];
                break;
            }
        }
    }

    // Phase 3: advance slots with no finger match this frame.
    for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {

        if (fingerForSlot[s] != SLOT_NONE) continue;

        // PENDING_RELEASE -> emit lift, enter COOLDOWN.
        if (pCtx->SlotPendingRelease[s]) {
            if (reportSlots < PTP_MAX_CONTACT_POINTS) {
                PtpReport->Contacts[reportSlots].ContactID  = (UCHAR)s;
                PtpReport->Contacts[reportSlots].X          = pCtx->LastNormX[s];
                PtpReport->Contacts[reportSlots].Y          = pCtx->LastNormY[s];
                PtpReport->Contacts[reportSlots].TipSwitch  = 0;
                PtpReport->Contacts[reportSlots].Confidence = 1;
                reportSlots++;
            }
            pCtx->SlotPendingRelease[s] = FALSE;
            pCtx->SlotCooldown[s]       = 2;

            // Coordinates are scoped to the gesture that just ended.
            pCtx->LastNormX[s] = 0;
            pCtx->LastNormY[s] = 0;
            continue;
        }

        // ACTIVE -> PENDING_RELEASE.
        if (pCtx->SlotInUse[s]) {
            pCtx->SlotInUse[s]          = FALSE;
            pCtx->SlotPendingRelease[s] = TRUE;
            // Clear key immediately so 2a cannot match a rapid re-tap to
            // this slot while it drains; HystX/Y likewise scoped to ACTIVE.
            pCtx->SlotFingerKey[s]    = KEY_NONE;
            pCtx->SlotTipConfirmed[s] = 0;
            pCtx->HystX[s] = 0;
            pCtx->HystY[s] = 0;
            continue;
        }

        // CONFIRMING (no match) -> FREE, no event.
        if (pCtx->SlotTipConfirmed[s] > 0) {
            pCtx->SlotTipConfirmed[s] = 0;
            pCtx->SlotFingerKey[s]    = KEY_NONE;
            continue;
        }
    }

    // Phase 4/5: emit contacts for matched fingers, advance debounce, update
    // hysteresis baseline and LastNormX/Y.
    for (i = 0; i < raw_n; i++) {
        if (!fingerTipDown[i]) continue;

        UCHAR slot = slotForFinger[i];
        if (slot >= PTP_MAX_CONTACT_POINTS) continue;

        if (pCtx->SlotTipConfirmed[slot] < TIP_CONFIRM_FRAMES) {
            pCtx->SlotTipConfirmed[slot]++;
        }

        BOOLEAN alreadyActive = pCtx->SlotInUse[slot];
        BOOLEAN justConfirmed = (!alreadyActive &&
                                  pCtx->SlotTipConfirmed[slot] >= TIP_CONFIRM_FRAMES);

        if (!alreadyActive && !justConfirmed) {
            continue;  // still CONFIRMING, don't emit yet
        }

        if (justConfirmed) {
            pCtx->SlotInUse[slot] = TRUE;
            pCtx->HystX[slot] = fingerNormX[i];
            pCtx->HystY[slot] = fingerNormY[i];
        }

        USHORT reportX = AmtApplyDeadzone(fingerNormX[i], &pCtx->HystX[slot]);
        USHORT reportY = AmtApplyDeadzone(fingerNormY[i], &pCtx->HystY[slot]);

        if (reportSlots < PTP_MAX_CONTACT_POINTS) {
            f = (const struct TRACKPAD_FINGER*)(f_base + i * pCtx->DeviceInfo->tp_fsize);
            BOOLEAN confidence = (AmtRawToInteger(f->touch_minor) << 1) > 0;

            PtpReport->Contacts[reportSlots].ContactID  = slot;
            PtpReport->Contacts[reportSlots].X          = reportX;
            PtpReport->Contacts[reportSlots].Y          = reportY;
            PtpReport->Contacts[reportSlots].TipSwitch  = 1;
            PtpReport->Contacts[reportSlots].Confidence = confidence ? 1 : 0;
            reportSlots++;
        }

        pCtx->LastNormX[slot] = reportX;
        pCtx->LastNormY[slot] = reportY;
    }

    *pReportSlots = reportSlots;
}