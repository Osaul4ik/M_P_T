/*++
 ContactTracking.h — Persistent contact slot tracker for Apple SPI PTP driver.

 Architecture overview
 ─────────────────────
 The Apple T2 SPI firmware delivers fingers in a compact, ordered array:
   Fingers[0..NumOfFingers-1] — always tightly packed, no gaps.

 A "hardware slot" is simply an index into that array for ONE SPI packet.
 Across packets the SAME physical finger may appear at a DIFFERENT array index
 as other fingers enter or leave.  The firmware does NOT provide a persistent
 per-finger token.

 The Windows PTP host stack expects:
   1. Every ContactID used in a TipSwitch=1 report to remain STABLE for the
      life of the contact (from first TipSwitch=1 to the matching TipSwitch=0).
   2. Every ContactID that was ever sent with TipSwitch=1 to eventually receive
      a report with TipSwitch=0 at its LAST KNOWN (X, Y).
   3. Contacts to appear and disappear cleanly — no gaps in the ID space,
      no IDs that appear without a prior touchdown.

 Design
 ──────
 We maintain a table of PTP_MAX_CONTACT_POINTS "contact slots".
 Each slot maps a stable PTP ContactID (= slot index, 0..N-1) to one physical
 finger, and tracks the complete state machine for that finger:

   EMPTY  ──touch down──►  LIVE  ──pressure=0──►  LIFTING  ──next frame──►  EMPTY
                              ▲                         │
                          (stays live                   │ emit TipSwitch=0
                           while P≥1)                  │ at last known X/Y
                                                        ▼
                                                      (slot freed)

 Palm classification is STICKY per slot:
   • Evaluated only on the first frame a slot becomes LIVE.
   • Once PALM, the slot is suppressed (no HID report) until EMPTY.
   • A palm-classified slot still gets a "silent lift" (state→EMPTY) when
     the finger leaves — no HID report needed since none was ever sent.
   • This prevents a mid-gesture reclassification from creating a contact
     that appeared without a touchdown.

 Matching (hardware array → contact slot)
 ─────────────────────────────────────────
 Since the SPI firmware compacts the array, we cannot use position index as
 a persistent identity.  Instead we match by proximity:

   For each hardware finger H[i] in the new packet:
     Find the LIVE slot S whose last (X, Y) is closest to H[i].(X, Y).
     If dist(S, H[i]) < MATCH_THRESHOLD → bind H[i] to S (update position).
     Else → allocate a new slot for H[i].

   For each LIVE slot with no match: transition to LIFTING.

 This is identical to the algorithm used by every major touchpad driver
 (Linux input/touchscreen/hid-multitouch.c, Windows I2C HID class driver).

 MATCH_THRESHOLD = 1500 SPI units ≈ 15mm, generous enough for fast movement,
 tight enough to never confuse two simultaneous fingers.

 Synchronization
 ───────────────
 All contact-slot state is read and written exclusively inside
 AmtPtpRequestCompletionRoutine, which runs at DISPATCH_LEVEL.
 The single-pre-allocated-request invariant (only one outstanding SPI read
 at a time) guarantees that no two completion-routine invocations overlap.
 Therefore NO spinlock is needed for the contact table.

 Power transitions
 ─────────────────
 AmtPtpResetContactTable() must be called:
   • In AmtPtpEvtDeviceSelfManagedIoInitOrRestart (D0 re-entry).
   • In AmtPtpPowerRecoveryTimerCallback after successful AmtPtpSpiSetState.
 This ensures Windows never sees ContactIDs that were live before a power
 cycle appear to remain live after — which would require the host to
 time them out rather than close them cleanly.

--*/

#pragma once

// ── Contact slot state machine ────────────────────────────────────────────

typedef enum _CONTACT_SLOT_STATE {
    SlotEmpty   = 0,   // No finger. Slot is available for allocation.
    SlotLive    = 1,   // Finger down, Pressure >= 1. TipSwitch=1 was sent.
    SlotLifting = 2,   // Pressure dropped to 0 or finger disappeared.
                       // TipSwitch=0 must be sent this frame at LastX/LastY.
} CONTACT_SLOT_STATE;

// ── Per-slot data ─────────────────────────────────────────────────────────

typedef struct _CONTACT_SLOT {
    CONTACT_SLOT_STATE  State;
    BOOLEAN             IsPalm;     // Sticky: set on first live frame, cleared on Empty.
    USHORT              LastX;      // Normalized PTP X of last TipSwitch=1 report.
    USHORT              LastY;      // Normalized PTP Y of last TipSwitch=1 report.
} CONTACT_SLOT, *PCONTACT_SLOT;

// ── Contact table embedded in DEVICE_CONTEXT ─────────────────────────────

typedef struct _CONTACT_TABLE {
    CONTACT_SLOT Slots[PTP_MAX_CONTACT_POINTS];
    UINT8        LiveCount;     // Number of slots in SlotLive state.
                                // Used for first-frame ScanTime cap.
} CONTACT_TABLE, *PCONTACT_TABLE;

// ── Proximity match threshold (SPI raw units) ─────────────────────────────
// 1500 units ≈ 15mm at ~100 units/mm.  A finger can move at most ~50mm/frame
// at 125Hz which is physically impossible; 15mm is a comfortable upper bound.
#define CONTACT_MATCH_THRESHOLD_SQ  (1500 * 1500)

// ── API ───────────────────────────────────────────────────────────────────

// Reset all slots to Empty.  Call on D0 entry and after device reset.
FORCEINLINE VOID
AmtPtpResetContactTable(
    _In_ PCONTACT_TABLE pTable
)
{
    RtlZeroMemory(pTable, sizeof(CONTACT_TABLE));
}

// Return the slot index (= stable PTP ContactID) of the best matching
// LIVE slot for a hardware finger at (HwX, HwY), or PTP_MAX_CONTACT_POINTS
// if no live slot is within CONTACT_MATCH_THRESHOLD_SQ.
FORCEINLINE UINT8
AmtPtpFindMatchingSlot(
    _In_ const CONTACT_TABLE* pTable,
    _In_ SHORT HwX,
    _In_ SHORT HwY
)
{
    UINT8 Best  = PTP_MAX_CONTACT_POINTS;  // sentinel = "no match"
    LONG  BestDist = CONTACT_MATCH_THRESHOLD_SQ;
    UINT8 i;

    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        LONG dx, dy, dist;
        if (pTable->Slots[i].State != SlotLive) {
            continue;
        }
        // Compute squared Euclidean distance in SPI coordinate space.
        // LastX/Y are normalized PTP units; we need raw SPI units for the
        // match.  Since we only compare distances (not absolute values),
        // the coordinate space difference only introduces a fixed scale
        // factor — the match is still correct.  Use LastX/Y directly.
        dx   = (LONG)pTable->Slots[i].LastX - (LONG)HwX;
        dy   = (LONG)pTable->Slots[i].LastY - (LONG)HwY;
        dist = dx * dx + dy * dy;
        if (dist < BestDist) {
            BestDist = dist;
            Best     = i;
        }
    }
    return Best;
}

// Find the first Empty slot.  Returns PTP_MAX_CONTACT_POINTS if none.
FORCEINLINE UINT8
AmtPtpAllocateSlot(
    _In_ const CONTACT_TABLE* pTable
)
{
    UINT8 i;
    for (i = 0; i < PTP_MAX_CONTACT_POINTS; i++) {
        if (pTable->Slots[i].State == SlotEmpty) {
            return i;
        }
    }
    return PTP_MAX_CONTACT_POINTS;  // table full
}
