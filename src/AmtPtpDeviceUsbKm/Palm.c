// Palm.c - Palm classification. See Palm.h.
//
// Scoring logic copied verbatim from the old AmtClassifyPalm (Match.c) -
// the thresholds here were tuned against real hardware behavior and are
// deliberately NOT touched by this refactor. Only the function boundary
// changed: this now takes raw geometry fields directly instead of a
// TRACKPAD_FINGER pointer, so it has zero dependency on the wire format.

#include "Driver.h"
#include "Palm.h"

#define PALM_LARGE_MAJOR    380
#define PALM_SCORE_THRESH   45

static inline INT
AmtPalmRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormX,
    _In_ INT                          NormY
)
{
    INT major = AmtPalmRawToInteger(Major);
    INT minor = AmtPalmRawToInteger(Minor);
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
        INT xRange   = DevInfo->x.max - DevInfo->x.min;
        INT yRange   = DevInfo->y.max - DevInfo->y.min;
        INT edgePctX = xRange / 28;
        INT edgePctY = yRange / 28;

        if (NormX < edgePctX || NormX > (xRange - edgePctX) ||
            NormY < edgePctY || NormY > (yRange - edgePctY))
            score += 10;
    }

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}