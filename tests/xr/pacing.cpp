#include <openxr/XRMath.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// tests/xr/pacing.cpp — damage-gated XR pacing (docs/openxr/02-virtual-monitors.md §Frame pacing,
// AndrewGaspar/Hyprland#1). The XR tick composites a monitor only when something on it changed;
// these pin the two pure decisions that make a static monitor free without ever leaving a quad
// empty or showing an old desktop.

// ---- xrPlanStaleLayer: the frame thread's answer to "no new desktop buffer this frame" ----

TEST(XRPacing, StaticMonitorKeepsLastImageForFree) {
    // the common case after #1: content on screen, nothing moving over it
    const auto P = xrPlanStaleLayer(/*hasContent*/ true, /*wantAnimTick*/ false, /*snapFresh*/ true);
    EXPECT_EQ(P.action, XR_STALE_KEEP);
    EXPECT_FALSE(P.wantFrame);
}

TEST(XRPacing, StaticMonitorWithoutSnapshotStillFree) {
    // nothing to redraw, so whether a snapshot exists is irrelevant
    const auto P = xrPlanStaleLayer(true, false, false);
    EXPECT_EQ(P.action, XR_STALE_KEEP);
    EXPECT_FALSE(P.wantFrame);
}

TEST(XRPacing, OverlayAnimatesFromFreshSnapshot) {
    // chrome fade / cursor / uniform fade over a static desktop: restore + re-overlay, no composite
    const auto P = xrPlanStaleLayer(true, true, true);
    EXPECT_EQ(P.action, XR_STALE_OVERLAY);
    EXPECT_FALSE(P.wantFrame);
}

TEST(XRPacing, OverlayWithStaleSnapshotWaitsForFreshBuffer) {
    // the last blit took no snapshot (chrome + cursor off, opaque): restoring would paint an old
    // desktop, so keep the current image and ask the main thread for a composite
    const auto P = xrPlanStaleLayer(true, true, false);
    EXPECT_EQ(P.action, XR_STALE_KEEP);
    EXPECT_TRUE(P.wantFrame);
}

TEST(XRPacing, NoContentAlwaysAsksForAFrame) {
    // first bind / swapchain recreate: a static desktop must not leave the quad unsubmitted forever
    for (const bool anim : {false, true}) {
        for (const bool snap : {false, true}) {
            const auto P = xrPlanStaleLayer(false, anim, snap);
            EXPECT_EQ(P.action, XR_STALE_FILL);
            EXPECT_TRUE(P.wantFrame);
        }
    }
}

// ---- xrPaceShouldSchedule: the main thread's per-tick decision ----

TEST(XRPacing, StaticMonitorIsNotScheduled) {
    EXPECT_FALSE(xrPaceShouldSchedule(/*paceAll*/ false, /*layerWantsFrame*/ false, /*monitorHasWork*/ false));
}

TEST(XRPacing, AnyReasonSchedules) {
    EXPECT_TRUE(xrPaceShouldSchedule(false, true, false)); // the frame thread needs a buffer
    EXPECT_TRUE(xrPaceShouldSchedule(false, false, true)); // damage / forced full frames pending
    EXPECT_TRUE(xrPaceShouldSchedule(false, true, true));
}

TEST(XRPacing, PaceAllRestoresEveryTickBehaviour) {
    // openxr:pace_static_monitors = 1
    EXPECT_TRUE(xrPaceShouldSchedule(true, false, false));
}
