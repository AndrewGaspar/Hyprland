// doc 05 §3.3: per-HMD xr_defaults, named profiles, `when`, name minting and the optional-name
// create grammar. Everything here is the pure half (src/openxr/XRDefaults.cpp + the create parser);
// the manager only builds the context and applies what these functions decide.

#include <openxr/XRDefaults.hpp>
#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

namespace {
    SXRSystemContext questCtx() {
        SXRSystemContext c;
        c.bound        = true;
        c.systemName   = "Meta Quest 3 on WiVRn";
        c.handTracking = true;
        c.eyeGaze      = false;
        c.passthrough  = true;
        return c;
    }

    SXRSystemContext xrealCtx() {
        SXRSystemContext c;
        c.bound      = true;
        c.systemName = "Monado: XREAL Air 2 Ultra";
        c.leasedDisplays.push_back({.description = "Nreal Air 2 Ultra 0x88888800", .shortDescription = "Nreal Air 2 Ultra"});
        return c;
    }

    SXRWhen whenHmd(const std::string& v) {
        SXRWhen w;
        EXPECT_TRUE(setXRWhenHmd(w, v).has_value());
        return w;
    }

    SXRDefaultsBlock block(std::optional<Vector2D> res = std::nullopt, std::optional<float> scale = std::nullopt, std::optional<float> size = std::nullopt,
                           std::optional<float> distance = std::nullopt) {
        SXRDefaultsBlock b;
        b.resolution = res;
        b.scale      = scale;
        b.size       = size;
        b.distance   = distance;
        return b;
    }
}

// ---- `when` ----

TEST(XRWhen, emptyMatchesEverythingIncludingNoSession) {
    SXRWhen w;
    EXPECT_TRUE(w.empty());
    EXPECT_TRUE(matchXRWhen(w, SXRSystemContext{}));
    EXPECT_TRUE(matchXRWhen(w, questCtx()));
}

TEST(XRWhen, nonEmptyNeverMatchesBeforeASystemIsBound) {
    auto             w = whenHmd(".*");
    SXRSystemContext unbound;
    EXPECT_FALSE(matchXRWhen(w, unbound));

    SXRWhen cap;
    ASSERT_TRUE(setXRWhenCapability(cap, "hand_tracking", false).has_value());
    EXPECT_FALSE(matchXRWhen(cap, unbound)) << "a capability predicate needs a bound system too";
}

TEST(XRWhen, regexIsASearchInTheSystemName) {
    EXPECT_TRUE(matchXRWhen(whenHmd("Quest 3"), questCtx()));
    EXPECT_TRUE(matchXRWhen(whenHmd("^Meta"), questCtx()));
    EXPECT_FALSE(matchXRWhen(whenHmd("^Quest"), questCtx())) << "anchors still anchor";
    EXPECT_FALSE(matchXRWhen(whenHmd("Quest 3"), xrealCtx()));
    // Monado's own "Monado: " prefix is just part of the name.
    EXPECT_TRUE(matchXRWhen(whenHmd("XREAL"), xrealCtx()));
}

TEST(XRWhen, descMatchesALeasedConnectorByPrefixNotTheSystemName) {
    EXPECT_TRUE(matchXRWhen(whenHmd("desc:Nreal Air 2 Ultra 0x88888800"), xrealCtx()));
    EXPECT_TRUE(matchXRWhen(whenHmd("desc:Nreal Air"), xrealCtx())) << "prefix, like a desc: monitor rule";
    EXPECT_TRUE(matchXRWhen(whenHmd("desc: Nreal Air 2 Ultra"), xrealCtx())) << "the prefix is trimmed";
    EXPECT_FALSE(matchXRWhen(whenHmd("desc:Air 2 Ultra"), xrealCtx())) << "not a substring search";
    // The system name says XREAL, but nothing is leased on a Quest: desc: never consults the name.
    EXPECT_FALSE(matchXRWhen(whenHmd("desc:Meta Quest"), questCtx()));
}

TEST(XRWhen, descAlsoMatchesTheShortDescription) {
    auto ctx                               = xrealCtx();
    ctx.leasedDisplays[0].description      = "something else entirely";
    ctx.leasedDisplays[0].shortDescription = "Nreal Air 2 Ultra";
    EXPECT_TRUE(matchXRWhen(whenHmd("desc:Nreal Air 2"), ctx));
}

TEST(XRWhen, keysAreANDed) {
    auto w = whenHmd("Quest");
    ASSERT_TRUE(setXRWhenCapability(w, "hand_tracking", true).has_value());
    EXPECT_TRUE(matchXRWhen(w, questCtx()));

    ASSERT_TRUE(setXRWhenCapability(w, "eye_gaze", true).has_value());
    EXPECT_FALSE(matchXRWhen(w, questCtx())) << "Quest 3 has no eye gaze";

    SXRWhen pt;
    ASSERT_TRUE(setXRWhenCapability(pt, "passthrough", true).has_value());
    EXPECT_TRUE(matchXRWhen(pt, questCtx()));
    EXPECT_FALSE(matchXRWhen(pt, xrealCtx()));
}

TEST(XRWhen, malformedValuesAreErrors) {
    SXRWhen w;
    EXPECT_FALSE(setXRWhenHmd(w, "").has_value());
    EXPECT_FALSE(setXRWhenHmd(w, "desc:").has_value());
    EXPECT_FALSE(setXRWhenHmd(w, "desc:   ").has_value());
    EXPECT_FALSE(setXRWhenHmd(w, "Quest (3").has_value()) << "invalid regex";
    auto bad = setXRWhenCapability(w, "eye_tracking", true);
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().find("eye_tracking"), std::string::npos);
    EXPECT_NE(bad.error().find("hand_tracking"), std::string::npos) << "the error names the valid keys";
    EXPECT_TRUE(w.empty()) << "a rejected value must not half-set the predicate";
}

TEST(XRWhen, describeIsCanonicalAndDistinguishesPredicates) {
    auto a = whenHmd("Quest 3");
    auto b = whenHmd("Quest 3");
    EXPECT_EQ(a.describe(), b.describe());
    ASSERT_TRUE(setXRWhenCapability(b, "passthrough", true).has_value());
    EXPECT_NE(a.describe(), b.describe());
    EXPECT_EQ(SXRWhen{}.describe(), "");
}

// ---- the resolver ----

TEST(XRDefaultsResolve, noBlocksIsTheGlobals) {
    SXRDefaultsGlobals g;
    g.scale = 1.25F;
    g.size  = 1.6F;
    auto r  = resolveXRDefaults({}, "", questCtx(), g);
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->resolution.value, (Vector2D{1920, 1080}));
    EXPECT_EQ(r->resolution.source, XR_DEFSRC_GLOBAL);
    EXPECT_FLOAT_EQ(r->refreshRate.value, 60.F);
    EXPECT_FLOAT_EQ(r->scale.value, 1.25F);
    EXPECT_FLOAT_EQ(r->size.value, 1.6F);
    EXPECT_FLOAT_EQ(r->distance.value, 1.5F);
    EXPECT_EQ(r->size.block, -1);
}

TEST(XRDefaultsResolve, baseBlocksMergeFieldWiseLastWinsInConfigOrder) {
    std::vector<SXRDefaultsBlock> blocks = {
        block(Vector2D{2560, 1440}, 1.5F, 2.0F),
        block(std::nullopt, 1.6F),                             // only scale
        block(std::nullopt, std::nullopt, std::nullopt, 1.2F), // only distance
    };
    auto r = resolveXRDefaults(blocks, "", questCtx(), {});
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->resolution.value, (Vector2D{2560, 1440}));
    EXPECT_EQ(r->resolution.block, 0);
    EXPECT_FLOAT_EQ(r->scale.value, 1.6F);
    EXPECT_EQ(r->scale.block, 1);
    EXPECT_EQ(r->scale.source, XR_DEFSRC_BLOCK);
    EXPECT_FLOAT_EQ(r->size.value, 2.0F);
    EXPECT_EQ(r->size.block, 0);
    EXPECT_FLOAT_EQ(r->distance.value, 1.2F);
    EXPECT_EQ(r->distance.block, 2);
}

TEST(XRDefaultsResolve, aModeWithoutRefreshMeans60) {
    std::vector<SXRDefaultsBlock> blocks = {block(Vector2D{2560, 1440})};
    blocks[0].refreshRate                = std::nullopt;
    auto r                               = resolveXRDefaults(blocks, "", questCtx(), {});
    ASSERT_TRUE(r.has_value());
    EXPECT_FLOAT_EQ(r->refreshRate.value, 60.F);
    EXPECT_EQ(r->refreshRate.source, XR_DEFSRC_BLOCK);

    blocks[0].refreshRate = 90.F;
    EXPECT_FLOAT_EQ(resolveXRDefaults(blocks, "", questCtx(), {})->refreshRate.value, 90.F);
}

TEST(XRDefaultsResolve, theXRealAndQuestBlocksPickTheirOwnHeadset) {
    auto xreal                                 = block(Vector2D{1920, 1080}, 1.6F, 1.4F);
    xreal.when                                 = whenHmd("desc:Nreal Air 2 Ultra 0x88888800");
    auto quest                                 = block(Vector2D{2560, 1440}, 1.25F, 2.2F);
    quest.when                                 = whenHmd("Quest 3");
    const std::vector<SXRDefaultsBlock> blocks = {xreal, quest};

    auto                                onXReal = resolveXRDefaults(blocks, "", xrealCtx(), {});
    ASSERT_TRUE(onXReal.has_value());
    EXPECT_EQ(onXReal->resolution.value, (Vector2D{1920, 1080}));
    EXPECT_FLOAT_EQ(onXReal->scale.value, 1.6F);
    EXPECT_EQ(onXReal->scale.block, 0);

    auto onQuest = resolveXRDefaults(blocks, "", questCtx(), {});
    ASSERT_TRUE(onQuest.has_value());
    EXPECT_EQ(onQuest->resolution.value, (Vector2D{2560, 1440}));
    EXPECT_FLOAT_EQ(onQuest->size.value, 2.2F);
    EXPECT_EQ(onQuest->size.block, 1);

    // Before any session: neither `when` matches, the globals stand.
    auto none = resolveXRDefaults(blocks, "", SXRSystemContext{}, {});
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->resolution.source, XR_DEFSRC_GLOBAL);
    EXPECT_EQ(none->scale.source, XR_DEFSRC_GLOBAL);
}

TEST(XRDefaultsResolve, profileLayersOverBaseRegardlessOfConfigOrder) {
    auto tall    = block(Vector2D{1080, 1920}, std::nullopt, 0.9F);
    tall.profile = "tall";
    // The profile block comes FIRST in the config and still wins over the base block after it.
    const std::vector<SXRDefaultsBlock> blocks = {tall, block(Vector2D{2560, 1440}, 1.6F, 2.0F)};

    auto                                r = resolveXRDefaults(blocks, "tall", questCtx(), {});
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->resolution.value, (Vector2D{1080, 1920}));
    EXPECT_EQ(r->resolution.source, XR_DEFSRC_PROFILE);
    EXPECT_EQ(r->resolution.block, 0);
    EXPECT_FLOAT_EQ(r->size.value, 0.9F);
    // Field-wise: the profile did not set scale, so the base block's survives.
    EXPECT_FLOAT_EQ(r->scale.value, 1.6F);
    EXPECT_EQ(r->scale.source, XR_DEFSRC_BLOCK);

    // Without the profile, profile blocks contribute nothing.
    auto base = resolveXRDefaults(blocks, "", questCtx(), {});
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base->resolution.value, (Vector2D{2560, 1440}));
}

TEST(XRDefaultsResolve, profileBlocksAreFilteredByWhenToo) {
    auto tallQuest                             = block(Vector2D{1440, 2560});
    tallQuest.profile                          = "tall";
    tallQuest.when                             = whenHmd("Quest");
    auto tallXReal                             = block(Vector2D{1080, 1920});
    tallXReal.profile                          = "tall";
    tallXReal.when                             = whenHmd("desc:Nreal");
    const std::vector<SXRDefaultsBlock> blocks = {tallQuest, tallXReal};

    EXPECT_EQ(resolveXRDefaults(blocks, "tall", questCtx(), {})->resolution.value, (Vector2D{1440, 2560}));
    EXPECT_EQ(resolveXRDefaults(blocks, "tall", xrealCtx(), {})->resolution.value, (Vector2D{1080, 1920}));
    // A KNOWN profile whose blocks do not match is not an error — it just adds nothing.
    auto none = resolveXRDefaults(blocks, "tall", SXRSystemContext{}, {});
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->resolution.source, XR_DEFSRC_GLOBAL);
}

TEST(XRDefaultsResolve, unknownProfileIsAnError) {
    const std::vector<SXRDefaultsBlock> blocks = {block(Vector2D{2560, 1440})};
    auto                                r      = resolveXRDefaults(blocks, "wide", questCtx(), {});
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().find("wide"), std::string::npos);
    EXPECT_FALSE(xrDefaultsHasProfile(blocks, "wide"));
    EXPECT_FALSE(resolveXRDefaults({}, "wide", questCtx(), {}).has_value());
}

TEST(XRDefaultsResolve, explicitFieldsOutrankEveryBlock) {
    auto tall                                  = block(Vector2D{1080, 1920}, 1.6F, 0.9F);
    tall.profile                               = "tall";
    const std::vector<SXRDefaultsBlock> blocks = {block(Vector2D{2560, 1440}, 1.25F, 2.0F), tall};

    SXRExplicitFields                   ex;
    ex.resolution = Vector2D{3840, 2160};
    ex.size       = 3.0F;
    auto r        = resolveXRDefaults(blocks, "tall", questCtx(), {}, ex);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->resolution.value, (Vector2D{3840, 2160}));
    EXPECT_EQ(r->resolution.source, XR_DEFSRC_EXPLICIT);
    EXPECT_FLOAT_EQ(r->refreshRate.value, 60.F) << "an explicit mode without @Hz is @60, not the block's refresh";
    EXPECT_FLOAT_EQ(r->size.value, 3.0F);
    EXPECT_EQ(r->size.source, XR_DEFSRC_EXPLICIT);
    // Scale is never explicit here (a `monitor =` rule owns that, downstream) — the profile's stands.
    EXPECT_FLOAT_EQ(r->scale.value, 1.6F);
    EXPECT_EQ(r->scale.source, XR_DEFSRC_PROFILE);
}

TEST(XRDefaultsResolve, requestedModeLeavesAPurelyGlobalModeUnrequestedUnlessPinned) {
    auto global = resolveXRDefaults({}, "", questCtx(), {});
    ASSERT_TRUE(global.has_value());
    auto [res, hz] = xrRequestedMode(*global, false);
    EXPECT_FALSE(res.has_value()) << "a declaration with no mode anywhere keeps the pre-xr_defaults behavior";
    EXPECT_FALSE(hz.has_value());

    auto [pres, phz] = xrRequestedMode(*global, true);
    ASSERT_TRUE(pres.has_value()) << "a runtime create always pinned 1920x1080@60";
    EXPECT_EQ(*pres, (Vector2D{1920, 1080}));
    EXPECT_FLOAT_EQ(*phz, 60.F);

    auto fromBlock   = resolveXRDefaults({block(Vector2D{2560, 1440})}, "", questCtx(), {});
    auto [bres, bhz] = xrRequestedMode(*fromBlock, false);
    ASSERT_TRUE(bres.has_value());
    EXPECT_EQ(*bres, (Vector2D{2560, 1440}));
}

TEST(XRDefaultsResolve, blockValidation) {
    EXPECT_TRUE(validateXRDefaultsBlock(block(Vector2D{1920, 1080}, 1.6F, 1.4F, 1.2F)).has_value());
    EXPECT_TRUE(validateXRDefaultsBlock(block(std::nullopt, 0.F)).has_value()) << "scale 0 = the PPI guess, like the global";
    EXPECT_FALSE(validateXRDefaultsBlock(block(std::nullopt, -1.F)).has_value());
    EXPECT_FALSE(validateXRDefaultsBlock(block(std::nullopt, std::nullopt, 0.F)).has_value());
    EXPECT_FALSE(validateXRDefaultsBlock(block(std::nullopt, std::nullopt, std::nullopt, -0.5F)).has_value());
}

// ---- name minting ----

TEST(XRMint, lowestFreeFromTwo) {
    EXPECT_EQ(mintXRMonitorName({}), "XR-2");
    EXPECT_EQ(mintXRMonitorName({"XR-1", "eDP-1"}), "XR-2") << "XR-1 is never minted, and is not in the way";
    EXPECT_EQ(mintXRMonitorName({"XR-2", "XR-3"}), "XR-4");
}

TEST(XRMint, fillsGaps) {
    EXPECT_EQ(mintXRMonitorName({"XR-2", "XR-4", "XR-5"}), "XR-3");
}

TEST(XRMint, declaredNamesAreTakenToo) {
    // The caller feeds live monitors AND every declared name (even ones whose `when` does not match
    // right now), so a declaration is never squatted on.
    EXPECT_EQ(mintXRMonitorName({"XR-main", "XR-2" /* declared */, "XR-3" /* live */}), "XR-4");
}

TEST(XRMint, modeShape) {
    EXPECT_TRUE(looksLikeXRMode("1920x1080"));
    EXPECT_TRUE(looksLikeXRMode("2560x1440@90"));
    EXPECT_TRUE(looksLikeXRMode("2560x1440@59.94"));
    EXPECT_FALSE(looksLikeXRMode("XR-box"));
    EXPECT_FALSE(looksLikeXRMode("x"));
    EXPECT_FALSE(looksLikeXRMode("1920x"));
    EXPECT_FALSE(looksLikeXRMode("1920x1080@"));
    EXPECT_FALSE(looksLikeXRMode("1920x1080@9.0.1"));
    EXPECT_FALSE(looksLikeXRMode("anchor:local"));
}

// ---- create grammar: optional name + profile:, every old form unchanged ----

TEST(XRCreateArgs, everyPreExistingFormParsesAsBefore) {
    auto a = parseXRMonitorCreateArgs("XR-scratch");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->m_name, "XR-scratch");

    auto b = parseXRMonitorCreateArgs("XR-scratch 2560x1440@120");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->m_name, "XR-scratch");
    EXPECT_EQ(*b->m_resolution, (Vector2D{2560, 1440}));

    auto c = parseXRMonitorCreateArgs("XR-scratch 1920x1080 anchor:head offset:0.4,-0.2,-1.0 size:0.6");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->m_name, "XR-scratch");
    EXPECT_EQ(c->m_anchor.mode, XR_ANCHOR_HEAD);

    auto d = parseXRMonitorCreateArgs("XR-scratch anchor:local pos:0,1,-1");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->m_name, "XR-scratch");
    EXPECT_FALSE(d->m_resolution.has_value());

    // A name with an 'x' in it was always a name — the strict mode shape keeps it one.
    auto e = parseXRMonitorCreateArgs("XR-box 1280x720");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->m_name, "XR-box");
    EXPECT_EQ(*e->m_resolution, (Vector2D{1280, 720}));
    EXPECT_TRUE(e->m_profile.empty());
}

TEST(XRCreateArgs, nameIsOptional) {
    auto bare = parseXRMonitorCreateArgs("");
    ASSERT_TRUE(bare.has_value()) << bare.error();
    EXPECT_TRUE(bare->m_name.empty());
    EXPECT_FALSE(bare->m_resolution.has_value());
    EXPECT_FALSE(bare->m_anchorProvided);

    auto mode = parseXRMonitorCreateArgs("2560x1440@90");
    ASSERT_TRUE(mode.has_value()) << mode.error();
    EXPECT_TRUE(mode->m_name.empty());
    EXPECT_EQ(*mode->m_resolution, (Vector2D{2560, 1440}));
    EXPECT_FLOAT_EQ(*mode->m_refreshRate, 90.F);

    auto anchor = parseXRMonitorCreateArgs("anchor:head offset:0,0,-1");
    ASSERT_TRUE(anchor.has_value()) << anchor.error();
    EXPECT_TRUE(anchor->m_name.empty());
    EXPECT_EQ(anchor->m_anchor.mode, XR_ANCHOR_HEAD);
}

TEST(XRCreateArgs, profileTokenAnywhere) {
    auto p = parseXRMonitorCreateArgs("profile:tall");
    ASSERT_TRUE(p.has_value()) << p.error();
    EXPECT_TRUE(p->m_name.empty());
    EXPECT_EQ(p->m_profile, "tall");

    auto q = parseXRMonitorCreateArgs("XR-5 1920x1080 profile:tall anchor:local pos:0,1,-1");
    ASSERT_TRUE(q.has_value()) << q.error();
    EXPECT_EQ(q->m_name, "XR-5");
    EXPECT_EQ(q->m_profile, "tall");
    EXPECT_EQ(*q->m_resolution, (Vector2D{1920, 1080}));
    EXPECT_TRUE(q->m_anchorProvided);

    auto r = parseXRMonitorCreateArgs("profile:tall XR-5");
    ASSERT_TRUE(r.has_value()) << r.error();
    EXPECT_EQ(r->m_name, "XR-5");

    auto s = parseXRMonitorCreateArgs("anchor:local pos:0,1,-1 profile:wide");
    ASSERT_TRUE(s.has_value()) << s.error();
    EXPECT_EQ(s->m_profile, "wide");
    EXPECT_TRUE(s->m_anchorProvided);
}

TEST(XRCreateArgs, malformedProfileIsAnError) {
    EXPECT_FALSE(parseXRMonitorCreateArgs("profile:").has_value());
    EXPECT_FALSE(parseXRMonitorCreateArgs("profile:a profile:b").has_value());
}

TEST(XRCreateArgs, garbageStillErrors) {
    EXPECT_FALSE(parseXRMonitorCreateArgs("XR-a junk").has_value());
    EXPECT_FALSE(parseXRMonitorCreateArgs("XR-a 1920x1080 size:2").has_value()) << "a kv tail still needs its anchor token";
}

// ---- which xr_monitor declarations are in force ----

namespace {
    SXRMonitorParams decl(const std::string& name, Vector2D mode, const std::string& hmd = "") {
        SXRMonitorParams p;
        p.m_name       = name;
        p.m_resolution = mode;
        if (!hmd.empty())
            p.m_when = whenHmd(hmd);
        return p;
    }
}

TEST(XRDeclarations, sameNameDifferentWhenPicksTheBoundHeadset) {
    const std::vector<SXRMonitorParams> all = {
        decl("XR-main", {1920, 1080}, "desc:Nreal Air 2 Ultra"),
        decl("XR-main", {2560, 1440}, "Quest 3"),
    };

    auto onXReal = effectiveXRMonitorDeclarations(all, xrealCtx());
    ASSERT_EQ(onXReal.size(), 1u);
    EXPECT_EQ(*onXReal[0].m_resolution, (Vector2D{1920, 1080}));

    auto onQuest = effectiveXRMonitorDeclarations(all, questCtx());
    ASSERT_EQ(onQuest.size(), 1u);
    EXPECT_EQ(*onQuest[0].m_resolution, (Vector2D{2560, 1440}));

    EXPECT_TRUE(effectiveXRMonitorDeclarations(all, SXRSystemContext{}).empty()) << "none match before a session: the monitor is not declared";
}

TEST(XRDeclarations, unconditionalFallbackPlusSpecificOverride) {
    const std::vector<SXRMonitorParams> all = {
        decl("XR-main", {1920, 1080}),
        decl("XR-chat", {1280, 720}),
        decl("XR-main", {2560, 1440}, "Quest"),
    };

    auto none = effectiveXRMonitorDeclarations(all, SXRSystemContext{});
    ASSERT_EQ(none.size(), 2u);
    EXPECT_EQ(none[0].m_name, "XR-main");
    EXPECT_EQ(*none[0].m_resolution, (Vector2D{1920, 1080}));

    auto quest = effectiveXRMonitorDeclarations(all, questCtx());
    ASSERT_EQ(quest.size(), 2u);
    EXPECT_EQ(quest[0].m_name, "XR-main") << "keeps the position of the first declaration";
    EXPECT_EQ(*quest[0].m_resolution, (Vector2D{2560, 1440}));
    EXPECT_EQ(quest[1].m_name, "XR-chat");
}

TEST(XRDeclarations, severalMatchingWhensIsLegalLastWinsAndIsReported) {
    const std::vector<SXRMonitorParams> all = {
        decl("XR-main", {1920, 1080}, "Quest"),
        decl("XR-main", {2560, 1440}, "Quest 3"),
        decl("XR-side", {1280, 720}),
        decl("XR-side", {1280, 800}),
    };

    std::vector<std::string> ambiguous;
    auto                     r = effectiveXRMonitorDeclarations(all, questCtx(), &ambiguous);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(*r[0].m_resolution, (Vector2D{2560, 1440}));
    ASSERT_EQ(ambiguous.size(), 1u) << "plain redeclaration (no `when`) is not ambiguous";
    EXPECT_EQ(ambiguous[0], "XR-main");
}
