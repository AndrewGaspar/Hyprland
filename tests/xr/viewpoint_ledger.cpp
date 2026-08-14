#include <openxr/XRViewpointLedger.hpp>

#include <gtest/gtest.h>
#include <limits>

using namespace OpenXR;

TEST(XRViewpointLedger, IssuedSamplesAreOneShot) {
    CXRViewpointIssuedSampleLedger ledger;

    EXPECT_FALSE(ledger.issue(0));
    EXPECT_TRUE(ledger.issue(7));
    EXPECT_FALSE(ledger.issue(7));
    EXPECT_TRUE(ledger.consume(7));
    EXPECT_FALSE(ledger.consume(7));
}

TEST(XRViewpointLedger, CapacityEvictsOldestWithoutGrowing) {
    CXRViewpointIssuedSampleLedger ledger{3};

    EXPECT_TRUE(ledger.issue(1));
    EXPECT_TRUE(ledger.issue(2));
    EXPECT_TRUE(ledger.issue(3));
    EXPECT_TRUE(ledger.issue(4));
    EXPECT_EQ(ledger.size(), 3U);
    EXPECT_FALSE(ledger.consume(1));
    EXPECT_TRUE(ledger.consume(2));
    ledger.clear();
    EXPECT_EQ(ledger.size(), 0U);
}

TEST(XRViewpointLedger, ZeroCapacityFailsClosed) {
    CXRViewpointIssuedSampleLedger ledger{0};
    EXPECT_FALSE(ledger.issue(1));
    EXPECT_FALSE(ledger.consume(1));
}

TEST(XRViewpointLedger, EpochNeverWrapsOrUsesZero) {
    uint64_t next = 99;
    EXPECT_TRUE(nextViewpointEpoch(0, next));
    EXPECT_EQ(next, 1U);
    EXPECT_TRUE(nextViewpointEpoch(std::numeric_limits<uint64_t>::max() - 1, next));
    EXPECT_EQ(next, std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(nextViewpointEpoch(std::numeric_limits<uint64_t>::max(), next));
    EXPECT_EQ(next, 0U);
}
