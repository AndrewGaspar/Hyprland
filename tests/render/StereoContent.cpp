#include <render/StereoContent.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

// WP S1/S2 — unit tests for per-window stereo CONTENT (research/24 §4.2, §5.2, §5.3, §5.6).
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

TEST(StereoContent, everyLayoutTokenParsesUnderEveryScope) {
    // the accept half of the grammar, exhaustively — one entry per line a user can actually type.
    // A token that parses in the .conf handler but not in the Lua mirror (or the reverse) is F8's
    // failure mode, and both front-ends bottom out right here.
    static const std::pair<const char*, eContentLayout> TOKENS[] = {
        {"off", CONTENT_OFF},   {"mono", CONTENT_OFF}, {"none", CONTENT_OFF},  {"0", CONTENT_OFF},     {"sbs", CONTENT_SBS},
        {"hsbs", CONTENT_HSBS}, {"tab", CONTENT_TAB},  {"htab", CONTENT_HTAB}, {"auto", CONTENT_AUTO},
    };
    static const std::pair<const char*, eContentGate> SCOPES[] = {{"", GATE_FULLSCREEN}, {" fullscreen", GATE_FULLSCREEN}, {" always", GATE_ALWAYS}};

    for (const auto& [TOKEN, LAYOUT] : TOKENS) {
        for (const auto& [SUFFIX, GATE] : SCOPES) {
            const std::string RAW  = std::string{TOKEN} + SUFFIX;
            const auto        DECL = parseDeclaration(RAW);
            ASSERT_TRUE(DECL.has_value()) << RAW << ": " << DECL.error();
            EXPECT_EQ(DECL->layout, LAYOUT) << RAW;
            EXPECT_EQ(DECL->gate, GATE) << RAW;
        }
    }
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

// --- the precedence table: which tier's instruction wins (§4.2, §4.3, §4.5) ---
//
// This is the whole of CWindow::stereoLayout() except the two lookups and the fullscreen query, so
// every row below is a question a user can ask out loud ("why is my window cropped?" / "why is it
// NOT cropped?") answered without a compositor.

namespace {
    // the shorthands the rows read in
    SDeclaration ruleOf(eContentLayout layout, eContentGate gate = GATE_FULLSCREEN) {
        return {.layout = layout, .gate = gate};
    }

    constexpr std::optional<eContentLayout> SILENT = std::nullopt; // the client set no tag at all
}

TEST(StereoContentResolution, noRuleIsOffNoMatterWhatTheClientClaims) {
    // the state of every window in every session that never configured this. A client cannot opt
    // ITSELF into a crop — the tag is only ever read because a rule asked for it.
    for (const auto& tagged : {SILENT, std::optional{CONTENT_SBS}, std::optional{CONTENT_OFF}})
        EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_OFF), tagged), SResolution{});
}

TEST(StereoContentResolution, offIsTheOffSwitchAndOutranksTheTag) {
    // `windowrule = stereo off` on a client that declares `stereo:sbs`: the user wins, and there is
    // no gate left to satisfy. This is the escape hatch for a client whose tag is simply wrong.
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_OFF, GATE_ALWAYS), std::optional{CONTENT_SBS}), SResolution{});
}

TEST(StereoContentResolution, anExplicitLayoutBeatsTheTag) {
    // §4.5: the last human instruction wins, in BOTH directions — a rule can promote a silent
    // client and can override a client that declared something else.
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_TAB, GATE_ALWAYS), SILENT), (SResolution{.layout = CONTENT_TAB}));
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_TAB, GATE_ALWAYS), std::optional{CONTENT_SBS}), (SResolution{.layout = CONTENT_TAB}));
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_TAB, GATE_ALWAYS), std::optional{CONTENT_OFF}), (SResolution{.layout = CONTENT_TAB}));
}

TEST(StereoContentResolution, autoIsTheOnlyRuleThatReadsTheTag) {
    for (const auto& layout : PACKED_LAYOUTS)
        EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_AUTO), std::optional{layout}).layout, layout) << layoutToString(layout);
}

TEST(StereoContentResolution, autoOnASilentClientResolvesToNothing) {
    // NOT "assume sbs". A guess here crops half of every window the rule matched — the failure mode
    // §4.3 exists to prevent — and `auto` is precisely the tier that promised not to guess.
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_AUTO), SILENT), SResolution{});
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_AUTO, GATE_ALWAYS), SILENT), SResolution{});
}

TEST(StereoContentResolution, aClientCanDeclareItselfMonoAndSuppressAuto) {
    // the difference between `stereo:mono` and no tag at all, which is the ONLY reason
    // layoutFromTag returns CONTENT_OFF rather than nullopt for it. Same answer here, different
    // reason — and a player that switches a stereo file for a flat one relies on it.
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_AUTO), std::optional{CONTENT_OFF}), SResolution{});
}

TEST(StereoContentResolution, aGuessedLayoutWaitsForTheWindowToOwnTheScreen) {
    // §4.3's negative heuristic: `windowrule = stereo sbs, class:^(mpv)$` is a guess about what mpv
    // happens to be playing, so it stays gated. The caller is the one that answers the gate.
    EXPECT_EQ(resolveDeclaration(ruleOf(CONTENT_SBS), SILENT), (SResolution{.layout = CONTENT_SBS, .gated = true}));
}

TEST(StereoContentResolution, alwaysAndAClientDeclarationAreNotGuessesAndSkipTheGate) {
    // the two ways to be exempt, and they are exempt independently: the hand-written verb...
    EXPECT_FALSE(resolveDeclaration(ruleOf(CONTENT_SBS, GATE_ALWAYS), SILENT).gated);
    // ...and a client that named its own packing, under a plain `auto` rule with no `always` on it.
    EXPECT_FALSE(resolveDeclaration(ruleOf(CONTENT_AUTO), std::optional{CONTENT_SBS}).gated);
    // a declaring client also lifts the gate off an explicit rule that disagrees with it: the layout
    // is the user's, but the window is demonstrably stereo content, so windowed is fine.
    EXPECT_FALSE(resolveDeclaration(ruleOf(CONTENT_HSBS), std::optional{CONTENT_SBS}).gated);
    // ...and `stereo:mono` is a declaration of NOT-stereo, so it lifts nothing.
    EXPECT_TRUE(resolveDeclaration(ruleOf(CONTENT_HSBS), std::optional{CONTENT_OFF}).gated);
}

TEST(StereoContentResolution, aResolvedLayoutIsNeverAutoAndNeverUnpacked) {
    // the renderer crops with whatever comes out of here, and cropForEye treats CONTENT_AUTO as
    // mono — so an `auto` leaking through would be a silent no-op, not a visible bug.
    for (const auto& rule : {CONTENT_OFF, CONTENT_SBS, CONTENT_HSBS, CONTENT_TAB, CONTENT_HTAB, CONTENT_AUTO}) {
        for (const auto& gate : {GATE_FULLSCREEN, GATE_ALWAYS}) {
            for (const auto& tagged : {SILENT, std::optional{CONTENT_OFF}, std::optional{CONTENT_SBS}, std::optional{CONTENT_HTAB}}) {
                const auto RES = resolveDeclaration(ruleOf(rule, gate), tagged);
                EXPECT_NE(RES.layout, CONTENT_AUTO) << layoutToString(rule);
                // and a gate is only ever attached to something worth gating
                EXPECT_TRUE(RES.layout != CONTENT_OFF || !RES.gated) << layoutToString(rule);
            }
        }
    }
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

// --- the aspect, derived from the destination box (§5.2) ---

TEST(StereoContent, thePaneIsHalfTheBufferOnTheAxisTheLayoutNames) {
    const Vector2D BUFFER{1920, 1080};
    EXPECT_EQ(contentPaneSize(BUFFER, CONTENT_OFF), BUFFER);
    EXPECT_EQ(contentPaneSize(BUFFER, CONTENT_AUTO), BUFFER); // unresolved is not packed
    EXPECT_EQ(contentPaneSize(BUFFER, CONTENT_SBS), Vector2D(960, 1080));
    EXPECT_EQ(contentPaneSize(BUFFER, CONTENT_HSBS), Vector2D(960, 1080));
    EXPECT_EQ(contentPaneSize(BUFFER, CONTENT_TAB), Vector2D(1920, 540));
    EXPECT_EQ(contentPaneSize(BUFFER, CONTENT_HTAB), Vector2D(1920, 540));
}

TEST(StereoContent, everyPackingOfTheSamePicturePresentsTheSameAspect) {
    // §5.2, and the single most confusable number in this file. Each of these buffers carries the
    // same 1920x1080 picture per eye, packed four different ways; getting `k` backwards (or applying
    // it to the wrong axis) gives 4x, 2x or 0.5x here and a subtly-wrong, hard-to-name image on the
    // headset. The XR quad pair (WP X1) derives its height from this.
    constexpr float EYE = 1080.F / 1920.F;

    EXPECT_FLOAT_EQ(presentedAspect({3840, 1080}, CONTENT_SBS), EYE);  // full: the frame is 2x wide
    EXPECT_FLOAT_EQ(presentedAspect({1920, 1080}, CONTENT_HSBS), EYE); // half: each eye squeezed by 2
    EXPECT_FLOAT_EQ(presentedAspect({1920, 2160}, CONTENT_TAB), EYE);  // full: the frame is 2x tall
    EXPECT_FLOAT_EQ(presentedAspect({1920, 1080}, CONTENT_HTAB), EYE); // half: each eye squeezed by 2
    EXPECT_FLOAT_EQ(presentedAspect({1920, 1080}, CONTENT_OFF), EYE);  // and mono is just the frame
}

TEST(StereoContent, theHalfLayoutsAreIndistinguishableFromMonoBySize) {
    // the reason a layout must be DECLARED and can never be measured: an hsbs frame, an htab frame
    // and a mono frame of the same picture are the same number of pixels in the same arrangement.
    // §4.4's pixel detector exists because of this, and §5.2 is why it can never be complete.
    const Vector2D FRAME{1920, 1080};
    EXPECT_FLOAT_EQ(presentedAspect(FRAME, CONTENT_HSBS), presentedAspect(FRAME, CONTENT_OFF));
    EXPECT_FLOAT_EQ(presentedAspect(FRAME, CONTENT_HTAB), presentedAspect(FRAME, CONTENT_OFF));
    // whereas the FULL layouts are self-describing from the frame alone (2:1 and 1:2 of the eye)
    EXPECT_FLOAT_EQ(presentedAspect({3840, 1080}, CONTENT_SBS), presentedAspect(FRAME, CONTENT_OFF));
    EXPECT_FLOAT_EQ(presentedAspect({1920, 2160}, CONTENT_TAB), presentedAspect(FRAME, CONTENT_OFF));
}

TEST(StereoContent, aDegenerateBufferDoesNotDivideByZero) {
    // a client can commit a 1x1 (or, briefly, a 0-sized) buffer; the aspect must be a number.
    EXPECT_TRUE(std::isfinite(presentedAspect({0, 0}, CONTENT_SBS)));
    EXPECT_TRUE(std::isfinite(presentedAspect({1, 1}, CONTENT_HTAB)));
}

// --- pane <-> content UV, both directions (§5.6) ---

TEST(StereoContent, paneUVMapsIntoTheEyesHalfAndBackAgain) {
    // §5.6 spelled out: u_content = u_pane / 2 for the left eye, 0.5 + u_pane / 2 for the right.
    EXPECT_EQ(paneUVToContentUV({0.0, 0.5}, CONTENT_SBS, 0), Vector2D(0.0, 0.5));
    EXPECT_EQ(paneUVToContentUV({1.0, 0.5}, CONTENT_SBS, 0), Vector2D(0.5, 0.5));
    EXPECT_EQ(paneUVToContentUV({0.0, 0.5}, CONTENT_SBS, 1), Vector2D(0.5, 0.5));
    EXPECT_EQ(paneUVToContentUV({1.0, 0.5}, CONTENT_SBS, 1), Vector2D(1.0, 0.5));

    // over-under moves the SAME mapping to the other axis, and leaves u alone
    EXPECT_EQ(paneUVToContentUV({0.25, 0.0}, CONTENT_TAB, 1), Vector2D(0.25, 0.5));
    EXPECT_EQ(paneUVToContentUV({0.25, 1.0}, CONTENT_TAB, 1), Vector2D(0.25, 1.0));
}

TEST(StereoContent, theTwoDirectionsAreExactInverses) {
    // get this backwards and the cursor is off by half a screen — so assert the round trip rather
    // than a hand-computed table, over a non-trivial base range (a viewporter'd stereo player).
    const SUVRange SOURCE{{0.2, 0.1}, {0.8, 0.9}};
    for (const auto& layout : {CONTENT_OFF, CONTENT_SBS, CONTENT_HSBS, CONTENT_TAB, CONTENT_HTAB}) {
        for (int eye = 0; eye < 2; ++eye) {
            for (const auto& uv : {Vector2D{0, 0}, Vector2D{1, 1}, Vector2D{0.25, 0.75}, Vector2D{0.5, 0.5}}) {
                const auto ROUND = contentUVToPaneUV(paneUVToContentUV(uv, layout, eye, SOURCE), layout, eye, SOURCE);
                EXPECT_NEAR(ROUND.x, uv.x, 1e-9) << layoutToString(layout) << " eye " << eye;
                EXPECT_NEAR(ROUND.y, uv.y, 1e-9) << layoutToString(layout) << " eye " << eye;
            }
        }
    }
}

TEST(StereoContent, monoUnmapsToItself) {
    // the identity is what makes this safe to call unconditionally — and it is also the mapping the
    // DEPTH producer needs (§5.6), where both panes are the same desktop.
    for (int eye = 0; eye < 2; ++eye) {
        EXPECT_EQ(paneUVToContentUV({0.3, 0.7}, CONTENT_OFF, eye), Vector2D(0.3, 0.7));
        EXPECT_EQ(contentUVToPaneUV({0.3, 0.7}, CONTENT_OFF, eye), Vector2D(0.3, 0.7));
    }
}

TEST(StereoContent, thePaneMappingAgreesWithTheCropTheRendererUses) {
    // the two must never drift: the crop decides which texels an eye shows, and the un-map decides
    // where a hit in that eye landed. Expressed in terms of each other on purpose, asserted anyway.
    for (const auto& layout : PACKED_LAYOUTS) {
        for (int eye = 0; eye < 2; ++eye) {
            const auto CROP = cropForEye(fullRange(), layout, eye);
            EXPECT_EQ(paneUVToContentUV({0, 0}, layout, eye), CROP.tl) << layoutToString(layout);
            EXPECT_EQ(paneUVToContentUV({1, 1}, layout, eye), CROP.br) << layoutToString(layout);
        }
    }
}
