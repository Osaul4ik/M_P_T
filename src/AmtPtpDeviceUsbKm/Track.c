VOID
AmtTrackEnterGrace(
    _Inout_ PTRACK  Tracks,
    _In_    size_t  index,
    _In_    LONGLONG NowQpc,
    _Out_   ULONG*  OldContactID,
    _Out_   USHORT* OldX,
    _Out_   USHORT* OldY
)
{
    PTRACK t = &Tracks[index];

#if DBG
    NT_ASSERT(t->State == TRACK_ACTIVE);
#endif

    *OldContactID = t->ContactID;
    *OldX         = t->ReportX;
    *OldY         = t->ReportY;

    // Quarantine rather than fully clear — current policy (see
    // AmtTrackExpireGrace and the TRACK_RETAP_POLICY note in Track.h)
    // never actually re-binds a GRACE track to a later retap, so this
    // state is observable for exactly one instant before the caller
    // (Interrupt.c) immediately calls AmtTrackExpireGrace on it.
    //
    // FIX (closing the "retap continuation" question permanently, not
    // provisionally): a real continuation would mean handing the SAME
    // ContactID to a later touch-down after this lift-off has already
    // been reported with TipSwitch=0. That is in direct conflict with
    // the monotonic-NextContactId invariant in Device.h — "never reuse
    // an ID while it might still be warm in Windows' contact tracking" —
    // which exists specifically to kill the cursor-teleport bug this
    // entire rewrite was built to fix. Reusing an ID across a reported
    // lift, even a few milliseconds later, reopens exactly that bug
    // class: Windows would interpret the new touch-down as a
    // continuation of the just-removed contact and "correct" the
    // cursor toward the new position instead of treating it as a fresh
    // press. GRACE therefore stays a pure quarantine, permanently, not
    // a held reservation — it exists only so a diagnostics/invariant
    // pass has an explicit state to assert against instead of inferring
    // "this was a gesture lift" from a side boolean. Any future
    // continuation feature would need a DIFFERENT signal to Windows
    // (e.g. a position correlation hint outside ContactID reuse), not
    // a change to this function.
    t->State = TRACK_GRACE;
    UNREFERENCED_PARAMETER(NowQpc);
}