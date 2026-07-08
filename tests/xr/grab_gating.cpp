#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/grab_gating.cpp — WP-G3 grab-region gating + corner-resize math from
// docs/openxr/research/04-grabbable-borders.md §5.2/§6-G3. Pure: builds and runs with no OpenXR
// runtime. Covers the grabActionForRegion truth table, corner sign mapping, and the CXRAnchor
// corner-resize (opposite-corner-pinned scale) across drags + clamps + anchor modes.

// ---- grabActionForRegion truth table ----

TEST(XRGrabGating, BarAlwaysMoves) {
    EXPECT_EQ(grabActionForRegion(XR_REGION_BAR, /*any*/ false, false), XR_GRAB_ACTION_MOVE);
    EXPECT_EQ(grabActionForRegion(XR_REGION_BAR, true, false), XR_GRAB_ACTION_MOVE);
    // Bar moves even when hands are active (bar is the hand move handle).
    EXPECT_EQ(grabActionForRegion(XR_REGION_BAR, false, true), XR_GRAB_ACTION_MOVE);
}

TEST(XRGrabGating, CornersAlwaysResizeFromThatCorner) {
    EXPECT_EQ(grabActionForRegion(XR_REGION_CORNER_TL, false, false), XR_GRAB_ACTION_RESIZE_TL);
    EXPECT_EQ(grabActionForRegion(XR_REGION_CORNER_TR, false, false), XR_GRAB_ACTION_RESIZE_TR);
    EXPECT_EQ(grabActionForRegion(XR_REGION_CORNER_BL, false, false), XR_GRAB_ACTION_RESIZE_BL);
    EXPECT_EQ(grabActionForRegion(XR_REGION_CORNER_BR, false, false), XR_GRAB_ACTION_RESIZE_BR);
    // Corners resize regardless of grab_anywhere / hand-active.
    EXPECT_EQ(grabActionForRegion(XR_REGION_CORNER_BR, true, true), XR_GRAB_ACTION_RESIZE_BR);
    for (auto r : {XR_REGION_CORNER_TL, XR_REGION_CORNER_TR, XR_REGION_CORNER_BL, XR_REGION_CORNER_BR})
        EXPECT_TRUE(xrGrabActionIsResize(grabActionForRegion(r, false, false)));
}

TEST(XRGrabGating, BodyMovesOnlyWithGrabAnywhereAndNoHands) {
    EXPECT_EQ(grabActionForRegion(XR_REGION_BODY, true, false), XR_GRAB_ACTION_MOVE);   // controller convenience
    EXPECT_EQ(grabActionForRegion(XR_REGION_BODY, false, false), XR_GRAB_ACTION_NONE);  // deliberate-borders
    EXPECT_EQ(grabActionForRegion(XR_REGION_BODY, true, true), XR_GRAB_ACTION_NONE);    // hands forced to chrome (WP-G5)
    EXPECT_EQ(grabActionForRegion(XR_REGION_BODY, false, true), XR_GRAB_ACTION_NONE);
}

TEST(XRGrabGating, MarginAndNoneNeverGrab) {
    for (bool ga : {false, true})
        for (bool ha : {false, true}) {
            EXPECT_EQ(grabActionForRegion(XR_REGION_MARGIN, ga, ha), XR_GRAB_ACTION_NONE);
            EXPECT_EQ(grabActionForRegion(XR_REGION_NONE, ga, ha), XR_GRAB_ACTION_NONE);
        }
}

TEST(XRGrabGating, CornerSigns) {
    float sx = 0.f, sy = 0.f;
    xrCornerSigns(XR_REGION_CORNER_TL, sx, sy);
    EXPECT_EQ(sx, -1.f);
    EXPECT_EQ(sy, 1.f); // top = +up
    xrCornerSigns(XR_REGION_CORNER_TR, sx, sy);
    EXPECT_EQ(sx, 1.f);
    EXPECT_EQ(sy, 1.f);
    xrCornerSigns(XR_REGION_CORNER_BL, sx, sy);
    EXPECT_EQ(sx, -1.f);
    EXPECT_EQ(sy, -1.f);
    xrCornerSigns(XR_REGION_CORNER_BR, sx, sy);
    EXPECT_EQ(sx, 1.f);
    EXPECT_EQ(sy, -1.f);
}

// ---- corner resize math (CXRAnchor) ----

namespace {
    // A LOCAL-anchored quad facing straight down -Z (identity orientation), content width w0,
    // aspect (h/w) = aspectHW. Returns the seeded anchor after one solve (so lastWorld is set).
    CXRAnchor makeLocalQuad(float w0, float aspectHW, Vec3 center = Vec3{0.f, 1.5f, -2.f}) {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode        = XR_ANCHOR_LOCAL;
        st.anchorPose  = SXRPose{center, Quat{0.f, 0.f, 0.f, 1.f}};
        st.widthMeters = w0;
        a.initFromState(st);
        SXRSolveInput in;
        in.view = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        in.pxW  = 1000;
        in.pxH  = (uint32_t)std::lround(1000.f * aspectHW);
        a.solve(in, SXRAnchorTuning{});
        return a;
    }

    // World position of a content corner given anchorPose (identity-rot local) + width + aspect.
    Vec3 cornerWorld(const CXRAnchor& a, eXRQuadRegion corner, float aspectHW) {
        const SXRPose& P = a.state().anchorPose;
        const float    w = a.state().widthMeters;
        const float    h = w * aspectHW;
        float          sx = 1.f, sy = 1.f;
        xrCornerSigns(corner, sx, sy);
        const Vec3 right = qRotate(P.rot, Vec3{1.f, 0.f, 0.f});
        const Vec3 up    = qRotate(P.rot, Vec3{0.f, 1.f, 0.f});
        return P.pos + right * (sx * w * 0.5f) + up * (sy * h * 0.5f);
    }

    SXRSolveInput localSolveIn() {
        SXRSolveInput in;
        in.view = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        in.pxW  = 1;
        in.pxH  = 1;
        return in;
    }
}

TEST(XRGrabResize, DragOutGrowsAndPinsOppositeCorner) {
    const float aspect = 0.5f;
    CXRAnchor   a      = makeLocalQuad(1.0f, aspect);

    // Grab the BR corner; the TL corner must stay pinned in world.
    const Vec3 pinTL   = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    const Vec3 grabbed = cornerWorld(a, XR_REGION_CORNER_BR, aspect);

    const SXRPose grip0{grabbed, Quat{0.f, 0.f, 0.f, 1.f}};
    a.beginResize(XR_HAND_RIGHT, XR_REGION_CORNER_BR, grip0, aspect);
    EXPECT_TRUE(a.grabbed());
    EXPECT_EQ(a.grabKind(), XR_GRABKIND_RESIZE);
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL); // resize does NOT device-lock the quad

    // Move the grip outward along the pin->corner diagonal by 0.25 m.
    const Vec3    diagUnit = (grabbed - pinTL).normalized();
    const SXRPose gripNow{grabbed + diagUnit * 0.25f, Quat{0.f, 0.f, 0.f, 1.f}};
    a.grabResizeCorner(gripNow, localSolveIn(), SXRAnchorTuning{});

    EXPECT_GT(a.state().widthMeters, 1.0f); // grew
    const Vec3 pinNow = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    EXPECT_NEAR(pinNow.x, pinTL.x, 1e-3f);
    EXPECT_NEAR(pinNow.y, pinTL.y, 1e-3f);
    EXPECT_NEAR(pinNow.z, pinTL.z, 1e-3f);
}

TEST(XRGrabResize, DragInShrinks) {
    const float aspect = 0.5f;
    CXRAnchor   a      = makeLocalQuad(1.5f, aspect);
    const Vec3  pinTL  = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    const Vec3  grabbed = cornerWorld(a, XR_REGION_CORNER_BR, aspect);

    a.beginResize(XR_HAND_LEFT, XR_REGION_CORNER_BR, SXRPose{grabbed, Quat{0.f, 0.f, 0.f, 1.f}}, aspect);
    const Vec3    diagUnit = (grabbed - pinTL).normalized();
    const SXRPose gripNow{grabbed - diagUnit * 0.3f, Quat{0.f, 0.f, 0.f, 1.f}}; // inward
    a.grabResizeCorner(gripNow, localSolveIn(), SXRAnchorTuning{});

    EXPECT_LT(a.state().widthMeters, 1.5f); // shrank
    const Vec3 pinNow = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    EXPECT_NEAR(pinNow.x, pinTL.x, 1e-3f); // opposite corner still pinned
    EXPECT_NEAR(pinNow.y, pinTL.y, 1e-3f);
}

TEST(XRGrabResize, ClampsToMaxWidth) {
    const float aspect = 0.5f;
    CXRAnchor   a      = makeLocalQuad(1.0f, aspect);
    const Vec3  pinTL  = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    const Vec3  grabbed = cornerWorld(a, XR_REGION_CORNER_BR, aspect);

    a.beginResize(XR_HAND_RIGHT, XR_REGION_CORNER_BR, SXRPose{grabbed, Quat{0.f, 0.f, 0.f, 1.f}}, aspect);
    const Vec3    diagUnit = (grabbed - pinTL).normalized();
    const SXRPose gripNow{grabbed + diagUnit * 50.f, Quat{0.f, 0.f, 0.f, 1.f}}; // absurd drag out
    a.grabResizeCorner(gripNow, localSolveIn(), SXRAnchorTuning{});
    EXPECT_NEAR(a.state().widthMeters, XR_WIDTH_MAX, 1e-4f);
    // Even clamped, the pinned corner holds (center recomputed from the clamped diagonal).
    const Vec3 pinNow = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    EXPECT_NEAR(pinNow.x, pinTL.x, 1e-3f);
    EXPECT_NEAR(pinNow.y, pinTL.y, 1e-3f);
}

TEST(XRGrabResize, EndResizeClearsAndLatchesSize) {
    const float aspect = 0.5f;
    CXRAnchor   a      = makeLocalQuad(1.0f, aspect);
    const Vec3  pinTL  = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    const Vec3  grabbed = cornerWorld(a, XR_REGION_CORNER_BR, aspect);

    a.beginResize(XR_HAND_RIGHT, XR_REGION_CORNER_BR, SXRPose{grabbed, Quat{0.f, 0.f, 0.f, 1.f}}, aspect);
    const Vec3 diagUnit = (grabbed - pinTL).normalized();
    // Final (latched) grip corresponds to a calm +0.2 m out; a release jerk is simply not passed.
    const SXRPose latched{grabbed + diagUnit * 0.2f, Quat{0.f, 0.f, 0.f, 1.f}};
    a.endResize(latched, localSolveIn(), SXRAnchorTuning{});

    EXPECT_FALSE(a.grabbed());
    EXPECT_EQ(a.grabKind(), XR_GRABKIND_NONE);
    EXPECT_GT(a.state().widthMeters, 1.0f);
    const Vec3 pinNow = cornerWorld(a, XR_REGION_CORNER_TL, aspect);
    EXPECT_NEAR(pinNow.x, pinTL.x, 1e-3f);
    EXPECT_NEAR(pinNow.y, pinTL.y, 1e-3f);
}

TEST(XRGrabResize, HeadAnchoredResizeUpdatesSizeStaysHead) {
    // On a head-leashed quad, a corner resize updates size and keeps the anchor in HEAD mode (the
    // offset semantics are preserved; it does not convert to LOCAL or device-lock).
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode        = XR_ANCHOR_HEAD;
    st.anchorPose  = SXRPose{Vec3{0.f, 0.f, -2.f}, Quat{0.f, 0.f, 0.f, 1.f}};
    st.widthMeters = 1.0f;
    a.initFromState(st);
    SXRSolveInput in = localSolveIn();
    a.solve(in, SXRAnchorTuning{});
    ASSERT_TRUE(a.hasLastWorld());

    const SXRPose grip0{a.lastWorld().pos, Quat{0.f, 0.f, 0.f, 1.f}};
    a.beginResize(XR_HAND_RIGHT, XR_REGION_CORNER_BR, grip0, 0.5f);
    const SXRPose gripNow{grip0.pos + Vec3{0.4f, -0.2f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
    a.grabResizeCorner(gripNow, in, SXRAnchorTuning{});
    EXPECT_EQ(a.state().mode, XR_ANCHOR_HEAD); // still head-leashed
    EXPECT_GT(a.state().widthMeters, 1.0f);    // and resized
    a.endResize(gripNow, in, SXRAnchorTuning{});
    EXPECT_EQ(a.state().mode, XR_ANCHOR_HEAD);
    EXPECT_FALSE(a.grabbed());
}
