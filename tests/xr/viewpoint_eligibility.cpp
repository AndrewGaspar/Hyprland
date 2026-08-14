#include <openxr/XRViewpointEligibility.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

TEST(XRViewpointEligibility, ExactFullSBSViewporterMappingIsEligible) {
    EXPECT_TRUE(viewpointSBSBufferMapping({3840, 1080}, {3840, 1080}, {3840, 1080}, false, true, {3840, 1080}, true, 1));
    EXPECT_TRUE(viewpointSBSBufferMapping({512, 144}, {3840, 1080}, {3840, 1080}, false, true, {3840, 1080}, true, 1));
    EXPECT_TRUE(viewpointSBSBufferMapping({256, 144}, {1920, 1080}, {1920, 1080}, false, true, {1920, 1080}, true, 1));
}

TEST(XRViewpointEligibility, ArbitraryViewportAndPackingMappingsFailClosed) {
    EXPECT_FALSE(viewpointSBSBufferMapping({512, 144}, {1920, 1080}, {1920, 1080}, false, true, {1920, 1080}, true, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({256, 144}, {3840, 1080}, {3840, 1080}, false, true, {3840, 1080}, true, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({256, 144}, {1920, 1080}, {1920, 1080}, true, true, {1920, 1080}, true, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({256, 144}, {1920, 1080}, {1920, 1080}, false, false, {}, true, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({258, 144}, {1920, 1080}, {1920, 1080}, false, true, {1920, 1080}, true, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({256, 144}, {1919, 1080}, {1920, 1080}, false, true, {1920, 1080}, true, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({256, 144}, {1920, 1080}, {1920, 1080}, false, true, {1920, 1080}, false, 1));
    EXPECT_FALSE(viewpointSBSBufferMapping({256, 144}, {1920, 1080}, {1920, 1080}, false, true, {1920, 1080}, true, 2));
}

TEST(XRViewpointEligibility, RuntimeValidityEmitsOnlyEdgesAndRecovers) {
    auto lost = viewpointRuntimeTransition(XR_VIEWPOINT_RUNTIME_UNKNOWN, false);
    EXPECT_EQ(lost.state, XR_VIEWPOINT_RUNTIME_INVALID);
    EXPECT_TRUE(lost.changed);

    auto repeated = viewpointRuntimeTransition(lost.state, false);
    EXPECT_EQ(repeated.state, XR_VIEWPOINT_RUNTIME_INVALID);
    EXPECT_FALSE(repeated.changed);

    auto recovered = viewpointRuntimeTransition(repeated.state, true);
    EXPECT_EQ(recovered.state, XR_VIEWPOINT_RUNTIME_VALID);
    EXPECT_TRUE(recovered.changed);
    EXPECT_FALSE(viewpointRuntimeTransition(recovered.state, true).changed);
}

TEST(XRViewpointEligibility, RepeatedEquivalentEligibilityPreservesActivation) {
    EXPECT_TRUE(viewpointActivationUnchanged(11, 11, 9, 7, 7, 1600000, 1600000, 900000, 900000, true));
    EXPECT_FALSE(viewpointActivationUnchanged(11, 12, 9, 7, 7, 1600000, 1600000, 900000, 900000, true));
    EXPECT_FALSE(viewpointActivationUnchanged(11, 11, 0, 7, 7, 1600000, 1600000, 900000, 900000, true));
    EXPECT_FALSE(viewpointActivationUnchanged(11, 11, 9, 7, 8, 1600000, 1600000, 900000, 900000, true));
    EXPECT_FALSE(viewpointActivationUnchanged(11, 11, 9, 7, 7, 1600000, 1600001, 900000, 900000, true));
    EXPECT_FALSE(viewpointActivationUnchanged(11, 11, 9, 7, 7, 1600000, 1600000, 900000, 900000, false));
}
