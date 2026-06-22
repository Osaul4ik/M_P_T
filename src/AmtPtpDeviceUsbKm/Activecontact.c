// ActiveContact.c - Contact lifecycle FSM. See ActiveContact.h for design
// rationale (pool position != identity, LastSlotHint vs identity test).

#include "Driver.h"
#include "ActiveContact.h"

#define XY_DEADZONE_UNITS    2
#define SMOOTHING_ALPHA_NUM  5
#define SMOOTHING_ALPHA_DEN  8

static inline USHORT
AmtContactSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal)
{
    INT blended = ((INT)rawVal * SMOOTHING_ALPHA_NUM +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

// ContactID issuance: pre-increment, 0 reserved, never reused while warm.
static inline ULONG
AmtContactAssignId(_Inout_ ULONG* NextContactId)
{
    return ++(*NextContactId);
}

VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool)
{
    RtlZeroMemory(Pool, sizeof(ACTIVE_CONTACT) * MAX_CONTACTS);
}

size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        if (Pool[i].State == CONTACT_FREE)
            return i;
    }
    return MAX_CONTACTS; // pool exhausted (should not happen)
}

VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_FREE);
#endif

    c->State             = CONTACT_ACTIVE;
    c->ContactID          = AmtContactAssignId(NextContactId);
    c->ReportX            = x;
    c->ReportY            = y;
    c->HystX              = x;
    c->HystY              = y;
    c->TipDropCount       = 0;
    c->WasInGesture       = FALSE;
    c->PendingFirstSample = TRUE;
    c->ReportedLastFrame  = FALSE;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0; // set by first AmtContactUpdate call
    c->FramesAlive         = 1; // birth frame counts as 1
}

// Seeds EMA baseline to recent-lift position so cursor doesn't jump on
// re-tap. PendingFirstSample=TRUE so the first AmtContactUpdate reports
// the REAL current finger position for DOWN, then EMA blends from there.
//
// FIX (Issue #3): PendingFirstSample was previously FALSE here, which
// caused the first DOWN report to use stale lift coordinates blended with
// EMA instead of the actual touch-down position. This broke double-tap
// detection: Windows saw DOWN at a slightly wrong position (old lift pos
// blended with new pos), potentially outside its spatial cluster threshold.
//
// With PendingFirstSample=TRUE:
//   - Frame N (DOWN):  reports real finger position (no EMA, no deadzone)
//   - Frame N+1 (MOVE): EMA blends from real pos toward real pos -> smooth
//   - The lift baseline (RecentLift X/Y) is stored in HystX/Y only, used
//     as EMA seed for frame N+1 onward, never as the reported DOWN position.
VOID
AmtContactBirthWithRetapSmoothing(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          RecentLiftX,
    _In_    USHORT          RecentLiftY,
    _In_    USHORT          slotHint
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_FREE);
#endif

    c->State             = CONTACT_ACTIVE;
    c->ContactID          = AmtContactAssignId(NextContactId);
    // Seed EMA baseline with lift position for smooth cursor continuity
    // on subsequent MOVE frames, but report real position on first DOWN.
    c->ReportX            = RecentLiftX;
    c->ReportY            = RecentLiftY;
    c->HystX              = RecentLiftX;
    c->HystY              = RecentLiftY;
    c->TipDropCount       = 0;
    c->WasInGesture       = FALSE;
    // FIX (Issue #3): TRUE so first Update bypasses deadzone+EMA and
    // reports the real finger position for the DOWN event.
    c->PendingFirstSample = TRUE;
    c->ReportedLastFrame  = FALSE;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0;
    c->FramesAlive         = 1;
}

BOOLEAN
AmtContactIsRecentLiftNearby(
    _In_ LONGLONG LiftQpc,
    _In_ USHORT   LiftX,
    _In_ USHORT   LiftY,
    _In_ LONGLONG NowQpc,
    _In_ LONGLONG PerfFrequencyHz,
    _In_ USHORT   CandX,
    _In_ USHORT   CandY
)
{
    if (LiftQpc == 0)
        return FALSE; // no recent lift recorded

    if (NowQpc < LiftQpc)
        return FALSE; // QPC must be monotonic

    if (PerfFrequencyHz <= 0)
        return FALSE; // no usable clock - fail closed

    LONGLONG deltaTicks  = NowQpc - LiftQpc;
    LONGLONG windowTicks = (RETAP_WINDOW_100NS * PerfFrequencyHz) / 10000000LL;

    if (deltaTicks > windowTicks)
        return FALSE;

    INT dx = (INT)CandX - (INT)LiftX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)CandY - (INT)LiftY;
    if (dy < 0) dy = -dy;

    return (dx <= RETAP_MAX_DISTANCE) && (dy <= RETAP_MAX_DISTANCE);
}

VOID
AmtContactKill(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE || c->State == CONTACT_GRACE);
#endif

    *OldContactID = c->ContactID;
    *OldX         = c->ReportX;
    *OldY         = c->ReportY;

    RtlZeroMemory(c, sizeof(ACTIVE_CONTACT));
}

VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE);
#endif

    *OldContactID = c->ContactID;
    *OldX         = c->ReportX;
    *OldY         = c->ReportY;

    c->State = CONTACT_GRACE;
}

VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_GRACE);
#endif

    RtlZeroMemory(c, sizeof(ACTIVE_CONTACT));
}

BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY
)
{
#if XY_DEADZONE_UNITS > 0
    INT dx = (INT)candX - (INT)Contact->HystX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)candY - (INT)Contact->HystY;
    if (dy < 0) dy = -dy;

    return (dx >= XY_DEADZONE_UNITS) || (dy >= XY_DEADZONE_UNITS);
#else
    UNREFERENCED_PARAMETER(Contact);
    UNREFERENCED_PARAMETER(candX);
    UNREFERENCED_PARAMETER(candY);
    return TRUE;
#endif
}

static inline VOID
AmtContactCommitSample(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          candX,
    _In_    USHORT          candY,
    _In_    BOOLEAN         passedDeadzone,
    _In_    BOOLEAN         aliveCountIsOne,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
    USHORT repX, repY;

    if (!passedDeadzone) {
        repX = Contact->ReportX;
        repY = Contact->ReportY;
    } else {
        Contact->HystX = candX;
        Contact->HystY = candY;

        BOOLEAN skipEma = Contact->PendingFirstSample ||
                          (Contact->WasInGesture && aliveCountIsOne);

        if (skipEma) {
            repX = candX;
            repY = candY;

            if (Contact->WasInGesture && aliveCountIsOne) {
                Contact->WasInGesture = FALSE;
            }
        } else {
            repX = AmtContactSmoothCoord(candX, Contact->ReportX);
            repY = AmtContactSmoothCoord(candY, Contact->ReportY);
        }
    }

    Contact->ReportX = repX;
    Contact->ReportY = repY;
    Contact->PendingFirstSample = FALSE;

    *OutX = repX;
    *OutY = repY;
}

VOID
AmtContactUpdate(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          rawX,
    _In_    USHORT          rawY,
    _In_    USHORT          slotHint,
    _In_    LONGLONG        nowQpc,
    _In_    BOOLEAN         aliveCountIsOne,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
#if DBG
    NT_ASSERT(Contact->State == CONTACT_ACTIVE);
#endif

    BOOLEAN passed;

    if (Contact->PendingFirstSample) {
        Contact->HystX = rawX;
        Contact->HystY = rawY;
        passed = TRUE;
    } else {
        passed = AmtContactEvaluateDeadzone(Contact, rawX, rawY);
    }

    AmtContactCommitSample(Contact, rawX, rawY, passed, aliveCountIsOne, OutX, OutY);

    Contact->LastSlotHint = slotHint;
    Contact->LastSeenQpc  = nowQpc;

    if (Contact->FramesAlive < 255)
        Contact->FramesAlive++;
}

#if DBG
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        const ACTIVE_CONTACT* c = &Pool[i];

        if (c->State == CONTACT_FREE) {
            NT_ASSERT(!c->PendingFirstSample);
            NT_ASSERT(!c->WasInGesture);
            NT_ASSERT(c->ContactID == 0);
            continue;
        }

        NT_ASSERT(c->ContactID != 0);

        for (size_t j = i + 1; j < MAX_CONTACTS; j++) {
            const ACTIVE_CONTACT* d = &Pool[j];
            if (d->State == CONTACT_FREE) continue;
            NT_ASSERT(c->ContactID != d->ContactID);
        }
    }
}
#endif