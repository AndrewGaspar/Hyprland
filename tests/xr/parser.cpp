#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using namespace OpenXR;

// The four doc 05 §2.4 examples must parse and materialize.

TEST(XRParser, ExampleLocal) {
    auto r = parseXRMonitorLine("XR-code, 2560x1440@90, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.8");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_name, "XR-code");
    ASSERT_TRUE(r->m_resolution.has_value());
    EXPECT_EQ(*r->m_resolution, Vector2D(2560, 1440));
    ASSERT_TRUE(r->m_refreshRate.has_value());
    EXPECT_FLOAT_EQ(*r->m_refreshRate, 90.f);
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_LOCAL);
    EXPECT_TRUE(r->m_anchorProvided);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.x, 0.f);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.y, 1.4f);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.z, -1.5f);
    // yaw:0 -> identity rotation
    EXPECT_NEAR(qYawOf(r->m_anchor.anchorPose.rot, 0.f), 0.f, 1e-5f);
    ASSERT_TRUE(r->m_sizeMeters.has_value());
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 1.8f);
}

TEST(XRParser, ExampleHead) {
    auto r = parseXRMonitorLine("XR-chat, 1280x720, anchor:head offset:0.4,-0.2,-1.0, size:0.6");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_name, "XR-chat");
    EXPECT_EQ(*r->m_resolution, Vector2D(1280, 720));
    EXPECT_FALSE(r->m_refreshRate.has_value()); // default (60) applied downstream
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_HEAD);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.x, 0.4f);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.y, -0.2f);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.z, -1.0f);
    // head display orientation is lookAt-driven; stored rot is identity
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.rot.w, 1.0f);
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 0.6f);
}

TEST(XRParser, ExampleBody) {
    auto r = parseXRMonitorLine("XR-music, 1920x1080@60, anchor:body offset:-0.8,1.2,-1.2, size:0.9");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_BODY);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.x, -0.8f);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.y, 1.2f);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.z, -1.2f);
    EXPECT_FLOAT_EQ(*r->m_refreshRate, 60.f);
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 0.9f);
}

TEST(XRParser, ExampleDevice) {
    auto r = parseXRMonitorLine("XR-palette, 800x800, anchor:device:left offset:0,0.08,-0.05, size:0.25");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_DEVICE);
    EXPECT_EQ(r->m_anchor.device, XR_HAND_LEFT);
    EXPECT_FLOAT_EQ(r->m_anchor.anchorPose.pos.y, 0.08f);
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 0.25f);
}

TEST(XRParser, DeviceRight) {
    auto r = parseXRMonitorLine("XR-r, 800x800, anchor:device:right offset:0,0,0");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_DEVICE);
    EXPECT_EQ(r->m_anchor.device, XR_HAND_RIGHT);
}

TEST(XRParser, RefreshDefaultingOmitted) {
    auto r = parseXRMonitorLine("XR-x, 640x480, anchor:local pos:0,0,-1");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_FALSE(r->m_refreshRate.has_value());
    EXPECT_FALSE(r->m_sizeMeters.has_value()); // no size: => openxr:default_size downstream
}

TEST(XRParser, PitchOptional) {
    auto r = parseXRMonitorLine("XR-p, 640x480, anchor:local pos:0,1,-1 yaw:30 pitch:-10");
    ASSERT_TRUE(r.has_value()) << r.error();
    // yaw/pitch are encoded into the stored quat (rot = qFromYaw ∘ qFromPitch, doc 03 §7).
    const Vec3      f       = qRotate(r->m_anchor.anchorPose.rot, Vec3{0.f, 0.f, -1.f});
    constexpr float RAD2DEG = 180.f / 3.14159265358979323846f;
    const float     pitch   = std::asin(std::clamp(f.y, -1.f, 1.f)) * RAD2DEG;
    const float     yaw     = std::atan2(-f.x, -f.z) * RAD2DEG;
    EXPECT_NEAR(yaw, 30.f, 1e-3f);
    EXPECT_NEAR(pitch, -10.f, 1e-3f);
}

// -------- malformed inputs --------

TEST(XRParser, ErrMissingName) {
    EXPECT_FALSE(parseXRMonitorLine(" , 640x480, anchor:local pos:0,0,-1").has_value());
}

TEST(XRParser, ErrMissingAnchor) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480").has_value());
}

TEST(XRParser, ErrMissingPos) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:local yaw:0").has_value());
}

TEST(XRParser, ErrBadFloat) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:local pos:0,abc,-1").has_value());
}

TEST(XRParser, ErrBadMode) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640, anchor:local pos:0,0,-1").has_value());
}

TEST(XRParser, ErrUnknownAnchorMode) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:elbow pos:0,0,-1").has_value());
}

TEST(XRParser, ErrUnknownKey) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:local pos:0,0,-1 wibble:3").has_value());
}

TEST(XRParser, ErrDeviceSide) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:device:middle offset:0,0,0").has_value());
}

TEST(XRParser, ErrPosOnLeash) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:head pos:0,0,-1").has_value());
}

TEST(XRParser, ErrOffsetOnLocal) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:local offset:0,0,-1").has_value());
}

// -------- create-verb parser --------

TEST(XRParser, CreateNameOnly) {
    auto r = parseXRMonitorCreateArgs("XR-scratch");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_name, "XR-scratch");
    EXPECT_FALSE(r->m_resolution.has_value()); // caller defaults
}

TEST(XRParser, CreateNameAndMode) {
    auto r = parseXRMonitorCreateArgs("XR-scratch 2560x1440@120");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r->m_resolution, Vector2D(2560, 1440));
    EXPECT_FLOAT_EQ(*r->m_refreshRate, 120.f);
}

TEST(XRParser, CreateFull) {
    auto r = parseXRMonitorCreateArgs("XR-scratch 1920x1080 anchor:head offset:0.4,-0.2,-1.0 size:0.6");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(*r->m_resolution, Vector2D(1920, 1080));
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_HEAD);
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 0.6f);
}

TEST(XRParser, CreateAnchorNoMode) {
    auto r = parseXRMonitorCreateArgs("XR-scratch anchor:local pos:0,1,-1");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_FALSE(r->m_resolution.has_value());
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_LOCAL);
}

// -------- layout round-trip (WP13, doc 03 §7 / `hyprctl openxr layout`) --------
//
// serializeXRMonitorLine (XRMonitorConfig.cpp) is the pure formatter COpenXRManager::layoutDump()
// uses to emit paste-ready `xrmonitor = ...` lines from the live layout. These tests feed its
// output straight back through parseXRMonitorLine and check the re-parsed params are equivalent
// to what was serialized — the round-trip the WP5 doc 03 §7 acceptance criterion and the WP13
// roadmap entry ask for.

TEST(XRLayoutRoundTrip, Local) {
    SXRAnchorState anchor;
    anchor.mode         = XR_ANCHOR_LOCAL;
    anchor.widthMeters   = 1.8f;
    SXRPose pose;
    pose.pos = Vec3{0.25f, 1.4f, -1.5f};
    pose.rot = qMul(qFromYaw(30.f * (float)M_PI / 180.f), qFromPitch(-10.f * (float)M_PI / 180.f));

    const std::string line = serializeXRMonitorLine("XR-rt-local", Vector2D(2560, 1440), 90.f, anchor, pose, 1.8f);

    auto reparsed = parseXRMonitorLine(line.substr(std::string("xrmonitor = ").size()));
    ASSERT_TRUE(reparsed.has_value()) << reparsed.error() << " (line was: " << line << ")";
    EXPECT_EQ(reparsed->m_name, "XR-rt-local");
    ASSERT_TRUE(reparsed->m_resolution.has_value());
    EXPECT_EQ(*reparsed->m_resolution, Vector2D(2560, 1440));
    ASSERT_TRUE(reparsed->m_refreshRate.has_value());
    EXPECT_FLOAT_EQ(*reparsed->m_refreshRate, 90.f);
    EXPECT_EQ(reparsed->m_anchor.mode, XR_ANCHOR_LOCAL);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.x, pose.pos.x, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.y, pose.pos.y, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.z, pose.pos.z, 1e-3f);
    EXPECT_NEAR(qAngleBetween(reparsed->m_anchor.anchorPose.rot, pose.rot), 0.f, 1e-2f);
    ASSERT_TRUE(reparsed->m_sizeMeters.has_value());
    EXPECT_FLOAT_EQ(*reparsed->m_sizeMeters, 1.8f);
}

TEST(XRLayoutRoundTrip, Head) {
    // head prints no rotation (display orientation is lookAt-driven, doc 03 §3.2) — the stored
    // rot is irrelevant to the serialized line and the parser always leaves it identity.
    SXRAnchorState anchor;
    anchor.mode = XR_ANCHOR_HEAD;
    SXRPose pose;
    pose.pos = Vec3{0.4f, -0.2f, -1.0f};
    pose.rot = Quat{}; // identity, per §3.2

    const std::string line = serializeXRMonitorLine("XR-rt-head", Vector2D(1280, 720), std::nullopt, anchor, pose, 0.6f);

    auto reparsed = parseXRMonitorLine(line.substr(std::string("xrmonitor = ").size()));
    ASSERT_TRUE(reparsed.has_value()) << reparsed.error() << " (line was: " << line << ")";
    EXPECT_EQ(reparsed->m_anchor.mode, XR_ANCHOR_HEAD);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.x, pose.pos.x, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.y, pose.pos.y, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.z, pose.pos.z, 1e-3f);
    EXPECT_FLOAT_EQ(*reparsed->m_sizeMeters, 0.6f);
}

TEST(XRLayoutRoundTrip, Body) {
    // body prints yaw only (pitch/roll forced to 0, doc 03 §3.3).
    SXRAnchorState anchor;
    anchor.mode = XR_ANCHOR_BODY;
    SXRPose pose;
    pose.pos = Vec3{-0.8f, 1.2f, -1.2f};
    pose.rot = qFromYaw(45.f * (float)M_PI / 180.f);

    const std::string line = serializeXRMonitorLine("XR-rt-body", Vector2D(1920, 1080), 60.f, anchor, pose, 0.9f);

    auto reparsed = parseXRMonitorLine(line.substr(std::string("xrmonitor = ").size()));
    ASSERT_TRUE(reparsed.has_value()) << reparsed.error() << " (line was: " << line << ")";
    EXPECT_EQ(reparsed->m_anchor.mode, XR_ANCHOR_BODY);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.x, pose.pos.x, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.y, pose.pos.y, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.z, pose.pos.z, 1e-3f);
    EXPECT_NEAR(qYawOf(reparsed->m_anchor.anchorPose.rot, 0.f), qYawOf(pose.rot, 0.f), 1e-2f);
}

TEST(XRLayoutRoundTrip, Device) {
    SXRAnchorState anchor;
    anchor.mode   = XR_ANCHOR_DEVICE;
    anchor.device = XR_HAND_RIGHT;
    SXRPose pose;
    pose.pos = Vec3{0.f, 0.08f, -0.05f};
    pose.rot = Quat{};

    const std::string line = serializeXRMonitorLine("XR-rt-device", Vector2D(800, 800), std::nullopt, anchor, pose, 0.25f);

    auto reparsed = parseXRMonitorLine(line.substr(std::string("xrmonitor = ").size()));
    ASSERT_TRUE(reparsed.has_value()) << reparsed.error() << " (line was: " << line << ")";
    EXPECT_EQ(reparsed->m_anchor.mode, XR_ANCHOR_DEVICE);
    EXPECT_EQ(reparsed->m_anchor.device, XR_HAND_RIGHT);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.y, pose.pos.y, 1e-3f);
    EXPECT_FLOAT_EQ(*reparsed->m_sizeMeters, 0.25f);
}

// Adaptive anchoring (research/13 §6.2/§6.4): the decorator tokens on an anchor:local line survive
// a serialize -> parse round-trip. serializeXRMonitorLine is fed the SAVED dock pose (as layoutDump
// does for an adaptive monitor), so the desk pose is what round-trips — not any live roam pose.
TEST(XRLayoutRoundTrip, AdaptiveLocal) {
    SXRAnchorState anchor;
    anchor.mode                   = XR_ANCHOR_LOCAL;
    anchor.adaptive.enabled       = true;
    anchor.adaptive.roamMode      = XR_ANCHOR_HEAD;
    anchor.adaptive.roamModeSet   = true;
    anchor.adaptive.roamOffset    = SXRPose{Vec3{0.f, 1.35f, -1.2f}, Quat{}};
    anchor.adaptive.hasRoamOffset = true;
    anchor.adaptive.leaveRadius   = 2.0f;
    anchor.adaptive.returnRadius  = 1.2f;
    anchor.adaptive.carryOverride = true;
    anchor.adaptive.carryOffset   = true;
    SXRPose pose;
    pose.pos = Vec3{0.3f, 1.05f, -1.2f}; // the desk pose
    pose.rot = Quat{};

    const std::string line = serializeXRMonitorLine("XR-rt-adaptive", Vector2D(1920, 1080), 60.f, anchor, pose, 1.8f);

    auto reparsed = parseXRMonitorLine(line.substr(std::string("xrmonitor = ").size()));
    ASSERT_TRUE(reparsed.has_value()) << reparsed.error() << " (line was: " << line << ")";
    const auto& ad = reparsed->m_anchor.adaptive;
    EXPECT_TRUE(ad.enabled);
    EXPECT_EQ(ad.roamMode, XR_ANCHOR_HEAD);
    EXPECT_TRUE(ad.roamModeSet);
    EXPECT_TRUE(ad.hasRoamOffset);
    EXPECT_NEAR(ad.roamOffset.pos.y, 1.35f, 1e-3f);
    EXPECT_NEAR(ad.roamOffset.pos.z, -1.2f, 1e-3f);
    EXPECT_NEAR(ad.leaveRadius, 2.0f, 1e-3f);
    EXPECT_NEAR(ad.returnRadius, 1.2f, 1e-3f);
    EXPECT_TRUE(ad.carryOverride);
    EXPECT_TRUE(ad.carryOffset);
    // The saved desk pose (not a roam pose) round-trips.
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.x, 0.3f, 1e-3f);
    EXPECT_NEAR(reparsed->m_anchor.anchorPose.pos.z, -1.2f, 1e-3f);
}

// The global roam-mode default is respected: an `adaptive:on` line with no `roam:` token leaves
// roamModeSet false so the runtime defers to openxr:adaptive_roam_mode, and re-serializes without a
// roam token (so a save/reload keeps deferring).
TEST(XRLayoutRoundTrip, AdaptiveDefersRoamMode) {
    auto p = parseXRMonitorLine("XR-defadapt, 1920x1080, anchor:local pos:0,1.4,-1.5 adaptive:on");
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_TRUE(p->m_anchor.adaptive.enabled);
    EXPECT_FALSE(p->m_anchor.adaptive.roamModeSet);

    const std::string line = serializeXRMonitorLine("XR-defadapt", Vector2D(1920, 1080), std::nullopt, p->m_anchor, p->m_anchor.anchorPose, 1.6f);
    EXPECT_EQ(line.find("roam:"), std::string::npos) << line;
    EXPECT_NE(line.find("adaptive:on"), std::string::npos) << line;
}

// Adaptive tokens are rejected on non-local anchors (the decorator sits on the desk pose).
TEST(XRParser, AdaptiveRejectedOnHead) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:head offset:0,0,-1 adaptive:on").has_value());
}

// return radius must be < leave radius (hysteresis) when both are given per-monitor.
TEST(XRParser, AdaptiveRejectsInvertedRadii) {
    EXPECT_FALSE(parseXRMonitorLine("XR-x, 640x480, anchor:local pos:0,0,-1 adaptive:on leave:1.0 return:1.5").has_value());
}
