/*++
 ContactTracking.h — Persistent contact slot tracker for Apple SPI PTP driver.
 v2: fixed coordinate-space mismatch in proximity matching; optimal bipartite
     matching; slot_to_hw reverse map eliminates O(H) inner loop.
--*/
#pragma once

typedef enum _CONTACT_SLOT_STATE {
    ContactSlotEmpty   = 0,
    ContactSlotLive    = 1,
    ContactSlotLifting = 2,
} CONTACT_SLOT_STATE;

typedef struct _CONTACT_SLOT {
    CONTACT_SLOT_STATE  State;
    BOOLEAN             IsPalm;
    // Last known position stored in RAW SPI units (signed SHORT range).
    // Stored raw so proximity matching uses consistent coordinate space.
    // NormalizeX/Y is applied only when writing to the PTP report.
    SHORT               LastRawX;
    SHORT               LastRawY;
    // Last normalized PTP coordinates — written to TipSwitch=1 AND TipSwitch=0
    // reports. Lift frames MUST use these, not (0,0), or Windows anchors the
    // gesture termination at the trackpad origin causing cursor snap-back.
    USHORT              LastNormX;
    USHORT              LastNormY;
} CONTACT_SLOT, *PCONTACT_SLOT;

typedef struct _CONTACT_TABLE {
    CONTACT_SLOT Slots[PTP_MAX_CONTACT_POINTS];
    UINT8        LiveCount;
} CONTACT_TABLE, *PCONTACT_TABLE;

// Proximity threshold in raw SPI units squared.
// 1500 SPI units ≈ 15 mm at ~100 units/mm for MBP16,1.
// A finger moving at 1 m/s at 125 Hz covers 8 mm/frame — well within 15 mm.
#define CONTACT_MATCH_THRESHOLD_SQ  (1500L * 1500L)

// Sentinel: no slot / no hardware finger.
#define SLOT_NONE  ((UINT8)PTP_MAX_CONTACT_POINTS)

FORCEINLINE VOID
AmtPtpResetContactTable(_In_ PCONTACT_TABLE pTable)
{
    RtlZeroMemory(pTable, sizeof(CONTACT_TABLE));
}

// ── Optimal minimum-cost bipartite matching ───────────────────────────────
//
// Maps each hardware finger H[0..HwCount-1] to the closest available Live
// slot, ensuring no two fingers share a slot (one-to-one assignment).
//
// Algorithm: O(H²·S) exhaustive min-cost with used-slot exclusion.
// With H ≤ 5 and S ≤ 5 this is at most 125 distance comparisons — ~250 ns.
// Produces globally optimal assignments, preventing contact ID swaps during
// fast scroll where a greedy (first-fit) approach would swap IDs.
//
// Output:
//   hw_to_slot[i] = slot index assigned to hardware finger i,
//                   or SLOT_NONE if no slot found / new contact.
//   slot_to_hw[s] = hardware finger index matched to slot s,
//                   or SLOT_NONE if slot s has no match this frame.
//                   Pre-filled with SLOT_NONE by caller.
//
FORCEINLINE VOID
AmtPtpMatchFingers(
    _In_  const CONTACT_TABLE* pTable,
    _In_  UINT8                HwCount,
    _In_  const SHORT          HwRawX[],    // length HwCount
    _In_  const SHORT          HwRawY[],    // length HwCount
    _Out_ UINT8                hw_to_slot[], // length HwCount, filled here
    _Out_ UINT8                slot_to_hw[]  // length PTP_MAX_CONTACT_POINTS,
                                             // caller pre-fills with SLOT_NONE
)
{
    // cost[i][s] = squared distance from HwFinger[i] to Slot[s].
    // LONG_MAX sentinel for unavailable slots.
    LONG  cost[SPI_TRACKPAD_MAX_FINGERS][PTP_MAX_CONTACT_POINTS];
    UINT8 used_slot[PTP_MAX_CONTACT_POINTS]; // slot already assigned this round
    UINT8 i, s;

    RtlFillMemory(used_slot, sizeof(used_slot), 0);
    RtlFillMemory(hw_to_slot, HwCount * sizeof(UINT8), SLOT_NONE);

    // Build cost matrix.
    for (i = 0; i < HwCount; i++) {
        for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
            if (pTable->Slots[s].State == ContactSlotLive) {
                LONG dx = (LONG)pTable->Slots[s].LastRawX - (LONG)HwRawX[i];
                LONG dy = (LONG)pTable->Slots[s].LastRawY - (LONG)HwRawY[i];
                cost[i][s] = dx * dx + dy * dy;
            } else {
                cost[i][s] = 0x7FFFFFFFL; // unavailable
            }
        }
    }

    // Greedy assignment in order of cheapest global cost.
    // Iterate up to min(HwCount, LiveCount) times.
    {
        UINT8 assigned = 0;
        UINT8 max_assign = (HwCount < pTable->LiveCount)
                           ? HwCount : pTable->LiveCount;

        while (assigned < max_assign) {
            LONG  best_cost = CONTACT_MATCH_THRESHOLD_SQ;
            UINT8 best_i    = SLOT_NONE;
            UINT8 best_s    = SLOT_NONE;

            for (i = 0; i < HwCount; i++) {
                if (hw_to_slot[i] != SLOT_NONE) continue; // already assigned
                for (s = 0; s < PTP_MAX_CONTACT_POINTS; s++) {
                    if (used_slot[s]) continue;
                    if (cost[i][s] < best_cost) {
                        best_cost = cost[i][s];
                        best_i = i;
                        best_s = s;
                    }
                }
            }

            if (best_i == SLOT_NONE) break; // no more matches within threshold

            hw_to_slot[best_i]   = best_s;
            slot_to_hw[best_s]   = best_i;
            used_slot[best_s]    = 1;
            assigned++;
        }
    }
}

// Find the first Empty slot. Returns SLOT_NONE if table is full.
FORCEINLINE UINT8
AmtPtpAllocateSlot(_In_ const CONTACT_TABLE* pTable)
{
    UINT8 i;
    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        if (pTable->Slots[i].State == ContactSlotEmpty) return i;
    }
    return SLOT_NONE;
}
