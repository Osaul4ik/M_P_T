// Match.h - L1 frame parsing and raw-slot -> Track matching.
//
// Owns L1 (parsing into MATCH_SAMPLE) and binding decisions
// (AmtMatchFrame). Gesture-session bookkeeping stays in Interrupt.c.

#pragma once

#include "Track.h"

EXTERN_C_START

// One decoded, normalized sample from a raw USB frame slot (L1).
typedef struct _MATCH_SAMPLE
{
    BOOLEAN Present;       // FALSE if this raw slot reported no contact
                           // (touch_major <= 0 && touch_minor <= 0) and
                           // was not kept alive by tip-drop debounce.
    BOOLEAN PalmLocal;     // Locally palm-classified; excluded from
                           // matching/reporting but does not blank the pad.
    BOOLEAN IdentityBreak; // TRUE if firmware signalled (origin == 0) that
                           // this raw slot index now represents a
                           // DIFFERENT physical finger than last frame.
    USHORT  X;
    USHORT  Y;
    UCHAR   TipDropApplied; // Non-zero if this sample's position was
                            // substituted from the previous good position
                            // by tip-size debounce rather than measured
                            // fresh this frame (feeds Confidence).
} MATCH_SAMPLE;

// L1: Decodes raw TRACKPAD_FINGER records into MATCH_SAMPLE[].
// Performs palm classification and tip-size debounce (read-only Track
// access). Sets *LargePalmDetected for full-pad palm blanking.
VOID
AmtMatchParseFrame(
    _In_  const UCHAR*                    FrameBase,
    _In_  size_t                          fingerSize,
    _In_  size_t                          raw_n,
    _In_  const struct BCM5974_CONFIG*    DevInfo,
    _In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK* Tracks,
    _Out_writes_(PTP_MAX_CONTACT_POINTS) MATCH_SAMPLE* Samples,
    _Out_ BOOLEAN* LargePalmDetected
);

// Decides CONTINUES vs NEW_IDENTITY for each Present slot.
// Two signals: (1) firmware origin==0 (IdentityBreak), (2) spatial
// sanity check — a position jump beyond MATCH_MAX_CONTINUATION_DELTA
// from last ReportX/Y is implausible in one USB polling interval.
// O(N) single-pass, N <= PTP_MAX_CONTACT_POINTS.
typedef enum { MATCH_CONTINUES = 0, MATCH_NEW_IDENTITY = 1 } MATCH_VERDICT;

VOID
AmtMatchFrame(
    _In_reads_(PTP_MAX_CONTACT_POINTS) const MATCH_SAMPLE* Samples,
    _In_  size_t                                            raw_n,
    _In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK*         Tracks,
    _Out_writes_(PTP_MAX_CONTACT_POINTS) MATCH_VERDICT*      Verdicts
);

EXTERN_C_END