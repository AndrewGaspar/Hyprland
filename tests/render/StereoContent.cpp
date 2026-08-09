#include <render/StereoContent.hpp>

#include <gtest/gtest.h>

#include <array>

// WP S1 — unit tests for per-window stereo CONTENT (research/24 §4.2, §5.2, §5.3).
//
// The renderer calls these exact functions: the window rule parses with parseDeclaration, the
// window resolves the tag with layoutFromTag, and calculateUVForSurface crops with cropForEye. So
// what is asserted here is what the compositor does, not a model of it.
//
// The load-bearing invariants, in order of how badly they break things:
//   1. a mono layout is the identity — a window without a stereo rule must sample exactly as before.
//   2. eye 0 is the LEFT eye and takes the left/top half, in every layout and every UV range.
//   3. the crop COMPOSES with an existing UV range (a viewporter source box) instead of replacing
//      it, or a stereo video in a player that crops its source would sample the wrong pixels.
//   4. the tag grammar is exactly `stereo:<layout>` — this is the string the Dead Space mod emits,
//      so it is a compatibility contract, not an implementation detail.

using namespace Render::Stereo;

namespace {
    constexpr std::array<eContentLayout, 4> PACKED_LAYOUTS = {CONTENT_SBS, CONTENT_HSBS, CONTENT_TAB, CONTENT_HTAB};

    SUVRange                                fullRange() {
        return SUVRange{};
    }
}

// --- layout vocabulary ---

TEST(StereoContent, layoutFromStringKnowsTheVocabulary) {
    EXPECT_EQ(layoutFromString("sbs"), CONTENT_SBS);
    EXPECT_EQ(layoutFromString("hsbs"), CONTENT_HSBS);
    EXPECT_EQ(layoutFromString("tab"), CONTENT_TAB);
    EXPECT_EQ(layoutFromString("htab"), CONTENT_HTAB);
    EXPECT_EQ(layoutFromString("auto"), CONTENT_AUTO);
}

TEST(StereoContent, layoutFromStringSpellsOffFourWays) {
    // `mono` is the tag convention's word and `off` is the rule's; both must mean the same thing or
    // a client that says "not stereo" cannot suppress a rule.
    for (const auto& token : {"off", "mono", "none", "0"})
        EXPECT_EQ(layoutFromString(token), CONTENT_OFF) << token;
}

TEST(StereoContent, layoutFromStringRejectsGarbage) {
    for (const auto& token : {"", "SBS", "sbs3d", "over_under", "lr", "3d"})
        EXPECT_FALSE(layoutFromString(token).has_value()) << token;
}

// --- the rule grammar: `windowrule = stereo <layout> [always|fullscreen]` ---

TEST(StereoContent, parseLayoutOnlyDefaultsToTheFullscreenGate) {
    const auto DECL = parseDeclaration("sbs");
    ASSERT_TRUE(DECL.has_value());
    EXPECT_EQ(DECL->layout, CONTENT_SBS);
    // §4.3's negative heuristic is the DEFAULT: a rule that did not say otherwise only engages on a
    // window that owns the screen.
    EXPECT_EQ(DECL->gate, GATE_FULLSCREEN);
}

TEST(StereoContent, parseAlwaysIsTheManualVerb) {
    const auto DECL = parseDeclaration("hsbs always");
    ASSERT_TRUE(DECL.has_value());
    EXPECT_EQ(DECL->layout, CONTENT_HSBS);
    EXPECT_EQ(DECL->gate, GATE_ALWAYS);
}

TEST(StereoContent, parseFullscreenScopeIsSpellableExplicitly) {
    const auto DECL = parseDeclaration("tab fullscreen");
    ASSERT_TRUE(DECL.has_value());
    EXPECT_EQ(DECL->gate, GATE_FULLSCREEN);
}

TEST(StereoContent, parseToleratesTheWhitespaceUsersType) {
    const auto DECL = parseDeclaration("  sbs   always  ");
    ASSERT_TRUE(DECL.has_value());
    EXPECT_EQ(DECL->layout, CONTENT_SBS);
    EXPECT_EQ(DECL->gate, GATE_ALWAYS);
}

TEST(StereoContent, parseRejectsWhatItCannotHonour) {
    // an unknown layout must be an error rather than a silent mono, or a typo means "stereo quietly
    // did nothing" — the failure mode that costs an evening of debugging.
    EXPECT_FALSE(parseDeclaration("").has_value());
    EXPECT_FALSE(parseDeclaration("sbs3d").has_value());
    EXPECT_FALSE(parseDeclaration("sbs sometimes").has_value());
    EXPECT_FALSE(parseDeclaration("sbs always extra").has_value());
}

TEST(StereoContent, declarationSurvivesThePackedInteger) {
    // the rule engine stores one int64 and the renderer reads it per surface per frame
    for (const auto& layout : {CONTENT_OFF, CONTENT_SBS, CONTENT_HSBS, CONTENT_TAB, CONTENT_HTAB, CONTENT_AUTO}) {
        for (const auto& gate : {GATE_FULLSCREEN, GATE_ALWAYS}) {
            const SDeclaration DECL{.layout = layout, .gate = gate};
            EXPECT_EQ(unpackDeclaration(packDeclaration(DECL)), DECL);
        }
    }
}

TEST(StereoContent, unpackingRubbishIsMonoAndGated) {
    // 0 is the prop's default, i.e. every window in a session with no stereo rules at all
    EXPECT_EQ(unpackDeclaration(0), (SDeclaration{.layout = CONTENT_OFF, .gate = GATE_FULLSCREEN}));
    EXPECT_EQ(unpackDeclaration(0x7FFF'FFFF), (SDeclaration{.layout = CONTENT_OFF, .gate = GATE_FULLSCREEN}));
}

// --- the tag grammar (§4.2) — the client-facing contract ---

TEST(StereoContent, tagGrammarIsExactlyStereoColonLayout) {
    EXPECT_EQ(layoutFromTag("stereo:sbs"), CONTENT_SBS);
    EXPECT_EQ(layoutFromTag("stereo:hsbs"), CONTENT_HSBS);
    EXPECT_EQ(layoutFromTag("stereo:tab"), CONTENT_TAB);
    EXPECT_EQ(layoutFromTag("stereo:htab"), CONTENT_HTAB);
}

TEST(StereoContent, tagMonoIsADeclarationOfNotStereo) {
    // NOT nullopt: a client saying "I am mono" must be distinguishable from a client saying nothing,
    // because the first suppresses a `stereo auto` rule and the second just does not resolve it.
    EXPECT_EQ(layoutFromTag("stereo:mono"), CONTENT_OFF);
}

TEST(StereoContent, tagRejectsEverythingElse) {
    for (const auto& tag :
         {"", "sbs", "STEREO:SBS", "stereo:", "stereo:lr", "stereo: sbs", "stereo:sbs ", "video:stereo:sbs", "stereo:auto"}) // a client cannot delegate the layout back to itself
        EXPECT_FALSE(layoutFromTag(tag).has_value()) << tag;
}

// --- geometry ---

TEST(StereoContent, monoHasOneViewAndPackedLayoutsHaveTwo) {
    EXPECT_EQ(viewCount(CONTENT_OFF), 1);
    EXPECT_EQ(viewCount(CONTENT_AUTO), 1); // unresolved is not packed
    for (const auto& layout : PACKED_LAYOUTS)
        EXPECT_EQ(viewCount(layout), 2) << layoutToString(layout);
}

TEST(StereoContent, sideBySideLayoutsSplitTheUAxisAndOverUnderTheV) {
    EXPECT_TRUE(splitsHorizontally(CONTENT_SBS));
    EXPECT_TRUE(splitsHorizontally(CONTENT_HSBS));
    EXPECT_FALSE(splitsVertically(CONTENT_SBS));

    EXPECT_TRUE(splitsVertically(CONTENT_TAB));
    EXPECT_TRUE(splitsVertically(CONTENT_HTAB));
    EXPECT_FALSE(splitsHorizontally(CONTENT_TAB));
}

TEST(StereoContent, aspectFactorIsTwoForTheHalfLayouts) {
    // §5.2's k. Unused on the flat presenter (the destination is the window's own box, so the crop
    // un-squeezes by construction) and load-bearing for the XR quad pair, where the presented size
    // is DERIVED from content pixels.
    EXPECT_FLOAT_EQ(aspectFactor(CONTENT_SBS), 1.F);
    EXPECT_FLOAT_EQ(aspectFactor(CONTENT_TAB), 1.F);
    EXPECT_FLOAT_EQ(aspectFactor(CONTENT_HSBS), 2.F);
    EXPECT_FLOAT_EQ(aspectFactor(CONTENT_HTAB), 2.F);
}

// --- the crop, which is the producer ---

TEST(StereoContent, monoIsTheIdentityForEveryEye) {
    // invariant 1: a window with no stereo rule samples exactly what it sampled before this WP
    const SUVRange VIEWPORTED{{0.25, 0.5}, {0.75, 1.0}};
    for (int eye = 0; eye < 2; ++eye) {
        EXPECT_EQ(cropForEye(fullRange(), CONTENT_OFF, eye), fullRange());
        EXPECT_EQ(cropForEye(VIEWPORTED, CONTENT_OFF, eye), VIEWPORTED);
        EXPECT_EQ(cropForEye(VIEWPORTED, CONTENT_AUTO, eye), VIEWPORTED);
    }
}

TEST(StereoContent, sbsGivesTheLeftEyeTheLeftHalf) {
    const auto LEFT = cropForEye(fullRange(), CONTENT_SBS, 0);
    EXPECT_EQ(LEFT.tl, Vector2D(0, 0));
    EXPECT_EQ(LEFT.br, Vector2D(0.5, 1.0));

    const auto RIGHT = cropForEye(fullRange(), CONTENT_SBS, 1);
    EXPECT_EQ(RIGHT.tl, Vector2D(0.5, 0.0));
    EXPECT_EQ(RIGHT.br, Vector2D(1, 1));
}

TEST(StereoContent, hsbsCropsTheSameUVsAsSbs) {
    // the two differ ONLY in how many source samples each eye gets — the same halves of the same
    // buffer, which is why the flat presenter needs no k and why the distinction must be DECLARED.
    for (int eye = 0; eye < 2; ++eye)
        EXPECT_EQ(cropForEye(fullRange(), CONTENT_HSBS, eye), cropForEye(fullRange(), CONTENT_SBS, eye));
}

TEST(StereoContent, tabGivesTheLeftEyeTheTopHalf) {
    const auto LEFT = cropForEye(fullRange(), CONTENT_TAB, 0);
    EXPECT_EQ(LEFT.tl, Vector2D(0, 0));
    EXPECT_EQ(LEFT.br, Vector2D(1.0, 0.5));

    const auto RIGHT = cropForEye(fullRange(), CONTENT_HTAB, 1);
    EXPECT_EQ(RIGHT.tl, Vector2D(0.0, 0.5));
    EXPECT_EQ(RIGHT.br, Vector2D(1, 1));
}

TEST(StereoContent, theCropComposesWithAViewporterSourceBox) {
    // invariant 3. A player that already cropped its buffer (wp_viewporter source) must have its
    // range HALVED, not overwritten — overwriting would sample black bars or another client's
    // padding, and would look like "stereo shifts the picture".
    const SUVRange SOURCE{{0.2, 0.1}, {0.8, 0.9}};

    const auto     LEFT = cropForEye(SOURCE, CONTENT_SBS, 0);
    EXPECT_EQ(LEFT.tl, Vector2D(0.2, 0.1));
    EXPECT_DOUBLE_EQ(LEFT.br.x, 0.5);
    EXPECT_DOUBLE_EQ(LEFT.br.y, 0.9);

    const auto RIGHT = cropForEye(SOURCE, CONTENT_SBS, 1);
    EXPECT_DOUBLE_EQ(RIGHT.tl.x, 0.5);
    EXPECT_DOUBLE_EQ(RIGHT.tl.y, 0.1);
    EXPECT_EQ(RIGHT.br, Vector2D(0.8, 0.9));
}

TEST(StereoContent, thePairOfEyesTilesTheWholeRangeExactlyOnce) {
    // no overlap and no gap, in either axis and at any base range: the two eyes together are the
    // whole packed frame, so a seam here is a visible line down the middle of one eye.
    const SUVRange SOURCE{{0.2, 0.1}, {0.8, 0.9}};
    for (const auto& layout : PACKED_LAYOUTS) {
        const auto L = cropForEye(SOURCE, layout, 0);
        const auto R = cropForEye(SOURCE, layout, 1);

        EXPECT_EQ(L.tl, SOURCE.tl) << layoutToString(layout);
        EXPECT_EQ(R.br, SOURCE.br) << layoutToString(layout);
        if (splitsHorizontally(layout)) {
            EXPECT_DOUBLE_EQ(L.br.x, R.tl.x) << layoutToString(layout);
            EXPECT_EQ(L.br.y, SOURCE.br.y) << layoutToString(layout);
        } else {
            EXPECT_DOUBLE_EQ(L.br.y, R.tl.y) << layoutToString(layout);
            EXPECT_EQ(L.br.x, SOURCE.br.x) << layoutToString(layout);
        }
    }
}

TEST(StereoContent, anOutOfRangePaneIndexClampsInsteadOfSamplingNowhere) {
    // a stereo output could one day pack more than two panes; a third pane must show an eye, not
    // sample past the end of the buffer.
    EXPECT_EQ(cropForEye(fullRange(), CONTENT_SBS, 5), cropForEye(fullRange(), CONTENT_SBS, 1));
    EXPECT_EQ(cropForEye(fullRange(), CONTENT_SBS, -3), cropForEye(fullRange(), CONTENT_SBS, 0));
}
