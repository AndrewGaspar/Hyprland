#include <openxr/XRViewpointTransport.hpp>

#include <gtest/gtest.h>
#include <hyprutils/memory/Casts.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

using namespace OpenXR;
using namespace Hyprutils::Memory;

static SXRViewpointGeometry stereoGeometry(float leftX = -0.032F, float rightX = 0.032F) {
    return {
        .valid        = true,
        .viewCount    = 2,
        .widthMeters  = 1.6F,
        .heightMeters = 0.9F,
        .viewPositions =
            {
                Vec3{leftX, 0.05F, 1.4F},
                Vec3{rightX, 0.05F, 1.4F},
            },
    };
}

static SXRViewpointEncodedSample encoded(uint64_t serial) {
    SXRViewpointEncodedSample sample;
    EXPECT_TRUE(encodeViewpointSample(stereoGeometry(), serial, 9, sample));
    return sample;
}

TEST(XRViewpointTransport, CommitLatchConsumesOnlyOnANewNonNullBuffer) {
    CXRViewpointCommitLatch       latch;
    const SXRViewpointAssociation association{.epoch = 7, .sample = 11};

    latch.stage(association);
    EXPECT_EQ(latch.commit(false), SXRViewpointCommitAssociation{});
    EXPECT_EQ(latch.commit(false), SXRViewpointCommitAssociation{});

    const auto tagged = latch.commit(true);
    EXPECT_TRUE(tagged.updated);
    EXPECT_EQ(tagged.association, association);

    const auto untagged = latch.commit(true);
    EXPECT_TRUE(untagged.updated);
    EXPECT_FALSE(untagged.association);
}

TEST(XRViewpointTransport, CommitLatchRevocationPreventsAStaleFutureAssociation) {
    CXRViewpointCommitLatch latch;
    latch.stage({.epoch = 7, .sample = 11});

    EXPECT_FALSE(latch.clear(6));
    EXPECT_TRUE(latch.clear(7));
    EXPECT_FALSE(latch.clear(7));

    const auto committed = latch.commit(true);
    EXPECT_TRUE(committed.updated);
    EXPECT_FALSE(committed.association);
}

TEST(XRViewpointTransport, AssociationClearIsScopedToItsEpoch) {
    std::optional<SXRViewpointAssociation> association = SXRViewpointAssociation{.epoch = 7, .sample = 11};

    EXPECT_FALSE(clearViewpointAssociation(association, 6));
    EXPECT_TRUE(association);
    EXPECT_TRUE(clearViewpointAssociation(association, 7));
    EXPECT_FALSE(association);
}

TEST(XRViewpointTransport, SignedPositionMicrometersRoundTripAndRoundHalfAwayFromZero) {
    for (const double meters : {-12.345678, -0.000001, 0.0, 0.000001, 27.125432}) {
        int32_t encodedUM = 7;
        ASSERT_TRUE(encodeViewpointPositionUM(meters, encodedUM));
        EXPECT_NEAR(decodeViewpointPositionUM(encodedUM), meters, 0.5 / XR_VIEWPOINT_UM_PER_METER);
    }

    int32_t positive = 0, negative = 0;
    ASSERT_TRUE(encodeViewpointPositionUM(0.5 / XR_VIEWPOINT_UM_PER_METER, positive));
    ASSERT_TRUE(encodeViewpointPositionUM(-0.5 / XR_VIEWPOINT_UM_PER_METER, negative));
    EXPECT_EQ(positive, 1);
    EXPECT_EQ(negative, -1);
}

TEST(XRViewpointTransport, SignedPositionBoundariesAndNonfiniteValuesFailClosed) {
    constexpr double SCALE = XR_VIEWPOINT_UM_PER_METER;
    int32_t          value = 17;

    ASSERT_TRUE(encodeViewpointPositionUM(sc<double>(std::numeric_limits<int32_t>::max()) / SCALE, value));
    EXPECT_EQ(value, std::numeric_limits<int32_t>::max());
    EXPECT_DOUBLE_EQ(decodeViewpointPositionUM(value), sc<double>(std::numeric_limits<int32_t>::max()) / SCALE);
    ASSERT_TRUE(encodeViewpointPositionUM(sc<double>(std::numeric_limits<int32_t>::min()) / SCALE, value));
    EXPECT_EQ(value, std::numeric_limits<int32_t>::min());
    EXPECT_DOUBLE_EQ(decodeViewpointPositionUM(value), sc<double>(std::numeric_limits<int32_t>::min()) / SCALE);

    EXPECT_FALSE(encodeViewpointPositionUM((sc<double>(std::numeric_limits<int32_t>::max()) + 1.0) / SCALE, value));
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(encodeViewpointPositionUM((sc<double>(std::numeric_limits<int32_t>::min()) - 1.0) / SCALE, value));
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(encodeViewpointPositionUM(std::numeric_limits<double>::infinity(), value));
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(encodeViewpointPositionUM(std::numeric_limits<double>::quiet_NaN(), value));
    EXPECT_EQ(value, 0);
    EXPECT_FALSE(encodeViewpointPositionUM(std::numeric_limits<double>::max(), value));
    EXPECT_EQ(value, 0);
}

TEST(XRViewpointTransport, UnsignedDimensionBoundariesAndNonfiniteValuesFailClosed) {
    constexpr double SCALE = XR_VIEWPOINT_UM_PER_METER;
    uint32_t         value = 17;

    ASSERT_TRUE(encodeViewpointDimensionUM(1.0 / SCALE, value));
    EXPECT_EQ(value, 1U);
    ASSERT_TRUE(encodeViewpointDimensionUM(sc<double>(std::numeric_limits<uint32_t>::max()) / SCALE, value));
    EXPECT_EQ(value, std::numeric_limits<uint32_t>::max());
    EXPECT_DOUBLE_EQ(decodeViewpointDimensionUM(value), sc<double>(std::numeric_limits<uint32_t>::max()) / SCALE);

    for (const double invalid : {0.0, -1.0, (sc<double>(std::numeric_limits<uint32_t>::max()) + 1.0) / SCALE, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max()}) {
        EXPECT_FALSE(encodeViewpointDimensionUM(invalid, value));
        EXPECT_EQ(value, 0U);
    }
}

TEST(XRViewpointTransport, SplitUint64WordsPreserveTheEntireValue) {
    constexpr std::array<uint64_t, 4> VALUES{0, 1, 0x0123456789ABCDEFULL, std::numeric_limits<uint64_t>::max()};
    for (const uint64_t value : VALUES)
        EXPECT_EQ(joinViewpointU64(splitViewpointU64(value)), value);

    EXPECT_EQ(splitViewpointU64(0x0123456789ABCDEFULL), (SXRViewpointU64{.hi = 0x01234567U, .lo = 0x89ABCDEFU}));
}

TEST(XRViewpointTransport, StereoSampleRoundTripsAsOneUnit) {
    const auto                geometry = stereoGeometry(-0.031234F, 0.033456F);
    SXRViewpointEncodedSample sample;
    ASSERT_TRUE(encodeViewpointSample(geometry, 0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL, sample));
    EXPECT_TRUE(encodedViewpointSampleValid(sample));
    EXPECT_EQ(sample.viewCount, 2U);
    EXPECT_EQ(sample.widthUM, 1'600'000U);
    EXPECT_EQ(sample.heightUM, 900'000U);

    SXRViewpointGeometry decodedGeometry;
    uint64_t             serial = 0, geometryId = 0;
    ASSERT_TRUE(decodeViewpointSample(sample, decodedGeometry, serial, geometryId));
    EXPECT_TRUE(decodedGeometry.valid);
    EXPECT_EQ(decodedGeometry.viewCount, 2U);
    EXPECT_EQ(serial, 0xFEDCBA9876543210ULL);
    EXPECT_EQ(geometryId, 0x0123456789ABCDEFULL);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_NEAR(decodedGeometry.viewPositions[i].x, geometry.viewPositions[i].x, 1e-6F);
        EXPECT_NEAR(decodedGeometry.viewPositions[i].y, geometry.viewPositions[i].y, 1e-6F);
        EXPECT_NEAR(decodedGeometry.viewPositions[i].z, geometry.viewPositions[i].z, 1e-6F);
    }
}

TEST(XRViewpointTransport, MonoSampleLeavesTheUnusedViewCanonicalAndZero) {
    auto geometry      = stereoGeometry();
    geometry.viewCount = 1;

    SXRViewpointEncodedSample sample;
    ASSERT_TRUE(encodeViewpointSample(geometry, 5, 6, sample));
    EXPECT_EQ(sample.viewCount, 1U);
    EXPECT_EQ(sample.viewPositions[1], SXRViewpointPositionUM{});
    EXPECT_TRUE(encodedViewpointSampleValid(sample));
}

TEST(XRViewpointTransport, MonoSampleRejectsANonzeroUnusedView) {
    auto geometry      = stereoGeometry();
    geometry.viewCount = 1;

    SXRViewpointEncodedSample sample;
    ASSERT_TRUE(encodeViewpointSample(geometry, 5, 6, sample));
    sample.viewPositions[1].x = 1;
    EXPECT_FALSE(encodedViewpointSampleValid(sample));

    SXRViewpointGeometry decodedGeometry = stereoGeometry();
    uint64_t             serial = 9, geometryId = 9;
    EXPECT_FALSE(decodeViewpointSample(sample, decodedGeometry, serial, geometryId));
    EXPECT_FALSE(decodedGeometry.valid);
    EXPECT_EQ(decodedGeometry.viewCount, 0U);
    EXPECT_EQ(decodedGeometry.widthMeters, 0.F);
    EXPECT_EQ(decodedGeometry.heightMeters, 0.F);
    for (const auto& view : decodedGeometry.viewPositions) {
        EXPECT_EQ(view.x, 0.F);
        EXPECT_EQ(view.y, 0.F);
        EXPECT_EQ(view.z, 0.F);
    }
    EXPECT_EQ(serial, 0U);
    EXPECT_EQ(geometryId, 0U);
}

TEST(XRViewpointTransport, AnyInvalidStereoEyeRejectsTheWholeEncodedSample) {
    auto geometry                 = stereoGeometry();
    geometry.viewPositions[1].z   = -0.1F;
    SXRViewpointEncodedSample out = encoded(77);

    EXPECT_FALSE(encodeViewpointSample(geometry, 1, 2, out));
    EXPECT_EQ(out, SXRViewpointEncodedSample{});

    auto malformed                   = encoded(78);
    malformed.viewPositions[1].z     = XR_VIEWPOINT_Z_EPSILON_UM;
    SXRViewpointGeometry decodedGeom = stereoGeometry();
    uint64_t             serial = 9, geometryId = 9;
    EXPECT_FALSE(decodeViewpointSample(malformed, decodedGeom, serial, geometryId));
    EXPECT_FALSE(decodedGeom.valid);
    EXPECT_EQ(decodedGeom.viewCount, 0U);
    EXPECT_EQ(serial, 0U);
    EXPECT_EQ(geometryId, 0U);
}

TEST(XRViewpointTransport, SecondEyeOverflowCannotLeakAPartialSample) {
    auto geometry                 = stereoGeometry();
    geometry.viewPositions[1].x   = 3000.F;
    SXRViewpointEncodedSample out = encoded(79);

    EXPECT_FALSE(encodeViewpointSample(geometry, 3, 4, out));
    EXPECT_EQ(out, SXRViewpointEncodedSample{});
}

TEST(XRViewpointTransport, MailboxCoalescesToLatestAndSignalsOnlyPendingEdges) {
    CXRViewpointSampleMailbox mailbox;

    const auto                first = mailbox.publish(encoded(10));
    EXPECT_TRUE(first.accepted);
    EXPECT_TRUE(first.shouldWake);
    EXPECT_EQ(first.generation, 1U);

    auto malformed                  = encoded(99);
    malformed.flags                 = 0;
    const auto rejectedWhilePending = mailbox.publish(malformed);
    EXPECT_FALSE(rejectedWhilePending.accepted);
    EXPECT_FALSE(rejectedWhilePending.shouldWake);
    EXPECT_EQ(rejectedWhilePending.generation, 1U);

    const auto second = mailbox.publish(encoded(11));
    const auto third  = mailbox.publish(encoded(12));
    EXPECT_TRUE(second.accepted);
    EXPECT_FALSE(second.shouldWake);
    EXPECT_EQ(second.generation, 2U);
    EXPECT_TRUE(third.accepted);
    EXPECT_FALSE(third.shouldWake);
    EXPECT_EQ(third.generation, 3U);

    SXRViewpointMailboxRead read;
    ASSERT_TRUE(mailbox.consumeLatest(read));
    EXPECT_EQ(joinViewpointU64(read.sample.serial), 12U);
    EXPECT_EQ(read.generation, 3U);
    EXPECT_EQ(read.superseded, 2U);
    EXPECT_FALSE(mailbox.pending());

    read.generation = 99;
    EXPECT_FALSE(mailbox.consumeLatest(read));
    EXPECT_EQ(read, SXRViewpointMailboxRead{});

    const auto fourth = mailbox.publish(encoded(13));
    EXPECT_TRUE(fourth.shouldWake);
    EXPECT_EQ(fourth.generation, 4U);
}

TEST(XRViewpointTransport, MailboxRejectsMalformedSamplesAndGenerationOverflow) {
    CXRViewpointSampleMailbox empty;
    auto                      malformed = encoded(20);
    malformed.flags                     = 0;
    const auto rejected                 = empty.publish(malformed);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_FALSE(rejected.shouldWake);
    EXPECT_EQ(rejected.generation, 0U);
    EXPECT_FALSE(empty.pending());

    CXRViewpointSampleMailbox nearOverflow{std::numeric_limits<uint64_t>::max() - 1};
    const auto                last = nearOverflow.publish(encoded(21));
    EXPECT_TRUE(last.accepted);
    EXPECT_TRUE(last.shouldWake);
    EXPECT_EQ(last.generation, std::numeric_limits<uint64_t>::max());

    const auto overflow = nearOverflow.publish(encoded(22));
    EXPECT_FALSE(overflow.accepted);
    EXPECT_FALSE(overflow.shouldWake);
    EXPECT_EQ(overflow.generation, std::numeric_limits<uint64_t>::max());

    SXRViewpointMailboxRead read;
    ASSERT_TRUE(nearOverflow.consumeLatest(read));
    EXPECT_EQ(joinViewpointU64(read.sample.serial), 21U);
}

TEST(XRViewpointTransport, MailboxClearDiscardsOnlyTheCurrentlyPendingSample) {
    CXRViewpointSampleMailbox mailbox;
    EXPECT_FALSE(mailbox.clearPending());

    EXPECT_TRUE(mailbox.publish(encoded(30)).shouldWake);
    EXPECT_FALSE(mailbox.publish(encoded(31)).shouldWake);
    EXPECT_EQ(mailbox.generation(), 2U);
    EXPECT_TRUE(mailbox.clearPending());
    EXPECT_FALSE(mailbox.pending());
    EXPECT_EQ(mailbox.generation(), 2U);
    EXPECT_FALSE(mailbox.clearPending());

    SXRViewpointMailboxRead read;
    read.generation = 99;
    read.superseded = 99;
    EXPECT_FALSE(mailbox.consumeLatest(read));
    EXPECT_EQ(read, SXRViewpointMailboxRead{});

    const auto afterClear = mailbox.publish(encoded(32));
    EXPECT_TRUE(afterClear.accepted);
    EXPECT_TRUE(afterClear.shouldWake);
    EXPECT_EQ(afterClear.generation, 3U);
    ASSERT_TRUE(mailbox.consumeLatest(read));
    EXPECT_EQ(joinViewpointU64(read.sample.serial), 32U);
    EXPECT_EQ(read.superseded, 0U);
}

TEST(XRViewpointTransport, ConcurrentProducerConsumerNeverObserveATornSample) {
    constexpr uint64_t        COUNT = 20'000;
    CXRViewpointSampleMailbox mailbox;
    std::atomic<bool>         producerDone   = false;
    const auto                templateSample = encoded(0);

    std::thread               producer([&, templateSample]() {
        for (uint64_t i = 1; i <= COUNT; ++i) {
            auto sample                        = templateSample;
            sample.serial                      = splitViewpointU64(i);
            sample.viewPositions[0].x          = sc<int32_t>(i);
            sample.viewPositions[1].x          = -sc<int32_t>(i);
            [[maybe_unused]] const auto result = mailbox.publish(sample);
        }
        producerDone.store(true, std::memory_order_release);
    });

    uint64_t                  newest = 0;
    while (!producerDone.load(std::memory_order_acquire) || mailbox.pending()) {
        SXRViewpointMailboxRead read;
        if (!mailbox.consumeLatest(read)) {
            std::this_thread::yield();
            continue;
        }

        const uint64_t serial = joinViewpointU64(read.sample.serial);
        EXPECT_GT(serial, newest);
        EXPECT_EQ(read.sample.viewPositions[0].x, sc<int32_t>(serial));
        EXPECT_EQ(read.sample.viewPositions[1].x, -sc<int32_t>(serial));
        newest = serial;
    }
    producer.join();

    EXPECT_EQ(newest, COUNT);
}

TEST(XRViewpointTransport, RenderedDispositionDropsStaleReportsSilently) {
    using OpenXR::eXRViewpointRenderedDisposition;
    using OpenXR::viewpointRenderedDisposition;

    // Deactivated viewpoint: every in-flight report is the benign wire race, never an error.
    EXPECT_EQ(viewpointRenderedDisposition(0, 1), eXRViewpointRenderedDisposition::XR_VIEWPOINT_RENDERED_IGNORE_STALE);
    EXPECT_EQ(viewpointRenderedDisposition(0, 0), eXRViewpointRenderedDisposition::XR_VIEWPOINT_RENDERED_IGNORE_STALE);

    // Re-activation bumped the epoch while a report for the old one was in flight.
    EXPECT_EQ(viewpointRenderedDisposition(2, 1), eXRViewpointRenderedDisposition::XR_VIEWPOINT_RENDERED_IGNORE_STALE);

    // An epoch the compositor never issued is indistinguishable from the same race on this axis;
    // only sample consumption inside the current epoch can prove a violation.
    EXPECT_EQ(viewpointRenderedDisposition(2, 3), eXRViewpointRenderedDisposition::XR_VIEWPOINT_RENDERED_IGNORE_STALE);

    // Current epoch: proceed to consume, where an unknown/replayed sample is a real protocol error.
    EXPECT_EQ(viewpointRenderedDisposition(2, 2), eXRViewpointRenderedDisposition::XR_VIEWPOINT_RENDERED_CONSUME);
}
