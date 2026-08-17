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

TEST(XRPresenceGating, DonnedAndVisiblePlugs) {
    // report-20 issue D: the gate is now the CONJUNCTION — both visible AND present must hold.
    EXPECT_TRUE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ true,
                                      /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRPresenceGating, DoffUnplugsEvenIfPresenceSticksPresent) {
    // The never-unplugs standby bug: WiVRn's user_presence sticks 'present' with the headset on the
    // shelf, but the session correctly drops out of VISIBLE. Requiring visibility too means the doff
    // unplugs (arms the grace) despite the stuck-present signal.
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ false,
                                       /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ true));
}

TEST(XRPresenceGating, PresenceAbsentUnplugsEvenIfStillVisible) {
    // Symmetric: a presence-absent event unplugs even while a stale VISIBLE bit lingers.
    EXPECT_FALSE(wantXRMonitorsPlugged(XR_FOLLOW_VISIBLE, /*up*/ true, /*vis*/ true,
                                       /*presenceSupported*/ true, /*presenceKnown*/ true, /*userPresent*/ false));
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

// ---- report-19/20: first-plug settle blip guard (xrDeferFirstPlug) ----

TEST(XRDeferFirstPlug, DefersBeforeBlipElapsed) {
    // Fresh session: a would-be plug is held until visibility is sustained past the blip window.
    EXPECT_TRUE(xrDeferFirstPlug(/*everPlugged*/ false, /*sustainedMs*/ 200, /*blipMs*/ 1500));
    EXPECT_FALSE(xrDeferFirstPlug(/*everPlugged*/ false, /*sustainedMs*/ 1500, /*blipMs*/ 1500));
    EXPECT_FALSE(xrDeferFirstPlug(/*everPlugged*/ false, /*sustainedMs*/ 3000, /*blipMs*/ 1500));
}

TEST(XRDeferFirstPlug, NeverDefersOncePlugged) {
    // After the first plug the anti-flap grace governs re-plugs — the blip gate must not re-arm.
    EXPECT_FALSE(xrDeferFirstPlug(/*everPlugged*/ true, /*sustainedMs*/ 0, /*blipMs*/ 1500));
}

TEST(XRDeferFirstPlug, DefersEvenWithPresence) {
    // report-20 issue D: the settle now applies to the visibility side REGARDLESS of presence —
    // WiVRn reported 'present' 0.5ms after session create, so the first plug still waits for
    // sustained visibility before trusting either signal.
    EXPECT_TRUE(xrDeferFirstPlug(/*everPlugged*/ false, /*sustainedMs*/ 5, /*blipMs*/ 1500));
}

TEST(XRDeferFirstPlug, ZeroBlipNeverDefers) {
    EXPECT_FALSE(xrDeferFirstPlug(/*everPlugged*/ false, /*sustainedMs*/ 0, /*blipMs*/ 0));
}

// ---- report-17 WP-L3 / report-20 issue B1: dormant re-probe backoff schedule ----

TEST(XRReprobeBackoff, DoublesFromBaseToCap) {
    EXPECT_EQ(xrReprobeBackoffMs(0, 2000, 30000), 2000);
    EXPECT_EQ(xrReprobeBackoffMs(1, 2000, 30000), 4000);
    EXPECT_EQ(xrReprobeBackoffMs(2, 2000, 30000), 8000);
    EXPECT_EQ(xrReprobeBackoffMs(3, 2000, 30000), 16000);
    EXPECT_EQ(xrReprobeBackoffMs(4, 2000, 30000), 30000); // capped (32000 -> 30000)
    EXPECT_EQ(xrReprobeBackoffMs(9, 2000, 30000), 30000); // stays capped
}

TEST(XRReprobeBackoff, DefensiveInputs) {
    EXPECT_EQ(xrReprobeBackoffMs(0, 0, 30000), 2000);    // base<=0 -> 2000 default
    EXPECT_EQ(xrReprobeBackoffMs(5, 5000, 1000), 5000);  // cap<base -> clamped to base
    EXPECT_EQ(xrReprobeBackoffMs(-1, 2000, 30000), 2000); // negative attempt behaves like 0
}

// ---- doc 03 §8.4: openxr:recenter — what the headset's own recenter button means ----
//
// The frame thread observes the reference-space change but may not read this STRING config, so it
// is parsed on the main thread into an atomic (publishRecenterPolicy) exactly like the grab string
// options. That makes the parse itself the whole policy surface — pinned here.

TEST(XRRecenterPolicyParse, Spellings) {
    EXPECT_EQ(parseRecenterPolicy("hold"), XR_RECENTER_HOLD);
    EXPECT_EQ(parseRecenterPolicy("follow"), XR_RECENTER_FOLLOW);
    // Two synonyms for the same intent, because "follow" reads ambiguously next to
    // monitors_follow_session (which is about plugging, not about where monitors are).
    EXPECT_EQ(parseRecenterPolicy("reseat"), XR_RECENTER_FOLLOW);
    EXPECT_EQ(parseRecenterPolicy("me"), XR_RECENTER_FOLLOW);
}

TEST(XRRecenterPolicyParse, CaseAndWhitespaceInsensitive) {
    EXPECT_EQ(parseRecenterPolicy("  FOLLOW "), XR_RECENTER_FOLLOW);
    EXPECT_EQ(parseRecenterPolicy("Hold"), XR_RECENTER_HOLD);
}

TEST(XRRecenterPolicyParse, DefaultsToHoldAndTakesNoBooleans) {
    // Unrecognized values keep the shipped behavior — a typo must never silently start moving
    // someone's monitors on every re-don.
    EXPECT_EQ(parseRecenterPolicy(""), XR_RECENTER_HOLD);
    EXPECT_EQ(parseRecenterPolicy("nonsense"), XR_RECENTER_HOLD);
    // Deliberately NOT boolean-compatible (unlike monitors_follow_session, which had a Bool past to
    // honour): there is no old on/off spelling here, and reading a stray `1` as "follow" would hand
    // someone the mode this option exists to make them opt into.
    EXPECT_EQ(parseRecenterPolicy("1"), XR_RECENTER_HOLD);
    EXPECT_EQ(parseRecenterPolicy("true"), XR_RECENTER_HOLD);
    EXPECT_EQ(parseRecenterPolicy("yes"), XR_RECENTER_HOLD);
}

TEST(XRRecenterPolicyParse, NameRoundTrips) {
    // `hyprctl openxr status` prints this; it must be the spelling the config accepts back.
    EXPECT_EQ(parseRecenterPolicy(recenterPolicyName(XR_RECENTER_HOLD)), XR_RECENTER_HOLD);
    EXPECT_EQ(parseRecenterPolicy(recenterPolicyName(XR_RECENTER_FOLLOW)), XR_RECENTER_FOLLOW);
    EXPECT_STREQ(recenterPolicyName(XR_RECENTER_HOLD), "hold");
    EXPECT_STREQ(recenterPolicyName(XR_RECENTER_FOLLOW), "follow");
}
