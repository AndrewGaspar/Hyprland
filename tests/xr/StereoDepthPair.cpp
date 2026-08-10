#include <openxr/XRStereoPair.hpp>
#include <output/StereoPacking.hpp>

#include <gtest/gtest.h>

using namespace OpenXR::Stereo;
using namespace Render::Stereo;

// tests/xr/StereoDepthPair.cpp — WP X3/X4, the DEPTH desktop on XR monitors (research/24 §6).
//
// X1's tests live next door and cover the CONTENT pair. This file covers the second producer, and
// its centre of gravity is deliberately §5.6: the two producers submit the same two quads and mean
// entirely different things by them, and the doc's warning is that a single shared un-map is how
// the cursor ends up half a screen from where the user is pointing. So the mapping tests assert
// both producers, in both directions, side by side — a shared implementation cannot pass them.
//
// The other half is geometry that only ever runs on the frame thread, where nothing may allocate,
// lock, refcount or read config. Every expression below is the one the frame thread runs.

namespace {
    // The shape the user validated in-headset: a 2560x1440 XR monitor. Declared per EYE, so the
    // packed mode is 5120x1440 and one pane is the declared size again.
    constexpr double PANEW = 2560, PANEH = 1440;

    // A pane with a 5% chrome margin all round, the shape createLayerSwapchain builds.
    SPaneGeom        packed(int panes = 2) {
        SPaneGeom g;
        g.panes           = panes;
        g.paneContent     = {PANEW, PANEH};
        g.paneFull        = {PANEW + 128, PANEH + 96};
        g.contentOffsetPx = {64, 48};
        return g;
    }

    SPaneGeom mono() {
        SPaneGeom g;
        g.panes           = 1;
        g.paneContent     = {PANEW, PANEH};
        g.paneFull        = {PANEW + 128, PANEH + 96};
        g.contentOffsetPx = {64, 48};
        return g;
    }

    SPairDecl depthDecl(bool submit = true) {
        return {.producer = PRODUCER_DEPTH, .layout = CONTENT_SBS, .submit = submit, .modeW = (uint32_t)(PANEW * 2), .modeH = (uint32_t)PANEH};
    }

    SPairDecl contentDecl(eContentLayout layout = CONTENT_SBS) {
        return {.producer = PRODUCER_CONTENT, .layout = layout, .submit = true, .modeW = 0, .modeH = 0};
    }
}

// ---------------------------------------------------------------------------------------------
// §5.6 — the un-map belongs to the PRODUCER
// ---------------------------------------------------------------------------------------------

// The whole point of the file. Both producers show ONE PANE per eye and both split side by side,
// so nothing about the LAYOUT distinguishes them — only who produced the panes does. Feed both the
// same pane uv and the answers must differ by exactly the half-a-screen §5.6 warns about.
TEST(XRDepthPair, TheTwoProducersUnmapTheSamePaneUVDifferently) {
    const Vector2D MIDDLE{0.5, 0.5};

    // DEPTH: the pane IS the whole logical desktop. The middle of the pane is the middle of the
    // desktop, in BOTH eyes — that is what makes the two quads fusible in the first place.
    EXPECT_EQ(paneUVToMonitorUV(MIDDLE, depthDecl(), 0), MIDDLE);
    EXPECT_EQ(paneUVToMonitorUV(MIDDLE, depthDecl(), 1), MIDDLE);

    // CONTENT: the pane is half of a packed image, so the middle of eye 0's pane is a QUARTER of
    // the way across the image. Hand a depth monitor this mapping and the cursor lands at 0.25 for
    // a pointer aimed dead centre — §5.6's exact failure, in one number.
    EXPECT_EQ(paneUVToMonitorUV(MIDDLE, contentDecl(), 0), (Vector2D{0.25, 0.5}));
    EXPECT_EQ(paneUVToMonitorUV(MIDDLE, contentDecl(), 1), (Vector2D{0.75, 0.5}));
}

TEST(XRDepthPair, DepthUnmapIsTheIdentityAcrossTheWholePaneAndBothEyes) {
    for (const double U : {0.0, 0.1, 0.5, 0.9, 1.0}) {
        for (const double V : {0.0, 0.33, 1.0}) {
            for (const int EYE : {0, 1}) {
                EXPECT_EQ(paneUVToMonitorUV({U, V}, depthDecl(), EYE), (Vector2D{U, V}));
                EXPECT_EQ(monitorUVToPaneUV({U, V}, depthDecl(), EYE), (Vector2D{U, V}));
            }
        }
    }
}

// Both directions, both producers, exact inverses. The reverse map is what a future "put the
// cursor there" path needs, and an un-map that is not invertible is one that has silently lost the
// eye it belongs to.
TEST(XRDepthPair, BothDirectionsRoundTripForBothProducers) {
    const SPairDecl DECLS[] = {depthDecl(), contentDecl(CONTENT_SBS), contentDecl(CONTENT_TAB), {}};

    for (const auto& DECL : DECLS) {
        for (const int EYE : {0, 1}) {
            for (const double U : {0.0, 0.25, 0.5, 0.75, 1.0}) {
                const Vector2D PANE{U, 1.0 - U};
                const auto     MON = paneUVToMonitorUV(PANE, DECL, EYE);
                const auto     BACK = monitorUVToPaneUV(MON, DECL, EYE);
                EXPECT_NEAR(BACK.x, PANE.x, 1e-9);
                EXPECT_NEAR(BACK.y, PANE.y, 1e-9);
            }
        }
    }
}

// A monitor with no pair at all must not be touched by either mapping. This is the case every
// session that has never configured stereo runs, on every pointer event, forever.
TEST(XRDepthPair, NoProducerIsTheIdentity) {
    const SPairDecl NONE;
    EXPECT_EQ(NONE.producer, PRODUCER_NONE);
    EXPECT_EQ(paneUVToMonitorUV({0.3, 0.7}, NONE, 0), (Vector2D{0.3, 0.7}));
    EXPECT_EQ(monitorUVToPaneUV({0.3, 0.7}, NONE, 1), (Vector2D{0.3, 0.7}));
}

// ---------------------------------------------------------------------------------------------
// The published word
// ---------------------------------------------------------------------------------------------

TEST(XRDepthPair, DeclarationSurvivesAPackUnpackRoundTrip) {
    const SPairDecl DECLS[] = {
        {},
        depthDecl(true),
        depthDecl(false),
        contentDecl(CONTENT_HSBS),
        {.producer = PRODUCER_DEPTH, .layout = CONTENT_SBS, .submit = true, .modeW = 16777215, .modeH = 16777215},
    };

    for (const auto& DECL : DECLS)
        EXPECT_EQ(unpackDecl(packDecl(DECL)), DECL);
}

// A word the frame thread has never been written (zero) must read as "one ordinary quad". That is
// the state every layer starts in, and it is read before the main thread has published anything.
TEST(XRDepthPair, AnUnwrittenWordIsAMonoQuad) {
    const auto DECL = unpackDecl(0);
    EXPECT_EQ(DECL.producer, PRODUCER_NONE);
    EXPECT_EQ(DECL.layout, CONTENT_OFF);
    EXPECT_FALSE(DECL.submit);
    EXPECT_EQ(quadsFor(DECL), 1u);
}

// Garbage in the enum fields must not select a producer or a layout that does not exist — the
// frame thread indexes on both.
TEST(XRDepthPair, OutOfRangeEnumFieldsFallBackToOff) {
    const auto DECL = unpackDecl(0xFFFFFFFFFFFFFFFFull);
    EXPECT_EQ(DECL.producer, PRODUCER_NONE);
    EXPECT_EQ(DECL.layout, CONTENT_OFF);
}

// The race guard. A declaration carrying a mode only applies to an image of that mode; anything
// else falls back to one honest quad rather than splitting an image it is not describing.
TEST(XRDepthPair, DeclarationOnlyDescribesTheModeItWasMadeFor) {
    const auto DECL = depthDecl();

    EXPECT_TRUE(describes(DECL, 5120, 1440));
    EXPECT_FALSE(describes(DECL, 2560, 1440)); // the pack has not landed yet
    EXPECT_FALSE(describes(DECL, 5120, 1080)); // a mode retry landed somewhere else

    // X1's declaration carries no mode and therefore constrains nothing — its swapchain never
    // changes size when the pair engages, so there is no mismatch to protect against.
    EXPECT_TRUE(describes(contentDecl(), 1920, 1080));
    EXPECT_TRUE(describes(contentDecl(), 5120, 1440));
}

// ---------------------------------------------------------------------------------------------
// The kill switch degrades to ONE MONO PANE, never to a doubled image
// ---------------------------------------------------------------------------------------------

// openxr:stereo_quad_pair = 0 on a depth-packed monitor. The monitor is still physically packed —
// the mode does not change, because changing it would be a modeset — so the single quad must show
// PANE 0 only. Submitting the whole image would put a side-by-side desktop in both eyes.
TEST(XRDepthPair, KillSwitchSubmitsOneQuadOfPaneZero) {
    const auto GEOM = packed();
    const auto OFF  = depthDecl(false);

    EXPECT_EQ(quadsFor(OFF), 1u);
    EXPECT_FALSE(OFF.submit);
    // The layout STAYS packed: the pixels are laid out that way regardless, and the quad's aspect
    // is derived from one pane either way. Clearing it here is how the quad would change shape when
    // the switch was thrown.
    EXPECT_EQ(OFF.layout, CONTENT_SBS);

    const auto RECT = paneFullRect(GEOM, 0);
    EXPECT_EQ(RECT.x, 0);
    EXPECT_EQ(RECT.w, (int32_t)GEOM.paneFull.x);
    EXPECT_LT(RECT.w, (int32_t)swapchainSizeFor(GEOM).x); // strictly less than the whole image
}

TEST(XRDepthPair, PairSubmitsTwoQuadsAndTheBudgetIsCheckedForBoth) {
    const auto ON  = depthDecl(true);
    const auto OFF = depthDecl(false);

    EXPECT_EQ(quadsFor(ON), 2u);

    // A pair with one slot left is refused WHOLE — one eye seeing the desktop and the other seeing
    // nothing is not a degraded picture, it is a nauseating one.
    EXPECT_FALSE(submissionFits(15, 16, ON));
    EXPECT_TRUE(submissionFits(14, 16, ON));
    // ...and the flattened monitor behind it still fits in that last slot.
    EXPECT_TRUE(submissionFits(15, 16, OFF));
}

// §8's "three depth monitors + a stereo game monitor = 8 quads against a floor of 16", made
// concrete: every monitor costing two, the budget refusing whole pairs, and a refused pair leaving
// its slot to a cheaper monitor rather than ending the frame.
TEST(XRDepthPair, ManyDepthPairsAgainstTheLayerFloor) {
    // An ODD budget is the interesting one, because it is the only way a pair can be refused while
    // a slot remains — which is exactly the case the `continue`-rather-than-`break` rule exists for.
    constexpr uint32_t BUDGET = 15;

    size_t             quads  = 0;
    int                paired = 0, mono = 0;
    for (int i = 0; i < 8; ++i) {
        if (!submissionFits(quads, BUDGET, depthDecl())) {
            EXPECT_EQ(i, 7) << "the first seven pairs must fit (14 quads)";
            continue;
        }
        quads += quadsFor(depthDecl());
        ++paired;
    }
    // ...and the cheaper mono layers behind it still take the slot the refused pair left.
    for (int i = 0; i < 3; ++i) {
        if (!submissionFits(quads, BUDGET, SPairDecl{}))
            continue;
        quads += 1;
        ++mono;
    }

    EXPECT_EQ(paired, 7);
    EXPECT_EQ(mono, 1);
    EXPECT_EQ(quads, BUDGET);

    // With an even budget the pairs tile it exactly and nothing is left over — the ordinary case.
    quads = 0;
    for (int i = 0; i < 9; ++i) {
        if (!submissionFits(quads, 16, depthDecl()))
            continue;
        quads += quadsFor(depthDecl());
    }
    EXPECT_EQ(quads, 16u);
}

// ---------------------------------------------------------------------------------------------
// Pane geometry inside the swapchain (WP X4)
// ---------------------------------------------------------------------------------------------

TEST(XRDepthPair, PackedSwapchainIsTwoMarginedPanes) {
    const auto GEOM = packed();

    EXPECT_EQ(swapchainSizeFor(GEOM), (Vector2D{(PANEW + 128) * 2, PANEH + 96}));

    const auto L = paneFullRect(GEOM, 0);
    const auto R = paneFullRect(GEOM, 1);

    // They tile the whole image with no gap and no overlap, and each is a whole margined pane —
    // which is what lets each eye's quad carry its own chrome ring (the reason X4 exists).
    EXPECT_EQ(L.x, 0);
    EXPECT_EQ(L.x + L.w, R.x);
    EXPECT_EQ(R.x + R.w, (int32_t)swapchainSizeFor(GEOM).x);
    EXPECT_EQ(L.w, R.w);
    EXPECT_EQ(L.h, R.h);
    EXPECT_EQ(L.y, 0);
    EXPECT_EQ(R.y, 0);
}

TEST(XRDepthPair, MonoLayerGeometryIsTheIdentity) {
    const auto GEOM = mono();

    EXPECT_EQ(swapchainSizeFor(GEOM), GEOM.paneFull);
    EXPECT_EQ(paneFullRect(GEOM, 0), (SImageRect{0, 0, (int32_t)GEOM.paneFull.x, (int32_t)GEOM.paneFull.y}));
    // An out-of-range pane index on a mono layer clamps to the only pane there is, rather than
    // producing a rect past the end of the image.
    EXPECT_EQ(paneFullRect(GEOM, 1), paneFullRect(GEOM, 0));
}

// The blit's two destinations. These are the rects blitBuffer scissors to, and the property that
// matters is that they are SEPARATED — a contiguous double-wide source cannot land in them with a
// single draw, which is the entire reason the blit loops.
TEST(XRDepthPair, PaneContentDestinationsAreSeparatedByTheMargins) {
    const auto GEOM = packed();
    const auto D0   = paneContentDestGL(GEOM, 0);
    const auto D1   = paneContentDestGL(GEOM, 1);

    EXPECT_EQ(D0.w, (int32_t)PANEW);
    EXPECT_EQ(D1.w, (int32_t)PANEW);
    EXPECT_EQ(D0.h, (int32_t)PANEH);
    EXPECT_EQ(D0.x, (int32_t)GEOM.contentOffsetPx.x);
    EXPECT_EQ(D1.x, (int32_t)(GEOM.paneFull.x + GEOM.contentOffsetPx.x));
    EXPECT_GT(D1.x, D0.x + D0.w) << "the two content rects must not touch — the margins sit between them";

    // GL bottom-left: the content's bottom edge is paneFull.y - offsetY - contentH, and the margins
    // in this fixture are symmetric, so it equals the top offset.
    EXPECT_EQ(D0.y, (int32_t)(GEOM.paneFull.y - GEOM.contentOffsetPx.y - PANEH));
    EXPECT_EQ(D1.y, D0.y);

    // Both stay inside the image.
    EXPECT_GE(D0.x, 0);
    EXPECT_LE(D1.x + D1.w, (int32_t)swapchainSizeFor(GEOM).x);
    EXPECT_GE(D0.y, 0);
    EXPECT_LE(D0.y + D0.h, (int32_t)swapchainSizeFor(GEOM).y);
}

// A chrome-less monitor (openxr:chrome_enabled = 0 / margin 0) is the degenerate case, and it must
// still tile exactly — the panes then abut with nothing between them.
TEST(XRDepthPair, PackedGeometryWithNoChromeStillTiles) {
    SPaneGeom g;
    g.panes           = 2;
    g.paneContent     = {PANEW, PANEH};
    g.paneFull        = {PANEW, PANEH};
    g.contentOffsetPx = {0, 0};

    const auto D0 = paneContentDestGL(g, 0);
    const auto D1 = paneContentDestGL(g, 1);
    EXPECT_EQ(D0.x + D0.w, D1.x);
    EXPECT_EQ(D1.x + D1.w, (int32_t)swapchainSizeFor(g).x);
    EXPECT_EQ(paneFullRect(g, 0), D0);
}

// ---------------------------------------------------------------------------------------------
// The quad's shape must not change when depth engages
// ---------------------------------------------------------------------------------------------

// The strongest property this WP has, and the one the user will notice if it breaks: turning the
// depth desktop on must not resize or reshape a single monitor in the headset. The pack doubles the
// MODE and the pane is the declared size, so presentedPaneSize hands the anchor solve exactly the
// pixels it had before.
TEST(XRDepthPair, PackingAMonitorDoesNotChangeItsApparentShape) {
    const Vector2D DECLARED{PANEW, PANEH};
    const Vector2D PACKEDMODE = Monitor::Stereo::requestedMode(DECLARED, Config::STEREO_SBS, /*virtualPack=*/true);

    EXPECT_EQ(PACKEDMODE, (Vector2D{PANEW * 2, PANEH}));

    // Before: a mono XR monitor's aspect comes from its whole mode.
    const float BEFORE = presentedAspect(DECLARED, CONTENT_OFF);
    // After: the packed mode's aspect through the pair's layout.
    const float AFTER = presentedAspect(PACKEDMODE, CONTENT_SBS);

    EXPECT_FLOAT_EQ(AFTER, BEFORE);
    EXPECT_EQ(presentedPaneSize(PACKEDMODE, CONTENT_SBS), DECLARED);
}

// ...and the inversion itself, which is where the two kinds of stereo output part ways. A physical
// panel names its mode and the logical desktop is halved out of it; an output with no panel
// declares the size it wants to work at and the mode is doubled into existence.
TEST(XRDepthPair, VirtualPackDerivesTheModeWhilePhysicalPackNamesIt) {
    const Vector2D DECLARED{PANEW, PANEH};

    EXPECT_EQ(Monitor::Stereo::requestedMode(DECLARED, Config::STEREO_SBS, true), (Vector2D{5120, 1440}));
    EXPECT_EQ(Monitor::Stereo::requestedMode(DECLARED, Config::STEREO_SBS, false), DECLARED);
    EXPECT_EQ(Monitor::Stereo::requestedMode(DECLARED, Config::STEREO_OFF, true), DECLARED);

    // ...and either way the pane, which is what everything above the final blit works at, is the
    // declared size for a virtual pack and half the named mode for a physical one.
    EXPECT_EQ(Monitor::Stereo::paneSize(Monitor::Stereo::requestedMode(DECLARED, Config::STEREO_SBS, true), Config::STEREO_SBS), DECLARED);
    EXPECT_EQ(Monitor::Stereo::paneSize(Monitor::Stereo::requestedMode({3840, 1080}, Config::STEREO_SBS, false), Config::STEREO_SBS), (Vector2D{1920, 1080}));
}

// Mode REQUESTS are not resolutions: `preferred` is (0,0) and highrr/highres/maxwidth are negative
// sentinels. Doubling one of those would invent a mode out of a sentinel.
TEST(XRDepthPair, VirtualPackNeverDoublesAModeSentinel) {
    for (const Vector2D SENTINEL : {Vector2D{}, Vector2D{-1, -1}, Vector2D{-1, -2}, Vector2D{-1, -3}})
        EXPECT_EQ(Monitor::Stereo::requestedMode(SENTINEL, Config::STEREO_SBS, true), SENTINEL);
}

// ---------------------------------------------------------------------------------------------
// §5.4 — the cursor's depth ease
// ---------------------------------------------------------------------------------------------

TEST(XRDepthPair, CursorDisparityEasesTowardTheTargetAndSettles) {
    float       v  = 0.f;
    const float TO = 0.01f;

    // One 90 Hz frame moves it a fraction of the way, never all of it — a snap is the bug.
    v = easeCursorDisparity(v, TO, 1.f / 90.f, CURSOR_DISPARITY_EASE_TAU_SEC);
    EXPECT_GT(v, 0.f);
    EXPECT_LT(v, TO);

    // ~80 ms (3τ) later it is essentially there. That is §5.4's window: long enough that a window
    // edge does not snap, short enough that the cursor does not lag the pointer.
    for (int i = 0; i < 6; ++i)
        v = easeCursorDisparity(v, TO, 1.f / 90.f, CURSOR_DISPARITY_EASE_TAU_SEC);
    EXPECT_NEAR(v, TO, TO * 0.1f);
}

TEST(XRDepthPair, CursorDisparityEaseIsInterruptible) {
    // Cross one edge, then another before the first ease finished. An exponential has no memory of
    // where it was going, so this needs no state — assert that it simply heads the other way.
    float v = easeCursorDisparity(0.f, 0.01f, 0.02f, CURSOR_DISPARITY_EASE_TAU_SEC);
    EXPECT_GT(v, 0.f);

    const float MID = v;
    v               = easeCursorDisparity(v, 0.f, 0.02f, CURSOR_DISPARITY_EASE_TAU_SEC);
    EXPECT_LT(v, MID);
    EXPECT_GE(v, 0.f);
}

TEST(XRDepthPair, CursorDisparityEaseDegeneratesToASnap) {
    // No time passed, or no time constant: hand back the target rather than dividing by zero or
    // freezing the cursor at a stale depth.
    EXPECT_FLOAT_EQ(easeCursorDisparity(0.f, 0.5f, 0.f, CURSOR_DISPARITY_EASE_TAU_SEC), 0.5f);
    EXPECT_FLOAT_EQ(easeCursorDisparity(0.f, 0.5f, 0.016f, 0.f), 0.5f);
}

// Pane 0 is the LEFT eye and takes the POSITIVE shift, matching Desktop::Depth::eyeSign — a raised
// thing moves right in the left eye. Getting this backwards puts the cursor BEHIND the screen when
// it should float in front, which is uncomfortable without ever looking broken.
TEST(XRDepthPair, CursorDisparitySignMatchesTheEyeConvention) {
    EXPECT_FLOAT_EQ(cursorDisparityForPane(0.01f, 0), 0.01f);
    EXPECT_FLOAT_EQ(cursorDisparityForPane(0.01f, 1), -0.01f);
    // Symmetric about the un-shifted position, so the fused cursor sits at the cyclopean point —
    // which is exactly the coordinate the DEPTH un-map above hands the pointer.
    EXPECT_FLOAT_EQ(cursorDisparityForPane(0.01f, 0) + cursorDisparityForPane(0.01f, 1), 0.f);
}
