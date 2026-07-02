#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

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
    EXPECT_FLOAT_EQ(r->m_anchor.posX, 0.f);
    EXPECT_FLOAT_EQ(r->m_anchor.posY, 1.4f);
    EXPECT_FLOAT_EQ(r->m_anchor.posZ, -1.5f);
    EXPECT_TRUE(r->m_anchor.hasYaw);
    EXPECT_FLOAT_EQ(r->m_anchor.yawDeg, 0.f);
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
    EXPECT_FLOAT_EQ(r->m_anchor.posX, 0.4f);
    EXPECT_FLOAT_EQ(r->m_anchor.posY, -0.2f);
    EXPECT_FLOAT_EQ(r->m_anchor.posZ, -1.0f);
    EXPECT_FALSE(r->m_anchor.hasYaw);
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 0.6f);
}

TEST(XRParser, ExampleBody) {
    auto r = parseXRMonitorLine("XR-music, 1920x1080@60, anchor:body offset:-0.8,1.2,-1.2, size:0.9");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_BODY);
    EXPECT_FLOAT_EQ(r->m_anchor.posX, -0.8f);
    EXPECT_FLOAT_EQ(r->m_anchor.posY, 1.2f);
    EXPECT_FLOAT_EQ(r->m_anchor.posZ, -1.2f);
    EXPECT_FLOAT_EQ(*r->m_refreshRate, 60.f);
    EXPECT_FLOAT_EQ(*r->m_sizeMeters, 0.9f);
}

TEST(XRParser, ExampleDevice) {
    auto r = parseXRMonitorLine("XR-palette, 800x800, anchor:device:left offset:0,0.08,-0.05, size:0.25");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_anchor.mode, XR_ANCHOR_DEVICE);
    EXPECT_EQ(r->m_anchor.device, XR_HAND_LEFT);
    EXPECT_FLOAT_EQ(r->m_anchor.posY, 0.08f);
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
    EXPECT_TRUE(r->m_anchor.hasYaw);
    EXPECT_FLOAT_EQ(r->m_anchor.yawDeg, 30.f);
    EXPECT_TRUE(r->m_anchor.hasPitch);
    EXPECT_FLOAT_EQ(r->m_anchor.pitchDeg, -10.f);
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
