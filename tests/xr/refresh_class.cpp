#include <openxr/XRMonitorConfig.hpp>
#include <config/supplementary/propRefresher/PropRefresher.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>

using namespace OpenXR;
using namespace Config::Supplementary;

// Refresh-class filtering: which config prop refreshes COpenXRManager is allowed to react to.
//
// THE PROBLEM. config.props_refreshed is a chatty hook. CPropRefresher::refreshProp() emits it
// unconditionally at the end of every refresh, whatever tripped it, and until lastRefreshedProps()
// the only thing it carried was a boolean saying whether it was the scheduled one. The OpenXR
// manager's listener therefore ran the FULL onConfigReload() pass — a dozen publish* passes,
// reloadXRRules(), refreshMonitorRuleOwnership(), reassertMonitorModeRules() (one monitorRuleMgr
// add() per live layer), updateMonitorsPlugged(), requestLayout2DSync() — for refreshes that had
// nothing to do with XR at all. An IME creating and destroying a virtual keyboard on every
// voice-typing engagement is the motivating case: it is an input-device event and nothing else.
//
// THE RULE THIS ENCODES. A refresh reaches the XR manager only if it carries at least one class
// that is NOT on the known-irrelevant list. Deny-list, never allow-list: an unrecognized class must
// fall through to "concerns XR", because a filter that guesses wrong in the other direction
// silently stops applying config, which is the exact failure mode the openxr:* keyword
// special-cases in CConfigManager::parseKeyword exist to fix.
//
// This is the FIRST line of defense. The second is the xrReloadAction() storm gate
// (tests/xr/reload_storm.cpp), which still guards every refresh that gets past here.

// ---- the classes that must NOT wake the XR manager ---------------------------------------------

TEST(refresh_class, input_devices_only_does_not_concern_xr) {
    // THE regression. A keyboard-layout / pointer-config re-apply reaches nothing onConfigReload()
    // reads: no publish* pass, no reassertMonitorModeRules, no updateMonitorsPlugged, no
    // requestLayout2DSync, and no probe policy evaluation (that gate is never even consulted).
    EXPECT_FALSE(xrRefreshConcernsXR(REFRESH_INPUT_DEVICES));
}

TEST(refresh_class, a_thousand_input_refreshes_yield_zero_xr_reactions) {
    // The shape of the live failure: an IME that churns a virtual keyboard per dictation, or a
    // script re-applying `hl.device{}`. The filter must not weaken with repetition — it has no
    // state, and this pins that it stays stateless.
    int reactions = 0;
    for (int i = 0; i < 1000; ++i) {
        if (xrRefreshConcernsXR(REFRESH_INPUT_DEVICES))
            ++reactions;
    }
    EXPECT_EQ(reactions, 0);
}

TEST(refresh_class, the_other_irrelevant_classes_do_not_concern_xr) {
    EXPECT_FALSE(xrRefreshConcernsXR(REFRESH_SCREEN_SHADER));
    EXPECT_FALSE(xrRefreshConcernsXR(REFRESH_BLUR_FB));
    EXPECT_FALSE(xrRefreshConcernsXR(REFRESH_CURSOR_ZOOMS));
    EXPECT_FALSE(xrRefreshConcernsXR(REFRESH_CONFIG_WATCHER));
    EXPECT_FALSE(xrRefreshConcernsXR(REFRESH_GRADIENTS_GROUPBAR));
}

TEST(refresh_class, any_combination_of_irrelevant_classes_is_still_irrelevant) {
    // The mask arrives ORed together: one event-loop iteration coalesces every scheduleRefresh()
    // into a single refreshProp(), so "input devices AND blur FB AND groupbar gradients" is a
    // perfectly ordinary mask and must still be filtered as a whole.
    const PropRefreshBits parts[] = {REFRESH_INPUT_DEVICES, REFRESH_SCREEN_SHADER, REFRESH_BLUR_FB, REFRESH_CURSOR_ZOOMS, REFRESH_CONFIG_WATCHER, REFRESH_GRADIENTS_GROUPBAR};

    for (unsigned combo = 1; combo < (1u << std::size(parts)); ++combo) {
        PropRefreshBits mask = 0;
        for (size_t i = 0; i < std::size(parts); ++i) {
            if (combo & (1u << i))
                mask = static_cast<PropRefreshBits>(mask | parts[i]);
        }
        ASSERT_FALSE(xrRefreshConcernsXR(mask)) << "combo " << combo << " mask " << mask;
    }
}

// ---- the classes that must STILL wake it -------------------------------------------------------

TEST(refresh_class, monitor_states_concerns_xr) {
    // The whole reason onConfigReload() has a monitor half: a reparse wiped the rule manager and
    // took the XR-owned mode/scale/offset rules with it.
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_MONITOR_STATES));
}

TEST(refresh_class, rules_and_window_states_concern_xr) {
    // WP X1: a windowrule change moves a stereo declaration (publishStereoPairTuning), and the
    // xrrule transparency snapshot is re-resolved off the same edge.
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_RULES));
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_WINDOW_STATES));
}

TEST(refresh_class, layouts_concerns_xr) {
    // Not filtered on purpose. REFRESH_MONITOR_STATES is the composite `(1<<6) | REFRESH_LAYOUTS`,
    // so putting REFRESH_LAYOUTS on the deny-list would couple the filter to that composite's
    // internals; and a monitor recalc is layout2d's world anyway.
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_LAYOUTS));
}

TEST(refresh_class, refresh_all_concerns_xr) {
    // A real `hyprctl reload` / file edit. Must always get the full pass.
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_ALL));
}

TEST(refresh_class, an_irrelevant_class_mixed_with_a_relevant_one_concerns_xr) {
    // Coalescing again, from the other side: one relevant bit anywhere in the mask wins.
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_INPUT_DEVICES | REFRESH_MONITOR_STATES));
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_BLUR_FB | REFRESH_RULES));
    EXPECT_TRUE(xrRefreshConcernsXR(REFRESH_CONFIG_WATCHER | REFRESH_GRADIENTS_GROUPBAR | REFRESH_LAYOUTS));
}

TEST(refresh_class, a_zero_mask_concerns_xr) {
    // LOAD-BEARING, and the least obvious clause in the whole filter. Not one openxr:* value in
    // ConfigValues.cpp declares a refresh class, so under the Lua config
    //   hyprctl eval 'hl.config{ openxr = { black_alpha = 0.35 } }'
    // parses the value and then calls scheduleRefresh(0). That refresh does no work and trips no
    // class — the props_refreshed emit at the end of it IS the entire mechanism by which every
    // documented hot-tune knob in docs/openxr/05-ipc-config.md applies live. Filtering a zero mask
    // would silently kill all of them at once, and the symptom would be "my openxr settings stopped
    // applying", not "the storm is fixed".
    EXPECT_TRUE(xrRefreshConcernsXR(0));
}

TEST(refresh_class, an_unknown_future_class_concerns_xr) {
    // Deny-list direction. A class added upstream after this filter was written must default to
    // waking the manager, not to being silently dropped.
    constexpr PropRefreshBits UNKNOWN = (1 << 15);
    static_assert((UNKNOWN & XR_IRRELEVANT_REFRESH_CLASSES) == 0, "pick a bit no class uses");
    EXPECT_TRUE(xrRefreshConcernsXR(UNKNOWN));
    EXPECT_TRUE(xrRefreshConcernsXR(UNKNOWN | REFRESH_INPUT_DEVICES));
}

// ---- and the core-side half: an input refresh must not re-apply monitor rules ------------------

TEST(refresh_class, input_devices_does_not_trip_the_monitor_states_block) {
    // CPropRefresher::refreshProp() gates its monitor pass on `m_propsTripped & REFRESH_MONITOR_STATES_OWN`
    // — that pass is the one that calls monitorRuleMgr()->scheduleReload(), which is what eventually
    // walks every monitor in ensureMonitorStatus(). Same expressions here: an input-device-only
    // refresh must not reach it, so an input refresh costs ZERO monitor-rule applications before the
    // XR listener is even consulted.
    const PropRefreshBits tripped = REFRESH_INPUT_DEVICES;
    EXPECT_EQ(tripped & REFRESH_MONITOR_STATES_OWN, 0);
    EXPECT_EQ(tripped & REFRESH_MONITOR_STATES, 0);
    EXPECT_EQ(tripped & REFRESH_LAYOUTS, 0);
    EXPECT_EQ(tripped & REFRESH_WINDOW_STATES, 0);
}

TEST(refresh_class, a_bare_layout_refresh_does_not_trip_the_monitor_pass) {
    // The composite trap, pinned. REFRESH_MONITOR_STATES is `OWN | REFRESH_LAYOUTS`, so the old
    // `tripped & REFRESH_MONITOR_STATES` test was truthy for a bare layout refresh — a runtime
    // `gaps_in` retune ran scheduleReload() + ensureVRR() + a persistent-workspace sweep and
    // recalculated every monitor twice. Testing OWN is what makes a layout refresh a layout refresh.
    EXPECT_EQ(REFRESH_LAYOUTS & REFRESH_MONITOR_STATES_OWN, 0);
    EXPECT_NE(REFRESH_LAYOUTS & REFRESH_MONITOR_STATES, 0); // the superset trap being avoided
    // ...while a real monitor-states refresh still trips both its own pass and the layout pass.
    EXPECT_NE(REFRESH_MONITOR_STATES & REFRESH_MONITOR_STATES_OWN, 0);
    EXPECT_NE(REFRESH_MONITOR_STATES & REFRESH_LAYOUTS, 0);
}

TEST(refresh_class, the_irrelevant_classes_are_disjoint_from_the_monitor_pass) {
    // Stronger form: nothing the XR filter drops can have scheduled a monitor-rule reload either.
    // If a future class is added to XR_IRRELEVANT_REFRESH_CLASSES that DOES trip the monitor pass,
    // this fails — and it should, because then dropping it on the XR side would be hiding work that
    // still happened.
    EXPECT_EQ(XR_IRRELEVANT_REFRESH_CLASSES & REFRESH_MONITOR_STATES, 0);
    EXPECT_EQ(XR_IRRELEVANT_REFRESH_CLASSES & REFRESH_WINDOW_STATES, 0);
}
