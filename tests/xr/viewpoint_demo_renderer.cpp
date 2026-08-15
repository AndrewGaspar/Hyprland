// Deterministic coverage for the protocol-neutral renderer used by the native
// hypxr_viewpoint_v1 visual demo. No compositor, Wayland display, or XR runtime
// is involved.

#include <PortalRenderer.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;
using namespace ViewpointDemo;

static SImage imageFor(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height, uint32_t stride = 0) {
    if (stride == 0)
        stride = width;
    pixels.assign(sc<size_t>(stride) * height, 0);
    return {.pixels = pixels, .width = width, .height = height, .stridePixels = stride};
}

static double ndcX(const SFrustum& frustum, double cameraX, double cameraZ) {
    const double NEAR_X = cameraX * frustum.near / -cameraZ;
    return 2.0 * (NEAR_X - frustum.left) / (frustum.right - frustum.left) - 1.0;
}

static double ndcY(const SFrustum& frustum, double cameraY, double cameraZ) {
    const double NEAR_Y = cameraY * frustum.near / -cameraZ;
    return 2.0 * (NEAR_Y - frustum.bottom) / (frustum.top - frustum.bottom) - 1.0;
}

TEST(ViewpointDemoRenderer, OffAxisFrustumMapsPhysicalCornersToNdc) {
    constexpr SVec3       EYE    = {.x = 0.17, .y = -0.09, .z = 1.25};
    constexpr SPortalSize PORTAL = {.widthMeters = 1.6, .heightMeters = 0.9};
    SFrustum              frustum;
    ASSERT_TRUE(offAxisFrustum(EYE, PORTAL, 0.1, frustum));

    // Translate world portal corners by -eye into the fixed -Z camera frame.
    EXPECT_NEAR(ndcX(frustum, -PORTAL.widthMeters * 0.5 - EYE.x, -EYE.z), -1.0, 1e-12);
    EXPECT_NEAR(ndcX(frustum, PORTAL.widthMeters * 0.5 - EYE.x, -EYE.z), 1.0, 1e-12);
    EXPECT_NEAR(ndcY(frustum, -PORTAL.heightMeters * 0.5 - EYE.y, -EYE.z), -1.0, 1e-12);
    EXPECT_NEAR(ndcY(frustum, PORTAL.heightMeters * 0.5 - EYE.y, -EYE.z), 1.0, 1e-12);
}

TEST(ViewpointDemoRenderer, PortalRayUsesPixelCentersAndFixedOrientation) {
    SRay ray;
    ASSERT_TRUE(portalRay({.x = 0.2, .y = -0.1, .z = 1.2}, {.widthMeters = 1.6, .heightMeters = 0.9}, 0, 0, 1, 1, ray));
    EXPECT_DOUBLE_EQ(ray.origin.x, 0.2);
    EXPECT_DOUBLE_EQ(ray.origin.y, -0.1);
    EXPECT_DOUBLE_EQ(ray.origin.z, 1.2);
    EXPECT_DOUBLE_EQ(ray.direction.x, -0.2);
    EXPECT_DOUBLE_EQ(ray.direction.y, 0.1);
    EXPECT_DOUBLE_EQ(ray.direction.z, -1.2);
}

TEST(ViewpointDemoRenderer, GeometryRejectsNonfiniteOrBehindViewer) {
    SFrustum frustum = {.left = 9.0};
    EXPECT_FALSE(offAxisFrustum({.z = 0.0}, {.widthMeters = 1.6, .heightMeters = 0.9}, 0.1, frustum));
    EXPECT_DOUBLE_EQ(frustum.left, 0.0);

    SRay ray{};
    ray.origin.x = 9.0;
    EXPECT_FALSE(portalRay({.x = std::numeric_limits<double>::quiet_NaN(), .z = 1.0}, {.widthMeters = 1.6, .heightMeters = 0.9}, 0, 0, 1, 1, ray));
    EXPECT_DOUBLE_EQ(ray.origin.x, 0.0);
}

TEST(ViewpointDemoRenderer, RenderSizePreservesPackedSbsDestinationAspectWithinBudget) {
    SRenderSize size;
    ASSERT_TRUE(fitSBSRenderSize(3840, 1080, 256, 144, size));
    EXPECT_EQ(size.width, 256U);
    EXPECT_EQ(size.height, 144U);
    EXPECT_EQ(sc<uint64_t>(size.width * 2U) * 1080U, sc<uint64_t>(size.height) * 3840U);

    ASSERT_TRUE(fitSBSRenderSize(1920, 1080, 256, 144, size));
    EXPECT_EQ(size.width, 128U);
    EXPECT_EQ(size.height, 144U);
    EXPECT_EQ(sc<uint64_t>(size.width * 2U) * 1080U, sc<uint64_t>(size.height) * 1920U);

    size = {.width = 9, .height = 9};
    EXPECT_FALSE(fitSBSRenderSize(1279, 720, 256, 144, size));
    EXPECT_EQ(size.width, 0U);
    EXPECT_EQ(size.height, 0U);

    EXPECT_FALSE(fitSBSRenderSize(2558, 720, 256, 144, size));
    EXPECT_EQ(size.width, 0U);
    EXPECT_EQ(size.height, 0U);
}

TEST(ViewpointDemoRenderer, TransientConfigureDoesNotStickyDisableFinalMapping) {
    SFeedbackState feedback = {.capabilitiesSupported = true};
    SRenderSize    size;

    EXPECT_FALSE(fitSBSRenderSize(1024, 1126, 256, 144, size));
    feedback.mappingSupported = false;
    EXPECT_FALSE(feedbackShouldBeEnabled(feedback));
    EXPECT_FALSE(feedback.stickyDisabled);

    ASSERT_TRUE(fitSBSRenderSize(2048, 1152, 256, 144, size));
    EXPECT_EQ(size.width, 128U);
    EXPECT_EQ(size.height, 144U);
    feedback.mappingSupported = true;
    EXPECT_TRUE(feedbackShouldBeEnabled(feedback));

    feedback.stickyDisabled = true;
    EXPECT_FALSE(feedbackShouldBeEnabled(feedback));
}

TEST(ViewpointDemoRenderer, FullSbsIsPairLatchedAndEyeOrdered) {
    std::vector<uint32_t>  pixelsA;
    std::vector<uint32_t>  pixelsB;
    const auto             imageA = imageFor(pixelsA, 160, 90);
    const auto             imageB = imageFor(pixelsB, 160, 90);
    constexpr SPortalSize  PORTAL = {.widthMeters = 1.6, .heightMeters = 0.9};
    constexpr SStereoViews VIEWS  = {.left = {.x = -0.032, .z = 1.2}, .right = {.x = 0.032, .z = 1.2}};

    ASSERT_TRUE(renderPortalSBS(imageA, PORTAL, VIEWS));
    ASSERT_TRUE(renderPortalSBS(imageB, PORTAL, {.left = VIEWS.right, .right = VIEWS.left}));

    constexpr uint32_t PANE = 80;
    for (uint32_t y = 0; y < imageA.height; ++y) {
        for (uint32_t x = 0; x < PANE; ++x) {
            EXPECT_EQ(pixelsA[sc<size_t>(y) * imageA.width + x], pixelsB[sc<size_t>(y) * imageB.width + PANE + x]);
            EXPECT_EQ(pixelsA[sc<size_t>(y) * imageA.width + PANE + x], pixelsB[sc<size_t>(y) * imageB.width + x]);
        }
    }
    EXPECT_NE(pixelHash(imageA), 0U);
}

static double colorCentroidX(const SImage& image, bool farBox) {
    const uint32_t PANE  = image.width / 2U;
    double         sum   = 0.0;
    uint32_t       count = 0;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < PANE; ++x) {
            const uint32_t COLOR = image.pixels[sc<size_t>(y) * image.stridePixels + x];
            const uint32_t RED   = (COLOR >> 16U) & 0xFFU;
            const uint32_t GREEN = (COLOR >> 8U) & 0xFFU;
            const uint32_t BLUE  = COLOR & 0xFFU;
            const bool     MATCH = farBox ? BLUE > RED * 3U / 2U && BLUE > GREEN * 3U / 2U : RED > GREEN * 5U / 4U && GREEN > BLUE * 3U / 2U;
            if (!MATCH)
                continue;
            sum += x;
            ++count;
        }
    }
    EXPECT_GT(count, 8U);
    return sum / count;
}

TEST(ViewpointDemoRenderer, HeadTranslationProducesDepthDependentImageParallax) {
    std::vector<uint32_t> centeredPixels;
    std::vector<uint32_t> translatedPixels;
    const auto            centered   = imageFor(centeredPixels, 320, 180);
    const auto            translated = imageFor(translatedPixels, 320, 180);
    constexpr SPortalSize PORTAL     = {.widthMeters = 1.6, .heightMeters = 0.9};

    ASSERT_TRUE(renderPortalSBS(centered, PORTAL, {.left = {.x = 0.0, .z = 1.2}, .right = {.x = 0.0, .z = 1.2}}));
    ASSERT_TRUE(renderPortalSBS(translated, PORTAL, {.left = {.x = 0.16, .z = 1.2}, .right = {.x = 0.16, .z = 1.2}}));
    EXPECT_NE(pixelHash(centered), pixelHash(translated));

    const double NEAR_SHIFT = colorCentroidX(translated, false) - colorCentroidX(centered, false);
    const double FAR_SHIFT  = colorCentroidX(translated, true) - colorCentroidX(centered, true);
    EXPECT_GT(NEAR_SHIFT, 2.0);
    EXPECT_GT(FAR_SHIFT, NEAR_SHIFT + 2.0);
}

static double aimMarkerCentroidX(const SImage& image) {
    const uint32_t PANE  = image.width / 2U;
    double         sum   = 0.0;
    uint32_t       count = 0;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < PANE; ++x) {
            const uint32_t COLOR = image.pixels[sc<size_t>(y) * image.stridePixels + x];
            const uint32_t RED   = (COLOR >> 16U) & 0xFFU;
            const uint32_t GREEN = (COLOR >> 8U) & 0xFFU;
            const uint32_t BLUE  = COLOR & 0xFFU;
            if (RED <= GREEN * 2U || RED <= BLUE * 2U)
                continue;
            sum += x;
            ++count;
        }
    }
    EXPECT_GT(count, 4U);
    return sum / count;
}

TEST(ViewpointDemoRenderer, AuthoritativeAimStateIsFixedWhileProjectionMoves) {
    const SVec3           BEFORE = authoritativeAimImpact();
    std::vector<uint32_t> centeredPixels;
    std::vector<uint32_t> translatedPixels;
    const auto            centered   = imageFor(centeredPixels, 320, 180);
    const auto            translated = imageFor(translatedPixels, 320, 180);
    constexpr SPortalSize PORTAL     = {.widthMeters = 1.6, .heightMeters = 0.9};

    ASSERT_TRUE(renderPortalSBS(centered, PORTAL, {.left = {.z = 1.2}, .right = {.z = 1.2}}));
    ASSERT_TRUE(renderPortalSBS(translated, PORTAL, {.left = {.x = 0.16, .z = 1.2}, .right = {.x = 0.16, .z = 1.2}}));
    const SVec3 AFTER = authoritativeAimImpact();

    EXPECT_DOUBLE_EQ(BEFORE.x, AFTER.x);
    EXPECT_DOUBLE_EQ(BEFORE.y, AFTER.y);
    EXPECT_DOUBLE_EQ(BEFORE.z, AFTER.z);
    EXPECT_GT(aimMarkerCentroidX(translated) - aimMarkerCentroidX(centered), 4.0);

    // The portal-locked cyan reticle remains centered in both images.
    const uint32_t CENTER_X = centered.width / 4U;
    const uint32_t CENTER_Y = centered.height / 2U;
    EXPECT_EQ(centered.pixels[sc<size_t>(CENTER_Y) * centered.stridePixels + CENTER_X], translated.pixels[sc<size_t>(CENTER_Y) * translated.stridePixels + CENTER_X]);
}

TEST(ViewpointDemoRenderer, InactiveFallbackHasZeroDisparityAndStableHash) {
    std::vector<uint32_t> pixels;
    const auto            image = imageFor(pixels, 160, 73, 164);
    ASSERT_TRUE(renderFallbackSBS(image));

    constexpr uint32_t PANE    = 80;
    constexpr uint32_t RIGHT_X = 80;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < PANE; ++x)
            EXPECT_EQ(pixels[sc<size_t>(y) * image.stridePixels + x], pixels[sc<size_t>(y) * image.stridePixels + RIGHT_X + x]);
    }
    EXPECT_EQ(pixelHash(image), 13153189908930127355ULL);
}

TEST(ViewpointDemoRenderer, OddFullSbsWidthIsRejected) {
    std::vector<uint32_t> pixels;
    const auto            image = imageFor(pixels, 161, 73);
    EXPECT_FALSE(renderFallbackSBS(image));
    EXPECT_FALSE(renderPortalSBS(image, {.widthMeters = 1.6, .heightMeters = 0.9}, {.left = {.z = 1.2}, .right = {.z = 1.2}}));
}

TEST(ViewpointDemoRenderer, WorkerCountNeverChangesARenderedByte) {
    // Frame hashes are part of the demo's contract (--render, --render-fallback,
    // the --debug commit log, and the pair-latching assertions above), so the row
    // partitioning must be a pure scheduling change. Compare whole buffers rather
    // than hashes: that covers the stride padding the hash deliberately skips.
    constexpr uint32_t     WIDTH  = 96;
    constexpr uint32_t     HEIGHT = 54;
    constexpr uint32_t     STRIDE = 101;
    constexpr SPortalSize  PORTAL = {.widthMeters = 1.4, .heightMeters = 0.8};
    constexpr SStereoViews VIEWS  = {.left = {.x = -0.031, .y = 0.02, .z = 1.1}, .right = {.x = 0.033, .y = 0.02, .z = 1.1}};
    // One count below, one at, and two above MAX_AUTO_RENDER_THREADS, plus a count
    // that exceeds the row count so the pool has to clamp its participants.
    constexpr std::array  COUNTS = {2U, 3U, 8U, MAX_AUTO_RENDER_THREADS, 24U, HEIGHT * 4U};

    std::vector<uint32_t> singlePixels;
    std::vector<uint32_t> threadedPixels;
    const auto            single   = imageFor(singlePixels, WIDTH, HEIGHT, STRIDE);
    const auto            threaded = imageFor(threadedPixels, WIDTH, HEIGHT, STRIDE);

    ASSERT_TRUE(renderPortalSBS(single, PORTAL, VIEWS, 1));
    for (const uint32_t COUNT : COUNTS) {
        std::fill(threadedPixels.begin(), threadedPixels.end(), 0U);
        ASSERT_TRUE(renderPortalSBS(threaded, PORTAL, VIEWS, COUNT));
        EXPECT_EQ(singlePixels, threadedPixels) << "portal render diverged on " << COUNT << " workers";
    }

    std::vector<uint32_t> singleFallback;
    std::vector<uint32_t> threadedFallback;
    const auto            fallbackSingle   = imageFor(singleFallback, WIDTH, HEIGHT, STRIDE);
    const auto            fallbackThreaded = imageFor(threadedFallback, WIDTH, HEIGHT, STRIDE);

    ASSERT_TRUE(renderFallbackSBS(fallbackSingle, 1));
    for (const uint32_t COUNT : COUNTS) {
        std::fill(threadedFallback.begin(), threadedFallback.end(), 0U);
        ASSERT_TRUE(renderFallbackSBS(fallbackThreaded, COUNT));
        EXPECT_EQ(singleFallback, threadedFallback) << "fallback render diverged on " << COUNT << " workers";
    }

    // The demo's own default must be a usable, bounded worker budget.
    EXPECT_GE(defaultRenderThreads(), 1U);
    EXPECT_LE(defaultRenderThreads(), MAX_AUTO_RENDER_THREADS);
}

TEST(ViewpointDemoRenderer, RenderingIsDeterministicAcrossStridePadding) {
    std::vector<uint32_t>  compactPixels;
    std::vector<uint32_t>  paddedPixels;
    const auto             compact = imageFor(compactPixels, 96, 54);
    const auto             padded  = imageFor(paddedPixels, 96, 54, 101);
    constexpr SStereoViews VIEWS   = {.left = {.x = -0.031, .y = 0.02, .z = 1.1}, .right = {.x = 0.033, .y = 0.02, .z = 1.1}};

    ASSERT_TRUE(renderPortalSBS(compact, {.widthMeters = 1.4, .heightMeters = 0.8}, VIEWS));
    ASSERT_TRUE(renderPortalSBS(padded, {.widthMeters = 1.4, .heightMeters = 0.8}, VIEWS));
    EXPECT_EQ(pixelHash(compact), pixelHash(padded));
}
