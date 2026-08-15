#include <openxr/XRAnchor.hpp>
#include <openxr/XRViewpointTransport.hpp>
#include <render/StereoContent.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace OpenXR;

// tests/xr/viewpoint_geometry_contract.cpp — the two-thread agreement a viewpoint subscription
// rests on.
//
// The main thread computes the rectangle a client is told to render for and encodes it as
// micrometres in the subscription; the frame thread solves the same rectangle every frame and its
// samples are only published while the two still describe the same shape. Getting that comparison
// wrong is invisible: the viewpoint simply never activates, with nothing logged. These tests pin
// the two rules that keep it honest — one derivation shared by both sides, and an agreement test
// with slack rather than bit equality.

namespace {
    // Exactly what each side does with a monitor's pixel mode for a full-SBS window.
    struct SSubscribedRect {
        uint32_t widthUM  = 0;
        uint32_t heightUM = 0;
        float    heightMeters = 0.F;
    };

    SSubscribedRect subscribe(const Vector2D& pixelMode, float widthMeters) {
        const auto      PANEPX = Render::Stereo::presentedPaneSize(pixelMode, Render::Stereo::CONTENT_SBS);
        SSubscribedRect out;
        out.heightMeters = quadHeightMeters(widthMeters, (uint32_t)std::max(1.0, PANEPX.x), (uint32_t)std::max(1.0, PANEPX.y));
        encodeViewpointDimensionUM(widthMeters, out.widthUM);
        encodeViewpointDimensionUM(out.heightMeters, out.heightUM);
        return out;
    }

    // The frame thread reaches the same number through CXRAnchor::solve, which is fed the pane
    // pixels as the uint32s SXRSolveInput carries.
    float solvedHeight(const Vector2D& pixelMode, float widthMeters) {
        CXRAnchor      a;
        SXRAnchorState st;
        st.mode        = XR_ANCHOR_LOCAL;
        st.anchorPose  = SXRPose{Vec3{0.f, 1.5f, -2.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        st.widthMeters = widthMeters;
        a.initFromState(st);

        const auto    PANEPX = Render::Stereo::presentedPaneSize(pixelMode, Render::Stereo::CONTENT_SBS);
        SXRSolveInput in;
        in.view = SXRPose{Vec3{0.f, 1.5f, 0.f}, Quat{0.f, 0.f, 0.f, 1.f}};
        in.pxW  = (uint32_t)std::max(1.0, PANEPX.x);
        in.pxH  = (uint32_t)std::max(1.0, PANEPX.y);
        return a.solve(in, SXRAnchorTuning{}).heightMeters;
    }

    SXRViewpointGeometry geometryOf(float widthMeters, float heightMeters) {
        return {
            .valid         = true,
            .viewCount     = 2,
            .widthMeters   = widthMeters,
            .heightMeters  = heightMeters,
            .viewPositions = {Vec3{-0.032F, 0.05F, 1.4F}, Vec3{0.032F, 0.05F, 1.4F}},
        };
    }

    // Modes chosen to be awkward: ultrawides whose pane aspect is a long binary fraction, the
    // classic 1366x768 (odd pane height after the halve is not the issue — the aspect is), and odd
    // widths, where a pane is a half pixel and only one side could round it away.
    const std::vector<Vector2D> ADVERSARIALMODES{
        {3440, 1440}, {2560, 1080}, {1366, 768}, {3840, 1080}, {1920, 1080}, {5120, 1440}, {1365, 768}, {3441, 1440}, {2559, 1079},
    };

    const std::vector<float> WIDTHS{0.5F, 1.0F, 1.6F, 2.4F, 3.7F, 8.5F};
}

// THE invariant. If these two ever disagree by a single micrometre, every viewpoint on such a
// monitor is permanently inactive and nothing says why.
TEST(XRViewpointGeometryContract, SubscribedHeightMatchesTheSolvedHeightExactly) {
    for (const auto& MODE : ADVERSARIALMODES) {
        for (const float W : WIDTHS) {
            const auto SUB = subscribe(MODE, W);
            ASSERT_NE(SUB.heightUM, 0u) << MODE.x << "x" << MODE.y << " @ " << W << "m";

            const float SOLVED = solvedHeight(MODE, W);
            EXPECT_FLOAT_EQ(SOLVED, SUB.heightMeters) << MODE.x << "x" << MODE.y << " @ " << W << "m";

            uint32_t solvedUM = 0;
            EXPECT_TRUE(encodeViewpointDimensionUM(SOLVED, solvedUM));
            EXPECT_EQ(solvedUM, SUB.heightUM) << MODE.x << "x" << MODE.y << " @ " << W << "m";

            // …and the interlock the publish path actually runs agrees too.
            EXPECT_TRUE(viewpointDimensionAgrees(SOLVED, SUB.heightUM));
            EXPECT_TRUE(viewpointDimensionAgrees(W, SUB.widthUM));
        }
    }
}

// The shared derivation is the reason the above holds: the association order and the integer pane
// width are both part of the answer, so writing it a second time by hand is how the two drift.
TEST(XRViewpointGeometryContract, HeightDerivationIsOneExpression) {
    // Association matters — this is the pair that used to straddle the two threads.
    EXPECT_FLOAT_EQ(quadHeightMeters(1.6F, 1720, 1440), 1.6F * 1440.F / 1720.F);
    // Degenerate pane width fails safe instead of dividing by zero.
    EXPECT_FLOAT_EQ(quadHeightMeters(1.6F, 0, 1440), 1.6F * 1440.F);
    // A square pane is the identity.
    EXPECT_FLOAT_EQ(quadHeightMeters(2.F, 1000, 1000), 2.F);
}

TEST(XRViewpointGeometryContract, AgreementToleratesARoundingStepButNotARealChange) {
    const uint32_t SUBSCRIBED = 900000; // 0.9 m

    EXPECT_TRUE(viewpointDimensionAgrees(0.9, SUBSCRIBED));
    // One micrometre either way is two honest roundings of the same rectangle.
    EXPECT_TRUE(viewpointDimensionAgrees(0.900001, SUBSCRIBED));
    EXPECT_TRUE(viewpointDimensionAgrees(0.899999, SUBSCRIBED));
    // Two is not, and neither is anything a mode change would produce.
    EXPECT_FALSE(viewpointDimensionAgrees(0.900002, SUBSCRIBED));
    EXPECT_FALSE(viewpointDimensionAgrees(0.899998, SUBSCRIBED));
    EXPECT_FALSE(viewpointDimensionAgrees(0.45, SUBSCRIBED));  // the pair went mono under us
    EXPECT_FALSE(viewpointDimensionAgrees(1.8, SUBSCRIBED));   // …or the other way
    // Nothing unencodable is ever "in agreement".
    EXPECT_FALSE(viewpointDimensionAgrees(0.0, SUBSCRIBED));
    EXPECT_FALSE(viewpointDimensionAgrees(-0.9, SUBSCRIBED));
    EXPECT_FALSE(viewpointDimensionAgrees(std::numeric_limits<double>::quiet_NaN(), SUBSCRIBED));
}

// The sample carries the SUBSCRIPTION's micrometres, not a second rounding of the frame thread's
// floats — that is what makes the client's activation contract and its samples byte-identical.
TEST(XRViewpointGeometryContract, PublishCarriesTheSubscribedDimensionsVerbatim) {
    // A geometry whose own rounding lands one micrometre off the subscription.
    const auto               GEOM = geometryOf(1.6F, 0.899999F);
    SXRViewpointEncodedSample sample;

    ASSERT_TRUE(encodeViewpointSample(GEOM, 7, 11, 1600000, 900000, sample));
    EXPECT_EQ(sample.widthUM, 1600000u);
    EXPECT_EQ(sample.heightUM, 900000u);
    EXPECT_EQ(joinViewpointU64(sample.serial), 7u);
    EXPECT_EQ(joinViewpointU64(sample.geometryId), 11u);
    EXPECT_TRUE(encodedViewpointSampleValid(sample));

    // The four-argument form still derives them, so every existing caller is unchanged.
    SXRViewpointEncodedSample derived;
    ASSERT_TRUE(encodeViewpointSample(GEOM, 7, 11, derived));
    EXPECT_EQ(derived.widthUM, 1600000u);
    EXPECT_EQ(derived.heightUM, 899999u);
}

TEST(XRViewpointGeometryContract, AuthoritativeDimensionsCannotLaunderABadGeometry) {
    SXRViewpointEncodedSample sample;

    // A degenerate rectangle stays rejected even when the caller offers believable micrometres.
    EXPECT_FALSE(encodeViewpointSample(geometryOf(0.F, 0.9F), 1, 1, 1600000, 900000, sample));
    EXPECT_FALSE(encodeViewpointSample(geometryOf(1.6F, -0.9F), 1, 1, 1600000, 900000, sample));
    EXPECT_FALSE(encodeViewpointSample(geometryOf(1.6F, std::numeric_limits<float>::infinity()), 1, 1, 1600000, 900000, sample));
    // …and so are zero dimensions offered by the caller.
    EXPECT_FALSE(encodeViewpointSample(geometryOf(1.6F, 0.9F), 1, 1, 0, 900000, sample));
    EXPECT_FALSE(encodeViewpointSample(geometryOf(1.6F, 0.9F), 1, 1, 1600000, 0, sample));
    // An invalid geometry is rejected whichever overload asks.
    auto invalid  = geometryOf(1.6F, 0.9F);
    invalid.valid = false;
    EXPECT_FALSE(encodeViewpointSample(invalid, 1, 1, 1600000, 900000, sample));
    EXPECT_FALSE(encodeViewpointSample(invalid, 1, 1, sample));
}
