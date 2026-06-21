// Input.h - InputAdapter: raw USB/Wellspring packet -> RAW_FRAME.
//
// Stateless by design. Does not read or write TRACK state. Does not
// classify palm. Does not debounce. Pure decode + coordinate
// normalization. See PTPCore.h for the full layer contract and the
// rationale for keeping this layer genuinely stateless.

#pragma once

#include "PTPCore.h"

EXTERN_C_START

// Decodes raw TRACKPAD_FINGER records into a RAW_FRAME. Drops fingers
// with major<=0 && minor<=0 (no contact) - does not attempt tip-size
// debounce, since that requires track history. Sets RawFrame->Contacts[].SlotIndex
// to the raw firmware slot index for every entry it emits.
VOID
AmtInputParseFrame(
    _In_  const UCHAR*                 FrameBase,
    _In_  size_t                       FingerSize,
    _In_  size_t                       RawContactCount,
    _In_  const struct BCM5974_CONFIG* DevInfo,
    _In_  LONGLONG                     TimestampQpc,
    _Out_ PRAW_FRAME                   OutFrame
);

EXTERN_C_END