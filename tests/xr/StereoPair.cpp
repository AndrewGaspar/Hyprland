#include <openxr/XRStereoPair.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace OpenXR::Stereo;
using namespace Render::Stereo;

// tests/xr/StereoPair.cpp — WP X1, the OpenXR quad pair (research/24 §5.1, §5.2).
//
// Two things are worth testing here and they fail in opposite directions. The ACTIVATION predicate
// fails "on" — it splits a screen nobody asked to split — and the failure is the whole desktop
// going half to each eye, which is the loudest possible bug. The RECT math fails "quietly": it
// still shows a picture, just the wrong half, or one pixel out of bounds, and the wearer only
// knows something is off because their eyes hurt. So the activation cases enumerate every way the
// answer must be OFF, and the rect cases assert the two panes' relationship to each other (they
// tile, they never overlap, they never leave the content rect) rather than only their coordinates.
//
// Nothing here needs a runtime, a session, a swapchain or a compositor.

namespace {
    constexpr eContentLayout PACKED_LAYOUTS[] = {CONTENT_SBS, CONTENT_HSBS, CONTENT_TAB, CONTENT_HTAB};
    constexpr eContentLayout ALL_LAYOUTS[]    = {CONTENT_OFF, CONTENT_SBS, CONTENT_HSBS, CONTENT_TAB, CONTENT_HTAB, CONTENT_AUTO};

    // The shape the frame thread hands paneImageRect: a 1920x1080 content rect sitting inside a
    // chrome-margined swapchain, y already flipped to GL's bottom-left origin.
    constexpr SImageRect CONTENT{192, 108, 1920, 1080};

    SPairQuery           engaged(eContentLayout layout) {
        return {.declared = layout, .coversOutput = true, .enabled = true};
    }
}

// ---------------------------------------------------------------------------------------------
// Activation
// ---------------------------------------------------------------------------------------------

TEST(XRStereoPair, EveryPackedLayoutEngagesWhenItOwnsTheOutput) {
    for (const auto LAYOUT : PACKED_LAYOUTS)
        EXPECT_EQ(resolvePairLayout(engaged(LAYOUT)), LAYOUT) << layoutToString(LAYOUT);
}

TEST(XRStereoPair, MonoNeverEngages) {
    EXPECT_EQ(resolvePairLayout(engaged(CONTENT_OFF)), CONTENT_OFF);
}

// CONTENT_AUTO is a delegation to the client's tag, not a layout. If one ever reaches a presenter
// the declaration fold failed to resolve it, and splitting the screen on a layout nobody chose is
// strictly worse than showing the packed frame doubled.
TEST(XRStereoPair, UnresolvedAutoNeverEngages) {
    EXPECT_EQ(resolvePairLayout(engaged(CONTENT_AUTO)), CONTENT_OFF);
}

// THE gate. A declared window that does not own the output cannot drive the pair, because the pair
// splits the whole content rect and would send half the desktop to each eye.
TEST(XRStereoPair, ADeclaredWindowThatDoesNotCoverTheOutputNeverEngages) {
    for (const auto LAYOUT : PACKED_LAYOUTS) {
        auto q         = engaged(LAYOUT);
        q.coversOutput = false;
        EXPECT_EQ(resolvePairLayout(q), CONTENT_OFF) << layoutToString(LAYOUT);
    }
}

// The kill switch has to beat everything, because the whole reason it exists is a runtime that got
// eyeVisibility or imageRect wrong — i.e. a situation where every other input looks perfect.
TEST(XRStereoPair, TheKillSwitchBeatsEveryOtherInput) {
    for (const auto LAYOUT : ALL_LAYOUTS) {
        auto q    = engaged(LAYOUT);
        q.enabled = false;
        EXPECT_EQ(resolvePairLayout(q), CONTENT_OFF) << layoutToString(LAYOUT);
    }
}

TEST(XRStereoPair, DefaultQueryIsOff) {
    EXPECT_EQ(resolvePairLayout(SPairQuery{}), CONTENT_OFF);
}

// ---------------------------------------------------------------------------------------------
// The layer budget
// ---------------------------------------------------------------------------------------------

TEST(XRStereoPair, APairCostsTwoLayersAndMonoCostsOne) {
    EXPECT_EQ(quadsFor(CONTENT_OFF), 1u);
    EXPECT_EQ(quadsFor(CONTENT_AUTO), 1u);
    for (const auto LAYOUT : PACKED_LAYOUTS)
        EXPECT_EQ(quadsFor(LAYOUT), 2u) << layoutToString(LAYOUT);
}

// The half-submitted pair is the failure this guard exists for: one eye showing the desktop and the
// other showing nothing. At the spec-minimum budget of 16, the 16th slot must NOT take a pair.
TEST(XRStereoPair, APairIsNeverHalfSubmittedAtTheBudgetEdge) {
    EXPECT_TRUE(submissionFits(14, 16, CONTENT_SBS));
    EXPECT_FALSE(submissionFits(15, 16, CONTENT_SBS)); // one slot left: a pair must be refused
    EXPECT_TRUE(submissionFits(15, 16, CONTENT_OFF));  // ...but a lone quad still fits
    EXPECT_FALSE(submissionFits(16, 16, CONTENT_OFF));
}

// A stereo monitor that does not fit must not starve the monitors behind it — the caller skips it
// and keeps going, so a cheaper layer can still take the remaining slot.
TEST(XRStereoPair, ARefusedPairStillLeavesRoomForAMonoQuad) {
    EXPECT_FALSE(submissionFits(3, 4, CONTENT_TAB));
    EXPECT_TRUE(submissionFits(3, 4, CONTENT_OFF));
}

// ---------------------------------------------------------------------------------------------
// The image rect
// ---------------------------------------------------------------------------------------------

TEST(XRStereoPair, MonoIsTheWholeContentRect) {
    EXPECT_EQ(paneImageRect(CONTENT, CONTENT_OFF, 0), CONTENT);
    EXPECT_EQ(paneImageRect(CONTENT, CONTENT_OFF, 1), CONTENT);
    EXPECT_EQ(paneImageRect(CONTENT, CONTENT_AUTO, 0), CONTENT);
}

TEST(XRStereoPair, SideBySideSplitsLeftRightAndTilesTheContentRect) {
    for (const auto LAYOUT : {CONTENT_SBS, CONTENT_HSBS}) {
        const auto L = paneImageRect(CONTENT, LAYOUT, 0);
        const auto R = paneImageRect(CONTENT, LAYOUT, 1);

        EXPECT_EQ(L, (SImageRect{192, 108, 960, 1080})) << layoutToString(LAYOUT);
        EXPECT_EQ(R, (SImageRect{1152, 108, 960, 1080})) << layoutToString(LAYOUT);

        // they tile the rect exactly, and neither moved in y
        EXPECT_EQ(L.x + L.w, R.x) << layoutToString(LAYOUT);
        EXPECT_EQ(L.y, CONTENT.y);
        EXPECT_EQ(R.h, CONTENT.h);
    }
}

// Over-under: eye 0 is the TOP half, and in the bottom-left space imageRect uses, the top half is
// the one with the HIGHER y. Get this backwards and the eyes are swapped — the single most likely
// silent defect in this file, which is why it is asserted as a y-ordering and not just a number.
TEST(XRStereoPair, OverUnderGivesTheTopHalfToTheLeftEye) {
    for (const auto LAYOUT : {CONTENT_TAB, CONTENT_HTAB}) {
        const auto TOP    = paneImageRect(CONTENT, LAYOUT, 0);
        const auto BOTTOM = paneImageRect(CONTENT, LAYOUT, 1);

        EXPECT_GT(TOP.y, BOTTOM.y) << layoutToString(LAYOUT);
        EXPECT_EQ(BOTTOM, (SImageRect{192, 108, 1920, 540})) << layoutToString(LAYOUT);
        EXPECT_EQ(TOP, (SImageRect{192, 648, 1920, 540})) << layoutToString(LAYOUT);
        EXPECT_EQ(BOTTOM.y + BOTTOM.h, TOP.y) << layoutToString(LAYOUT);
    }
}

// The generic invariants, on every packed layout and a spread of rects including odd sizes: the two
// panes are the same size, they never overlap, and they never leave the content rect. The last one
// is what stops a runtime-side validation failure (Monado rejects an out-of-bounds imageRect
// outright, taking the whole frame with it).
TEST(XRStereoPair, PanesAreEqualNonOverlappingAndInsideTheContentRect) {
    const std::vector<SImageRect> RECTS{
        {0, 0, 1920, 1080}, {192, 108, 1920, 1080}, {5, 7, 1921, 1081}, {0, 0, 3840, 1080}, {11, 3, 1, 1}, {0, 0, 0, 0},
    };

    for (const auto& CONTENTRECT : RECTS) {
        for (const auto LAYOUT : PACKED_LAYOUTS) {
            const auto A = paneImageRect(CONTENTRECT, LAYOUT, 0);
            const auto B = paneImageRect(CONTENTRECT, LAYOUT, 1);

            EXPECT_EQ(A.w, B.w);
            EXPECT_EQ(A.h, B.h);
            EXPECT_GE(A.w, 0);
            EXPECT_GE(A.h, 0);

            for (const auto& PANE : {A, B}) {
                EXPECT_GE(PANE.x, CONTENTRECT.x);
                EXPECT_GE(PANE.y, CONTENTRECT.y);
                EXPECT_LE(PANE.x + PANE.w, CONTENTRECT.x + CONTENTRECT.w);
                EXPECT_LE(PANE.y + PANE.h, CONTENTRECT.y + CONTENTRECT.h);
            }

            const bool DISJOINT = A.x + A.w <= B.x || B.x + B.w <= A.x || A.y + A.h <= B.y || B.y + B.h <= A.y;
            EXPECT_TRUE(DISJOINT || A.w == 0 || A.h == 0) << layoutToString(LAYOUT);
        }
    }
}

// An odd content size drops the middle row/column rather than overlapping or overrunning, and the
// two panes stay symmetric about the rect's centre.
TEST(XRStereoPair, AnOddContentSizeDropsTheMiddleAndStaysSymmetric) {
    const SImageRect ODD{0, 0, 1921, 1081};

    const auto       L = paneImageRect(ODD, CONTENT_SBS, 0);
    const auto       R = paneImageRect(ODD, CONTENT_SBS, 1);
    EXPECT_EQ(L, (SImageRect{0, 0, 960, 1081}));
    EXPECT_EQ(R, (SImageRect{961, 0, 960, 1081}));
    EXPECT_EQ(L.x - ODD.x, ODD.x + ODD.w - (R.x + R.w)); // symmetric about the centre

    const auto TOP    = paneImageRect(ODD, CONTENT_TAB, 0);
    const auto BOTTOM = paneImageRect(ODD, CONTENT_TAB, 1);
    EXPECT_EQ(BOTTOM, (SImageRect{0, 0, 1921, 540}));
    EXPECT_EQ(TOP, (SImageRect{0, 541, 1921, 540}));
}

// Out-of-range eye indices clamp instead of reading a third pane into existence.
TEST(XRStereoPair, EyeIndicesClamp) {
    EXPECT_EQ(paneImageRect(CONTENT, CONTENT_SBS, -3), paneImageRect(CONTENT, CONTENT_SBS, 0));
    EXPECT_EQ(paneImageRect(CONTENT, CONTENT_SBS, 7), paneImageRect(CONTENT, CONTENT_SBS, 1));
}

// ---------------------------------------------------------------------------------------------
// The aspect the pair must submit (§5.2's `k`, via the shared header)
// ---------------------------------------------------------------------------------------------

// The invariant §5.2 is built on, stated from the XR side because X1 is its only caller: every
// packing of the same picture asks the quad for the same shape. A 3840x1080 full-SBS frame and a
// 1920x1080 half-SBS one are the same picture and must present identically — that is the entire
// reason half-vs-full has to be DECLARED and cannot be measured.
TEST(XRStereoPair, EveryPackingOfOnePictureAsksForTheSameQuadShape) {
    const float SBS  = presentedAspect({3840, 1080}, CONTENT_SBS);
    const float HSBS = presentedAspect({1920, 1080}, CONTENT_HSBS);
    const float TAB  = presentedAspect({1920, 2160}, CONTENT_TAB);
    const float HTAB = presentedAspect({1920, 1080}, CONTENT_HTAB);

    EXPECT_FLOAT_EQ(SBS, 1080.f / 1920.f);
    EXPECT_FLOAT_EQ(HSBS, SBS);
    EXPECT_FLOAT_EQ(TAB, SBS);
    EXPECT_FLOAT_EQ(HTAB, SBS);
}

// What the frame thread actually feeds the anchor solve. The half layouts are indistinguishable
// from mono BY SIZE — which is why a `hsbs` declaration leaves an XR monitor's geometry alone,
// while `sbs` on the same monitor genuinely makes the panel twice as tall (each eye's picture is
// half as wide, and the quad's width is what the user configured).
TEST(XRStereoPair, PresentedPaneSizeIsWhatTheQuadDerivesItsHeightFrom) {
    const Vector2D MODE{1920, 1080};

    EXPECT_EQ(presentedPaneSize(MODE, CONTENT_OFF), MODE);
    EXPECT_EQ(presentedPaneSize(MODE, CONTENT_HSBS), MODE);
    EXPECT_EQ(presentedPaneSize(MODE, CONTENT_HTAB), MODE);
    EXPECT_EQ(presentedPaneSize(MODE, CONTENT_SBS), (Vector2D{960, 1080}));
    EXPECT_EQ(presentedPaneSize(MODE, CONTENT_TAB), (Vector2D{1920, 540}));

    // ...and a genuinely double-wide mode un-packs to the mode's own shape, which is the case the
    // XREAL's 3840x1080 stereo mode produces.
    EXPECT_EQ(presentedPaneSize({3840, 1080}, CONTENT_SBS), (Vector2D{1920, 1080}));
}
