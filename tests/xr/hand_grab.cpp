#include <openxr/XRMath.hpp>
#include <openxr/XRAnchor.hpp>

#include <gtest/gtest.h>

#include <cmath>

using namespace OpenXR;

// tests/xr/hand_grab.cpp — WP-G5 hand pinch grab (docs/openxr/research/04-grabbable-borders.md
// §5.3/§8). Pure: builds + runs with no OpenXR runtime. Covers the profile→input-kind mapping
// (active-device detection), the openxr:hand_grab parse + gesture-value + anchor-selection helpers,
// and the CXRAnchor pinch-anchored MOVE grab (carry against the pinch pose + PINCH space selector +
// reanchor on release). The live headset behavior (a real hand-interaction profile, pinch strength,
// pinch pose) is Quest-only and out of scope here — these lock the pure decision + math the live
// path is built on.

// ---- active-device detection: profile path -> input kind ----

TEST(XRHandGrab, ProfileMapsToInputKind) {
    EXPECT_EQ(xrInputKindForProfile("/interaction_profiles/ext/hand_interaction_ext"), XR_INPUT_HANDS);
    // Every controller profile (and the string constant) maps to CONTROLLER.
    EXPECT_EQ(xrInputKindForProfile("/interaction_profiles/valve/index_controller"), XR_INPUT_CONTROLLER);
    EXPECT_EQ(xrInputKindForProfile("/interaction_profiles/oculus/touch_controller"), XR_INPUT_CONTROLLER);
    EXPECT_EQ(xrInputKindForProfile("/interaction_profiles/khr/simple_controller"), XR_INPUT_CONTROLLER);
    // We bind no hand actions for microsoft/hand_interaction, so it is treated as a controller.
    EXPECT_EQ(xrInputKindForProfile("/interaction_profiles/microsoft/hand_interaction"), XR_INPUT_CONTROLLER);
    // Empty / unknown (no profile bound yet) -> controller (the safe default).
    EXPECT_EQ(xrInputKindForProfile(""), XR_INPUT_CONTROLLER);
    EXPECT_EQ(xrInputKindForProfile("/interaction_profiles/made/up"), XR_INPUT_CONTROLLER);
    EXPECT_STREQ(xrInputKindName(XR_INPUT_HANDS), "hands");
    EXPECT_STREQ(xrInputKindName(XR_INPUT_CONTROLLER), "controllers");
}

// ---- openxr:hand_grab parse ----

TEST(XRHandGrab, ParseHandGrab) {
    EXPECT_EQ(xrParseHandGrab("pinch"), XR_HANDGRAB_PINCH);
    EXPECT_EQ(xrParseHandGrab("grasp"), XR_HANDGRAB_GRASP);
    EXPECT_EQ(xrParseHandGrab("both"), XR_HANDGRAB_BOTH);
    // Anything else (incl. empty / typo) falls back to the pinch default.
    EXPECT_EQ(xrParseHandGrab(""), XR_HANDGRAB_PINCH);
    EXPECT_EQ(xrParseHandGrab("fist"), XR_HANDGRAB_PINCH);
    EXPECT_STREQ(xrHandGrabName(XR_HANDGRAB_PINCH), "pinch");
    EXPECT_STREQ(xrHandGrabName(XR_HANDGRAB_GRASP), "grasp");
    EXPECT_STREQ(xrHandGrabName(XR_HANDGRAB_BOTH), "both");
}

// ---- gesture value selection (fed to the grab Schmitt when hands are active) ----

TEST(XRHandGrab, GrabValueByMode) {
    const float pinch = 0.8f, grasp = 0.3f;
    EXPECT_FLOAT_EQ(xrHandGrabValue(XR_HANDGRAB_PINCH, pinch, grasp), pinch);
    EXPECT_FLOAT_EQ(xrHandGrabValue(XR_HANDGRAB_GRASP, pinch, grasp), grasp);
    EXPECT_FLOAT_EQ(xrHandGrabValue(XR_HANDGRAB_BOTH, pinch, grasp), pinch); // max
    EXPECT_FLOAT_EQ(xrHandGrabValue(XR_HANDGRAB_BOTH, 0.1f, 0.9f), 0.9f);    // max the other way
}

// ---- which pose a hand grab anchors to (pinch pose vs wrist grip) ----

TEST(XRHandGrab, UsesPinchByMode) {
    // PINCH: always the pinch pose.
    EXPECT_TRUE(xrHandGrabUsesPinch(XR_HANDGRAB_PINCH, 0.9f, 0.0f));
    EXPECT_TRUE(xrHandGrabUsesPinch(XR_HANDGRAB_PINCH, 0.0f, 0.9f));
    // GRASP: never (grasp_ext has no pose -> wrist grip).
    EXPECT_FALSE(xrHandGrabUsesPinch(XR_HANDGRAB_GRASP, 0.9f, 0.0f));
    EXPECT_FALSE(xrHandGrabUsesPinch(XR_HANDGRAB_GRASP, 0.0f, 0.9f));
    // BOTH: the stronger contributor wins; pinch on a tie (the steadier anchor).
    EXPECT_TRUE(xrHandGrabUsesPinch(XR_HANDGRAB_BOTH, 0.8f, 0.2f));
    EXPECT_FALSE(xrHandGrabUsesPinch(XR_HANDGRAB_BOTH, 0.2f, 0.8f));
    EXPECT_TRUE(xrHandGrabUsesPinch(XR_HANDGRAB_BOTH, 0.5f, 0.5f));
}

// ---- CXRAnchor: pinch-anchored MOVE grab ----

namespace {
    CXRAnchor makeLocalQuad(Vec3 center = Vec3{0.f, 1.5f, -2.f}, float w0 = 1.0f) {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode        = XR_ANCHOR_LOCAL;
        st.anchorPose  = SXRPose{center, Quat{0.f, 0.f, 0.f, 1.f}};
        st.widthMeters = w0;
        a.initFromState(st);
        SXRSolveInput in;
        in.view = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        in.pxW  = 1;
        in.pxH  = 1;
        a.solve(in, SXRAnchorTuning{});
        return a;
    }
}

TEST(XRHandGrab, PinchGrabSelectsPinchSpaceAndCarriesPinchPose) {
    CXRAnchor a = makeLocalQuad(Vec3{0.f, 1.5f, -2.f});
    ASSERT_TRUE(a.hasLastWorld());

    // Begin a pinch-anchored grab with the RIGHT hand. The offset is captured against the pinch
    // pose, NOT the grip pose.
    const SXRPose pinch0{Vec3{0.2f, 1.5f, -1.0f}, Quat{0.f, 0.f, 0.f, 1.f}};
    a.beginGrab(XR_HAND_RIGHT, pinch0, /*usePinch*/ true);
    EXPECT_TRUE(a.grabbed());
    EXPECT_EQ(a.grabKind(), XR_GRABKIND_MOVE);

    SXRSolveInput in;
    in.view       = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
    in.pinchRight = pinch0;
    // A wild grip pose must NOT influence a pinch grab (proves it reads pinch, not grip).
    in.gripRight = SXRPose{Vec3{9.f, 9.f, 9.f}, Quat{0.f, 0.f, 0.f, 1.f}};

    const SXRSolveResult r0 = a.solve(in, SXRAnchorTuning{});
    EXPECT_EQ(r0.space, XR_SPACE_PINCH_RIGHT);
    // Pinch unmoved -> quad stays where it was seeded.
    EXPECT_NEAR(r0.worldPose.pos.x, 0.f, 1e-4f);
    EXPECT_NEAR(r0.worldPose.pos.y, 1.5f, 1e-4f);
    EXPECT_NEAR(r0.worldPose.pos.z, -2.f, 1e-4f);

    // Move the pinch pose +0.3 m in x: the quad follows rigidly (identity rotations).
    SXRPose pinch1 = pinch0;
    pinch1.pos.x += 0.3f;
    in.pinchRight  = pinch1;
    const SXRSolveResult r1 = a.solve(in, SXRAnchorTuning{});
    EXPECT_EQ(r1.space, XR_SPACE_PINCH_RIGHT);
    EXPECT_NEAR(r1.worldPose.pos.x, 0.3f, 1e-4f);
    EXPECT_NEAR(r1.worldPose.pos.z, -2.f, 1e-4f);

    // Release: reanchor from the (explicit) release world pose bakes it into LOCAL.
    a.endGrab(r1.worldPose, in, SXRAnchorTuning{});
    EXPECT_FALSE(a.grabbed());
    EXPECT_EQ(a.state().mode, XR_ANCHOR_LOCAL);
    EXPECT_NEAR(a.state().anchorPose.pos.x, 0.3f, 1e-4f);
    EXPECT_NEAR(a.state().anchorPose.pos.z, -2.f, 1e-4f);
}

TEST(XRHandGrab, GraspGrabStillUsesGripSpace) {
    // usePinch=false (grasp / controller): the grab must still device-lock to the GRIP space, so a
    // mixed session or hand_grab=grasp keeps the old wrist-anchored behavior.
    CXRAnchor a = makeLocalQuad(Vec3{0.f, 1.5f, -2.f});
    const SXRPose grip0{Vec3{0.f, 1.4f, -0.8f}, Quat{0.f, 0.f, 0.f, 1.f}};
    a.beginGrab(XR_HAND_LEFT, grip0, /*usePinch*/ false);

    SXRSolveInput in;
    in.view     = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
    in.gripLeft = grip0;
    // pinch present but must be ignored for a grasp grab.
    in.pinchLeft = SXRPose{Vec3{5.f, 5.f, 5.f}, Quat{0.f, 0.f, 0.f, 1.f}};

    const SXRSolveResult r = a.solve(in, SXRAnchorTuning{});
    EXPECT_EQ(r.space, XR_SPACE_GRIP_LEFT);
    EXPECT_NEAR(r.worldPose.pos.x, 0.f, 1e-4f);
    EXPECT_NEAR(r.worldPose.pos.z, -2.f, 1e-4f);
}
