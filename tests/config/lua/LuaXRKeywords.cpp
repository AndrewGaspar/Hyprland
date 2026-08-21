// The XR keywords have two config front ends: the classic `xrmonitor =` / `xrrule =` lines and
// Lua's hl.xr_monitor / hl.xr_rule. The entire design of the Lua side is that it is NOT a second
// parser — it names fields, emits the shared grammar's tokens, and hands them to the one parser the
// classic keyword uses. These tests exist to keep that true: every one of them writes the same
// declaration both ways and asserts the two land on identical state. If someone ever "optimizes"
// the Lua path into its own parse, these fail immediately rather than a year later in a headset.

#include <config/lua/ConfigManager.hpp>
#include <config/shared/xr/XRDeclarationManager.hpp>
#include <config/shared/monitor/MonitorRuleManager.hpp>
#include <config/shared/monitor/MonitorRule.hpp>

#include <openxr/XRMonitorConfig.hpp>
#include <openxr/XRRule.hpp>

#include <re2/re2.h>

#include <Compositor.hpp>
#include <managers/KeybindManager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>

extern "C" {
#include <lualib.h>
#include <lauxlib.h>
}

using namespace Config::Lua;

namespace Config::Lua {
    class CConfigManagerXRLuaTestAccessor {
      public:
        static void initializeOwnedLuaState(CConfigManager& mgr, const std::filesystem::path& mainConfigPath) {
            mgr.m_mainConfigPath = mainConfigPath.string();
            mgr.m_configPaths.clear();
            mgr.m_configPaths.emplace_back(mgr.m_mainConfigPath);
            mgr.reinitLuaState();
        }

        static lua_State* luaState(CConfigManager& mgr) {
            return mgr.m_lua;
        }

        // Errors raised outside a parse pop a desktop notification instead of joining the config
        // error list, so a test that wants to READ the error has to be inside one. This also makes
        // the tests exercise the file-parse path rather than the `hyprctl eval` one.
        static void setParsingConfig(CConfigManager& mgr, bool parsing) {
            mgr.m_isParsingConfig = parsing;
        }
    };
}

namespace {
    class CScopedCompositor {
      public:
        CScopedCompositor() : m_prevCompositor(std::move(g_pCompositor)), m_prevKeybindManager(std::move(g_pKeybindManager)) {
            g_pCompositor     = makeUnique<CCompositor>(true);
            g_pKeybindManager = makeUnique<CKeybindManager>();
        }

        ~CScopedCompositor() {
            g_pKeybindManager = std::move(m_prevKeybindManager);
            g_pCompositor     = std::move(m_prevCompositor);
        }

      private:
        UP<CCompositor>     m_prevCompositor;
        UP<CKeybindManager> m_prevKeybindManager;
    };

    class CTempDir {
      public:
        CTempDir() {
            const auto NOW = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path         = std::filesystem::temp_directory_path() / std::format("hyprland-lua-xr-{}", NOW);
            std::filesystem::create_directories(m_path);
        }

        ~CTempDir() {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }

        const std::filesystem::path& path() const {
            return m_path;
        }

      private:
        std::filesystem::path m_path;
    };

    // One Lua config manager on a scratch config, with the declared-XR store wiped so a test only
    // ever sees what it declared itself (the store is a process-global, like the monitor-rule one).
    class CXRLuaFixture {
      public:
        CXRLuaFixture() {
            Config::xrDeclarationMgr()->clear();
            m_mainConfig = m_tmp.path() / "hyprland.lua";
            std::ofstream(m_mainConfig) << "";
            CConfigManagerXRLuaTestAccessor::initializeOwnedLuaState(m_mgr, m_mainConfig);
            CConfigManagerXRLuaTestAccessor::setParsingConfig(m_mgr, true);
        }

        ~CXRLuaFixture() {
            CConfigManagerXRLuaTestAccessor::setParsingConfig(m_mgr, false);
            Config::xrDeclarationMgr()->clear();
        }

        // Runs a chunk and requires that Lua itself was happy. A rejected DECLARATION is not a Lua
        // error — the bindings report those the way every other binding does, through the config
        // error list, so one bad rule does not take the rest of the config down with it.
        void run(const char* code) {
            const auto L = CConfigManagerXRLuaTestAccessor::luaState(m_mgr);
            ASSERT_EQ(luaL_dostring(L, code), LUA_OK) << lua_tostring(L, -1);
        }

        std::string errors() {
            return m_mgr.getErrors();
        }

      private:
        CScopedCompositor     m_compositor;
        CTempDir              m_tmp;
        std::filesystem::path m_mainConfig;
        CConfigManager        m_mgr;
    };

    const std::vector<SXRMonitorParams>& declaredMonitors() {
        return Config::xrDeclarationMgr()->monitors();
    }

    const std::vector<OpenXR::SXRRule>& declaredRules() {
        return Config::xrDeclarationMgr()->rules();
    }

    // What the classic front end would have produced for the same line. handleXRMonitor is a thin
    // wrapper around exactly this call plus the store append, so this IS the classic result.
    SXRMonitorParams classicMonitor(const std::string& value) {
        auto parsed = OpenXR::parseXRMonitorLine(value);
        EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? std::string{} : parsed.error());
        return parsed.value_or(SXRMonitorParams{});
    }

    OpenXR::SXRRule classicRule(const std::string& value) {
        auto parsed = OpenXR::parseXRRuleLine(value);
        EXPECT_TRUE(parsed.has_value()) << (parsed.has_value() ? std::string{} : parsed.error());
        return parsed.value_or(OpenXR::SXRRule{});
    }

    void expectSameMonitor(const SXRMonitorParams& lua, const SXRMonitorParams& classic) {
        EXPECT_EQ(lua.m_name, classic.m_name);
        EXPECT_EQ(lua.m_resolution.has_value(), classic.m_resolution.has_value());
        if (lua.m_resolution && classic.m_resolution) {
            EXPECT_EQ(*lua.m_resolution, *classic.m_resolution);
        }
        EXPECT_EQ(lua.m_refreshRate, classic.m_refreshRate);
        EXPECT_EQ(lua.m_sizeMeters, classic.m_sizeMeters);
        EXPECT_EQ(lua.m_anchorProvided, classic.m_anchorProvided);
        // SXRAnchorState::operator== covers mode, device, the full pose quaternion and every
        // adaptive field — the whole reason the parser builds the quaternion rather than storing
        // degrees.
        EXPECT_TRUE(lua.m_anchor == classic.m_anchor);
    }

    std::string rePattern(const std::shared_ptr<const re2::RE2>& re) {
        return re ? re->pattern() : std::string{"<none>"};
    }

    void expectSameRule(const OpenXR::SXRRule& lua, const OpenXR::SXRRule& classic) {
        EXPECT_EQ(lua.effects.alpha, classic.effects.alpha);
        EXPECT_EQ(lua.effects.blackAlpha, classic.effects.blackAlpha);
        EXPECT_EQ(lua.effects.blackKnee, classic.effects.blackKnee);
        EXPECT_EQ(rePattern(lua.conds.monitorRe), rePattern(classic.conds.monitorRe));
        EXPECT_EQ(rePattern(lua.conds.focusClassRe), rePattern(classic.conds.focusClassRe));
        EXPECT_EQ(rePattern(lua.conds.focusTitleRe), rePattern(classic.conds.focusTitleRe));
        EXPECT_EQ(lua.conds.anchorState, classic.conds.anchorState);
        EXPECT_EQ(lua.conds.fullscreen, classic.conds.fullscreen);
    }
}

// ---- hl.xr_monitor ----

// The real thing: the adaptive XR-main line out of a working session config, written both ways.
TEST(ConfigLuaXRMonitor, tableFormMatchesTheClassicKeywordLine) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_monitor({
            name     = "XR-main",
            mode     = "2560x1440@90",
            anchor   = "local",
            pos      = { 0.017, 1.457, -1.408 },
            yaw      = 5.3,
            pitch    = 3.0,
            adaptive = true,
            roam     = "body",
            size     = 2.23,
        })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 1u);
    expectSameMonitor(declaredMonitors()[0], classicMonitor("XR-main, 2560x1440@90, anchor:local pos:0.017,1.457,-1.408 yaw:5.3 pitch:3.0 adaptive:on roam:body, size:2.23"));
}

// The string overload takes the value half of a classic line verbatim — the paste target for
// `hyprctl openxr layout`.
TEST(ConfigLuaXRMonitor, stringFormIsTheClassicKeywordLine) {
    CXRLuaFixture     fx;
    const std::string LINE = "XR-main, 2560x1440@90, anchor:local pos:0.017,1.457,-1.408 yaw:5.3 pitch:3.0 adaptive:on roam:body, size:2.23";

    fx.run(std::format("hl.xr_monitor([[{}]])", LINE).c_str());

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 1u);
    expectSameMonitor(declaredMonitors()[0], classicMonitor(LINE));
}

// A pose written as a string and as a table must not be two different poses.
TEST(ConfigLuaXRMonitor, vec3StringAndTableFormsAgree) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "local", pos = "0.5,1.2,-1.75" })
        hl.xr_monitor({ name = "B", mode = "1920x1080@60", anchor = "local", pos = { 0.5, 1.2, -1.75 } })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 2u);
    EXPECT_TRUE(declaredMonitors()[0].m_anchor == declaredMonitors()[1].m_anchor);
    expectSameMonitor(declaredMonitors()[1], classicMonitor("B, 1920x1080@60, anchor:local pos:0.5,1.2,-1.75"));
}

// The grammar spells the mode token `anchor:local`; the table field is just `local`. A spec copied
// out of a .conf keeps working when pasted into a table, so accept both.
TEST(ConfigLuaXRMonitor, anchorAcceptsBareAndPrefixedMode) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "local",        pos = { 0, 1, -1 } })
        hl.xr_monitor({ name = "B", mode = "1920x1080@60", anchor = "anchor:local", pos = { 0, 1, -1 } })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 2u);
    EXPECT_TRUE(declaredMonitors()[0].m_anchor == declaredMonitors()[1].m_anchor);
}

// Head/body/device anchors take `offset:` rather than `pos:`, and their stored rotation is built
// differently (head is lookAt-driven, body is yaw-only). All of that is the shared parser's job.
TEST(ConfigLuaXRMonitor, bodyAnchorMatchesTheClassicLine) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_monitor({ name = "XR-hud", mode = "1080x1920@72", anchor = "body", offset = { 0, -0.2, -1.1 }, yaw = 12.5, size = 0.8 })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 1u);
    expectSameMonitor(declaredMonitors()[0], classicMonitor("XR-hud, 1080x1920@72, anchor:body offset:0,-0.2,-1.1 yaw:12.5 size:0.8"));
}

// `return` is a Lua keyword. Both spellings must reach the same `return:` token.
TEST(ConfigLuaXRMonitor, returnRadiusAliasMatchesTheReservedSpelling) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "local", pos = { 0, 1, -1 },
                        adaptive = true, leave = 1.5, return_radius = 1.0 })
        hl.xr_monitor({ name = "B", mode = "1920x1080@60", anchor = "local", pos = { 0, 1, -1 },
                        adaptive = true, leave = 1.5, ["return"] = 1.0 })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 2u);
    EXPECT_TRUE(declaredMonitors()[0].m_anchor == declaredMonitors()[1].m_anchor);
    expectSameMonitor(declaredMonitors()[1], classicMonitor("B, 1920x1080@60, anchor:local pos:0,1,-1 adaptive:on leave:1.5 return:1.0"));
}

// Name-keyed, exactly as the classic keyword is: a second declaration of a name replaces the first
// rather than materializing two monitors.
TEST(ConfigLuaXRMonitor, laterDeclarationOfTheSameNameReplacesTheEarlierOne) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_monitor({ name = "XR-main", mode = "1920x1080@60", anchor = "local", pos = { 0, 1, -1 } })
        hl.xr_monitor({ name = "XR-main", mode = "2560x1440@90", anchor = "local", pos = { 0, 1.5, -2 } })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredMonitors().size(), 1u);
    EXPECT_EQ(declaredMonitors()[0].m_resolution, (Vector2D{2560, 1440}));
}

// ---- hl.xr_monitor, malformed ----

// Cross-field validation lives in the shared parser, so the Lua front end inherits it: `pos:` on a
// body anchor is rejected with the parser's own words, as a config error, with nothing stored.
TEST(ConfigLuaXRMonitor, posOnANonLocalAnchorIsAConfigErrorNotACrash) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "body", pos = { 0, 1, -1 } }) )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("only valid for anchor:local"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, adaptiveHysteresisIsValidatedByTheSharedParser) {
    CXRLuaFixture fx;

    // return >= leave is not hysteresis, it is an oscillator. The parser says so; Lua must not have
    // its own opinion.
    fx.run(R"LUA( hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "local", pos = { 0, 1, -1 },
                                  adaptive = true, leave = 1.0, return_radius = 1.5 }) )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("hysteresis"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, unknownFieldIsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "local", pos = { 0, 1, -1 }, wobble = 3 }) )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("unknown field 'wobble'"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, missingNameIsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_monitor({ mode = "1920x1080@60", anchor = "local", pos = { 0, 1, -1 } }) )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("'name' field is required"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, badModeIsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_monitor({ name = "A", mode = "not-a-mode", anchor = "local", pos = { 0, 1, -1 } }) )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("invalid mode"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, malformedVec3IsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_monitor({ name = "A", mode = "1920x1080@60", anchor = "local", pos = { 0, 1 } }) )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("element 3 is not a number"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, malformedStringFormIsAConfigError) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_monitor("XR-main, 2560x1440@90") )LUA");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("expected <name>, <mode>, <anchor-spec>"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRMonitor, wrongArgumentTypeIsAConfigError) {
    CXRLuaFixture fx;

    fx.run("hl.xr_monitor(42)");

    EXPECT_TRUE(declaredMonitors().empty());
    EXPECT_NE(fx.errors().find("must be a table"), std::string::npos) << fx.errors();
}

// ---- hl.xr_rule ----

// The three rules a real session ships with, written both ways.
TEST(ConfigLuaXRRule, tableFormMatchesTheClassicKeywordLines) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_rule({ alpha = 0.55, match = { anchorstate = "follow" } })
        hl.xr_rule({ alpha = 1.0, blackalpha = "off",
                     match = { anchorstate = "docked", focusclass = "^(steam_app_|steam_proton)", fullscreen = true } })
        hl.xr_rule({ blackalpha = "off", match = { focusclass = "(mpv|vlc)" } })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredRules().size(), 3u);
    expectSameRule(declaredRules()[0], classicRule("alpha 0.55, anchorstate:follow"));
    expectSameRule(declaredRules()[1], classicRule("alpha 1.0 blackalpha off, anchorstate:docked focusclass:^(steam_app_|steam_proton) fullscreen:1"));
    expectSameRule(declaredRules()[2], classicRule("blackalpha off, focusclass:(mpv|vlc)"));
}

// Order is the whole precedence model for xrrule (a later match overrides an earlier one PER
// effect), so the store must append, never key.
TEST(ConfigLuaXRRule, configOrderIsPreserved) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.xr_rule({ alpha = 0.1, match = { monitor = "first" } })
        hl.xr_rule({ alpha = 0.2, match = { monitor = "second" } })
        hl.xr_rule({ alpha = 0.3, match = { monitor = "third" } })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredRules().size(), 3u);
    EXPECT_EQ(rePattern(declaredRules()[0].conds.monitorRe), "first");
    EXPECT_EQ(rePattern(declaredRules()[1].conds.monitorRe), "second");
    EXPECT_EQ(rePattern(declaredRules()[2].conds.monitorRe), "third");
}

// `hyprctl openxr rules` prints SXRRule::raw back at the user, so a Lua rule has to be able to name
// itself in the same language a .conf rule does: the emitted line must re-parse to itself.
TEST(ConfigLuaXRRule, theEmittedRawLineRoundTrips) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 0.55, blackalpha_knee = 0.25, match = { anchorstate = "follow", fullscreen = false } }) )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredRules().size(), 1u);
    expectSameRule(declaredRules()[0], classicRule(declaredRules()[0].raw));
}

// A title regex with spaces has to survive the trip through the token line.
TEST(ConfigLuaXRRule, conditionValuesWithSpacesAreQuoted) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 0.5, match = { focustitle = "Mozilla Firefox" } }) )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredRules().size(), 1u);
    EXPECT_EQ(rePattern(declaredRules()[0].conds.focusTitleRe), "Mozilla Firefox");
    expectSameRule(declaredRules()[0], classicRule("alpha 0.5, focustitle:\"Mozilla Firefox\""));
}

TEST(ConfigLuaXRRule, stringFormIsTheClassicKeywordLine) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule("alpha 1.0 blackalpha off, anchorstate:docked fullscreen:1") )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_EQ(declaredRules().size(), 1u);
    expectSameRule(declaredRules()[0], classicRule("alpha 1.0 blackalpha off, anchorstate:docked fullscreen:1"));
    EXPECT_EQ(declaredRules()[0].raw, "alpha 1.0 blackalpha off, anchorstate:docked fullscreen:1");
}

// ---- hl.xr_rule, malformed ----

TEST(ConfigLuaXRRule, noEffectsIsAConfigError) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ match = { anchorstate = "follow" } }) )LUA");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_NE(fx.errors().find("no effects given"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRRule, alphaOutOfRangeIsRejectedByTheSharedParser) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 4.0, match = { anchorstate = "follow" } }) )LUA");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_FALSE(fx.errors().empty());
}

TEST(ConfigLuaXRRule, unknownAnchorStateIsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 0.5, match = { anchorstate = "sideways" } }) )LUA");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_NE(fx.errors().find("unknown anchorstate"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRRule, unknownEffectIsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 0.5, sparkle = 1 }) )LUA");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_NE(fx.errors().find("unknown effect 'sparkle'"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRRule, unknownMatchConditionIsRejected) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 0.5, match = { phase_of_moon = "waxing" } }) )LUA");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_NE(fx.errors().find("unknown match condition 'phase_of_moon'"), std::string::npos) << fx.errors();
}

TEST(ConfigLuaXRRule, badRegexIsAConfigErrorNotACrash) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.xr_rule({ alpha = 0.5, match = { focusclass = "^(unclosed" } }) )LUA");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_FALSE(fx.errors().empty());
}

TEST(ConfigLuaXRRule, wrongArgumentTypeIsAConfigError) {
    CXRLuaFixture fx;

    fx.run("hl.xr_rule(42)");

    EXPECT_TRUE(declaredRules().empty());
    EXPECT_NE(fx.errors().find("must be a table"), std::string::npos) << fx.errors();
}

// ---- the rest of the XR session surface, from Lua ----
//
// xrmonitor and xrrule were the two keywords with no Lua form at all. The other four things an XR
// session config does already had one — but "already had one" was an assumption nobody had run, and
// the whole point of this work is that a session can be written in Lua without a .conf. So run it.

// `monitor = …, stereo:sbs` — the XReal SBS pack. hl.monitor has had a `stereo` field since the
// flat-SBS work; this pins that it parses rather than landing in "unknown field".
TEST(ConfigLuaXRSurface, monitorTakesTheStereoToken) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.monitor({ output = "desc:Nreal Air 2 Ultra 0x88888800", mode = "3840x1080@60", position = "auto", scale = "1", stereo = "sbs" })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    const auto& rules = Config::monitorRuleMgr()->all();
    const auto  IT    = std::ranges::find_if(rules, [](const auto& r) { return r.m_name == "desc:Nreal Air 2 Ultra 0x88888800"; });
    ASSERT_NE(IT, rules.end());
    EXPECT_EQ(IT->m_stereo, Config::STEREO_SBS);
}

// The per-window stereo layout and the viewpoint-portal authorization, both fork window-rule
// effects. A missing entry in the Lua effect table is silent (it is name-keyed), which is exactly
// the failure mode the table's own comment warns about — so assert, do not assume.
TEST(ConfigLuaXRSurface, windowRuleTakesStereoAndViewpoint) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.window_rule({ name = "xr-stereo",    stereo = "hsbs always", match = { tag = "stereo-hsbs" } })
        hl.window_rule({ name = "xr-viewpoint", viewpoint = true,       match = { class = "^(hypxr-viewpoint-demo)$" } })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
}

// Every `openxr:` key is reachable as hl.config({ openxr = { … } }), including the hot ones that
// under the classic front end need `hyprctl keyword` — which does not exist for a Lua config
// (`hyprctl keyword` refuses anything but the legacy parser and points at `eval`). Under Lua the
// hot path is `hyprctl eval` running this same call, and hl.config schedules the refresh that
// COpenXRManager listens for.
TEST(ConfigLuaXRSurface, openxrSectionKeysAreSettableFromLua) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.config({
            openxr = {
                enabled       = true,
                overlay       = true,
                gpu           = "/dev/dri/renderD129",
                blend_mode    = "alpha",
                black_alpha   = 0.2,
                hand_input    = "off",
                depth_desktop = true,
            },
        })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
}

// A bad key must be named, not swallowed — the classic front end errors on an unknown `openxr:`
// keyword and Lua has to as well, or a typo silently disables half a session.
TEST(ConfigLuaXRSurface, unknownOpenxrKeyIsAConfigError) {
    CXRLuaFixture fx;

    fx.run(R"LUA( hl.config({ openxr = { not_a_real_key = 1 } }) )LUA");

    EXPECT_NE(fx.errors().find("unknown config key 'openxr.not_a_real_key'"), std::string::npos) << fx.errors();
}

// The xrmonitor DISPATCHER, bound from Lua. hl.dsp is curated and xrmonitor is a fork dispatcher,
// so without hl.dsp.xrmonitor every XR keybind would have to shell out to hyprctl per press.
TEST(ConfigLuaXRSurface, xrmonitorDispatcherIsBindableFromLua) {
    CXRLuaFixture fx;

    fx.run(R"LUA(
        hl.bind("SUPER + SHIFT + G",   hl.dsp.xrmonitor("gazegrab"))
        hl.bind("SUPER + ALT + equal", hl.dsp.xrmonitor("gazepush 0.1"), { repeating = true })
        hl.bind("SUPER + ALT + H",     hl.dsp.xrmonitor("handinput toggle"), { description = "Toggle XR hand input" })
    )LUA");

    EXPECT_EQ(fx.errors(), "");
    ASSERT_TRUE(g_pKeybindManager);
    EXPECT_EQ(std::ranges::count_if(g_pKeybindManager->m_keybinds, [](const auto& kb) { return kb->handler == "__lua"; }), 3);
}

// ---- the shipped example ----

// contrib/hyprland-xr.lua is the worked translation of a real XR session config, and a worked
// example that does not load is worse than none at all — it teaches the wrong spelling. Load the
// actual shipped file (not a copy of it in this test) and require it to produce zero config errors
// and the XR state it claims to.
//
// The example opens with `require("hyprland")`, its analogue of the classic
// `source = ~/.config/hypr/hyprland.conf`, so the fixture drops a stub of that next to it; `require`
// resolves against the MAIN config's directory, which is what makes the composition work at all.
TEST(ConfigLuaXRSurface, theShippedExampleConfigLoadsClean) {
    const std::filesystem::path EXAMPLE = std::filesystem::path(HYPRLAND_SOURCE_ROOT) / "contrib" / "hyprland-xr.lua";
    ASSERT_TRUE(std::filesystem::exists(EXAMPLE)) << EXAMPLE.string();

    CScopedCompositor compositor;
    CTempDir          tmp;
    Config::xrDeclarationMgr()->clear();

    const auto MAIN = tmp.path() / "hyprland-xr.lua";
    std::filesystem::copy_file(EXAMPLE, MAIN);
    std::ofstream(tmp.path() / "hyprland.lua") << "-- stands in for the Omarchy desktop config\n";

    CConfigManager mgr;
    CConfigManagerXRLuaTestAccessor::initializeOwnedLuaState(mgr, MAIN);
    CConfigManagerXRLuaTestAccessor::setParsingConfig(mgr, true);

    const auto L = CConfigManagerXRLuaTestAccessor::luaState(mgr);
    ASSERT_EQ(luaL_dofile(L, MAIN.c_str()), LUA_OK) << lua_tostring(L, -1);
    EXPECT_EQ(mgr.getErrors(), "");

    // The declarations the example is actually FOR.
    ASSERT_EQ(Config::xrDeclarationMgr()->monitors().size(), 1u);
    EXPECT_EQ(Config::xrDeclarationMgr()->monitors()[0].m_name, "XR-main");
    EXPECT_EQ(Config::xrDeclarationMgr()->monitors()[0].m_resolution, (Vector2D{2560, 1440}));
    EXPECT_TRUE(Config::xrDeclarationMgr()->monitors()[0].m_anchor.adaptive.enabled);
    EXPECT_EQ(Config::xrDeclarationMgr()->rules().size(), 3u);

    CConfigManagerXRLuaTestAccessor::setParsingConfig(mgr, false);
    Config::xrDeclarationMgr()->clear();
}
