#include <openxr/XRDonDoff.hpp>

#include <gtest/gtest.h>

// tests/xr/donoff_fsm.cpp — research/30 §4.3: the pure transition function.

using namespace OpenXR;

static SXRDonStep run(eXRDonState s, eXRDonInput in, SXRDonFacts f = {}) {
    return xrDonTransition(s, in, f);
}

TEST(DonOffFsm, SessionLifecycle) {
    EXPECT_EQ(run(XR_DON_NO_SESSION, XR_DIN_SESSION_UP).next, XR_DON_UNWORN);
    EXPECT_EQ(run(XR_DON_NO_SESSION, XR_DIN_WORN_UP).next, XR_DON_NO_SESSION);
    EXPECT_EQ(run(XR_DON_NO_SESSION, XR_DIN_AUTHORED).actions, 0);
}

TEST(DonOffFsm, PhaseOneDonGoesStraightToWorn) {
    // Phases 0/1: no hide, no settle — the don is the old behaviour
    const auto st = run(XR_DON_UNWORN, XR_DIN_WORN_UP, SXRDonFacts{.decisionCertainResume = false, .donGate = false});
    EXPECT_EQ(st.next, XR_DON_WORN);
    EXPECT_EQ(st.actions, 0);
}

TEST(DonOffFsm, PhaseTwoDonHidesUnlessCertainResume) {
    auto st = run(XR_DON_UNWORN, XR_DIN_WORN_UP, SXRDonFacts{.decisionCertainResume = false, .donGate = true});
    EXPECT_EQ(st.next, XR_DON_DONNING);
    EXPECT_TRUE(st.actions & XR_DA_PUBLISH_DONNING);
    st = run(XR_DON_UNWORN, XR_DIN_WORN_UP, SXRDonFacts{.decisionCertainResume = true, .donGate = true});
    EXPECT_EQ(st.next, XR_DON_WORN);
    EXPECT_EQ(run(XR_DON_DONNING, XR_DIN_HYDRATED).next, XR_DON_WORN);
    st = run(XR_DON_DONNING, XR_DIN_BACKSTOP);
    EXPECT_EQ(st.next, XR_DON_WORN);
    EXPECT_TRUE(st.actions & XR_DA_SHOW_IN_PLACE);
    EXPECT_TRUE(st.actions & XR_DA_LOG_WARN);
    st = run(XR_DON_DONNING, XR_DIN_WORN_DOWN);
    EXPECT_EQ(st.next, XR_DON_UNWORN);
    EXPECT_EQ(st.actions, 0); // nothing was worn long enough to remember
    st = run(XR_DON_DONNING, XR_DIN_SESSION_DOWN);
    EXPECT_EQ(st.next, XR_DON_NO_SESSION);
    EXPECT_EQ(st.actions, 0);
}

TEST(DonOffFsm, DoffCommitsExactlyOnce) {
    auto st = run(XR_DON_WORN, XR_DIN_WORN_DOWN);
    EXPECT_EQ(st.next, XR_DON_DOFFING);
    EXPECT_EQ(st.actions, XR_DA_COMMIT_FROM_HISTORY);
    // a second WORN_DOWN while the commit is pending, and after it, commit nothing
    EXPECT_EQ(run(XR_DON_DOFFING, XR_DIN_WORN_DOWN).actions, 0);
    st = run(XR_DON_DOFFING, XR_DIN_COMMIT_DONE);
    EXPECT_EQ(st.next, XR_DON_UNWORN);
    EXPECT_EQ(run(XR_DON_UNWORN, XR_DIN_WORN_DOWN).actions, 0);
}

TEST(DonOffFsm, SessionEndCommitsUnlessADoffAlreadyDid) {
    auto st = run(XR_DON_WORN, XR_DIN_SESSION_DOWN);
    EXPECT_EQ(st.next, XR_DON_NO_SESSION);
    EXPECT_TRUE(st.actions & XR_DA_COMMIT_FROM_HISTORY);
    st = run(XR_DON_UNWORN, XR_DIN_SESSION_DOWN, SXRDonFacts{.committedSinceWorn = true});
    EXPECT_EQ(st.actions, 0);
    st = run(XR_DON_UNWORN, XR_DIN_SESSION_DOWN, SXRDonFacts{.committedSinceWorn = false});
    EXPECT_TRUE(st.actions & XR_DA_COMMIT_FROM_HISTORY);
}

TEST(DonOffFsm, ReseatAndAuthoredEdits) {
    auto st = run(XR_DON_WORN, XR_DIN_RESEAT_DONE);
    EXPECT_EQ(st.next, XR_DON_WORN);
    EXPECT_EQ(st.actions, XR_DA_COMMIT_RESEAT);
    st = run(XR_DON_WORN, XR_DIN_AUTHORED);
    EXPECT_EQ(st.next, XR_DON_WORN);
    EXPECT_EQ(st.actions, XR_DA_MARK_AUTHORED);
    EXPECT_EQ(run(XR_DON_UNWORN, XR_DIN_AUTHORED).actions, XR_DA_MARK_AUTHORED);
}

TEST(DonOffFsm, DiscontinuityOnlyRehydratesWithTheDonGate) {
    EXPECT_EQ(run(XR_DON_WORN, XR_DIN_DISCONTINUITY).next, XR_DON_WORN);
    auto st = run(XR_DON_WORN, XR_DIN_DISCONTINUITY, SXRDonFacts{.donGate = true});
    EXPECT_EQ(st.next, XR_DON_DONNING);
    EXPECT_TRUE(st.actions & XR_DA_COMMIT_FROM_HISTORY);
    st = run(XR_DON_WORN, XR_DIN_DISCONTINUITY, SXRDonFacts{.lateDiscontinuity = true, .donGate = true});
    EXPECT_FALSE(st.actions & XR_DA_COMMIT_FROM_HISTORY);
}

TEST(DonOffFsm, EveryPairIsTotal) {
    for (int s = XR_DON_NO_SESSION; s <= XR_DON_DOFFING; ++s) {
        for (int i = XR_DIN_SESSION_UP; i <= XR_DIN_COMMIT_DONE; ++i) {
            const auto st = run(static_cast<eXRDonState>(s), static_cast<eXRDonInput>(i));
            EXPECT_LE(st.next, XR_DON_DOFFING);
            EXPECT_STRNE(xrDonStateName(st.next), "?");
        }
    }
}
