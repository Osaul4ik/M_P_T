// Input.c - InputAdapter implementation. See Input.h for the contract.
//
// Extracted from the old AmtMatchParseFrame (Match.c). That function
// mixed three concerns: geometry decode, palm classification, and
// tip-size debounce. This file keeps ONLY geometry decode + coordinate
// normalization - the part that genuinely has no state. Palm
// classification moved to Palm.c. Tip-size debounce moved into PTPCore
// (Match.c L1.5 / Track.c), since it requires reading previous track
// position - see PTPCore.h note #2 for why that can't live here.

#include "Driver.h"
#include "Input.h"

static inline INT
AmtInputRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

static inline USHORT
AmtInputClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT shifted = raw - minVal;
    if (shifted < 0)               shifted = 0;
    if (shifted > maxVal - minVal) shifted = maxVal - minVal;
    return (USHORT)shifted;
}

VOID
AmtInputParseFrame(
    _In_  const UCHAR*                 FrameBase,
    _In_  size_t                       FingerSize,
    _In_  size_t                       RawContactCount,
    _In_  const struct BCM5974_CONFIG* DevInfo,
    _In_  LONGLONG                     TimestampQpc,
    _Out_ PRAW_FRAME                   OutFrame
)
{
    RtlZeroMemory(OutFrame, sizeof(RAW_FRAME));
    OutFrame->TimestampQpc = TimestampQpc;

    if (RawContactCount > PTP_MAX_CONTACT_POINTS)
        RawContactCount = PTP_MAX_CONTACT_POINTS;

    UCHAR emitted = 0;

    for (size_t i = 0; i < RawContactCount; i++) {
        const struct TRACKPAD_FINGER* f =
            (const struct TRACKPAD_FINGER*)(FrameBase + i * FingerSize);

        INT major = AmtInputRawToInteger(f->touch_major);
        INT minor = AmtInputRawToInteger(f->touch_minor);

        // No contact at all - InputAdapter does not debounce this; a
        // downstream layer with track history may choose to bridge it.
        if (major <= 0 && minor <= 0)
            continue;

        INT nx = (INT)AmtInputClampCoord(
            AmtInputRawToInteger(f->abs_x), DevInfo->x.min, DevInfo->x.max);

        INT yRange = DevInfo->y.max - DevInfo->y.min;
        INT nyRaw  = DevInfo->y.max - AmtInputRawToInteger(f->abs_y);
        INT ny     = (nyRaw < 0) ? 0 : (nyRaw > yRange ? yRange : nyRaw);

        PRAW_CONTACT rc = &OutFrame->Contacts[emitted];
        rc->SlotIndex = (USHORT)i;
        rc->X         = (USHORT)nx;
        rc->Y         = (USHORT)ny;
        rc->Major     = (USHORT)major;
        rc->Minor     = (USHORT)minor;
        rc->Origin    = (UCHAR)f->origin;
        emitted++;
    }

    OutFrame->ContactCount = emitted;
}