#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Old-signature shim: the pre-report-19 tests exercise the no-presence fallback (presence
// unsupported), where `visible` mode == raw visibility. report-19 added the presence params; this
// keeps those truth-table rows readable while the presence rows below pass the full signature.
static bool wantVis(eXRMonitorFollowMode mode, bool sessionUp, bool sessionVisible) {
    return wantXRMonitorsPlugged(mode, sessionUp, sessionVisible, /*presenceSupported*/ false, /*presenceKnown*/ false, /*userPresent*/ false);
}

// wantXRMonitorsPlugged / parseMonitorFollowMode (XRMonitorConfig.cpp) are the pure plugged-state
// policy behind research/18 + the report-18 addendum (XR monitors behave like unplugged external
// monitors while their session is not usable). COpenXRManager::updateMonitorsPlugged and
// createXRMonitor both consult them; these tests pin the full truth table so the compositor-side
// wiring (mode decision + the doff-grace timer) can rely on it blindly. The anti-flap grace itself
// is manager-side timer wiring (updateMonitorsPlugged), exercised by the hyprtester suite; the
// instantaneous predicate here is what the grace defers.

// ---- parse: new mode spellings + legacy boolean compat + default ----

TEST(XRFollowModeParse, NewSpellings) {
    EXPECT_EQ(parseMonitorFollowMode("off"), XR_FOLLOW_OFF);
    EXPECT_EQ(parseMonitorFollowMode("session"), XR_FOLLOW_SESSION);
    EXPECT_EQ(parseMonitorFollowMode("visible"), XR_FOLLOW_VISIBLE);
}

TEST(XRFollowModeParse, LegacyBooleanCompat) {
    // Pre-report-18 configs used a Bool: false/0 was the always-present opt-out (off), true/1 was
    // the existence-gated feature (session).
    EXPECT_EQ(parseMonitorFollowMode("0"), XR_FOLLOW_OFF);
    EXPECT_EQ(parseMonitorFollowMode("false"), XR_FOLLOW_OFF);
    EXPECT_EQ(parseMonitorFollowMode("no"), XR_FOLLOW_OFF);
    EXPECT_EQ(parseMonitorFollowMode("1"), XR_FOLLOW_SESSION);
    EXPECT_EQ(parseMonitorFollowMode("true"), XR_FOLLOW_SESSION);
    EXPECT_EQ(parseMonitorFollowMode("yes"), XR_FOLLOW_SESSION);
}

TEST(XRFollowModeParse, CaseAndWhitespaceInsensitive) {
    EXPECT_EQ(parseMonitorFollowMode("  Visible "), XR_FOLLOW_VISIBLE);
    EXPECT_EQ(parseMonitorFollowMode("SESSION"), XR_FOLLOW_SESSION);
    EXPECT_EQ(parseMonitorFollowMode("Off"), XR_FOLLOW_OFF);
    EXPECT_EQ(parseMonitorFollowMode("focused"), XR_FOLLOW_VISIBLE);
}

TEST(XRFollowModeParse, UnknownAndEmptyDefaultVisible) {
    // report-18 addendum: the default is `visible` (a doffed/standby headset reads as unplugged).
    EXPECT_EQ(parseMonitorFollowMode(""), XR_FOLLOW_VISIBLE);
    EXPECT_EQ(parseMonitorFollowMode("garbage"), XR_FOLLOW_VISIBLE);
}

// ---- mode = visible (the default): plugged tracks session VISIBILITY ----

TEST(XRPluggedPolicy, VisibleUnpluggedWhileDoffed) {
    // The report-18 case: WiVRn keeps a session alive with the headset on the shelf (sessionUp but
    // not visible) — the monitors must read as unplugged.
    EXPECT_FALSE(wantVis(XR_FOLLOW_VISIBLE, /*sessionUp*/ true, /*sessionVisible*/ false));
}

TEST(XRPluggedPolicy, VisiblePluggedWhileWorn) {
    EXPECT_TRUE(wantVis(XR_FOLLOW_VISIBLE, /*sessionUp*/ true, /*sessionVisible*/ true));
}

TEST(XRPluggedPolicy, VisibleUnpluggedWhileSessionless) {
    EXPECT_FALSE(wantVis(XR_FOLLOW_VISIBLE, /*sessionUp*/ false, /*sessionVisible*/ false));
}

// ---- mode = session: plugged tracks session EXISTENCE (the research/18 default) ----

TEST(XRPluggedPolicy, SessionUnpluggedWhileSessionless) {
    // The fishfood login case: xrmonitor= lines materialize at init() with no headset in sight.
    EXPECT_FALSE(wantVis(XR_FOLLOW_SESSION, /*sessionUp*/ false, /*sessionVisible*/ false));
}

TEST(XRPluggedPolicy, SessionPluggedWhileSessionUp) {
    // Existence, not visibility: a doffed-but-alive session still counts as plugged in this mode.
    EXPECT_TRUE(wantVis(XR_FOLLOW_SESSION, /*sessionUp*/ true, /*sessionVisible*/ false));
    EXPECT_TRUE(wantVis(XR_FOLLOW_SESSION, /*sessionUp*/ true, /*sessionVisible*/ true));
}

// ---- mode = off: the pre-feature always-present behavior ----

TEST(XRPluggedPolicy, OffAlwaysPlugged) {
    EXPECT_TRUE(wantVis(XR_FOLLOW_OFF, /*sessionUp*/ false, /*sessionVisible*/ false));
    EXPECT_TRUE(wantVis(XR_FOLLOW_OFF, /*sessionUp*/ true, /*sessionVisible*/ false));
    EXPECT_TRUE(wantVis(XR_FOLLOW_OFF, /*sessionUp*/ true, /*sessionVisible*/ true));
}

// ---- report-19: mode = visible with a presence-capable runtime (WiVRn) ----
// The live defect: WiVRn creates a session (and sprints IDLE->VISIBLE->FOCUSED) with the headset on
// the shelf. Under presence gating, visibility no longer plugs — only a real donned signal does.

TEST(XRPresenceGating, ShelfSessionNeverPlugsDespiteVisible) {
    // sessionUp AND sessionVisible (the 40ms FOCUSED sprint) but presence is known-absent (doffed).
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ true,
                                       /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ false));
}

TEST(XRPresenceGating, UnknownBeforeFirstEventReadsAbsent) {
    // Presence supported but no event yet: read as ABSENT so the session-create visibility blip can
    // never plug before the runtime's initial presence report lands.
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ true,
                                       /*presenceSupported*/ true, /*presenceKnown*/ false, /*userPresent*/ false));
    // Even if a stale userPresent=true slipped in, !presenceKnown still gates it off.
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ true,
                                       /*presenceSupported*/ true, /*presenceKnown*/ false, /*userPresent*/ true));
}

TEST(XRPresenceGating, DonnedPlugs) {
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ true,
                                      /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
    // Presence is authoritative: donned plugs even if the visibility bit lags behind the sensor.
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ false,
                                      /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRPresenceGating, PresencePathStillRequiresSession) {
    // A vanished session unplugs regardless of a stale present=true.
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ false, /*vis*/ false,
                                       /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRPresenceGating, SessionAndOffModesIgnorePresence) {
    // OFF is unconditional; SESSION keys purely on existence — presence must not perturb either.
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_OFF, false, false, true, true, false));
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_SESSION, /*up*/ true, /*vis*/ false, true, true, false));
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_SESSION, /*up*/ false, /*vis*/ false, true, true, true));
}

// ---- report-19: no-presence fallback first-plug blip guard (xrDeferFirstPlug) ----

TEST(XRDeferFirstPlug, DefersBeforeBlipElapsed) {
    // Fresh session, no presence: a would-be plug is held until visibility is sustained past the blip.
    EXPECT_TRUE(xrDeferFirstPlug(/*presenceSupported*/ false, /*everPlugged*/ false, /*sustainedMs*/ 200, /*blipMs*/ 1500));
    EXPECT_FALSE(xrDeferFirstPlug(/*presenceSupported*/ false, /*everPlugged*/ false, /*sustainedMs*/ 1500, /*blipMs*/ 1500));
    EXPECT_FALSE(xrDeferFirstPlug(/*presenceSupported*/ false, /*everPlugged*/ false, /*sustainedMs*/ 3000, /*blipMs*/ 1500));
}

TEST(XRDeferFirstPlug, NeverDefersOncePlugged) {
    // After the first plug the anti-flap grace governs re-plugs — the blip gate must not re-arm.
    EXPECT_FALSE(xrDeferFirstPlug(/*presenceSupported*/ false, /*everPlugged*/ true, /*sustainedMs*/ 0, /*blipMs*/ 1500));
}

TEST(XRDeferFirstPlug, NeverDefersWhenPresenceSupported) {
    // Presence already suppresses the blip; the fallback guard must stay out of the presence path.
    EXPECT_FALSE(xrDeferFirstPlug(/*presenceSupported*/ true, /*everPlugged*/ false, /*sustainedMs*/ 0, /*blipMs*/ 1500));
}

TEST(XRDeferFirstPlug, ZeroBlipNeverDefers) {
    EXPECT_FALSE(xrDeferFirstPlug(/*presenceSupported*/ false, /*everPlugged*/ false, /*sustainedMs*/ 0, /*blipMs*/ 0));
}
