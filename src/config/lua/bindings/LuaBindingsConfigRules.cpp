#include "LuaBindingsInternal.hpp"
#include "Check.hpp"

#include "../objects/LuaLayerRule.hpp"
#include "../objects/LuaWindowRule.hpp"
#include "../objects/LuaWorkspaceRule.hpp"

#include "../types/LuaConfigBool.hpp"
#include "../types/LuaConfigCssGap.hpp"
#include "../types/LuaConfigFloat.hpp"
#include "../types/LuaConfigGradient.hpp"
#include "../types/LuaConfigInt.hpp"
#include "../types/LuaConfigString.hpp"
#include "../types/LuaConfigVec2.hpp"

#include "../../supplementary/executor/Executor.hpp"
#include "../../supplementary/propRefresher/PropRefresher.hpp"
#include "../../shared/animation/AnimationTree.hpp"
#include "../../shared/monitor/MonitorRuleManager.hpp"
#include "../../shared/xr/XRDeclarationManager.hpp"
#include "../../shared/monitor/Parser.hpp"
#include "../../shared/workspace/WorkspaceRuleManager.hpp"

#include "../../../desktop/rule/Engine.hpp"
#include "../../../desktop/rule/layerRule/LayerRule.hpp"
#include "../../../desktop/rule/layerRule/LayerRuleEffectContainer.hpp"
#include "../../../desktop/rule/windowRule/WindowRule.hpp"
#include "../../../desktop/rule/windowRule/WindowRuleEffectContainer.hpp"
#include "../../../layout/LayoutManager.hpp"
#include "../../../layout/supplementary/WorkspaceAlgoMatcher.hpp"
#include "../../../animation/AnimationManager.hpp"
#include "../../../managers/eventLoop/EventLoopManager.hpp"
#include "../../../openxr/OpenXRManager.hpp"
#include "../../../managers/input/InputManager.hpp"
#include "../../../managers/input/trackpad/TrackpadGestures.hpp"
#include "../../../managers/input/trackpad/gestures/CloseGesture.hpp"
#include "../../../managers/input/trackpad/gestures/CursorZoomGesture.hpp"
#include "../../../managers/input/trackpad/gestures/DispatcherGesture.hpp"
#include "../../../managers/input/trackpad/gestures/FloatGesture.hpp"
#include "../../../managers/input/trackpad/gestures/FullscreenGesture.hpp"
#include "../../../managers/input/trackpad/gestures/LuaFunctionGesture.hpp"
#include "../../../managers/input/trackpad/gestures/MoveGesture.hpp"
#include "../../../managers/input/trackpad/gestures/ResizeGesture.hpp"
#include "../../../managers/input/trackpad/gestures/SpecialWorkspaceGesture.hpp"
#include "../../../managers/input/trackpad/gestures/WorkspaceSwipeGesture.hpp"
#include "../../../managers/input/trackpad/gestures/ScrollMoveGesture.hpp"
#include "../../../managers/permissions/DynamicPermissionManager.hpp"

#include <hyprutils/utils/ScopeGuard.hpp>

#include <vector>

using namespace Config;
using namespace Config::Lua;
using namespace Config::Lua::Bindings;
using namespace Hyprutils::Utils;

namespace {
    struct SFieldDesc {
        const char* name;
        ILuaConfigValue* (*factory)();
    };

    struct SMonitorFieldDesc {
        const char* name;
        ILuaConfigValue* (*factory)();
        bool (*apply)(ILuaConfigValue*, CMonitorRuleParser&);
    };

    struct SLayerRuleEffectDesc {
        const char* name;
        ILuaConfigValue* (*factory)();
        uint16_t effect;
    };

    struct SWorkspaceRuleFieldDesc {
        const char* name;
        ILuaConfigValue* (*factory)();
        void (*apply)(ILuaConfigValue*, Config::CWorkspaceRule&);
    };

    using LE = Desktop::Rule::eLayerRuleEffect;

    inline constexpr SMonitorFieldDesc MONITOR_FIELDS[] = {
        {"mode", []() -> ILuaConfigValue* { return new CLuaConfigString("preferred"); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) { return p.parseMode(*sc<const Config::STRING*>(v->data())); }},
        {"position", []() -> ILuaConfigValue* { return new CLuaConfigString("auto"); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) { return p.parsePosition(*sc<const Config::STRING*>(v->data())); }},
        {"scale", []() -> ILuaConfigValue* { return new CLuaConfigString("auto"); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) { return p.parseScale(*sc<const Config::STRING*>(v->data())); }},
        {"reserved", []() -> ILuaConfigValue* { return new CLuaConfigCssGap(0); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             const auto& gap = *sc<const Config::CCssGapData*>(v->data());
             return p.setReserved(Desktop::CReservedArea(gap.m_top, gap.m_right, gap.m_bottom, gap.m_left));
         }},
        {"reserved_area", []() -> ILuaConfigValue* { return new CLuaConfigCssGap(0); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             const auto& gap = *sc<const Config::CCssGapData*>(v->data());
             return p.setReserved(Desktop::CReservedArea(gap.m_top, gap.m_right, gap.m_bottom, gap.m_left));
         }},
        {"disabled", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_disabled = *sc<const Config::BOOL*>(v->data());
             return true;
         }},
        {"transform", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 7); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_transform = sc<wl_output_transform>(*sc<const Config::INTEGER*>(v->data()));
             return true;
         }},
        {"mirror", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.setMirror(*sc<const Config::STRING*>(v->data()));
             return true;
         }},
        {"bitdepth", []() -> ILuaConfigValue* { return new CLuaConfigInt(8); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_enable10bit = *sc<const Config::INTEGER*>(v->data()) == 10;
             return true;
         }},
        {"cm", []() -> ILuaConfigValue* { return new CLuaConfigString("srgb"); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) { return p.parseCM(*sc<const Config::STRING*>(v->data())); }},
        {"sdr_eotf", []() -> ILuaConfigValue* { return new CLuaConfigString("default"); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_sdrEotf = NTransferFunction::fromString(*sc<const Config::STRING*>(v->data()));
             return true;
         }},
        {"sdrbrightness", []() -> ILuaConfigValue* { return new CLuaConfigFloat(1.F); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_sdrBrightness = *sc<const Config::FLOAT*>(v->data());
             return true;
         }},
        {"sdrsaturation", []() -> ILuaConfigValue* { return new CLuaConfigFloat(1.F); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_sdrSaturation = *sc<const Config::FLOAT*>(v->data());
             return true;
         }},
        {"vrr", []() -> ILuaConfigValue* { return new CLuaConfigInt(-1, -1, 3); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             const auto VRR = sc<int>(*sc<const Config::INTEGER*>(v->data()));
             p.rule().m_vrr = VRR < 0 ? std::nullopt : std::optional(VRR);
             return true;
         }},
        {"icc", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) { return p.parseICC(*sc<const Config::STRING*>(v->data())); }},
        {"stereo", []() -> ILuaConfigValue* { return new CLuaConfigString("off"); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) { return p.parseStereo(*sc<const Config::STRING*>(v->data())); }},
        {"supports_wide_color", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, -1, 1); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_supportsWideColor = sc<int>(*sc<const Config::INTEGER*>(v->data()));
             return true;
         }},
        {"supports_hdr", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, -1, 1); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_supportsHDR = sc<int>(*sc<const Config::INTEGER*>(v->data()));
             return true;
         }},
        {"sdr_min_luminance", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.2F); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_sdrMinLuminance = *sc<const Config::FLOAT*>(v->data());
             return true;
         }},
        {"sdr_max_luminance", []() -> ILuaConfigValue* { return new CLuaConfigInt(80); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_sdrMaxLuminance = sc<int>(*sc<const Config::INTEGER*>(v->data()));
             return true;
         }},
        {"min_luminance", []() -> ILuaConfigValue* { return new CLuaConfigFloat(-1.F); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_minLuminance = *sc<const Config::FLOAT*>(v->data());
             return true;
         }},
        {"max_luminance", []() -> ILuaConfigValue* { return new CLuaConfigInt(-1); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_maxLuminance = sc<int>(*sc<const Config::INTEGER*>(v->data()));
             return true;
         }},
        {"max_avg_luminance", []() -> ILuaConfigValue* { return new CLuaConfigInt(-1); },
         [](ILuaConfigValue* v, CMonitorRuleParser& p) {
             p.rule().m_maxAvgLuminance = sc<int>(*sc<const Config::INTEGER*>(v->data()));
             return true;
         }},
    };

    static_assert(sizeof(Internal::WINDOW_RULE_EFFECT_DESCS) / sizeof(Internal::SWindowRuleEffectDesc) == Internal::WE::WINDOW_RULE_EFFECT_LAST_STATIC - 1);

    inline constexpr SLayerRuleEffectDesc LAYER_RULE_EFFECT_DESCS[] = {
        {"no_anim", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, LE::LAYER_RULE_EFFECT_NO_ANIM},
        {"blur", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, LE::LAYER_RULE_EFFECT_BLUR},
        {"blur_popups", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, LE::LAYER_RULE_EFFECT_BLUR_POPUPS},
        {"ignore_alpha", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F, 0.F, 1.F); }, LE::LAYER_RULE_EFFECT_IGNORE_ALPHA},
        {"dim_around", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, LE::LAYER_RULE_EFFECT_DIM_AROUND},
        {"xray", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, LE::LAYER_RULE_EFFECT_XRAY},
        {"animation", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, LE::LAYER_RULE_EFFECT_ANIMATION},
        {"order", []() -> ILuaConfigValue* { return new CLuaConfigInt(0); }, LE::LAYER_RULE_EFFECT_ORDER},
        {"above_lock", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 2); }, LE::LAYER_RULE_EFFECT_ABOVE_LOCK},
        {"no_screen_share", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, LE::LAYER_RULE_EFFECT_NO_SCREEN_SHARE},
        // no min/max — it would REJECT where the classic front end clamps; see the window-rule
        // `depth` entry in LuaBindingsInternal.hpp
        {"depth", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, LE::LAYER_RULE_EFFECT_DEPTH},
    };

    static_assert(sizeof(LAYER_RULE_EFFECT_DESCS) / sizeof(SLayerRuleEffectDesc) == LE::LAYER_RULE_EFFECT_LAST_STATIC - 1);

    inline constexpr SWorkspaceRuleFieldDesc WORKSPACE_RULE_FIELDS[] = {
        {"monitor", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_monitor = *sc<const Config::STRING*>(v->data()); }},
        {"default", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_isDefault = *sc<const Config::BOOL*>(v->data()); }},
        {"persistent", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_isPersistent = *sc<const Config::BOOL*>(v->data()); }},
        {"gaps_in", []() -> ILuaConfigValue* { return new CLuaConfigCssGap(5); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_gapsIn = *sc<const Config::CCssGapData*>(v->data()); }},
        {"gaps_out", []() -> ILuaConfigValue* { return new CLuaConfigCssGap(20); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_gapsOut = *sc<const Config::CCssGapData*>(v->data()); }},
        {"float_gaps", []() -> ILuaConfigValue* { return new CLuaConfigCssGap(0); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_floatGaps = *sc<const Config::CCssGapData*>(v->data()); }},
        {"border_size", []() -> ILuaConfigValue* { return new CLuaConfigInt(-1); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_borderSize = *sc<const Config::INTEGER*>(v->data()); }},
        {"no_border", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_noBorder = *sc<const Config::BOOL*>(v->data()); }},
        {"no_rounding", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_noRounding = *sc<const Config::BOOL*>(v->data()); }},
        {"decorate", []() -> ILuaConfigValue* { return new CLuaConfigBool(true); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_decorate = *sc<const Config::BOOL*>(v->data()); }},
        {"no_shadow", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_noShadow = *sc<const Config::BOOL*>(v->data()); }},
        {"on_created_empty", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_onCreatedEmptyRunCmd = *sc<const Config::STRING*>(v->data()); }},
        {"default_name", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_defaultName = *sc<const Config::STRING*>(v->data()); }},
        {"layout", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_layout = *sc<const Config::STRING*>(v->data()); }},
        {"animation", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); },
         [](ILuaConfigValue* v, Config::CWorkspaceRule& r) { r.m_animationStyle = *sc<const Config::STRING*>(v->data()); }},
    };

    // ---- XR: the `xrmonitor` / `xrrule` keywords, as Lua (docs/openxr/05 §3 / §3.5) ----
    //
    // Same contract as everything else in this file: the Lua front end NEVER re-implements a
    // grammar. It reads named fields, turns each one into exactly one token of the SHARED parser's
    // own vocabulary, and hands the tokens to that parser. Field names below therefore ARE the
    // grammar's key names — not a parallel vocabulary that could drift from the `.conf` spelling.

    // How a parsed field becomes its token's value.
    enum eXRTokenKind : uint8_t {
        XR_TOK_STRING = 0, // verbatim
        XR_TOK_FLOAT,      // shortest round-trip decimal. NOT std::to_string / ILuaConfigValue::
                           // toString(), whose 6-place truncation would silently move a pose
                           // pasted back from `hyprctl openxr layout`.
        XR_TOK_BOOL,       // on/off — how the anchor grammar spells a flag
        XR_TOK_VEC3,       // "x,y,z"; the table form {x, y, z} is accepted and formatted to it
        XR_TOK_NUM_OR_STR, // a number, or a keyword like "off" (xrrule's blackalpha)
    };

    struct SXRFieldDesc {
        const char* name;
        ILuaConfigValue* (*factory)();
        eXRTokenKind kind;
        const char*  token; // the grammar's key; differs from `name` only where Lua reserves a word
    };

    // hl.xr_monitor's anchor-spec fields (doc 05 §3). `name` / `mode` / `anchor` are positional in
    // the classic line and handled separately, exactly as hl.monitor handles `output`.
    inline constexpr SXRFieldDesc XR_MONITOR_FIELDS[] = {
        {"pos", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_VEC3, "pos"},
        {"offset", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_VEC3, "offset"},
        {"yaw", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "yaw"},
        {"pitch", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "pitch"},
        {"size", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "size"},
        {"adaptive", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, XR_TOK_BOOL, "adaptive"},
        {"roam", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_STRING, "roam"},
        {"roam_offset", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_VEC3, "roam_offset"},
        {"roam_yaw", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "roam_yaw"},
        {"leave", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "leave"},
        // `return` is a Lua keyword, so `return_radius` is the spelling that can be written as a
        // bare table key. `["return"]` still works; both emit the `return:` token of the grammar.
        // (Apostrophes are deliberately absent from every comment INSIDE these initializers: the
        // LuaLS stub generator brace-matches this array with a scanner that treats one as a string
        // delimiter, and an odd count silently swallows the arrays that follow.)
        {"return_radius", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "return"},
        {"return", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F); }, XR_TOK_FLOAT, "return"},
        {"carry", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, XR_TOK_BOOL, "carry"},
    };

    // hl.xr_rule's effects — the half BEFORE the comma in `xrrule = <effects>, <conditions>`.
    inline constexpr SXRFieldDesc XR_RULE_EFFECT_FIELDS[] = {
        {"alpha", []() -> ILuaConfigValue* { return new CLuaConfigFloat(1.F); }, XR_TOK_FLOAT, "alpha"},
        // number, or the string "off" (keying disabled) — the grammar accepts both.
        {"blackalpha", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_NUM_OR_STR, "blackalpha"},
        {"blackalpha_knee", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.1F); }, XR_TOK_FLOAT, "blackalpha_knee"},
    };

    // hl.xr_rule's conditions — the half AFTER the comma. In `match = { ... }`, mirroring
    // hl.window_rule, whose conditions live in a `match` table too.
    inline constexpr SXRFieldDesc XR_RULE_MATCH_FIELDS[] = {
        {"monitor", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_STRING, "monitor"},
        {"anchorstate", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_STRING, "anchorstate"},
        {"focusclass", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_STRING, "focusclass"},
        {"focustitle", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }, XR_TOK_STRING, "focustitle"},
        {"fullscreen", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }, XR_TOK_BOOL, "fullscreen"},
    };

    inline constexpr SFieldDesc DEVICE_FIELDS[] = {
        {"sensitivity", []() -> ILuaConfigValue* { return new CLuaConfigFloat(0.F, -1.F, 1.F); }},
        {"accel_profile", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"rotation", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 359); }},
        {"kb_file", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"kb_layout", []() -> ILuaConfigValue* { return new CLuaConfigString("us"); }},
        {"kb_variant", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"kb_options", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"kb_rules", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"kb_model", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"repeat_rate", []() -> ILuaConfigValue* { return new CLuaConfigInt(25, 0, 200); }},
        {"repeat_delay", []() -> ILuaConfigValue* { return new CLuaConfigInt(600, 0, 2000); }},
        {"natural_scroll", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"tap_button_map", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"numlock_by_default", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"resolve_binds_by_sym", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"disable_while_typing", []() -> ILuaConfigValue* { return new CLuaConfigBool(true); }},
        {"clickfinger_behavior", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"middle_button_emulation", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"tap_to_click", []() -> ILuaConfigValue* { return new CLuaConfigBool(true); }},
        {"tap_and_drag", []() -> ILuaConfigValue* { return new CLuaConfigBool(true); }},
        {"drag_lock", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 2); }},
        {"left_handed", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"scroll_method", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"scroll_button", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 300); }},
        {"scroll_button_lock", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"scroll_points", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"scroll_factor", []() -> ILuaConfigValue* { return new CLuaConfigFloat(1.F, 0.F, 100.F); }},
        {"transform", []() -> ILuaConfigValue* { return new CLuaConfigInt(-1); }},
        {"output", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
        {"enabled", []() -> ILuaConfigValue* { return new CLuaConfigBool(true); }},
        {"region_position", []() -> ILuaConfigValue* { return new CLuaConfigVec2({0, 0}); }},
        {"absolute_region_position", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"region_size", []() -> ILuaConfigValue* { return new CLuaConfigVec2({0, 0}); }},
        {"relative_input", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"active_area_position", []() -> ILuaConfigValue* { return new CLuaConfigVec2({0, 0}); }},
        {"active_area_size", []() -> ILuaConfigValue* { return new CLuaConfigVec2({0, 0}); }},
        {"flip_x", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"flip_y", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"drag_3fg", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 2); }},
        {"keybinds", []() -> ILuaConfigValue* { return new CLuaConfigBool(true); }},
        {"share_states", []() -> ILuaConfigValue* { return new CLuaConfigInt(0, 0, 2); }},
        {"release_pressed_on_close", []() -> ILuaConfigValue* { return new CLuaConfigBool(false); }},
        {"tags", []() -> ILuaConfigValue* { return new CLuaConfigString(STRVAL_EMPTY); }},
    };

}

static int hlCurve(lua_State* L) {
    CLuaConfigString nameParser("");
    lua_pushvalue(L, 1);
    auto nameErr = nameParser.parse(L);
    lua_pop(L, 1);
    if (nameErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.curve: first argument (name) must be a string: {}", nameErr.message));

    const auto& name = nameParser.parsed();

    if (!lua_istable(L, 2))
        return Internal::configError(L, "hl.curve: second argument must be a table, e.g. { type = \"bezier\", points = { {0, 0}, {1, 1} } }");

    CLuaConfigString typeParser("");
    auto             typeErr = Internal::parseTableField(L, 2, "type", typeParser);
    if (typeErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.curve(\"{}\"): {}", name, typeErr.message));

    const auto& curveType = typeParser.parsed();

    if (curveType == "bezier") {
        lua_getfield(L, 2, "points");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return Internal::configError(L, std::format("hl.curve(\"{}\"): missing or invalid \"points\" field, expected a table of two points", name));
        }
        int pointsIdx = lua_gettop(L);

        if (luaL_len(L, pointsIdx) != 2) {
            lua_pop(L, 1);
            return Internal::configError(L, std::format("hl.curve(\"{}\"): \"points\" must contain exactly 2 points, e.g. {{ {{0, 0}}, {{1, 1}} }}", name));
        }

        float coords[4] = {};
        for (int pt = 1; pt <= 2; pt++) {
            lua_rawgeti(L, pointsIdx, pt);
            if (!lua_istable(L, -1) || luaL_len(L, -1) != 2) {
                lua_pop(L, 2);
                return Internal::configError(L, std::format("hl.curve(\"{}\"): point {} must be a table of 2 numbers, e.g. {{0.25, 0.1}}", name, pt));
            }
            int ptIdx = lua_gettop(L);

            for (int comp = 0; comp < 2; comp++) {
                lua_rawgeti(L, ptIdx, comp + 1);
                CLuaConfigFloat coordParser(0.F, -1.F, 2.F);
                auto            coordErr = coordParser.parse(L);
                lua_pop(L, 1);
                if (coordErr.errorCode != PARSE_ERROR_OK) {
                    lua_pop(L, 2);
                    return Internal::configError(L, std::format("hl.curve(\"{}\"): point {}[{}]: {}", name, pt, comp + 1, coordErr.message));
                }
                coords[((pt - 1) * 2) + comp] = coordParser.parsed();
            }

            lua_pop(L, 1);
        }
        lua_pop(L, 1);

        Animation::mgr()->addBezierWithName(name, Vector2D(coords[0], coords[1]), Vector2D(coords[2], coords[3]));
    } else if (curveType == "spring") {

        Hyprutils::Animation::SSpringCurve curve;

        {
            CScopeGuard x([L] { lua_pop(L, 1); });

            lua_getfield(L, 2, "stiffness");

            if (!lua_isnumber(L, -1))
                return Internal::configError(L, std::format("hl.curve(\"{}\"): stiffness expects a number", name));

            curve.stiffness = lua_tonumber(L, -1);

            if (curve.stiffness <= 0.5F)
                return Internal::configError(L, std::format("hl.curve(\"{}\"): stiffness expects a number >= 0.5", name));
        }

        {
            CScopeGuard x([L] { lua_pop(L, 1); });

            lua_getfield(L, 2, "dampening");

            if (!lua_isnumber(L, -1))
                return Internal::configError(L, std::format("hl.curve(\"{}\"): dampening expects a number", name));

            curve.damping = lua_tonumber(L, -1);

            if (curve.damping <= 0.5F)
                return Internal::configError(L, std::format("hl.curve(\"{}\"): dampening expects a number >= 0.5", name));
        }

        {
            CScopeGuard x([L] { lua_pop(L, 1); });

            lua_getfield(L, 2, "mass");

            if (!lua_isnumber(L, -1))
                return Internal::configError(L, std::format("hl.curve(\"{}\"): mass expects a number", name));

            curve.mass = lua_tonumber(L, -1);

            if (curve.mass <= 0.5F)
                return Internal::configError(L, std::format("hl.curve(\"{}\"): mass expects a number >= 0.5", name));
        }

        Animation::mgr()->addSpringWithName(name, curve);
    } else
        return Internal::configError(L, std::format(R"(hl.curve("{}"): unknown curve type "{}", expected "bezier" or "spring")", name, curveType));

    return 0;
}

static int hlAnimation(lua_State* L) {
    if (!lua_istable(L, 1))
        return Internal::configError(L, R"(hl.animation: expected a table, e.g. { leaf = "global", enabled = true, speed = 5, bezier = "default" })");

    CLuaConfigString leafParser("");
    auto             leafErr = Internal::parseTableField(L, 1, "leaf", leafParser);
    if (leafErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.animation: {}", leafErr.message));

    const auto leaf = leafParser.parsed();

    if (!Config::animationTree()->nodeExists(leaf))
        return Internal::configError(L, std::format("hl.animation: no such animation leaf \"{}\"", leaf));

    CLuaConfigBool enabledParser(true);
    auto           enabledErr = Internal::parseTableField(L, 1, "enabled", enabledParser);
    if (enabledErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.animation(\"{}\"): {}", leaf, enabledErr.message));

    bool enabled = enabledParser.parsed();

    if (!enabled) {
        Config::animationTree()->setConfigForNode(leaf, false, 1, "default");
        return 0;
    }

    CLuaConfigFloat speedParser(0.F, 0.F, 100.F);
    auto            speedErr = Internal::parseTableField(L, 1, "speed", speedParser);
    if (speedErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.animation(\"{}\"): {}", leaf, speedErr.message));

    float speed = speedParser.parsed();

    if (speed <= 0)
        return Internal::configError(L, std::format("hl.animation(\"{}\"): speed must be greater than 0", leaf));

    std::string curveName;

    if (Internal::hasTableField(L, 1, "bezier")) {
        CLuaConfigString bezierParser("");
        auto             bezierErr = Internal::parseTableField(L, 1, "bezier", bezierParser);
        if (bezierErr.errorCode != PARSE_ERROR_OK)
            return Internal::configError(L, std::format("hl.animation(\"{}\"): {}", leaf, bezierErr.message));

        const auto& bezierName = bezierParser.parsed();

        if (!Animation::mgr()->bezierExists(bezierName))
            return Internal::configError(L, std::format(R"(hl.animation("{}"): no such bezier "{}")", leaf, bezierName));

        curveName = bezierName;
    } else if (Internal::hasTableField(L, 1, "spring")) {
        CLuaConfigString springParser("");
        auto             springErr = Internal::parseTableField(L, 1, "spring", springParser);
        if (springErr.errorCode != PARSE_ERROR_OK)
            return Internal::configError(L, std::format("hl.animation(\"{}\"): {}", leaf, springErr.message));

        const auto& springName = springParser.parsed();

        if (!Animation::mgr()->springExists(springName))
            return Internal::configError(L, std::format(R"(hl.animation("{}"): no such spring "{}")", leaf, springName));

        curveName = "spring:" + springName;
    } else
        return Internal::configError(L, std::format(R"(hl.animation("{}"): bezier or spring is required)", leaf));

    std::string style;
    lua_getfield(L, 1, "style");
    if (!lua_isnil(L, -1)) {
        CLuaConfigString styleParser("");
        auto             styleErr = styleParser.parse(L);
        if (styleErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return Internal::configError(L, std::format(R"(hl.animation("{}"): field "style": {})", leaf, styleErr.message));
        }
        style = styleParser.parsed();
    }
    lua_pop(L, 1);

    if (!style.empty()) {
        auto err = Animation::mgr()->styleValidInConfigVar(leaf, style);
        if (!err.empty())
            return Internal::configError(L, std::format("hl.animation(\"{}\"): {}", leaf, err));
    }

    Config::animationTree()->setConfigForNode(leaf, true, speed, curveName, style);
    return 0;
}

static int hlEnv(lua_State* L) {
    auto*            mgr = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    CLuaConfigString nameParser("");
    lua_pushvalue(L, 1);
    auto nameErr = nameParser.parse(L);
    lua_pop(L, 1);
    if (nameErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.env: first argument (name) must be a string: {}", nameErr.message));

    const auto& name = nameParser.parsed();

    if (name.empty())
        return Internal::configError(L, "hl.env: name must not be empty");

    CLuaConfigString valueParser("");
    lua_pushvalue(L, 2);
    auto valueErr = valueParser.parse(L);
    lua_pop(L, 1);
    if (valueErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.env: second argument (value) must be a string: {}", valueErr.message));

    const auto& value = valueParser.parsed();

    if (!mgr->isFirstLaunch()) {
        const auto* ENV = getenv(name.c_str());
        if (ENV && ENV == value)
            return 0;
    }

    setenv(name.c_str(), value.c_str(), 1);

    bool dbus = false;
    if (!lua_isnoneornil(L, 3)) {
        CLuaConfigBool dbusParser(false);
        lua_pushvalue(L, 3);
        auto dbusErr = dbusParser.parse(L);
        lua_pop(L, 1);
        if (dbusErr.errorCode != PARSE_ERROR_OK)
            return Internal::configError(L, std::format("hl.env: third argument (dbus) must be a boolean: {}", dbusErr.message));

        dbus = dbusParser.parsed();
    }

    if (dbus) {
        std::string CMD;
#ifdef USES_SYSTEMD
        CMD = "systemctl --user import-environment '" + name + "' && hash dbus-update-activation-environment 2>/dev/null && ";
#endif
        CMD += "dbus-update-activation-environment --systemd '" + name + "'";
        if (mgr->isFirstLaunch())
            Config::Supplementary::executor()->addExecOnce({CMD, false});
        else
            Config::Supplementary::executor()->spawnRaw(CMD);
    }

    return 0;
}

static int hlPluginLoad(lua_State* L) {
    auto*            mgr = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    CLuaConfigString pathParser("");
    lua_pushvalue(L, 1);
    auto pathErr = pathParser.parse(L);
    lua_pop(L, 1);
    if (pathErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.plugin.load: first argument (path) must be a string: {}", pathErr.message));

    const auto& path = pathParser.parsed();

    if (path.empty())
        return Internal::configError(L, "hl.plugin.load: path must not be empty");

    mgr->m_registeredPlugins.emplace_back(path);
    return 0;
}

static int hlPermission(lua_State* L) {
    auto*       mgr = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    std::string binary;
    std::string typeStr;
    std::string modeStr;

    if (lua_istable(L, 1)) {
        auto b = Internal::tableOptStr(L, 1, "binary");
        if (!b)
            b = Internal::tableOptStr(L, 1, "target");
        auto t = Internal::tableOptStr(L, 1, "type");
        auto m = Internal::tableOptStr(L, 1, "mode");

        if (!b || !t || !m)
            return Internal::configError(L, "hl.permission: expected { binary, type, mode }");

        binary  = *b;
        typeStr = *t;
        modeStr = *m;
    } else {
        auto b = Check::string(L, 1);
        auto t = Check::string(L, 2);
        auto m = Check::string(L, 3);

        if (!b || !t || !m)
            return Internal::configError(L, "hl.permission: expected binary, type, mode");

        binary  = *b;
        typeStr = *t;
        modeStr = *m;
    }

    if (binary.empty())
        return Internal::configError(L, "hl.permission: binary must not be empty");

    eDynamicPermissionType      type = PERMISSION_TYPE_UNKNOWN;
    eDynamicPermissionAllowMode mode = PERMISSION_RULE_ALLOW_MODE_UNKNOWN;

    if (typeStr == "screencopy")
        type = PERMISSION_TYPE_SCREENCOPY;
    else if (typeStr == "cursorpos")
        type = PERMISSION_TYPE_CURSOR_POS;
    else if (typeStr == "plugin")
        type = PERMISSION_TYPE_PLUGIN;
    else if (typeStr == "keyboard" || typeStr == "keeb")
        type = PERMISSION_TYPE_KEYBOARD;
    else if (typeStr == "input-capture")
        type = PERMISSION_TYPE_INPUT_CAPTURE;

    if (modeStr == "ask")
        mode = PERMISSION_RULE_ALLOW_MODE_ASK;
    else if (modeStr == "allow")
        mode = PERMISSION_RULE_ALLOW_MODE_ALLOW;
    else if (modeStr == "deny")
        mode = PERMISSION_RULE_ALLOW_MODE_DENY;

    if (type == PERMISSION_TYPE_UNKNOWN)
        return Internal::configError(L, "hl.permission: unknown permission type");
    if (mode == PERMISSION_RULE_ALLOW_MODE_UNKNOWN)
        return Internal::configError(L, "hl.permission: unknown permission allow mode");

    if (mgr->isFirstLaunch() && g_pDynamicPermissionManager)
        g_pDynamicPermissionManager->addConfigPermissionRule(binary, type, mode);

    return 0;
}

static int hlWorkspaceRule(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (!lua_istable(L, 1)) {
        self->addError("hl.workspace_rule: argument must be a table");
        return 0;
    }

    const std::string sourceInfo = Internal::getSourceInfo(L);

    lua_getfield(L, 1, "workspace");
    if (!lua_isstring(L, -1)) {
        self->addError(std::format("{}: hl.workspace_rule: 'workspace' field is required and must be a string", sourceInfo));
        lua_pop(L, 1);
        return 0;
    }
    const std::string wsStr = lua_tostring(L, -1);
    lua_pop(L, 1);

    bool enabled = true;
    lua_getfield(L, 1, "enabled");
    if (lua_isboolean(L, -1))
        enabled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    const auto& [wsId, wsName, isAutoID] = getWorkspaceIDNameFromString(wsStr);

    Config::CWorkspaceRule wsRule;
    wsRule.m_workspaceString = wsStr;
    wsRule.m_workspaceName   = wsName;
    wsRule.m_workspaceId     = isAutoID ? WORKSPACE_INVALID : wsId;
    wsRule.setEnabled(enabled);

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }

        std::string_view key = lua_tostring(L, -2);
        if (key == "workspace" || key == "enabled") {
            lua_pop(L, 1);
            continue;
        }

        if (key == "layout_opts") {
            if (!lua_istable(L, -1)) {
                self->addError(std::format("{}: hl.workspace_rule: field 'layout_opts' must be a table", sourceInfo));
                lua_pop(L, 1);
                continue;
            }

            const int optsIdx = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, optsIdx) != 0) {
                if (lua_type(L, -2) != LUA_TSTRING) {
                    self->addError(std::format("{}: hl.workspace_rule: field 'layout_opts' keys must be strings", sourceInfo));
                    lua_pop(L, 1);
                    continue;
                }

                std::string optKey = lua_tostring(L, -2);
                std::string optVal;

                if (lua_type(L, -1) == LUA_TBOOLEAN)
                    optVal = lua_toboolean(L, -1) ? "true" : "false";
                else if (lua_type(L, -1) == LUA_TNUMBER) {
                    if (lua_isinteger(L, -1))
                        optVal = std::to_string(lua_tointeger(L, -1));
                    else
                        optVal = std::to_string(lua_tonumber(L, -1));
                } else if (lua_isstring(L, -1))
                    optVal = lua_tostring(L, -1);
                else {
                    self->addError(std::format("{}: hl.workspace_rule: field 'layout_opts.{}' must be string, bool, or number", sourceInfo, optKey));
                    lua_pop(L, 1);
                    continue;
                }

                wsRule.m_layoutopts[std::move(optKey)] = std::move(optVal);
                lua_pop(L, 1);
            }

            lua_pop(L, 1);
            continue;
        }

        const auto* desc = Internal::findDescByName(WORKSPACE_RULE_FIELDS, key);
        if (!desc) {
            self->addError(std::format("{}: hl.workspace_rule: unknown field '{}'", sourceInfo, key));
            lua_pop(L, 1);
            continue;
        }

        auto val = UP<ILuaConfigValue>(desc->factory());
        auto err = val->parse(L);
        if (err.errorCode != PARSE_ERROR_OK)
            self->addError(std::format("{}: hl.workspace_rule: field '{}': {}", sourceInfo, key, err.message));
        else
            desc->apply(val.get(), wsRule);

        lua_pop(L, 1);
    }

    const auto RULE = Config::workspaceRuleMgr()->replaceOrAdd(std::move(wsRule));

    Supplementary::refresher()->scheduleRefresh(Supplementary::REFRESH_MONITOR_STATES | Config::Supplementary::REFRESH_WINDOW_STATES);

    Objects::CLuaWorkspaceRule::push(L, RULE);
    return 1;
}

static int hlGesture(lua_State* L) {
    if (!lua_istable(L, 1))
        return Internal::configError(L, R"(hl.gesture: expected a table, e.g. { fingers = 3, direction = "horizontal", action = "workspace" })");

    CLuaConfigInt fingersParser(0, 2, 9);
    auto          fingersErr = Internal::parseTableField(L, 1, "fingers", fingersParser);
    if (fingersErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.gesture: {}", fingersErr.message));

    size_t           fingerCount = fingersParser.parsed();

    CLuaConfigString dirParser("");
    auto             dirErr = Internal::parseTableField(L, 1, "direction", dirParser);
    if (dirErr.errorCode != PARSE_ERROR_OK)
        return Internal::configError(L, std::format("hl.gesture: {}", dirErr.message));

    const auto direction = g_pTrackpadGestures->dirForString(dirParser.parsed());
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return Internal::configError(L, std::format("hl.gesture: invalid direction \"{}\"", dirParser.parsed()));

    struct SLuaGestureRefGuard {
        lua_State*       L = nullptr;
        std::vector<int> refs;
        bool             disarm = false;

        ~SLuaGestureRefGuard() {
            if (disarm)
                return;

            for (const auto ref : refs) {
                if (ref != LUA_NOREF && ref != LUA_REFNIL)
                    luaL_unref(L, LUA_REGISTRYINDEX, ref);
            }
        }

        void add(int ref) {
            if (ref != LUA_NOREF && ref != LUA_REFNIL)
                refs.emplace_back(ref);
        }

        bool hasRefs() const {
            return !refs.empty();
        }

        void registerWithManager() {
            const auto mgr = Lua::mgr();
            if (!mgr)
                return;

            for (const auto ref : refs) {
                mgr->registerLuaRef(ref);
            }

            disarm = true;
        }
    } luaGestureRefs{.L = L};

    int functionRef = LUA_NOREF;
    int startRef    = LUA_NOREF;
    int updateRef   = LUA_NOREF;
    int endRef      = LUA_NOREF;

    {
        // check if the action arg is a lua fn, that's fine
        // we can ref that fucker and call him later
        lua_getfield(L, 1, "action");

        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -1);
            functionRef = luaL_ref(L, LUA_REGISTRYINDEX);
            luaGestureRefs.add(functionRef);
        } else if (lua_istable(L, -1)) {
            const int actionIdx = lua_gettop(L);

            auto      readCallback = [&](const char* name) -> std::expected<int, std::string> {
                lua_getfield(L, actionIdx, name);

                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    return LUA_NOREF;
                }

                if (!lua_isfunction(L, -1)) {
                    lua_pop(L, 1);
                    return std::unexpected(std::format("action.{} must be a function", name));
                }

                const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
                luaGestureRefs.add(ref);
                return ref;
            };

            auto startResult = readCallback("start");
            if (!startResult) {
                lua_pop(L, 1);
                return Internal::configError(L, std::format("hl.gesture: {}", startResult.error()));
            }
            startRef = *startResult;

            auto updateResult = readCallback("update");
            if (!updateResult) {
                lua_pop(L, 1);
                return Internal::configError(L, std::format("hl.gesture: {}", updateResult.error()));
            }
            updateRef = *updateResult;

            auto endResult = readCallback("finish");
            if (!endResult) {
                lua_pop(L, 1);
                return Internal::configError(L, std::format("hl.gesture: {}", endResult.error()));
            }
            endRef = *endResult;

            if (startRef == LUA_NOREF && updateRef == LUA_NOREF && endRef == LUA_NOREF) {
                lua_pop(L, 1);
                return Internal::configError(L, "hl.gesture: action callback table must define at least one of start, update, end, or finish");
            }
        }

        lua_pop(L, 1);
    }

    // bitch ass macro because it's kinda long to get these things and it's ugly
#define GET_ACTION_STRING(var, name)                                                                                                                                               \
    std::string var;                                                                                                                                                               \
    lua_getfield(L, 1, name);                                                                                                                                                      \
    if (!lua_isnil(L, -1)) {                                                                                                                                                       \
        CLuaConfigString argParser("");                                                                                                                                            \
        auto             argErr = argParser.parse(L);                                                                                                                              \
        if (argErr.errorCode != PARSE_ERROR_OK) {                                                                                                                                  \
            lua_pop(L, 1);                                                                                                                                                         \
            return Internal::configError(L, std::format("hl.gesture: field \"" name "\": {}", argErr.message));                                                                    \
        }                                                                                                                                                                          \
        var = argParser.parsed();                                                                                                                                                  \
    }                                                                                                                                                                              \
    lua_pop(L, 1);

    GET_ACTION_STRING(zoomLevel, "zoom_level");
    GET_ACTION_STRING(workspaceName, "workspace_name");
    GET_ACTION_STRING(mode, "mode");

#undef GET_ACTION_STRING

    uint32_t modMask = 0;
    lua_getfield(L, 1, "mods");
    if (!lua_isnil(L, -1)) {
        CLuaConfigString modsParser("");
        auto             modsErr = modsParser.parse(L);
        if (modsErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return Internal::configError(L, std::format("hl.gesture: field \"mods\": {}", modsErr.message));
        }
        modMask = g_pKeybindManager->stringToModMask(modsParser.parsed());
    }
    lua_pop(L, 1);

    float deltaScale = 1.F;
    lua_getfield(L, 1, "scale");
    if (!lua_isnil(L, -1)) {
        CLuaConfigFloat scaleParser(1.F, 0.1F, 10.F);
        auto            scaleErr = scaleParser.parse(L);
        if (scaleErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return Internal::configError(L, std::format("hl.gesture: field \"scale\": {}", scaleErr.message));
        }
        deltaScale = scaleParser.parsed();
    }
    lua_pop(L, 1);

    bool disableInhibit = false;
    lua_getfield(L, 1, "disable_inhibit");
    if (!lua_isnil(L, -1)) {
        CLuaConfigBool disableInhibitParser(false);
        auto           disableInhibitErr = disableInhibitParser.parse(L);
        if (disableInhibitErr.errorCode != PARSE_ERROR_OK) {
            lua_pop(L, 1);
            return Internal::configError(L, std::format("hl.gesture: field \"disable_inhibit\": {}", disableInhibitErr.message));
        }
        disableInhibit = disableInhibitParser.parsed();
    }
    lua_pop(L, 1);

    std::expected<void, std::string> result;

    if (luaGestureRefs.hasRefs() && !Lua::mgr())
        return Internal::configError(L, "hl.gesture: internal error: lua callback manager unavailable");

    if (functionRef != LUA_NOREF) {
        // this is a lua fn gesture
        result = g_pTrackpadGestures->addGesture(makeUnique<CLuaFunctionGesture>(functionRef), fingerCount, direction, modMask, deltaScale, disableInhibit);
    } else if (startRef != LUA_NOREF || updateRef != LUA_NOREF || endRef != LUA_NOREF) {
        result = g_pTrackpadGestures->addGesture(makeUnique<CLuaFunctionGesture>(startRef, updateRef, endRef), fingerCount, direction, modMask, deltaScale, disableInhibit);
    } else {
        CLuaConfigString actionParser("");
        auto             actionErr = Internal::parseTableField(L, 1, "action", actionParser);
        if (actionErr.errorCode != PARSE_ERROR_OK)
            return Internal::configError(L, std::format("hl.gesture: {}", actionErr.message));

        const auto& action = actionParser.parsed();

        if (action == "workspace")
            result = g_pTrackpadGestures->addGesture(makeUnique<CWorkspaceSwipeGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "resize")
            result = g_pTrackpadGestures->addGesture(makeUnique<CResizeTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "move")
            result = g_pTrackpadGestures->addGesture(makeUnique<CMoveTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "special")
            result = g_pTrackpadGestures->addGesture(makeUnique<CSpecialWorkspaceGesture>(workspaceName), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "close")
            result = g_pTrackpadGestures->addGesture(makeUnique<CCloseTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "float")
            result = g_pTrackpadGestures->addGesture(makeUnique<CFloatTrackpadGesture>(mode), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "fullscreen")
            result = g_pTrackpadGestures->addGesture(makeUnique<CFullscreenTrackpadGesture>(mode), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "cursor_zoom" || action == "cursorZoom")
            result = g_pTrackpadGestures->addGesture(makeUnique<CCursorZoomTrackpadGesture>(zoomLevel, mode), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "scroll_move")
            result = g_pTrackpadGestures->addGesture(makeUnique<CScrollMoveTrackpadGesture>(), fingerCount, direction, modMask, deltaScale, disableInhibit);
        else if (action == "unset")
            result = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
        else
            return Internal::configError(L, std::format("hl.gesture: unknown action \"{}\"", action));
    }

    if (!result)
        return Internal::configError(L, std::format("hl.gesture: {}", result.error()));

    luaGestureRefs.registerWithManager();

    return 0;
}

static int hlConfig(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (!lua_istable(L, 1)) {
        self->addError("hl.config: argument must be a table");
        return 0;
    }

    const std::string                            sourceInfo = Internal::getSourceInfo(L);

    std::function<void(const std::string&, int)> walk = [&](const std::string& prefix, int tableIdx) {
        lua_pushnil(L);
        while (lua_next(L, tableIdx) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 1);
                continue;
            }

            const std::string key = lua_tostring(L, -2);
            std::string       fullKey;
            if (!prefix.empty()) {
                fullKey.reserve(prefix.size() + 1 + key.size());
                fullKey = prefix;
                fullKey += '.';
            }
            fullKey += key;

            auto it = self->m_configValues.find(fullKey);

            if (it == self->m_configValues.end() && lua_istable(L, -1))
                walk(fullKey, lua_gettop(L));
            else {
                if (it == self->m_configValues.end())
                    self->addError(std::format("{}: unknown config key '{}'", sourceInfo, fullKey));
                else {
                    const auto err = it->second->parse(L);
                    if (err.errorCode != PARSE_ERROR_OK)
                        self->addError(std::format("{}: error setting '{}': {}", sourceInfo, it->first, err.message));
                    else if (self->isDynamicParse())
                        Supplementary::refresher()->scheduleRefresh(it->second->refreshBits());
                }
            }

            lua_pop(L, 1);
        }
    };

    walk("", 1);
    return 0;
}

static int hlGetConfig(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    auto  arg = Check::string(L, 1);
    if (!arg)
        return Internal::configError(L, std::format("hl.get_config: bad type for arg 1, {}", arg.error()));

    std::string key = *arg;

    auto        it = self->m_configValues.find(key);
    if (it == self->m_configValues.end()) {
        std::ranges::replace(key, ':', '.');
        it = self->m_configValues.find(key);
    }

    if (it == self->m_configValues.end()) {
        lua_pushnil(L);
        const auto msg = std::format("unknown config key '{}'", key);
        lua_pushstring(L, msg.c_str());
        return 2;
    }

    it->second->push(L);
    return 1;
}

static int hlDevice(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (!lua_istable(L, 1)) {
        self->addError("hl.device: argument must be a table");
        return 0;
    }

    const std::string sourceInfo = Internal::getSourceInfo(L);

    lua_getfield(L, 1, "name");
    if (!lua_isstring(L, -1)) {
        self->addError(std::format("{}: hl.device: 'name' field is required and must be a string", sourceInfo));
        lua_pop(L, 1);
        return 0;
    }
    std::string devName = lua_tostring(L, -1);
    lua_pop(L, 1);
    std::ranges::replace(devName, ' ', '-');

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }

        const char* key = lua_tostring(L, -2);

        if (std::string_view{key} == "name") {
            lua_pop(L, 1);
            continue;
        }

        const auto* desc = Internal::findDescByName(DEVICE_FIELDS, key);

        if (!desc) {
            self->addError(std::format("{}: hl.device: unknown field '{}'", sourceInfo, key));
            lua_pop(L, 1);
            continue;
        }

        auto val = UP<ILuaConfigValue>(desc->factory());
        auto err = val->parse(L);
        if (err.errorCode != PARSE_ERROR_OK)
            self->addError(std::format("{}: hl.device: field '{}': {}", sourceInfo, key, err.message));
        else
            self->m_deviceConfigs[devName].values.insert_or_assign(key, std::move(val));

        lua_pop(L, 1);
    }

    Supplementary::refresher()->scheduleRefresh(Supplementary::REFRESH_INPUT_DEVICES);

    return 0;
}

static int hlMonitor(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (!lua_istable(L, 1)) {
        self->addError("hl.monitor: argument must be a table");
        return 0;
    }

    const std::string sourceInfo = Internal::getSourceInfo(L);

    lua_getfield(L, 1, "output");
    if (!lua_isstring(L, -1)) {
        self->addError(std::format("{}: hl.monitor: 'output' field is required and must be a string", sourceInfo));
        lua_pop(L, 1);
        return 0;
    }
    const std::string output = lua_tostring(L, -1);
    lua_pop(L, 1);

    CMonitorRuleParser parser(output);

    const auto         existing = std::ranges::find_if(Config::monitorRuleMgr()->all(), [&output](const auto& rule) { return rule.m_name == output; });
    if (existing != Config::monitorRuleMgr()->all().end())
        parser.rule() = *existing;

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }

        const char* key = lua_tostring(L, -2);

        if (std::string_view{key} == "output") {
            lua_pop(L, 1);
            continue;
        }

        const auto* desc = Internal::findDescByName(MONITOR_FIELDS, key);

        if (!desc) {
            self->addError(std::format("{}: hl.monitor: unknown field '{}'", sourceInfo, key));
            lua_pop(L, 1);
            continue;
        }

        auto val = UP<ILuaConfigValue>(desc->factory());
        auto err = val->parse(L);
        if (err.errorCode != PARSE_ERROR_OK)
            self->addError(std::format("{}: hl.monitor: field '{}': {}", sourceInfo, key, err.message));
        else if (!desc->apply(val.get(), parser))
            self->addError(std::format("{}: hl.monitor: error applying field '{}'", sourceInfo, key));

        lua_pop(L, 1);
    }

    Config::monitorRuleMgr()->add(std::move(parser.rule()));

    Supplementary::refresher()->scheduleRefresh(Supplementary::REFRESH_MONITOR_STATES);

    return 0;
}

// ---- XR keywords, as Lua (docs/openxr/05 §3 and §3.5) ----
//
// The `xrmonitor` and `xrrule` keywords predate this fork's Lua front end and were reachable only
// from a `.conf`, which is why the XR session was the last thing that could not be written in Lua.
// These two bindings close that, WITHOUT growing a second grammar: each reads named fields, emits
// the shared parser's own tokens, and lets that parser do every bit of validation. The parsed
// result then lands in Config::xrDeclarationMgr(), the same store the classic keyword fills — so
// the two front ends produce identical state by construction, not by agreement.

namespace {
    // Format a Lua number for a token. Shortest round-trip, so a pose pasted out of
    // `hyprctl openxr layout` survives a trip through a Lua config unchanged.
    std::string xrNum(double v) {
        return std::format("{}", v);
    }

    // One field -> the string that follows its `key:`. Leaves the stack as it found it.
    std::expected<std::string, std::string> xrTokenValue(lua_State* L, const SXRFieldDesc& desc) {
        if (desc.kind == XR_TOK_VEC3) {
            // "x,y,z" verbatim, or {x, y, z}. The table form is the reason to bother: a pose is
            // three numbers, and making the user paste them into a string is a papercut.
            if (lua_istable(L, -1)) {
                std::string out;
                for (int i = 1; i <= 3; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (!lua_isnumber(L, -1)) {
                        lua_pop(L, 1);
                        return std::unexpected(std::format("expected 3 numbers, element {} is not a number", i));
                    }
                    if (i > 1)
                        out += ',';
                    out += xrNum(lua_tonumber(L, -1));
                    lua_pop(L, 1);
                }
                lua_rawgeti(L, -1, 4);
                const bool tooLong = !lua_isnil(L, -1);
                lua_pop(L, 1);
                if (tooLong)
                    return std::unexpected("expected exactly 3 numbers");
                return out;
            }
            if (lua_isstring(L, -1) && !lua_isnumber(L, -1))
                return std::string(lua_tostring(L, -1));
            return std::unexpected("expected \"x,y,z\" or { x, y, z }");
        }

        if (desc.kind == XR_TOK_NUM_OR_STR) {
            if (lua_isnumber(L, -1))
                return xrNum(lua_tonumber(L, -1));
            if (lua_isstring(L, -1))
                return std::string(lua_tostring(L, -1));
            return std::unexpected("expected a number or a string");
        }

        // Everything else goes through the same ILuaConfigValue machinery the other rule bindings
        // use, so type errors read the same as they do for hl.monitor / hl.window_rule.
        auto       val = UP<ILuaConfigValue>(desc.factory());
        const auto err = val->parse(L);
        if (err.errorCode != PARSE_ERROR_OK)
            return std::unexpected(err.message);

        switch (desc.kind) {
            case XR_TOK_FLOAT: return xrNum(*sc<const Config::FLOAT*>(val->data()));
            // The anchor grammar's flags are on/off; `fullscreen:` takes 0/1 and also accepts these.
            case XR_TOK_BOOL: return *sc<const Config::BOOL*>(val->data()) ? std::string("on") : std::string("off");
            default: return *sc<const Config::STRING*>(val->data());
        }
    }

    // A condition value may contain spaces (a title regex). The shared tokenizer un-quotes, so
    // quote here; a value that already contains a quote cannot be expressed either way.
    std::expected<std::string, std::string> xrQuoteIfNeeded(const std::string& v) {
        if (v.contains('"'))
            return std::unexpected("value may not contain a double quote");
        if (v.find_first_of(" \t") == std::string::npos)
            return v;
        return "\"" + v + "\"";
    }

    // A dynamic `hyprctl keyword xrmonitor` reconciles explicitly because it fires no reload event;
    // under Lua the same hole is `hyprctl eval`, and isDynamicParse() is how this front end names
    // it. A full reload must NOT reconcile per line — it reconciles once, from onConfigReload().
    void xrScheduleReconcile(CConfigManager* self, void (COpenXRManager::*fn)()) {
#ifdef HAVE_OPENXR
        if (!self->isDynamicParse() || !g_pEventLoopManager)
            return;
        g_pEventLoopManager->doLater([fn] {
            if (g_pOpenXRManager)
                (g_pOpenXRManager.get()->*fn)();
        });
#else
        (void)self;
        (void)fn;
#endif
    }
}

static int hlXRMonitor(lua_State* L) {
    auto*             self       = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));
    const std::string sourceInfo = Internal::getSourceInfo(L);

    SXRMonitorParams  params;

    // String form: the value half of a classic `xrmonitor =` line, verbatim. Not a shortcut — it is
    // the paste target for `hyprctl openxr layout`, which prints paste-ready keyword lines so you
    // can arrange the desktop in-headset and persist the arrangement (doc 05 §5).
    if (lua_isstring(L, 1) && !lua_isnumber(L, 1)) {
        auto parsed = OpenXR::parseXRMonitorLine(lua_tostring(L, 1));
        if (!parsed) {
            self->addError(std::format("{}: hl.xr_monitor: {}", sourceInfo, parsed.error()));
            return 0;
        }
        params = std::move(*parsed);
    } else {
        if (!lua_istable(L, 1)) {
            self->addError(std::format("{}: hl.xr_monitor: argument must be a table, or a classic `xrmonitor =` value string", sourceInfo));
            return 0;
        }

        lua_getfield(L, 1, "name");
        if (!lua_isstring(L, -1)) {
            self->addError(std::format("{}: hl.xr_monitor: 'name' field is required and must be a string", sourceInfo));
            lua_pop(L, 1);
            return 0;
        }
        params.m_name = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 1, "mode");
        if (!lua_isnil(L, -1)) {
            if (!lua_isstring(L, -1)) {
                self->addError(std::format("{}: hl.xr_monitor: 'mode' must be a string like \"2560x1440@90\"", sourceInfo));
                lua_pop(L, 1);
                return 0;
            }
            if (auto r = OpenXR::parseXRMonitorMode(lua_tostring(L, -1), params); !r) {
                self->addError(std::format("{}: hl.xr_monitor: {}", sourceInfo, r.error()));
                lua_pop(L, 1);
                return 0;
            }
        }
        lua_pop(L, 1);

        lua_getfield(L, 1, "anchor");
        if (!lua_isstring(L, -1)) {
            self->addError(std::format("{}: hl.xr_monitor: 'anchor' field is required (local|head|body|device:left|device:right)", sourceInfo));
            lua_pop(L, 1);
            return 0;
        }
        std::string anchorMode = lua_tostring(L, -1);
        lua_pop(L, 1);
        // Both spellings: the bare mode, and the grammar's own `anchor:local` token, so a line
        // copied out of a .conf keeps working when it is pasted into a table.
        if (anchorMode.starts_with("anchor:"))
            anchorMode = anchorMode.substr(std::string_view("anchor:").size());

        // Collect by descriptor index so tokens come out in a fixed order regardless of Lua's
        // table iteration order — an error message that moves between reloads is a bad error
        // message.
        std::vector<std::optional<std::string>> values(std::size(XR_MONITOR_FIELDS));
        bool                                    failed = false;

        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 1);
                continue;
            }

            const std::string_view key = lua_tostring(L, -2);
            if (key == "name" || key == "mode" || key == "anchor") {
                lua_pop(L, 1);
                continue;
            }

            const auto* desc = Internal::findDescByName(XR_MONITOR_FIELDS, key);
            if (!desc) {
                self->addError(std::format("{}: hl.xr_monitor: unknown field '{}'", sourceInfo, key));
                failed = true;
                lua_pop(L, 1);
                continue;
            }

            auto val = xrTokenValue(L, *desc);
            if (!val) {
                self->addError(std::format("{}: hl.xr_monitor: field '{}': {}", sourceInfo, key, val.error()));
                failed = true;
            } else
                values[desc - XR_MONITOR_FIELDS] = std::format("{}:{}", desc->token, *val);

            lua_pop(L, 1);
        }

        if (failed)
            return 0;

        std::vector<std::string> tokens{"anchor:" + anchorMode};
        for (auto& v : values) {
            if (v)
                tokens.emplace_back(std::move(*v));
        }

        if (auto r = OpenXR::parseXRAnchorSpec(tokens, params); !r) {
            self->addError(std::format("{}: hl.xr_monitor: {}", sourceInfo, r.error()));
            return 0;
        }
    }

    Config::xrDeclarationMgr()->addMonitor(std::move(params));
    xrScheduleReconcile(self, &COpenXRManager::reconcileDeclaredMonitors);
    return 0;
}

static int hlXRRule(lua_State* L) {
    auto*             self       = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));
    const std::string sourceInfo = Internal::getSourceInfo(L);

    std::string       line;

    // String form: the value half of a classic `xrrule =` line, for the same reason hl.xr_monitor
    // takes one — a rule copied out of a .conf or the docs should paste in unedited.
    if (lua_isstring(L, 1) && !lua_isnumber(L, 1))
        line = lua_tostring(L, 1);
    else {
        if (!lua_istable(L, 1)) {
            self->addError(std::format("{}: hl.xr_rule: argument must be a table, or a classic `xrrule =` value string", sourceInfo));
            return 0;
        }

        std::vector<std::optional<std::string>> effects(std::size(XR_RULE_EFFECT_FIELDS));
        std::vector<std::optional<std::string>> conds(std::size(XR_RULE_MATCH_FIELDS));
        bool                                    failed = false;

        lua_getfield(L, 1, "match");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                self->addError(std::format("{}: hl.xr_rule: 'match' must be a table", sourceInfo));
                lua_pop(L, 1);
                return 0;
            }

            const int matchIdx = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, matchIdx) != 0) {
                if (lua_type(L, -2) != LUA_TSTRING) {
                    lua_pop(L, 1);
                    continue;
                }

                const std::string_view key  = lua_tostring(L, -2);
                const auto*            desc = Internal::findDescByName(XR_RULE_MATCH_FIELDS, key);
                if (!desc) {
                    self->addError(std::format("{}: hl.xr_rule: unknown match condition '{}' (valid: monitor, anchorstate, focusclass, focustitle, fullscreen)", sourceInfo, key));
                    failed = true;
                    lua_pop(L, 1);
                    continue;
                }

                auto val = xrTokenValue(L, *desc);
                if (!val) {
                    self->addError(std::format("{}: hl.xr_rule: match '{}': {}", sourceInfo, key, val.error()));
                    failed = true;
                    lua_pop(L, 1);
                    continue;
                }

                auto quoted = xrQuoteIfNeeded(*val);
                if (!quoted) {
                    self->addError(std::format("{}: hl.xr_rule: match '{}': {}", sourceInfo, key, quoted.error()));
                    failed = true;
                } else
                    conds[desc - XR_RULE_MATCH_FIELDS] = std::format("{}:{}", desc->token, *quoted);

                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);

        lua_pushnil(L);
        while (lua_next(L, 1) != 0) {
            if (lua_type(L, -2) != LUA_TSTRING) {
                lua_pop(L, 1);
                continue;
            }

            const std::string_view key = lua_tostring(L, -2);
            if (key == "match") {
                lua_pop(L, 1);
                continue;
            }

            const auto* desc = Internal::findDescByName(XR_RULE_EFFECT_FIELDS, key);
            if (!desc) {
                self->addError(std::format("{}: hl.xr_rule: unknown effect '{}' (valid: alpha, blackalpha, blackalpha_knee)", sourceInfo, key));
                failed = true;
                lua_pop(L, 1);
                continue;
            }

            auto val = xrTokenValue(L, *desc);
            if (!val) {
                self->addError(std::format("{}: hl.xr_rule: effect '{}': {}", sourceInfo, key, val.error()));
                failed = true;
            } else
                effects[desc - XR_RULE_EFFECT_FIELDS] = std::format("{} {}", desc->token, *val);

            lua_pop(L, 1);
        }

        if (failed)
            return 0;

        // Re-emit the canonical keyword line. That is not a detour: SXRRule carries `raw`, which
        // `hyprctl openxr rules` prints back at you, so a Lua rule has to be able to name itself in
        // the same language a .conf rule does.
        for (auto& e : effects) {
            if (!e)
                continue;
            if (!line.empty())
                line += ' ';
            line += *e;
        }

        std::string condStr;
        for (auto& c : conds) {
            if (!c)
                continue;
            if (!condStr.empty())
                condStr += ' ';
            condStr += *c;
        }
        if (!condStr.empty())
            line += ", " + condStr;
    }

    auto parsed = OpenXR::parseXRRuleLine(line);
    if (!parsed) {
        self->addError(std::format("{}: hl.xr_rule: {}", sourceInfo, parsed.error()));
        return 0;
    }

    Config::xrDeclarationMgr()->addRule(std::move(*parsed));
    xrScheduleReconcile(self, &COpenXRManager::reloadXRRules);
    return 0;
}

static int hlWindowRule(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (!lua_istable(L, 1)) {
        self->addError("hl.window_rule: argument must be a table");
        return 0;
    }

    const std::string sourceInfo = Internal::getSourceInfo(L);

    std::string       name;
    lua_getfield(L, 1, "name");
    if (lua_isstring(L, -1))
        name = lua_tostring(L, -1);
    lua_pop(L, 1);

    bool enabled = true;
    lua_getfield(L, 1, "enabled");
    if (lua_isboolean(L, -1))
        enabled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    SP<Desktop::Rule::CWindowRule> rule;
    if (!name.empty() && self->m_luaWindowRules.contains(name)) {
        rule = self->m_luaWindowRules[name];
    } else {
        rule = makeShared<Desktop::Rule::CWindowRule>(name);
        if (!name.empty())
            self->m_luaWindowRules[name] = rule;
        Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>(rule));
    }
    rule->setEnabled(enabled);

    lua_getfield(L, 1, "match");
    if (lua_istable(L, -1)) {
        int matchIdx = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, matchIdx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::string matchKey = lua_tostring(L, -2);
                std::string matchVal;
                if (lua_type(L, -1) == LUA_TBOOLEAN)
                    matchVal = lua_toboolean(L, -1) ? "true" : "false";
                else if (lua_type(L, -1) == LUA_TNUMBER)
                    matchVal = std::to_string(lua_tointeger(L, -1));
                else if (lua_isstring(L, -1))
                    matchVal = lua_tostring(L, -1);
                else {
                    self->addError(std::format("{}: hl.window_rule: match value for '{}' must be string, bool, or number", sourceInfo, matchKey));
                    lua_pop(L, 1);
                    continue;
                }
                auto prop = Desktop::Rule::matchPropFromString(matchKey);
                if (prop.has_value())
                    rule->registerMatch(*prop, matchVal);
                else
                    self->addError(std::format("{}: hl.window_rule: unknown match property '{}'", sourceInfo, matchKey));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }
        std::string_view key = lua_tostring(L, -2);
        if (key == "name" || key == "enabled" || key == "match") {
            lua_pop(L, 1);
            continue;
        }

        const auto* desc = Internal::findDescByName(Internal::WINDOW_RULE_EFFECT_DESCS, key);
        if (!desc) {
            const auto dynamicEffect = Desktop::Rule::windowEffects()->get(key);
            if (!dynamicEffect.has_value()) {
                self->addError(std::format("{}: hl.window_rule: unknown field '{}'", sourceInfo, key));
                lua_pop(L, 1);
                continue;
            }

            auto val = Internal::ruleValueToString(L);
            if (!val)
                self->addError(std::format("{}: hl.window_rule: field '{}': {}", sourceInfo, key, val.error()));
            else {
                auto res = rule->addEffect(*dynamicEffect, *val);
                if (!res)
                    self->addError(std::format("{}: hl.window_rule: field '{}': {}", sourceInfo, key, res.error()));
            }

            lua_pop(L, 1);
            continue;
        }

        auto res = Internal::addWindowRuleEffectFromLua(L, *desc, rule);
        if (!res)
            self->addError(std::format("{}: hl.window_rule: field '{}': {}", sourceInfo, key, res.error()));

        lua_pop(L, 1);
    }

    Supplementary::refresher()->scheduleRefresh(Supplementary::REFRESH_WINDOW_STATES);

    Objects::CLuaWindowRule::push(L, rule);
    return 1;
}

static int hlLayerRule(lua_State* L) {
    auto* self = sc<CConfigManager*>(lua_touserdata(L, lua_upvalueindex(1)));

    if (!lua_istable(L, 1)) {
        self->addError("hl.layer_rule: argument must be a table");
        return 0;
    }

    const std::string sourceInfo = Internal::getSourceInfo(L);

    std::string       name;
    lua_getfield(L, 1, "name");
    if (lua_isstring(L, -1))
        name = lua_tostring(L, -1);
    lua_pop(L, 1);

    bool enabled = true;
    lua_getfield(L, 1, "enabled");
    if (lua_isboolean(L, -1))
        enabled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    SP<Desktop::Rule::CLayerRule> rule;
    if (!name.empty() && self->m_luaLayerRules.contains(name)) {
        rule = self->m_luaLayerRules[name];
    } else {
        rule = makeShared<Desktop::Rule::CLayerRule>(name);
        if (!name.empty())
            self->m_luaLayerRules[name] = rule;
        Desktop::Rule::ruleEngine()->registerRule(SP<Desktop::Rule::IRule>(rule));
    }
    rule->setEnabled(enabled);

    lua_getfield(L, 1, "match");
    if (lua_istable(L, -1)) {
        int matchIdx = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, matchIdx) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                std::string matchKey = lua_tostring(L, -2);
                std::string matchVal;
                if (lua_type(L, -1) == LUA_TBOOLEAN)
                    matchVal = lua_toboolean(L, -1) ? "true" : "false";
                else if (lua_isstring(L, -1))
                    matchVal = lua_tostring(L, -1);
                else {
                    self->addError(std::format("{}: hl.layer_rule: match value for '{}' must be string or bool", sourceInfo, matchKey));
                    lua_pop(L, 1);
                    continue;
                }
                auto prop = Desktop::Rule::matchPropFromString(matchKey);
                if (prop.has_value())
                    rule->registerMatch(*prop, matchVal);
                else
                    self->addError(std::format("{}: hl.layer_rule: unknown match property '{}'", sourceInfo, matchKey));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 1);
            continue;
        }
        std::string_view key = lua_tostring(L, -2);
        if (key == "name" || key == "enabled" || key == "match") {
            lua_pop(L, 1);
            continue;
        }

        const auto* desc = Internal::findDescByName(LAYER_RULE_EFFECT_DESCS, key);
        if (!desc) {
            const auto dynamicEffect = Desktop::Rule::layerEffects()->get(key);
            if (!dynamicEffect.has_value()) {
                self->addError(std::format("{}: hl.layer_rule: unknown field '{}'", sourceInfo, key));
                lua_pop(L, 1);
                continue;
            }

            auto val = Internal::ruleValueToString(L);
            if (!val)
                self->addError(std::format("{}: hl.layer_rule: field '{}': {}", sourceInfo, key, val.error()));
            else {
                auto res = rule->addEffect(*dynamicEffect, *val);
                if (!res)
                    self->addError(std::format("{}: hl.layer_rule: field '{}': {}", sourceInfo, key, res.error()));
            }

            lua_pop(L, 1);
            continue;
        }

        auto val = UP<ILuaConfigValue>(desc->factory());
        auto err = val->parse(L);
        if (err.errorCode != PARSE_ERROR_OK)
            self->addError(std::format("{}: hl.layer_rule: field '{}': {}", sourceInfo, key, err.message));
        else {
            auto str = val->toString();
            auto res = rule->addEffect(desc->effect, str);
            if (!res)
                self->addError(std::format("{}: hl.layer_rule: field '{}': {}", sourceInfo, key, res.error()));
        }

        lua_pop(L, 1);
    }

    Supplementary::refresher()->scheduleRefresh(Supplementary::REFRESH_RULES);

    Objects::CLuaLayerRule::push(L, rule);
    return 1;
}

void Internal::registerConfigRuleBindings(lua_State* L, CConfigManager* mgr) {
    Internal::setMgrFn(L, mgr, "config", hlConfig);
    Internal::setMgrFn(L, mgr, "get_config", hlGetConfig);
    Internal::setMgrFn(L, mgr, "device", hlDevice);
    Internal::setMgrFn(L, mgr, "monitor", hlMonitor);
    Internal::setMgrFn(L, mgr, "xr_monitor", hlXRMonitor);
    Internal::setMgrFn(L, mgr, "xr_rule", hlXRRule);
    Internal::setMgrFn(L, mgr, "window_rule", hlWindowRule);
    Internal::setMgrFn(L, mgr, "layer_rule", hlLayerRule);
    Internal::setMgrFn(L, mgr, "workspace_rule", hlWorkspaceRule);
    Internal::setMgrFn(L, mgr, "env", hlEnv);
    Internal::setMgrFn(L, mgr, "permission", hlPermission);

    lua_newtable(L);
    Internal::setMgrFn(L, mgr, "load", hlPluginLoad);
    lua_setfield(L, -2, "plugin");

    Internal::setFn(L, "gesture", hlGesture);
    Internal::setFn(L, "curve", hlCurve);
    Internal::setFn(L, "animation", hlAnimation);
}
