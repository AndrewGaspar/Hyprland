#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// parseIdleInhibitMode / wantXRIdleInhibit (XRMonitorConfig.cpp) are the pure idle-inhibit policy
// behind research/20 phase 2: openxr:inhibit_idle widened from a bool to off|focused|present.
// COpenXRManager::shouldInhibitIdle() supplies the live facts and does nothing else, and
// CInputManager::recheckIdleInhibitorStatus() is the sole writer of the inhibit bit — so this truth
// table IS the specification.
//
// COVERAGE NOTE (honesty, mirroring tests/xr/plugged.cpp): the hyprtester XR suite runs against the
// monado null/remote driver, which cannot script headset don/doff, so a real presence-driven
// engage/release cannot be exercised end-to-end there. The suite case (hyprtester xr_idle_inhibit)
// covers mode PARSING live over hyprctl plus the no-presence FALLBACK (the null runtime reports
// presence `unsupported`, so `present` must behave exactly like `focused` there); every
// presence-supported row below is covered here and only here.

// ---- parse: new mode spellings + legacy boolean compat + default ----

TEST(XRIdleInhibitParse, NewSpellings) {
    EXPECT_EQ(parseIdleInhibitMode("off"), XR_INHIBIT_OFF);
    EXPECT_EQ(parseIdleInhibitMode("focused"), XR_INHIBIT_FOCUSED);
    EXPECT_EQ(parseIdleInhibitMode("present"), XR_INHIBIT_PRESENT);
}

TEST(XRIdleInhibitParse, LegacyBooleanCompat) {
    // The load-bearing compat decision: an EXISTING explicit `inhibit_idle = true` keeps its exact
    // old semantics (FOCUSED). It must NOT be silently widened to the new default.
    EXPECT_EQ(parseIdleInhibitMode("true"), XR_INHIBIT_FOCUSED);
    EXPECT_EQ(parseIdleInhibitMode("1"), XR_INHIBIT_FOCUSED);
    EXPECT_EQ(parseIdleInhibitMode("yes"), XR_INHIBIT_FOCUSED);
    EXPECT_EQ(parseIdleInhibitMode("on"), XR_INHIBIT_FOCUSED);

    EXPECT_EQ(parseIdleInhibitMode("false"), XR_INHIBIT_OFF);
    EXPECT_EQ(parseIdleInhibitMode("0"), XR_INHIBIT_OFF);
    EXPECT_EQ(parseIdleInhibitMode("no"), XR_INHIBIT_OFF);
    EXPECT_EQ(parseIdleInhibitMode("none"), XR_INHIBIT_OFF);
}

TEST(XRIdleInhibitParse, Aliases) {
    EXPECT_EQ(parseIdleInhibitMode("focus"), XR_INHIBIT_FOCUSED);
    EXPECT_EQ(parseIdleInhibitMode("worn"), XR_INHIBIT_PRESENT);
    EXPECT_EQ(parseIdleInhibitMode("presence"), XR_INHIBIT_PRESENT);
    EXPECT_EQ(parseIdleInhibitMode("user_presence"), XR_INHIBIT_PRESENT);
}

TEST(XRIdleInhibitParse, CaseAndWhitespaceInsensitive) {
    EXPECT_EQ(parseIdleInhibitMode("  OFF "), XR_INHIBIT_OFF);
    EXPECT_EQ(parseIdleInhibitMode("Focused"), XR_INHIBIT_FOCUSED);
    EXPECT_EQ(parseIdleInhibitMode("\tPRESENT\n"), XR_INHIBIT_PRESENT);
    EXPECT_EQ(parseIdleInhibitMode("True"), XR_INHIBIT_FOCUSED);
}

TEST(XRIdleInhibitParse, UnknownAndEmptyDefaultToPresent) {
    EXPECT_EQ(parseIdleInhibitMode(""), XR_INHIBIT_PRESENT);
    EXPECT_EQ(parseIdleInhibitMode("banana"), XR_INHIBIT_PRESENT);
    EXPECT_EQ(parseIdleInhibitMode("visible"), XR_INHIBIT_PRESENT); // not a mode; do not silently mean FOCUSED
}

TEST(XRIdleInhibitParse, RoundTripsThroughString) {
    for (const auto* s : {"off", "focused", "present"})
        EXPECT_EQ(idleInhibitModeToString(parseIdleInhibitMode(s)), s);
}

// ---- predicate: off ----

TEST(XRIdleInhibit, OffNeverInhibits) {
    // Every combination of live facts, including the most "obviously inhibiting" one.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_OFF, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                   /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_OFF, false, false, false, false, false, false));
}

// ---- predicate: focused (the historical semantics — must not drift) ----

TEST(XRIdleInhibit, FocusedIsFocusOnly) {
    EXPECT_TRUE(wantXRIdleInhibit(XR_INHIBIT_FOCUSED, /*up*/ true, /*vis*/ true, /*focus*/ true, false, false, false));
    // VISIBLE but not focused (runtime dashboard in front) deliberately does NOT inhibit.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_FOCUSED, /*up*/ true, /*vis*/ true, /*focus*/ false, false, false, false));
    // Session exists but idle/synchronized.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_FOCUSED, /*up*/ true, /*vis*/ false, /*focus*/ false, false, false, false));
    // No session at all.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_FOCUSED, /*up*/ false, /*vis*/ false, /*focus*/ false, false, false, false));
}

TEST(XRIdleInhibit, FocusedIgnoresPresenceEntirely) {
    // Worn but not focused: still no inhibit under `focused`. That gap is exactly what `present` closes.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_FOCUSED, /*up*/ true, /*vis*/ true, /*focus*/ false,
                                   /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
    // Focused but doffed-per-presence: `focused` still inhibits (it has no wear input by design).
    EXPECT_TRUE(wantXRIdleInhibit(XR_INHIBIT_FOCUSED, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                  /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ false));
}

// ---- predicate: present, runtime WITHOUT XR_EXT_user_presence (fallback to focused) ----

TEST(XRIdleInhibit, PresentFallsBackToFocusedWithoutPresenceSupport) {
    // This is the null/remote-runtime and XREAL-over-direct-Monado path.
    EXPECT_TRUE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                  /*presenceSupported*/ false, /*presenceKnown*/ false, /*userPresent*/ false));
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ false,
                                   /*presenceSupported*/ false, /*presenceKnown*/ false, /*userPresent*/ false));
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ false, /*focus*/ false, false, false, false));
    // Stale presence bits with support=false must not leak into the decision.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ false,
                                   /*presenceSupported*/ false, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRIdleInhibit, PresentRequiresASession) {
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ false, /*vis*/ false, /*focus*/ false,
                                   /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ false, /*vis*/ false, /*focus*/ false, false, false, false));
}

// ---- predicate: present, runtime WITH XR_EXT_user_presence (the WiVRn path) ----

TEST(XRIdleInhibit, PresentWornAndVisibleInhibits) {
    // Worn + focused: the ordinary case.
    EXPECT_TRUE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                  /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
    // Worn but NOT focused (runtime dashboard in front / overlay mode with another app focused):
    // THE hole research/20 §5.D set out to close — `focused` says no, `present` says yes.
    EXPECT_TRUE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ false,
                                  /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRIdleInhibit, PresentDoffReleasesEvenWhenPresenceSticks) {
    // The load-bearing row. WiVRn's user_presence STICKS at `present` while the headset sits doffed
    // in standby, but the session correctly drops VISIBLE -> SYNCHRONIZED. Requiring visibility too
    // is what makes a doff actually release the inhibit — without it, phase 2 would reinstate on the
    // Wayland side the exact bug phase 1 fixes on the logind side.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ false, /*focus*/ false,
                                   /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRIdleInhibit, PresentAbsentReleasesEvenWhileStillVisible) {
    // The symmetric row: a real presence-absent event releases even if a stale VISIBLE bit lingers.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                   /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ false));
}

TEST(XRIdleInhibit, PresentReadsAbsentBeforeTheFirstPresenceEvent) {
    // presenceKnown=false: a session created with the headset on the shelf sprints to VISIBLE/FOCUSED
    // within ~40ms on WiVRn. Until a presence event actually lands we must read ABSENT, so that blip
    // cannot raise the inhibit bit.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                   /*presenceSupported*/ true, /*presenceKnown*/ false, /*userPresent*/ false));
    // ...and not even if a stale userPresent bit is somehow set without an event.
    EXPECT_FALSE(wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                   /*presenceSupported*/ true, /*presenceKnown*/ false, /*userPresent*/ true));
}

// ---- don/doff edge sequence: what the USER_PRESENCE recheck hook must produce ----

TEST(XRIdleInhibit, DonDoffEdgeFlipsTheBit) {
    // A session that is up + visible + focused throughout; only presence moves. This is the sequence
    // COpenXRManager::dispatchStateEvent(USER_PRESENCE) -> recheckIdleInhibitorStatus() drives, and
    // the reason that hook had to be added: without it nothing would re-evaluate on these edges.
    auto at = [](bool known, bool present) {
        return wantXRIdleInhibit(XR_INHIBIT_PRESENT, /*up*/ true, /*vis*/ true, /*focus*/ true,
                                 /*presenceSupported*/ true, known, present);
    };
    EXPECT_FALSE(at(/*known*/ false, /*present*/ false)); // session created, no event yet
    EXPECT_TRUE(at(/*known*/ true, /*present*/ true));    // don  -> engage
    EXPECT_FALSE(at(/*known*/ true, /*present*/ false));  // doff -> release
    EXPECT_TRUE(at(/*known*/ true, /*present*/ true));    // re-don -> engage again
}

// ---- present is a superset of focused whenever a wear signal says "worn" ----

TEST(XRIdleInhibit, PresentIsWiderThanFocusedWhenWorn) {
    for (const bool focus : {false, true}) {
        const bool focused = wantXRIdleInhibit(XR_INHIBIT_FOCUSED, true, true, focus, true, true, true);
        const bool present = wantXRIdleInhibit(XR_INHIBIT_PRESENT, true, true, focus, true, true, true);
        EXPECT_TRUE(present || !focused) << "present must inhibit wherever focused does, when worn";
        EXPECT_TRUE(present);
    }
}
