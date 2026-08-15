#include <protocols/types/SurfaceState.hpp>
#include <protocols/types/SurfaceStateQueue.hpp>

#include <gtest/gtest.h>

// SSurfaceState::reset() runs after every wl_surface.commit and defines what the NEXT commit starts
// from. A field left behind here is copied into every queued state the surface enqueues afterwards.

TEST(SurfaceState, ResetClearsTheViewpointAssociation) {
    SSurfaceState state;
    state.updated.bits.viewpoint = true;
    state.viewpointAssociation   = OpenXR::SXRViewpointAssociation{.epoch = 7, .sample = 42};

    state.reset();

    EXPECT_FALSE(state.updated.bits.viewpoint);
    EXPECT_FALSE(state.viewpointAssociation.has_value());
}

// The whole point of the reset: a commit that carried an association must not lend it to the next
// commit, which the surface enqueues as a plain copy of the (already reset) pending state.
TEST(SurfaceState, ACommitAfterATaggedOneDoesNotInheritItsAssociation) {
    SSurfaceState pending;

    // Commit 1: a buffer tagged with a rendered sample.
    pending.updated.bits.viewpoint = true;
    pending.viewpointAssociation   = OpenXR::SXRViewpointAssociation{.epoch = 3, .sample = 11};
    const SSurfaceState TAGGED     = pending;
    pending.reset();

    // Commit 2: damage only. The latch does not fire, so nothing sets the bit or the field.
    const SSurfaceState UNTAGGED = pending;

    EXPECT_TRUE(TAGGED.updated.bits.viewpoint);
    EXPECT_EQ(TAGGED.viewpointAssociation, (OpenXR::SXRViewpointAssociation{.epoch = 3, .sample = 11}));
    EXPECT_FALSE(UNTAGGED.updated.bits.viewpoint);
    EXPECT_FALSE(UNTAGGED.viewpointAssociation.has_value());
}

// updateFrom() applies the association only under the updated bit, so an association without the bit
// is inert. clearViewpointAssociations must respect the same rule rather than promote such a state
// into carrying a (clearing) viewpoint update it never had.
TEST(SurfaceState, ClearingAnEpochOnlyTouchesStatesThatCarryAViewpointUpdate) {
    CSurfaceStateQueue queue;

    auto               tagged            = makeUnique<SSurfaceState>();
    tagged->updated.bits.viewpoint       = true;
    tagged->viewpointAssociation         = OpenXR::SXRViewpointAssociation{.epoch = 5, .sample = 1};
    auto otherEpoch                      = makeUnique<SSurfaceState>();
    otherEpoch->updated.bits.viewpoint   = true;
    otherEpoch->viewpointAssociation     = OpenXR::SXRViewpointAssociation{.epoch = 6, .sample = 2};
    auto inert                           = makeUnique<SSurfaceState>();
    inert->viewpointAssociation          = OpenXR::SXRViewpointAssociation{.epoch = 5, .sample = 3};

    const auto TAGGED     = queue.enqueue(std::move(tagged));
    const auto OTHEREPOCH = queue.enqueue(std::move(otherEpoch));
    const auto INERT      = queue.enqueue(std::move(inert));

    queue.clearViewpointAssociations(5);

    EXPECT_FALSE(TAGGED->viewpointAssociation.has_value());
    EXPECT_TRUE(TAGGED->updated.bits.viewpoint);
    EXPECT_EQ(OTHEREPOCH->viewpointAssociation, (OpenXR::SXRViewpointAssociation{.epoch = 6, .sample = 2}));
    EXPECT_FALSE(INERT->updated.bits.viewpoint);
    EXPECT_EQ(INERT->viewpointAssociation, (OpenXR::SXRViewpointAssociation{.epoch = 5, .sample = 3}));
}
