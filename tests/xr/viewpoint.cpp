#include <openxr/XRViewpoint.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <utility>

using namespace OpenXR;

static void expectVecNear(const Vec3& actual, const Vec3& expected, float epsilon = 1e-5F) {
    EXPECT_NEAR(actual.x, expected.x, epsilon);
    EXPECT_NEAR(actual.y, expected.y, epsilon);
    EXPECT_NEAR(actual.z, expected.z, epsilon);
}

static Vec3 worldFromSurface(const SXRPose& surface, const Vec3& local) {
    return surface.pos + qRotate(surface.rot, local);
}

TEST(XRViewpoint, StraightOnViewerUsesSurfaceLocalAxes) {
    const SXRPose surface{{0.F, 0.F, -1.5F}, Quat{}};
    const auto    result = surfaceRelativeViewpoint(surface, 1.6F, 0.9F, {Vec3{0.F, 0.F, 0.F}, Vec3{}}, 1);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.viewCount, 1U);
    EXPECT_FLOAT_EQ(result.widthMeters, 1.6F);
    EXPECT_FLOAT_EQ(result.heightMeters, 0.9F);
    expectVecNear(result.viewPositions[0], {0.F, 0.F, 1.5F});
}

TEST(XRViewpoint, StereoOrderAndEyeSeparationArePreserved) {
    const SXRPose surface{{0.F, 0.F, -1.5F}, Quat{}};
    const auto    result = surfaceRelativeViewpoint(surface, 1.6F, 0.9F, {Vec3{-0.032F, 0.F, 0.F}, Vec3{0.032F, 0.F, 0.F}}, 2);

    ASSERT_TRUE(result.valid);
    ASSERT_EQ(result.viewCount, 2U);
    expectVecNear(result.viewPositions[0], {-0.032F, 0.F, 1.5F});
    expectVecNear(result.viewPositions[1], {0.032F, 0.F, 1.5F});
}

TEST(XRViewpoint, ArbitrarySurfacePoseRecoversKnownLocalPositions) {
    const SXRPose surface{{1.4F, -0.3F, 2.2F}, qMul(qFromYaw(0.7F), qFromPitch(-0.35F))};
    const Vec3    leftLocal{-0.18F, 0.09F, 1.7F};
    const Vec3    rightLocal{0.24F, -0.04F, 1.45F};
    const auto    result = surfaceRelativeViewpoint(surface, 2.1F, 1.2F, {worldFromSurface(surface, leftLocal), worldFromSurface(surface, rightLocal)}, 2);

    ASSERT_TRUE(result.valid);
    expectVecNear(result.viewPositions[0], leftLocal);
    expectVecNear(result.viewPositions[1], rightLocal);
}

TEST(XRViewpoint, TranslatingSurfaceAndViewerTogetherDoesNotChangeGeometry) {
    const SXRPose baseSurface{{0.2F, 1.1F, -1.8F}, qFromYaw(-0.4F)};
    const Vec3    leftLocal{-0.03F, 0.05F, 1.4F};
    const Vec3    rightLocal{0.03F, 0.05F, 1.4F};
    const Vec3    translation{7.5F, -2.2F, 3.1F};
    SXRPose       movedSurface = baseSurface;
    movedSurface.pos += translation;

    const std::array<Vec3, 2> baseViews{worldFromSurface(baseSurface, leftLocal), worldFromSurface(baseSurface, rightLocal)};
    const std::array<Vec3, 2> movedViews{baseViews[0] + translation, baseViews[1] + translation};
    const auto                base  = surfaceRelativeViewpoint(baseSurface, 1.8F, 1.F, baseViews, 2);
    const auto                moved = surfaceRelativeViewpoint(movedSurface, 1.8F, 1.F, movedViews, 2);

    ASSERT_TRUE(base.valid);
    ASSERT_TRUE(moved.valid);
    expectVecNear(moved.viewPositions[0], base.viewPositions[0]);
    expectVecNear(moved.viewPositions[1], base.viewPositions[1]);
}

TEST(XRViewpoint, CommonRigidTransformDoesNotChangeGeometry) {
    const SXRPose             surface{{0.35F, 1.25F, -2.1F}, qMul(qFromYaw(-0.55F), qFromPitch(0.18F))};
    const Vec3                leftLocal{-0.04F, 0.11F, 1.3F};
    const Vec3                rightLocal{0.04F, 0.11F, 1.3F};
    const std::array<Vec3, 2> views{worldFromSurface(surface, leftLocal), worldFromSurface(surface, rightLocal)};

    const SXRPose             rigid{{4.2F, -0.7F, 3.5F}, qMul(qFromYaw(0.9F), qFromPitch(-0.27F))};
    const SXRPose             transformedSurface = poseCompose(rigid, surface);
    const std::array<Vec3, 2> transformedViews{rigid.pos + qRotate(rigid.rot, views[0]), rigid.pos + qRotate(rigid.rot, views[1])};

    const auto                base        = surfaceRelativeViewpoint(surface, 1.7F, 0.95F, views, 2);
    const auto                transformed = surfaceRelativeViewpoint(transformedSurface, 1.7F, 0.95F, transformedViews, 2);

    ASSERT_TRUE(base.valid);
    ASSERT_TRUE(transformed.valid);
    expectVecNear(transformed.viewPositions[0], base.viewPositions[0]);
    expectVecNear(transformed.viewPositions[1], base.viewPositions[1]);
}

TEST(XRViewpoint, ViewerMotionUsesSurfaceRightUpAndOutSigns) {
    const SXRPose surface{{-0.8F, 0.6F, 0.4F}, qMul(qFromYaw(1.1F), qFromPitch(0.2F))};
    const Vec3    local{0.25F, 0.15F, 1.1F};
    const auto    result = surfaceRelativeViewpoint(surface, 1.4F, 0.8F, {worldFromSurface(surface, local), Vec3{}}, 1);

    ASSERT_TRUE(result.valid);
    expectVecNear(result.viewPositions[0], local);
    EXPECT_GT(result.viewPositions[0].x, 0.F);
    EXPECT_GT(result.viewPositions[0].y, 0.F);
    EXPECT_GT(result.viewPositions[0].z, 0.F);
}

TEST(XRViewpoint, OnPlaneAndBehindViewersStayUnclampedAndInvalidateTheSample) {
    const SXRPose surface{{0.F, 0.F, 0.F}, Quat{}};

    const Vec3    onPlaneLocal{0.1F, -0.2F, XR_VIEWPOINT_Z_EPSILON};
    const auto    onPlane = surfaceRelativeViewpoint(surface, 1.F, 1.F, {worldFromSurface(surface, onPlaneLocal), Vec3{}}, 1);
    EXPECT_FALSE(onPlane.valid);
    expectVecNear(onPlane.viewPositions[0], onPlaneLocal);

    const Vec3 behindLocal{-0.3F, 0.4F, -0.25F};
    const auto behind = surfaceRelativeViewpoint(surface, 1.F, 1.F, {worldFromSurface(surface, behindLocal), Vec3{}}, 1);
    EXPECT_FALSE(behind.valid);
    expectVecNear(behind.viewPositions[0], behindLocal);
}

TEST(XRViewpoint, AViewJustInFrontOfTheEpsilonIsValid) {
    const SXRPose surface{{0.F, 0.F, 0.F}, Quat{}};
    const Vec3    local{0.F, 0.F, XR_VIEWPOINT_Z_EPSILON + 1e-5F};
    const auto    result = surfaceRelativeViewpoint(surface, 1.F, 1.F, {worldFromSurface(surface, local), Vec3{}}, 1);

    ASSERT_TRUE(result.valid);
    expectVecNear(result.viewPositions[0], local, 1e-7F);
}

TEST(XRViewpoint, OneInvalidStereoViewInvalidatesTheWholeSampleWithoutClampingEitherView) {
    const SXRPose surface{{0.F, 0.F, 0.F}, Quat{}};
    const Vec3    leftLocal{-0.03F, 0.02F, 1.2F};
    const Vec3    rightLocal{0.03F, 0.02F, -0.1F};
    const auto    result = surfaceRelativeViewpoint(surface, 1.6F, 0.9F, {worldFromSurface(surface, leftLocal), worldFromSurface(surface, rightLocal)}, 2);

    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.viewCount, 2U);
    expectVecNear(result.viewPositions[0], leftLocal);
    expectVecNear(result.viewPositions[1], rightLocal);
}

TEST(XRViewpoint, InvalidViewCountsFailClosed) {
    const SXRPose             surface{{0.F, 0.F, -1.F}, Quat{}};
    const std::array<Vec3, 2> views{Vec3{0.F, 0.F, 0.F}, Vec3{0.03F, 0.F, 0.F}};

    for (const size_t count : {0U, 3U}) {
        const auto result = surfaceRelativeViewpoint(surface, 1.F, 1.F, views, count);
        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.viewCount, 0U);
        EXPECT_FLOAT_EQ(result.widthMeters, 0.F);
        EXPECT_FLOAT_EQ(result.heightMeters, 0.F);
    }
}

TEST(XRViewpoint, InvalidDimensionsFailClosed) {
    const SXRPose             surface{{0.F, 0.F, -1.F}, Quat{}};
    const std::array<Vec3, 2> views{Vec3{0.F, 0.F, 0.F}, Vec3{}};
    const float               nan = std::numeric_limits<float>::quiet_NaN();
    const float               inf = std::numeric_limits<float>::infinity();

    for (const auto& [width, height] : {std::pair{0.F, 1.F}, std::pair{-1.F, 1.F}, std::pair{1.F, 0.F}, std::pair{1.F, -1.F}, std::pair{nan, 1.F}, std::pair{1.F, inf}}) {
        const auto result = surfaceRelativeViewpoint(surface, width, height, views, 1);
        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.viewCount, 0U);
        EXPECT_FLOAT_EQ(result.widthMeters, 0.F);
        EXPECT_FLOAT_EQ(result.heightMeters, 0.F);
    }
}

TEST(XRViewpoint, NonFinitePositionsAndDegenerateOrientationFailClosed) {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    for (const SXRPose pose : {SXRPose{{inf, 0.F, 0.F}, Quat{}}, SXRPose{{0.F, 0.F, 0.F}, Quat{nan, 0.F, 0.F, 1.F}}, SXRPose{{0.F, 0.F, 0.F}, Quat{0.F, 0.F, 0.F, 0.F}}}) {
        const auto result = surfaceRelativeViewpoint(pose, 1.F, 1.F, {Vec3{0.F, 0.F, 1.F}, Vec3{}}, 1);
        EXPECT_FALSE(result.valid);
        EXPECT_EQ(result.viewCount, 0U);
        EXPECT_FLOAT_EQ(result.widthMeters, 0.F);
        EXPECT_FLOAT_EQ(result.heightMeters, 0.F);
    }

    const auto invalidView = surfaceRelativeViewpoint(SXRPose{{0.F, 0.F, 0.F}, Quat{}}, 1.F, 1.F, {Vec3{inf, 0.F, 1.F}, Vec3{}}, 1);
    EXPECT_FALSE(invalidView.valid);
    EXPECT_EQ(invalidView.viewCount, 1U);
    EXPECT_FLOAT_EQ(invalidView.widthMeters, 1.F);
    EXPECT_FLOAT_EQ(invalidView.heightMeters, 1.F);
}
