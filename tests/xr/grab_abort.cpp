#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/grab_abort.cpp — CXRAnchor::abortGrab, the release path for a grab ended by something
// other than the hand holding it. `openxr view off` is the caller: the view latch keeps every layer
// alive but takes it out of the frame loop's presentation set, so CXRInput is handed no target for
// a monitor being carried and force-releases only its OWN bookkeeping (that branch exists for a
// layer DESTROYED mid-grab). Nothing else would clear the anchor's grab flags, and a monitor stuck
// grabbed comes back device-locked on `view on`, reports grabbed in status forever, and is barred
// from viewpoint eligibility (which requires !grabbed()) for the rest of the session.
//
// These pin the contract abortGrab owes that caller: the flags always drop, and the quad stays
// exactly where it was last displayed.

namespace {
    SXRSolveInput localSolveIn() {
        SXRSolveInput in;
        in.view = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        in.pxW  = 1000;
        in.pxH  = 500;
        return in;
    }

    CXRAnchor makeLocalQuad(float w0 = 1.0f, Vec3 center = Vec3{0.f, 1.5f, -2.f}) {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode        = XR_ANCHOR_LOCAL;
        st.anchorPose  = SXRPose{center, Quat{0.f, 0.f, 0.f, 1.f}};
        st.widthMeters = w0;
        a.initFromState(st);
        a.solve(localSolveIn(), SXRAnchorTuning{});
        return a;
    }
}

TEST(XRGrabAbort, MoveGrabDropsTheLockAndKeepsTheDisplayedPose) {
    CXRAnchor a = makeLocalQuad();
    ASSERT_TRUE(a.hasLastWorld());

    const SXRPose grip0{Vec3{0.2f, 1.5f, -1.0f}, Quat{0.f, 0.f, 0.f, 1.f}};
    a.beginGrab(XR_HAND_RIGHT, grip0);
    EXPECT_TRUE(a.grabbed());

    // Carry the quad 0.4 m to the right, then hide the view mid-carry.
    SXRSolveInput in = localSolveIn();
    SXRPose       gripNow = grip0;
    gripNow.pos.x += 0.4f;
    in.gripRight            = gripNow;
    const SXRSolveResult r  = a.solve(in, SXRAnchorTuning{});
    EXPECT_NEAR(r.worldPose.pos.x, 0.4f, 1e-4f);

    EXPECT_TRUE(a.abortGrab(in, SXRAnchorTuning{}));

    EXPECT_FALSE(a.grabbed());
    EXPECT_EQ(a.grabKind(), XR_GRABKIND_NONE);
    // Re-anchored where the user last SAW it, in the persistent mode — not back at the pre-grab
    // pose, and not left riding the hand.
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL);
    EXPECT_NEAR(a.state().anchorPose.pos.x, 0.4f, 1e-4f);
    EXPECT_NEAR(a.state().anchorPose.pos.y, 1.5f, 1e-4f);
    EXPECT_NEAR(a.state().anchorPose.pos.z, -2.f, 1e-4f);

    // The quad no longer follows the hand: move the grip a long way and re-solve.
    in.gripRight->pos.x += 5.f;
    const SXRSolveResult after = a.solve(in, SXRAnchorTuning{});
    EXPECT_NEAR(after.worldPose.pos.x, 0.4f, 1e-4f);
}

TEST(XRGrabAbort, ResizeGrabDropsTheResizeAndKeepsTheDraggedSize) {
    const float aspect = 0.5f;
    CXRAnchor   a      = makeLocalQuad(1.0f);

    const SXRPose grip0{Vec3{0.5f, 1.25f, -2.f}, Quat{0.f, 0.f, 0.f, 1.f}};
    a.beginResize(XR_HAND_RIGHT, XR_REGION_CORNER_BR, grip0, aspect);
    EXPECT_TRUE(a.grabbed());
    EXPECT_EQ(a.grabKind(), XR_GRABKIND_RESIZE);

    SXRPose gripNow = grip0;
    gripNow.pos.x += 0.2f;
    gripNow.pos.y -= 0.1f;
    a.grabResizeCorner(gripNow, localSolveIn(), SXRAnchorTuning{});
    const float DRAGGEDWIDTH = a.state().widthMeters;
    const auto  DRAGGEDPOSE  = a.state().anchorPose;
    EXPECT_GT(DRAGGEDWIDTH, 1.0f);

    EXPECT_TRUE(a.abortGrab(localSolveIn(), SXRAnchorTuning{}));

    EXPECT_FALSE(a.grabbed());
    EXPECT_EQ(a.grabKind(), XR_GRABKIND_NONE);
    // The size AND the pose the user dragged to both survive — the drag re-anchored at every step,
    // so aborting only has to stop the resize, never undo or re-apply it. In particular it must not
    // re-anchor from m_lastWorld: that is one solve behind the drag, and using it would rewind the
    // last step and unpin the corner the resize was holding.
    EXPECT_FLOAT_EQ(a.state().widthMeters, DRAGGEDWIDTH);
    EXPECT_NEAR(a.state().anchorPose.pos.x, DRAGGEDPOSE.pos.x, 1e-4f);
    EXPECT_NEAR(a.state().anchorPose.pos.y, DRAGGEDPOSE.pos.y, 1e-4f);
    EXPECT_NEAR(a.state().anchorPose.pos.z, DRAGGEDPOSE.pos.z, 1e-4f);
}

TEST(XRGrabAbort, NoGrabIsANoOpAndAbortIsIdempotent) {
    CXRAnchor  a       = makeLocalQuad();
    const auto BEFORE  = a.state();

    EXPECT_FALSE(a.abortGrab(localSolveIn(), SXRAnchorTuning{}));
    EXPECT_FALSE(a.grabbed());
    EXPECT_EQ(a.state().widthMeters, BEFORE.widthMeters);
    EXPECT_NEAR(a.state().anchorPose.pos.z, BEFORE.anchorPose.pos.z, 1e-6f);

    // …and a second abort after a real one reports nothing to do, so the caller's "did I release
    // anything" log stays honest across repeated `openxr view off`.
    a.beginGrab(XR_HAND_LEFT, SXRPose{Vec3{0.f, 1.5f, -1.f}, Quat{0.f, 0.f, 0.f, 1.f}});
    EXPECT_TRUE(a.abortGrab(localSolveIn(), SXRAnchorTuning{}));
    EXPECT_FALSE(a.abortGrab(localSolveIn(), SXRAnchorTuning{}));
}

TEST(XRGrabAbort, GrabBeforeAnySolveStillReleases) {
    // A grab that began before a pose was ever composed has no lastWorld to re-anchor from. The
    // flags must still drop — that is the wedge this exists to prevent.
    CXRAnchor      a;
    SXRAnchorState st;
    st.mode        = XR_ANCHOR_LOCAL;
    st.anchorPose  = SXRPose{Vec3{0.f, 1.5f, -2.f}, Quat{0.f, 0.f, 0.f, 1.f}};
    st.widthMeters = 1.f;
    a.initFromState(st);
    ASSERT_FALSE(a.hasLastWorld());

    a.beginGrab(XR_HAND_RIGHT, SXRPose{Vec3{0.f, 1.5f, -1.f}, Quat{0.f, 0.f, 0.f, 1.f}});
    EXPECT_TRUE(a.grabbed());
    EXPECT_TRUE(a.abortGrab(localSolveIn(), SXRAnchorTuning{}));
    EXPECT_FALSE(a.grabbed());
    EXPECT_NEAR(a.state().anchorPose.pos.z, -2.f, 1e-6f);
}
