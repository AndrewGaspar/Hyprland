#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

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
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*sessionUp*/ true, /*sessionVisible*/ false));
}

TEST(XRPluggedPolicy, VisiblePluggedWhileWorn) {
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*sessionUp*/ true, /*sessionVisible*/ true));
}

TEST(XRPluggedPolicy, VisibleUnpluggedWhileSessionless) {
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*sessionUp*/ false, /*sessionVisible*/ false));
}

// ---- mode = session: plugged tracks session EXISTENCE (the research/18 default) ----

TEST(XRPluggedPolicy, SessionUnpluggedWhileSessionless) {
    // The fishfood login case: xrmonitor= lines materialize at init() with no headset in sight.
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_SESSION, /*sessionUp*/ false, /*sessionVisible*/ false));
}

TEST(XRPluggedPolicy, SessionPluggedWhileSessionUp) {
    // Existence, not visibility: a doffed-but-alive session still counts as plugged in this mode.
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_SESSION, /*sessionUp*/ true, /*sessionVisible*/ false));
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_SESSION, /*sessionUp*/ true, /*sessionVisible*/ true));
}

// ---- mode = off: the pre-feature always-present behavior ----

TEST(XRPluggedPolicy, OffAlwaysPlugged) {
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_OFF, /*sessionUp*/ false, /*sessionVisible*/ false));
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_OFF, /*sessionUp*/ true, /*sessionVisible*/ false));
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_OFF, /*sessionUp*/ true, /*sessionVisible*/ true));
}
