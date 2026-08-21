#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// Reload-storm containment: the policy that decides what a config reload / prop refresh may do to a
// dormant XR manager, and whether a re-arm may push out a re-probe that is already pending.
//
// EVIDENCE PINNED HERE (live session 2026-08-21, 02:50:00-03:00:18, build 632814a03):
//   - 559 config prop refreshes in 618s (~1 Hz), each one reaching COpenXRManager::onConfigReload()
//     through the config.props_refreshed listener. Not a file edit, not `hyprctl reload` — the
//     compositor log shows the input-device re-apply (CPropRefresher::refreshProp) BEFORE the single
//     "[OPENXR] N xrrule(s) loaded" line of the cycle, which is the props_refreshed ordering; a real
//     reload emits config.reloaded FIRST and would have produced two xrrule lines per cycle.
//   - Every one of those refreshes ran a full bring-up attempt, because onConfigReload() called
//     start() unconditionally whenever openxr:enabled was set and the state was UNAVAILABLE.
//   - Each attempt went STARTING (cancelReprobe -> timer disarmed, inotify watch torn down) and then
//     back to UNAVAILABLE (timer + watch rebuilt from scratch). Re-arming a 16000ms timer once a
//     second means it NEVER fires: the attempt counter moved 5 times in 618s (0,1,2,3,4,5), while
//     `hyprctl openxr status` reported a perfectly truthful "retrying in 15916ms".
//   - `hyprctl openxr disable` did not stick: it called stop(), and the next refresh (< 1s later)
//     read openxr:enabled = 1, saw DISABLED, and started straight back up.

namespace {
    SXRReloadInputs dormantUnavailable() {
        SXRReloadInputs in;
        in.enabled            = true;
        in.stateUnavailable   = true;
        in.msSinceLastProbe   = 60000; // long past the floor
        in.minProbeIntervalMs = 2000;
        return in;
    }
}

// ---- the storm gate ---------------------------------------------------------------------------

TEST(reload_storm, unchanged_config_while_dormant_does_not_probe) {
    // THE regression. A reload that changes nothing probe-relevant must cost nothing: no start(),
    // hence no STARTING transition, hence no timer reset and no watch teardown/rebuild.
    auto in               = dormantUnavailable();
    in.probeInputsChanged = false;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

TEST(reload_storm, a_thousand_unchanged_reloads_never_probe) {
    // The shape of the live failure: the gate must not weaken with repetition, and must not depend
    // on how long ago the last probe was — only the re-probe timer may decide that.
    auto in               = dormantUnavailable();
    in.probeInputsChanged = false;
    for (int i = 0; i < 1000; ++i) {
        in.msSinceLastProbe = i * 1000; // even hours later, an unchanged reload is not a probe
        ASSERT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY) << "iteration " << i;
    }
}

TEST(reload_storm, probe_relevant_change_does_probe) {
    auto in               = dormantUnavailable();
    in.probeInputsChanged = true;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);
}

TEST(reload_storm, explicit_user_reassert_probes_even_when_nothing_changed) {
    // report-17 WP-L7: `hyprctl keyword openxr:enabled 1` while already 1 and dormant must still
    // kick a fresh attempt. The VALUE is unchanged, so only forceProbe can carry that intent.
    auto in               = dormantUnavailable();
    in.probeInputsChanged = false;
    in.forceProbe         = true;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);
}

TEST(reload_storm, force_probe_also_beats_the_floor) {
    auto in               = dormantUnavailable();
    in.probeInputsChanged = true;
    in.msSinceLastProbe   = 5; // milliseconds after the last attempt
    in.forceProbe         = true;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);
}

// ---- the floor --------------------------------------------------------------------------------

TEST(reload_storm, changed_config_still_honours_the_probe_floor) {
    // A config being edited in a tight loop (or a stop/start flap) must not out-run the documented
    // cadence either. The floor is the reprobe base interval.
    auto in               = dormantUnavailable();
    in.probeInputsChanged = true;
    in.msSinceLastProbe   = 1999;
    in.minProbeIntervalMs = 2000;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);

    in.msSinceLastProbe = 2000; // exactly at the floor is allowed
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);
}

TEST(reload_storm, never_probed_yet_is_not_blocked_by_the_floor) {
    // msSinceLastProbe < 0 means "no attempt this session"; the floor has nothing to measure from.
    auto in               = dormantUnavailable();
    in.probeInputsChanged = true;
    in.msSinceLastProbe   = -1;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);
}

TEST(reload_storm, with_reprobe_off_a_reload_stays_the_users_retry) {
    // The gate is only justified by the re-probe timer being there instead. openxr:reprobe = 0 means
    // nothing else will ever try again, so a reload must still probe (bounded by the floor).
    auto in               = dormantUnavailable();
    in.probeInputsChanged = false;
    in.reprobeEnabled     = false;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);

    in.msSinceLastProbe = 500; // ... but not faster than the base cadence
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

TEST(reload_storm, with_reprobe_off_a_sticky_disable_is_still_sticky) {
    auto in             = dormantUnavailable();
    in.reprobeEnabled   = false;
    in.manualDisable    = true;
    in.stateDisabled    = true;
    in.stateUnavailable = false;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

// ---- state gating -----------------------------------------------------------------------------

TEST(reload_storm, disabled_state_is_exempt_from_the_change_gate) {
    // DISABLED runs no re-probe timer, so a reload is the ONLY thing that could ever heal it. The
    // floor still applies, but "nothing changed" must not strand a manager that config says is on.
    SXRReloadInputs in;
    in.enabled            = true;
    in.stateDisabled      = true;
    in.probeInputsChanged = false;
    in.msSinceLastProbe   = -1;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_START);

    in.msSinceLastProbe = 10; // ... but not faster than the floor
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

TEST(reload_storm, a_live_or_in_flight_session_is_never_restarted_by_a_reload) {
    // Neither stateDisabled nor stateUnavailable = STARTING / IDLE / VISIBLE / FOCUSED / STOPPING.
    SXRReloadInputs in;
    in.enabled            = true;
    in.probeInputsChanged = true; // even a real config change
    in.forceProbe         = true; // even an explicit re-assert
    in.msSinceLastProbe   = 60000;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

TEST(reload_storm, disabling_via_config_stops_a_running_session_once) {
    SXRReloadInputs in;
    in.enabled = false;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_STOP);

    in.stateDisabled = true; // already stopped: idempotent, and cheap
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

// ---- sticky manual disable --------------------------------------------------------------------

TEST(reload_storm, manual_disable_survives_a_reload_that_reapplies_enabled) {
    // The live failure: `hyprctl openxr disable` -> stop() -> DISABLED, then the next refresh reads
    // openxr:enabled = 1 and restarts. The latch outranks the config value.
    SXRReloadInputs in;
    in.enabled            = true; // openxr:enabled is still 1 in the config, as it always is
    in.manualDisable      = true;
    in.stateDisabled      = true;
    in.probeInputsChanged = false;
    in.msSinceLastProbe   = 60000;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

TEST(reload_storm, manual_disable_survives_a_storm_of_reloads) {
    SXRReloadInputs in;
    in.enabled          = true;
    in.manualDisable    = true;
    in.stateDisabled    = true;
    in.msSinceLastProbe = 60000;
    for (int i = 0; i < 600; ++i) {
        in.probeInputsChanged = (i % 7) == 0; // even genuine config churn must not lift it
        in.msSinceLastProbe   = 60000 + i;
        ASSERT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY) << "iteration " << i;
    }
}

TEST(reload_storm, manual_disable_beats_even_an_explicit_force_probe) {
    // forceProbe carries "the user re-asserted openxr:enabled". A manual disable is a LATER, more
    // specific instruction from the same user; clearing it is the `enable` verb's job (userEnable),
    // not a side effect of any reload path.
    SXRReloadInputs in;
    in.enabled       = true;
    in.manualDisable = true;
    in.stateDisabled = true;
    in.forceProbe    = true;
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_RECONCILE_ONLY);
}

TEST(reload_storm, manual_disable_tears_down_a_session_that_is_somehow_still_up) {
    SXRReloadInputs in;
    in.enabled       = true;
    in.manualDisable = true;
    in.stateDisabled = false; // still IDLE/VISIBLE for whatever reason
    EXPECT_EQ(xrReloadAction(in), XR_RELOAD_STOP);
}

// ---- the re-probe timer must not be pushed out -------------------------------------------------

TEST(reload_storm, rearm_never_delays_a_pending_probe) {
    // The mechanism that starved the timer: re-arming an armed 16000ms timer with another 16000ms,
    // once a second, walks the deadline forward forever.
    EXPECT_FALSE(xrShouldRearmReprobe(/*armed=*/true, /*leftMs=*/15916, /*wantMs=*/16000));
    EXPECT_FALSE(xrShouldRearmReprobe(true, 16000, 16000)); // equal is still a push-out
    EXPECT_FALSE(xrShouldRearmReprobe(true, 1, 30000));
}

TEST(reload_storm, rearm_allowed_when_it_would_fire_sooner) {
    // The watch-driven "probe now" leg and the reset-to-base-cadence leg must still win.
    EXPECT_TRUE(xrShouldRearmReprobe(/*armed=*/true, /*leftMs=*/15916, /*wantMs=*/150));
    EXPECT_TRUE(xrShouldRearmReprobe(true, 30000, 2000));
}

TEST(reload_storm, rearm_always_allowed_when_nothing_is_pending) {
    EXPECT_TRUE(xrShouldRearmReprobe(/*armed=*/false, /*leftMs=*/0, /*wantMs=*/30000));
    EXPECT_TRUE(xrShouldRearmReprobe(false, 99999, 1));
}

TEST(reload_storm, a_storm_of_rearms_cannot_walk_the_deadline_forward) {
    // Simulate the live cadence: an armed 16s timer, a re-arm every second with the same delay.
    // With the guard, the pending deadline only ever shrinks, so the probe actually happens.
    int64_t leftMs = 16000;
    for (int sec = 0; sec < 15; ++sec) {
        leftMs -= 1000; // a second of real time passes
        ASSERT_FALSE(xrShouldRearmReprobe(true, leftMs, 16000)) << "second " << sec;
    }
    EXPECT_LE(leftMs, 1000); // the timer reached the point of firing instead of being pushed out
}
