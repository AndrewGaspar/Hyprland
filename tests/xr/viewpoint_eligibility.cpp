#include <openxr/XRViewpointEligibility.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <ranges>
#include <vector>

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

TEST(XRViewpointEligibility, SurfaceOffsetRequiresReevaluation) {
    EXPECT_FALSE(viewpointSurfaceStateRequiresReevaluation(false, false, false, false, false));
    EXPECT_TRUE(viewpointSurfaceStateRequiresReevaluation(false, false, false, true, false));
    EXPECT_TRUE(viewpointSurfaceStateRequiresReevaluation(false, false, false, false, true));
}

TEST(XRViewpointEligibility, SubsurfaceWatchBudgetFailsClosedAtEitherLimit) {
    EXPECT_TRUE(viewpointSubsurfaceWatchWithinBudget(0, 0));
    EXPECT_TRUE(viewpointSubsurfaceWatchWithinBudget(XR_VIEWPOINT_MAX_SUBSURFACE_DEPTH - 1, XR_VIEWPOINT_MAX_SUBSURFACE_WATCHES - 1));
    EXPECT_FALSE(viewpointSubsurfaceWatchWithinBudget(XR_VIEWPOINT_MAX_SUBSURFACE_DEPTH, 0));
    EXPECT_FALSE(viewpointSubsurfaceWatchWithinBudget(0, XR_VIEWPOINT_MAX_SUBSURFACE_WATCHES));
}

// The eligibility walk sorts viewpoints before claiming monitors, and the list routinely mixes
// viewpoints whose wl_surface is gone with viewpoints that still have one. The comparator must be a
// strict weak ordering over that mix: the previous "id-order when both have a surface, pointer-order
// otherwise" rule was not, and a cyclic comparator makes libstdc++'s insertion sort run off the
// front of the range. These tests check the SWO axioms exhaustively over a key set built to contain
// exactly the shape that used to close a cycle.
namespace {
    std::vector<SXRViewpointOrderKey> orderKeyFixture() {
        // Identities deliberately run OPPOSITE to the surface ids, which is what made the old
        // comparator cycle: A < C by id, and a surface-less B falls between them by identity.
        return {
            {.hasSurface = true, .surfaceId = 10, .identity = 0x9000},  // A
            {.hasSurface = false, .surfaceId = 0, .identity = 0x5000},  // B (surface destroyed)
            {.hasSurface = true, .surfaceId = 20, .identity = 0x1000},  // C
            {.hasSurface = false, .surfaceId = 0, .identity = 0xF000},  // D (surface destroyed)
            {.hasSurface = true, .surfaceId = 20, .identity = 0x2000},  // same id, different object
            {.hasSurface = true, .surfaceId = 0, .identity = 0x3000},   // live surface, id 0
        };
    }
}

TEST(XRViewpointEligibility, MixedSurfaceOrderIsAStrictWeakOrdering) {
    const auto KEYS = orderKeyFixture();

    for (const auto& a : KEYS)
        EXPECT_FALSE(viewpointOrderBefore(a, a)) << "irreflexivity";

    for (const auto& a : KEYS)
        for (const auto& b : KEYS)
            EXPECT_FALSE(viewpointOrderBefore(a, b) && viewpointOrderBefore(b, a)) << "asymmetry";

    for (const auto& a : KEYS)
        for (const auto& b : KEYS)
            for (const auto& c : KEYS) {
                if (viewpointOrderBefore(a, b) && viewpointOrderBefore(b, c))
                    EXPECT_TRUE(viewpointOrderBefore(a, c)) << "transitivity";

                // Transitivity of incomparability — the axiom the old comparator broke.
                const bool AB = !viewpointOrderBefore(a, b) && !viewpointOrderBefore(b, a);
                const bool BC = !viewpointOrderBefore(b, c) && !viewpointOrderBefore(c, b);
                if (AB && BC)
                    EXPECT_TRUE(!viewpointOrderBefore(a, c) && !viewpointOrderBefore(c, a)) << "transitivity of incomparability";
            }
}

TEST(XRViewpointEligibility, MixedSurfaceOrderPutsLiveSurfacesFirstAndIsDeterministic) {
    auto keys = orderKeyFixture();
    std::ranges::sort(keys, viewpointOrderBefore);

    // Every surfaced viewpoint precedes every surface-less one; the claim walk therefore decides
    // contested monitors from surface ids and never from where the allocator put a dead entry.
    const auto FIRSTDEAD = std::ranges::find_if(keys, [](const auto& k) { return !k.hasSurface; });
    EXPECT_TRUE(std::all_of(FIRSTDEAD, keys.end(), [](const auto& k) { return !k.hasSurface; }));
    EXPECT_TRUE(std::ranges::is_sorted(keys, viewpointOrderBefore));

    // Same set, any starting permutation, same answer.
    auto shuffled = orderKeyFixture();
    std::ranges::reverse(shuffled);
    std::ranges::sort(shuffled, viewpointOrderBefore);
    EXPECT_EQ(keys, shuffled);
}
