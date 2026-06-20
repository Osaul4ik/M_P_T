// Match.h - L1 frame parsing and raw-slot -> Track matching.
//
// FIX (separation of concerns): the previous Interrupt.c interleaved
// three distinct responsibilities in one pass over the raw finger array:
//   L1 - decoding raw TRACKPAD_FINGER bytes into normalized coordinates,
//        palm classification, tip-size debounce.
//   L2 - deciding which logical Track a raw slot index continues
//        (the `f->origin == 0` reindex check).
//   L3 - gesture-state bookkeeping (multi-finger taint marking).
// All three were read/mutated through the same shared per-slot arrays
// in the same loop bodies, which is what made the matching logic hard to
// reason about and is the direct cause of the "1-frame ambiguity" this
// rewrite is meant to remove.
//
// This header owns L1 (parsing into MATCH_SAMPLE) and the raw-slot ->
// Track binding decision (AmtMatchFrame). Gesture-session bookkeeping
// stays in Interrupt.c, which is the only place with full frame context
// (alive count across ALL slots, not just one).

#pragma once

#include "Track.h"

EXTERN_C_START

// One decoded, normalized sample from the raw USB frame for a single
// raw slot index. Produced entirely by AmtMatchParseFrame (L1); contains
// no Track-FSM state and no gesture-state.
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

// AmtMatchParseFrame - L1. Decodes raw_n raw TRACKPAD_FINGER records into
// Samples[0..raw_n-1]. Performs palm classification and tip-size
// debounce (which needs each track's last good position, hence the
// Tracks parameter - read-only access for the debounce comparison; no
// Track state is mutated here). Returns the count of slots classified as
// PALM_LARGE-driving-a-full-pad-blank via *LargePalmDetected.
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

// AmtMatchFrame - decides, for each raw slot index that has a Present
// sample this frame, whether it continues the Track currently occupying
// that index (same physical finger) or represents a brand new physical
// finger arriving in that slot.
//
// Two independent signals feed this decision, both checked for any slot
// whose Track is currently ACTIVE:
//
//   1. Samples[i].IdentityBreak (the firmware's own origin==0 signal) -
//      authoritative when present: the hardware is explicitly telling us
//      "this index used to mean something else".
//
//   2. FIX (spatial sanity / defense-in-depth against an undetected
//      reindex): even when origin != 0, a candidate position that has
//      jumped further than MATCH_MAX_CONTINUATION_DELTA from the track's
//      last REPORTED position within a single USB polling interval is
//      not physically plausible for a continuing finger - real fingers
//      do not teleport across the pad in ~8ms. This guards against a
//      firmware edge case where a reindex happens WITHOUT the origin bit
//      being asserted (the entire premise this driver's matching design
//      otherwise trusts blindly). Misclassifying a genuine continuation
//      as NEW_IDENTITY here costs nothing worse than one extra,
//      never-reused ContactID (see Track.h); trusting a bad origin bit
//      blindly risks reintroducing exactly the cursor-teleport class of
//      bug this whole rewrite exists to remove. The threshold is
//      deliberately generous (see Match.c) so it never fires on
//      legitimate fast motion.
//
// This is still, by design, a CONSTANT-TIME check per raw slot (O(1),
// O(N) total for N<=PTP_MAX_CONTACT_POINTS<=5) - it is a bound check
// against ONE track (the one already occupying that raw index), not a
// nearest-neighbour search across all tracks. A real grid/partition-
// based spatial matcher is intentionally NOT implemented: with hardware
// that caps simultaneous contacts at 5 and (per the firmware's own
// index-compaction contract) already guarantees raw-index stability
// except where IdentityBreak says otherwise, an O(N^2) or grid-based
// search would add real complexity for zero measurable benefit. If a
// future hardware revision is found that does NOT honour that contract,
// this function's body - and only this function's body - is where a
// real spatial matcher would go; the MATCH_VERDICT[] output shape is
// already future-proofed for that swap.
typedef enum { MATCH_CONTINUES = 0, MATCH_NEW_IDENTITY = 1 } MATCH_VERDICT;

VOID
AmtMatchFrame(
    _In_reads_(PTP_MAX_CONTACT_POINTS) const MATCH_SAMPLE* Samples,
    _In_  size_t                                            raw_n,
    _In_reads_(PTP_MAX_CONTACT_POINTS) const TRACK*         Tracks,
    _Out_writes_(PTP_MAX_CONTACT_POINTS) MATCH_VERDICT*      Verdicts
);

EXTERN_C_END