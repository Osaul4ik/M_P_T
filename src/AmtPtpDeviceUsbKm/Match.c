// Match.c - L1 frame parsing and raw-slot -> Track matching.
// See Match.h for the responsibility split this enforces.

#include "Driver.h"
#include "Match.h"

#define PALM_LARGE_MAJOR    380
#define PALM_SCORE_THRESH   45
#define TIP_DROP_DEBOUNCE_FRAMES 2
#define TIP_DROP_MAX_REPOSITION_DELTA 300

// FIX (spatial sanity check on continuation - see the long comment on
// AmtMatchFrame in Match.h): 4000 normalized units is ~20% of this
// hardware's full coordinate range (device x-range is ~20000 units, see
// AppleDefinition.h). At the USB polling rate this driver runs at, no
// real finger crosses 20% of the pad's width in a single frame even
// during a fast swipe - this threshold exists to catch an implausible
// teleport (a different finger's position appearing under a track that
// firmware did NOT flag as reindexed), not to constrain normal motion.
// Deliberately generous so it can never misfire on legitimate input.
#define MATCH_MAX_CONTINUATION_DELTA 4000

static inline INT
AmtRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

static inline USHORT
AmtClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT shifted = raw - minVal;
    if (shifted < 0)               shifted = 0;
    if (shifted > maxVal - minVal) shifted = maxVal - minVal;
    return (USHORT)shifted;
}

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

static PALM_CLASS
AmtClassifyPalm(
    _In_ const struct TRACKPAD_FINGER* f,
    _In_ const struct BCM5974_CONFIG*  devInfo,
    _In_ INT normX,
    _In_ INT normY)
{
    INT major = AmtRawToInteger(f->touch_major);
    INT minor = AmtRawToInteger(f->touch_minor);
    INT score = 0;

    if (major <= 0 && minor <= 0)
        return PALM_NONE;

    if (major >= PALM_LARGE_MAJOR)
        return PALM_LARGE;

    if      (major > 260) score += 35;
    else if (major > 190) score += 15;
    else if (major > 130) score +=  8;

    if (minor > 0 && major > 120) {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

    if (major > 130) {
        INT xRange   = devInfo->x.max - devInfo->x.min;
        INT yRange   = devInfo->y.max - devInfo->y.min;
        INT edgePctX = xRange / 28;
        INT edgePctY = yRange / 28;

        if (normX < edgePctX || normX > (xRange - edgePctX) ||
            normY < edgePctY || normY > (yRange - edgePctY))
            score += 10;
    }

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}

VOID
AmtMatchParseFrame(
    _In_  const UCHAR*                    FrameBase,
    _In_  size_t                          fingerSize,
    _In_  size_t                          raw_n,
    _In_  const struct BCM5974_CONFIG*    DevInfo,
    _In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK* Tracks,
    _Out_writes_(PTP_MAX_CONTACT_POINTS) MATCH_SAMPLE* Samples,
    _Out_ BOOLEAN* LargePalmDetected)
{
    *LargePalmDetected = FALSE;

    for (size_t i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        RtlZeroMemory(&Samples[i], sizeof(MATCH_SAMPLE));
    }

    for (size_t i = 0; i < raw_n; i++) {
        const struct TRACKPAD_FINGER* f =
            (const struct TRACKPAD_FINGER*)(FrameBase + i * fingerSize);

        INT major = AmtRawToInteger(f->touch_major);
        INT minor = AmtRawToInteger(f->touch_minor);

        Samples[i].IdentityBreak = (f->origin == 0);

        if (major <= 0 && minor <= 0) {
            // No contact at all this frame - tip-drop debounce never
            // applies here (that's for borderline-small-but-present
            // contacts, not absent ones).
            continue;
        }

        INT nx = (INT)AmtClampCoord(AmtRawToInteger(f->abs_x),
                                    DevInfo->x.min, DevInfo->x.max);
        INT yRange = DevInfo->y.max - DevInfo->y.min;
        INT nyRaw  = DevInfo->y.max - AmtRawToInteger(f->abs_y);
        INT ny     = (nyRaw < 0) ? 0 : (nyRaw > yRange ? yRange : nyRaw);

        PALM_CLASS palm = AmtClassifyPalm(f, DevInfo, nx, ny);

        if (palm == PALM_LARGE) {
            *LargePalmDetected = TRUE;
            // Caller blanks the whole pad on large-palm; no point
            // continuing to parse the rest of this frame's slots.
            for (size_t j = 0; j < PTP_MAX_CONTACT_POINTS; j++) {
                RtlZeroMemory(&Samples[j], sizeof(MATCH_SAMPLE));
            }
            return;
        }

        if (palm == PALM_LOCAL) {
            Samples[i].PalmLocal = TRUE;
            continue;
        }

        BOOLEAN tip = (major << 1) >= 200 || (minor << 1) >= 150;

        if (!tip) {
            // Borderline-small contact. Tip-drop debounce: if this track
            // is currently ACTIVE and the candidate position is close to
            // its last reported position, keep it alive on that last
            // position for a short number of frames rather than treating
            // a single weak sample as a lift-off.
            const TRACK* t = &Tracks[i];

            if (t->State == TRACK_ACTIVE) {
                INT dxAbs = nx - (INT)t->ReportX;
                if (dxAbs < 0) dxAbs = -dxAbs;
                INT dyAbs = ny - (INT)t->ReportY;
                if (dyAbs < 0) dyAbs = -dyAbs;

                BOOLEAN samePositionAsBefore =
                    (dxAbs <= TIP_DROP_MAX_REPOSITION_DELTA) &&
                    (dyAbs <= TIP_DROP_MAX_REPOSITION_DELTA);

                if (samePositionAsBefore &&
                    t->TipDropCount < TIP_DROP_DEBOUNCE_FRAMES) {
                    Samples[i].Present        = TRUE;
                    Samples[i].X              = t->ReportX;
                    Samples[i].Y              = t->ReportY;
                    Samples[i].TipDropApplied = (UCHAR)(t->TipDropCount + 1);
                    continue;
                }
            }
            // Debounce window exhausted (or track wasn't active): treat
            // as genuinely absent this frame.
            continue;
        }

        Samples[i].Present = TRUE;
        Samples[i].X       = (USHORT)nx;
        Samples[i].Y       = (USHORT)ny;
    }
}

VOID
AmtMatchFrame(
    _In_reads_(PTP_MAX_CONTACT_POINTS) const MATCH_SAMPLE* Samples,
    _In_  size_t                                            raw_n,
    _In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK*         Tracks,
    _Out_writes_(PTP_MAX_CONTACT_POINTS) MATCH_VERDICT*      Verdicts)
{
    // See the long comment in Match.h: matching is by raw slot index in
    // the current hardware model (the firmware already guarantees index
    // stability except where it explicitly signals a break via
    // origin==0, OR where the spatial sanity check below catches an
    // undetected one). O(N) single pass, N <= PTP_MAX_CONTACT_POINTS ==
    // 5 - no O(N^2) search and no grid/partition structure needed at
    // this scale; this function exists as the single, isolated place
    // that decision is made, per the L1/matching/gesture separation this
    // header establishes.
    for (size_t i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        Verdicts[i] = MATCH_CONTINUES;
    }

    for (size_t i = 0; i < raw_n; i++) {
        if (!Samples[i].Present || Samples[i].PalmLocal)
            continue;

        if (Tracks[i].State != TRACK_ACTIVE) {
            Verdicts[i] = MATCH_CONTINUES; // no existing identity to break
            continue;
        }

        BOOLEAN identityChanged;

        if (Samples[i].IdentityBreak) {
            // Authoritative signal straight from firmware: this index
            // now belongs to a different physical finger.
            identityChanged = TRUE;
        } else {
            // FIX (spatial sanity / defense-in-depth - see Match.h):
            // origin != 0 claims continuity, but verify it's physically
            // plausible against the track's last REPORTED position
            // before trusting it. Compared against ReportX/Y (the
            // post-EMA value Windows last saw), not HystX/Y, since that
            // is what a real teleport would visibly jump away from.
            INT dx = (INT)Samples[i].X - (INT)Tracks[i].ReportX;
            if (dx < 0) dx = -dx;
            INT dy = (INT)Samples[i].Y - (INT)Tracks[i].ReportY;
            if (dy < 0) dy = -dy;

            identityChanged = (dx > MATCH_MAX_CONTINUATION_DELTA) ||
                               (dy > MATCH_MAX_CONTINUATION_DELTA);
        }

        Verdicts[i] = identityChanged ? MATCH_NEW_IDENTITY : MATCH_CONTINUES;
    }
}