#include "ConfigValues.hpp"

using namespace Config;
using namespace Config::Values;

template <typename T>
static std::string opt(std::optional<T> x) {
    if (x)
        return std::format("{}", x.value());
    return "null";
}

template <>
std::string opt<Values::OptionMap>(std::optional<Values::OptionMap> x) {
    if (x) {
        std::string json = "[";
        for (const auto& [k, v] : *x) {
            json += std::format("{{ \"{}\": {} }},", k, v);
        }
        if (!json.empty())
            json.pop_back();
        json += "]";
        return json;
    }
    return "null";
}

static std::string jsonify(SP<IValue> v) {

    if (auto x = dc<CBoolValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": {},
        "current": {}
    }},)#",
            x->name(), x->description(), x->defaultVal(), x->value());
    }

    if (auto x = dc<CIntValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": {},
        "current": {},
        "min": {},
        "max": {},
        "map": {}
    }},)#",
            x->name(), x->description(), x->defaultVal(), x->value(), opt(x->m_min), opt(x->m_max), opt(x->m_map));
    }

    if (auto x = dc<CFloatValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": {},
        "current": {},
        "min": {},
        "max": {}
    }},)#",
            x->name(), x->description(), x->defaultVal(), x->value(), opt(x->m_min), opt(x->m_max));
    }

    if (auto x = dc<CCssGapValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": "{}",
        "current": "{}",
        "min": {},
        "max": {}
    }},)#",
            x->name(), x->description(), x->defaultVal().toString(), x->value().toString(), opt(x->m_min), opt(x->m_max));
    }

    if (auto x = dc<CFontWeightValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": "{}",
        "current": "{}"
    }},)#",
            x->name(), x->description(), x->defaultVal().toString(), x->value().toString());
    }

    if (auto x = dc<CGradientValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": "{}",
        "current": "{}"
    }},)#",
            x->name(), x->description(), x->defaultVal().toString(), x->value().toString());
    }

    if (auto x = dc<CStringValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": "{}",
        "current": "{}"
    }},)#",
            x->name(), x->description(), x->defaultVal(), x->value());
    }

    if (auto x = dc<CVec2Value*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": [{}, {}],
        "current": [{}, {}]
    }},)#",
            x->name(), x->description(), x->defaultVal().x, x->defaultVal().y, x->value().x, x->value().y);
    }

    if (auto x = dc<CColorValue*>(v.get()); x) {
        return std::format(
            R"#(
    {{
        "name": "{}",
        "description": "{}",
        "default": "{:x}",
        "current": "{:x}"
    }},)#",
            x->name(), x->description(), x->defaultVal(), x->value());
    }

    Log::logger->log(Log::ERR, "values/jsonify: invalid value {}", v->name());
    return "{},";
}

std::string Values::getAsJson() {
    std::string json = "[\n";
    for (const auto& v : CONFIG_VALUES) {
        json += jsonify(v);
    }
    json.pop_back();
    json += "\n]";
    return json;
}

std::vector<SP<IValue>> Values::getConfigValues() {
#define MS makeConfigValue

    return std::vector<SP<IValue>>{

        /*
         * general:
         */

        MS<Int>("general:border_size", "size of the border around windows", 1, {.min = 0, .max = 20, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<CssGap>("general:gaps_in", "gaps between windows", 5, {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<CssGap>("general:gaps_out", "gaps between windows and monitor edges", 20, {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<CssGap>("general:float_gaps", "gaps between windows and monitor edges for floating windows", 0, {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Int>("general:gaps_workspaces", "gaps between workspaces. Stacks with gaps_out.", 0, {.min = 0, .max = 100, .refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Gradient>("general:col.inactive_border", "border color for inactive windows", CHyprColor{0xff444444}, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("general:col.active_border", "border color for the active window", CHyprColor{0xffffffff}, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("general:col.nogroup_border", "inactive border color for window that cannot be added to a group", CHyprColor{0xffffaaff},
                     {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("general:col.nogroup_border_active", "active border color for window that cannot be added to a group", CHyprColor{0xffff00ff},
                     {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<String>("general:layout", "which layout to use. [dwindle/master/scrolling/monocle/lua:<name>]", "dwindle", {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Bool>("general:no_focus_fallback", "if true, will not fall back to the next available window when moving focus in a direction where no window was found", false),
        MS<Bool>("general:resize_on_border", "enables resizing windows by clicking and dragging on borders and gaps", false),
        MS<Int>("general:extend_border_grab_area", "extends the area around the border where you can click and drag on, only used when general:resize_on_border is on.", 15,
                {.min = 0, .max = 100}),
        MS<Bool>("general:hover_icon_on_border", "show a cursor icon when hovering over borders, only used when general:resize_on_border is on.", true),
        MS<Bool>("general:allow_tearing", "master switch for allowing tearing to occur.", false),
        MS<Int>("general:resize_corner", "force floating windows to use a specific corner when being resized (1-4 going clockwise from top left, 0 to disable)", 0,
                {.min = 0, .max = 4, .map = OptionMap{{"disable", 0}, {"top_left", 1}, {"top_right", 2}, {"bottom_right", 3}, {"bottom_left", 4}}}),
        MS<Bool>("general:snap:enabled", "enable snapping for floating windows", false),
        MS<Int>("general:snap:window_gap", "minimum gap in pixels between windows before snapping", 10, {.min = 0, .max = 100}),
        MS<Int>("general:snap:monitor_gap", "minimum gap in pixels between window and monitor edges before snapping", 10, {.min = 0, .max = 100}),
        MS<Bool>("general:snap:border_overlap", "if true, windows snap such that only one border's worth of space is between them", false),
        MS<Bool>("general:snap:respect_gaps", "if true, snapping will respect gaps between windows", false),
        MS<Bool>("general:modal_parent_blocking", "if true, parent windows of modals will not be interactive.", true),
        MS<String>("general:locale", "overrides the system locale", ""),

        /*
         * decoration:
         */

        MS<Int>("decoration:rounding", "rounded corners' radius (in layout px)", 0,
                {.min = 0, .max = 20, .refresh = Supplementary::REFRESH_WINDOW_STATES | Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:rounding_power", "rounding power of corners (2 is a circle)", 2,
                  {.min = 2, .max = 10, .refresh = Supplementary::REFRESH_WINDOW_STATES | Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:active_opacity", "opacity of active windows.", 1, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:inactive_opacity", "opacity of inactive windows.", 1, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:fullscreen_opacity", "opacity of fullscreen windows.", 1, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Bool>("decoration:shadow:enabled", "enable drop shadows on windows", true, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Int>("decoration:shadow:range", "Shadow range (size) in layout px", 4, {.min = 0, .max = 100, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Int>("decoration:shadow:render_power", "in what power to render the falloff (more power, the faster the falloff)", 3,
                {.min = 1, .max = 4, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Bool>("decoration:shadow:sharp", "whether the shadow should be sharp or not.", false, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("decoration:shadow:color", "shadow's color. Alpha dictates shadow's opacity.", CHyprColor{0xee1a1a1a}, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("decoration:shadow:color_inactive", "inactive shadow color. (if not set, will fall back to col.shadow)", -1,
                     {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Vec2>("decoration:shadow:offset", "shadow's rendering offset.", Config::VEC2{},
                 {.validator = vec2Range(-250, -250, 250, 250), .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:shadow:scale", "shadow's scale.", 1, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Bool>("decoration:glow:enabled", "enable inner glow on windows", false, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Int>("decoration:glow:range", "glow range (size) in layout px", 10, {.min = 0, .max = 100, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Int>("decoration:glow:render_power", "in what power to render the falloff (more power, the faster the falloff)", 3,
                {.min = 1, .max = 4, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("decoration:glow:color", "glow's color. Alpha dictates glow's opacity.", CHyprColor{0xee33ccff}, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Gradient>("decoration:glow:color_inactive", "inactive glow color. (if not set, will fall back to decoration:glow:color)", -1,
                     {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Bool>("decoration:dim_modal", "enables dimming of parents of modal windows", true, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Bool>("decoration:dim_inactive", "enables dimming of inactive windows", false, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:dim_strength", "how much inactive windows should be dimmed", 0.5, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:dim_special", "how much to dim the rest of the screen by when a special workspace is open.", 0.2,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:dim_around", "how much the dimaround window rule should dim by.", 0.4, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_focused", "how high the focused window floats above the wallpaper plane, 0-1 (stereo depth desktop)", 0.6,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_unfocused", "how high ordinary windows float above the wallpaper plane, 0-1", 0.2,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_layers", "how high top/overlay layer surfaces (bars, notifications) float, 0-1. background/bottom stay pinned at 0", 0.8,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_scale", "metres of rise at depth 1.0. The comfort knob: keep the steps between tiers small", 0.12,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_distance", "metres from your eyes to the screen plane. With depth_screen_width, turns a depth into pixels of disparity", 1.5,
                  {.min = 0.1, .max = 20, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_screen_width", "metres wide that ONE eye's image appears. 1.6 is a headset-sized virtual screen; measure your panel for a flat one", 1.6,
                  {.min = 0.05, .max = 20, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Float>("decoration:depth_edge_slack", "pixels of disparity an element pinned to the screen edge (a full-width bar) may still use. 0 keeps such elements flat", 2.0,
                  {.min = 0, .max = 64, .refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<String>("decoration:screen_shader", "a path to a custom shader to be applied at the end of rendering.", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_SCREEN_SHADER}),
        MS<Bool>("decoration:border_part_of_window", "whether the border should be treated as a part of the window.", true),

        /*
         * blur:
         */

        MS<Bool>("decoration:blur:enabled", "enable kawase window background blur", true, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Int>("decoration:blur:size", "blur size (distance)", 8, {.min = 0, .max = 100, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Int>("decoration:blur:passes", "the amount of passes to perform", 1, {.min = 0, .max = 10, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Bool>("decoration:blur:ignore_opacity", "make the blur layer ignore the opacity of the window", true, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Bool>("decoration:blur:new_optimizations", "whether to enable further optimizations to the blur.", true, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Bool>("decoration:blur:xray", "if enabled, floating windows will ignore tiled windows in their blur.", false, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:noise", "how much noise to apply.", 0.0117, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:contrast", "contrast modulation for blur.", 0.8916, {.min = 0, .max = 2, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:brightness", "brightness modulation for blur.", 1, {.min = 0, .max = 2, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:vibrancy", "Increase saturation of blurred colors.", 0.1696, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:vibrancy_darkness", "How strong the effect of vibrancy is on dark areas.", 0, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Bool>("decoration:blur:special", "whether to blur behind the special workspace (note: expensive)", false, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Bool>("decoration:blur:popups", "whether to blur popups (e.g. right-click menus)", false, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:popups_ignorealpha", "works like ignorealpha in layer rules. If pixel opacity is below set value, will not blur.", 0.2,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Bool>("decoration:blur:input_methods", "whether to blur input methods (e.g. fcitx5)", false, {.refresh = Supplementary::REFRESH_BLUR_FB}),
        MS<Float>("decoration:blur:input_methods_ignorealpha", "works like ignorealpha in layer rules. If pixel opacity is below set value, will not blur.", 0.2,
                  {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_BLUR_FB}),

        MS<Bool>("decoration:motion_blur:enabled", "enable motion blur for moving and resizing windows", false, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Int>("decoration:motion_blur:samples", "amount of samples used for motion blur", 7, {.min = 1, .max = 64, .refresh = Supplementary::REFRESH_WINDOW_STATES}),

        /*
         * animations:
         */

        MS<Bool>("animations:enabled", "enable animations", true),
        MS<Bool>("animations:workspace_wraparound", "changes the direction of slide animations between the first and last workspaces", false),

        /*
         * input:
         */

        MS<String>("input:kb_model", "Appropriate XKB keymap parameter.", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:kb_layout", "Appropriate XKB keymap parameter", "us", {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:kb_variant", "Appropriate XKB keymap parameter", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:kb_options", "Appropriate XKB keymap parameter", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:kb_rules", "Appropriate XKB keymap parameter", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:kb_file", "Appropriate XKB keymap file", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:numlock_by_default", "Engage numlock by default.", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:resolve_binds_by_sym", "Determines how keybinds act when multiple layouts are used.", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:repeat_rate", "The repeat rate for held-down keys, in repeats per second.", 25, {.min = 0, .max = 200, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:repeat_delay", "Delay before a held-down key is repeated, in milliseconds.", 600, {.min = 0, .max = 2000}),
        MS<Float>("input:sensitivity", "Sets the mouse input sensitivity. Value is clamped to the range -1.0 to 1.0.", 0,
                  {.min = -1, .max = 1, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:accel_profile", "Sets the cursor acceleration profile. [adaptive/flat/custom]", STRVAL_EMPTY,
                   {.validator = strChoice({"adaptive", "flat", "custom"}), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:force_no_accel", "Force no cursor acceleration.", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:rotation", "Sets the rotation of a device in degrees clockwise. Value is clamped to the range 0 to 359.", 0,
                {.min = 0, .max = 359, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:left_handed", "Switches RMB and LMB", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:scroll_points", "Sets the scroll acceleration profile, when accel_profile is set to custom.", STRVAL_EMPTY,
                   {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:scroll_method", "Sets the scroll method. [2fg/edge/on_button_down/no_scroll]", STRVAL_EMPTY,
                   {.validator = strChoice({"2fg", "edge", "on_button_down", "no_scroll"}), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:scroll_button", "Sets the scroll button. 0 means default.", 0, {.min = 0, .max = 300, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:scroll_button_lock", "If the scroll button lock is enabled, the button does not need to be held down.", false,
                 {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Float>("input:scroll_factor", "Multiplier added to scroll movement for external mice.", 1, {.min = 0, .max = 2, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:natural_scroll", "Inverts scrolling direction.", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:follow_mouse", "Specify if and how cursor movement should affect window focus.", 1,
                {.min = 0, .max = 3, .map = OptionMap{{"disabled", 0}, {"follow", 1}, {"detached", 2}, {"separate", 3}}, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Float>("input:follow_mouse_threshold", "The smallest distance in logical pixels the mouse needs to travel for the window under it to get focused.", 0),
        MS<Int>("input:focus_on_close", "Controls the window focus behavior when a window is closed.", 0,
                {.min = 0, .max = 2, .map = OptionMap{{"next", 0}, {"cursor", 1}, {"mru", 2}}, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:mouse_refocus", "if disabled, mouse focus won't switch to the hovered window unless the mouse crosses a window boundary when follow_mouse=1.", true),
        MS<Int>("input:float_switch_override_focus",
                "If enabled (1 or 2), focus will change to the window under the cursor when changing from tiled-to-floating and vice versa. If 2, focus will also follow mouse on "
                "float-to-float switches.",
                1, {.min = 0, .max = 2, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:special_fallthrough", "if enabled, having only floating windows in the special workspace will not block focusing windows in the regular workspace.", false),
        MS<Int>("input:off_window_axis_events", "How to handle axis events around a focused window.", 1,
                {.min = 0, .max = 3, .map = OptionMap{{"ignore", 0}, {"send", 1}, {"clamp", 2}, {"warp", 3}}, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:emulate_discrete_scroll", "Emulates discrete scrolling from high resolution scrolling events.", 1,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"non_standard", 1}, {"force_all", 2}}, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:follow_mouse_shrink",
                "Shrinks the inactive window hitboxes used for focus detection by the specified number of pixels. This creates a dead zone in gaps between windows where moving "
                "the cursor will not change focus. Works only with follow_mouse = 1.",
                0, {.min = 0, .max = 300, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),

        /*
         * input:touchpad:
         */

        MS<Bool>("input:touchpad:disable_while_typing", "Disable the touchpad while typing.", true, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:natural_scroll", "Inverts scrolling direction.", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Float>("input:touchpad:scroll_factor", "Multiplier applied to the amount of scroll movement.", 1, {.min = 0, .max = 2, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:middle_button_emulation", "Sending LMB and RMB simultaneously will be interpreted as a middle click.", false,
                 {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:touchpad:tap_button_map", "Sets the tap button mapping for touchpad button emulation. [lrm/lmr]", STRVAL_EMPTY,
                   {.validator = strChoice({"lrm", "lmr"}), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:clickfinger_behavior", "Button presses with 1, 2, or 3 fingers will be mapped to LMB, RMB, and MMB respectively.", false,
                 {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:tap-to-click", "Tapping on the touchpad with 1, 2, or 3 fingers will send LMB, RMB, and MMB respectively.", true,
                 {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:touchpad:drag_lock", "When enabled, lifting the finger off while dragging will not drop the dragged item.", 0,
                {.min = 0, .max = 2, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:tap-and-drag", "Sets the tap and drag mode for the touchpad", true, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:flip_x", "Inverts the horizontal movement of the touchpad", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchpad:flip_y", "Inverts the vertical movement of the touchpad", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:touchpad:drag_3fg", "Whether to use 3 or 4 finger drag.", 0,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"3_finger", 1}, {"4_finger", 2}}, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),

        /*
         * input:touchdevice:
         */

        MS<Int>("input:touchdevice:transform", "Transform the input from touchdevices.", 0, {.min = 0, .max = 6, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:touchdevice:output", "The monitor to bind touch devices.", "[[Auto]]", {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:touchdevice:enabled", "Whether input is enabled for touch devices.", true, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),

        /*
         * input:virtualkeyboard:
         */

        MS<Int>("input:virtualkeyboard:share_states", "Unify key down states and modifier states with other keyboards.", 2,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"only_non_ime", 2}}, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:virtualkeyboard:release_pressed_on_close", "Release all pressed keys by virtual keyboard on close.", false,
                 {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),

        /*
         * input:tablet:
         */

        MS<Int>("input:tablet:transform", "transform the input from tablets.", 0, {.min = 0, .max = 6, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<String>("input:tablet:output", "the monitor to bind tablets.", STRVAL_EMPTY, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Vec2>("input:tablet:region_position", "position of the mapped region in monitor layout.", Config::VEC2{},
                 {.validator = vec2Range(-20000, -20000, 20000, 20000), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:tablet:absolute_region_position", "whether to treat the region_position as an absolute position in monitor layout.", false,
                 {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Vec2>("input:tablet:region_size", "size of the mapped region.", Config::VEC2{},
                 {.validator = vec2Range(-100, -100, 4000, 4000), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:tablet:relative_input", "whether the input should be relative", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Bool>("input:tablet:left_handed", "if enabled, the tablet will be rotated 180 degrees", false, {.refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Vec2>("input:tablet:active_area_size", "size of tablet's active area in mm", Config::VEC2{},
                 {.validator = vec2Range(0, 0, 500, 500), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Vec2>("input:tablet:active_area_position", "position of the active area in mm", Config::VEC2{},
                 {.validator = vec2Range(0, 0, 500, 500), .refresh = Supplementary::REFRESH_INPUT_DEVICES}),

        /*
         * input:tablettool:
         */

        MS<Int>("input:tablettool:eraser_button_mode",
                "Change the eraser button behavior on the tool. When set to 0, use the default hardware behavior of the tool. "
                "When set to 1, the eraser button on the tool sends a button event instead.",
                0, {.min = 0, .max = 6, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Int>("input:tablettool:eraser_button_override",
                "Set a button to be button event when eraser_button_mode is set to 1. Has to be an int, cannot be a string. Must be a valid button (e.g. BTN_STYLUS) "
                "excluding fake buttons (e.g. BTN_TOOL_*) and keys (KEY_*). Check wev if you have any doubts regarding the ID. 0 means default.",
                0, {.min = 0, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Float>("input:tablettool:pressure_range_min",
                  "Set the minimum pressure range for the tool, a negative number will set the default minimum pressure value. This is usually 0.0", -1.0,
                  {.min = -1.0, .max = 1.0, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),
        MS<Float>("input:tablettool:pressure_range_max",
                  "Set the maximum pressure range for the tool, a negative number will set the default maximum pressure value. This is usually 1.0", -1.0,
                  {.min = -1.0, .max = 1.0, .refresh = Supplementary::REFRESH_INPUT_DEVICES}),

        /*
         * gestures:
         */

        MS<Int>("gestures:workspace_swipe_distance", "in px, the distance of the touchpad gesture", 300, {.min = 0, .max = 2000}),
        MS<Bool>("gestures:workspace_swipe_touch", "enable workspace swiping from the edge of a touchscreen", false),
        MS<Bool>("gestures:workspace_swipe_invert", "invert the direction (touchpad only)", true),
        MS<Bool>("gestures:workspace_swipe_touch_invert", "invert the direction (touchscreen only)", false),
        MS<Int>("gestures:workspace_swipe_min_speed_to_force", "minimum speed in px per timepoint to force the change ignoring cancel_ratio.", 30, {.min = 0, .max = 200}),
        MS<Float>("gestures:workspace_swipe_cancel_ratio", "how much the swipe has to proceed in order to commence it.", 0.5, {.min = 0, .max = 1}),
        MS<Bool>("gestures:workspace_swipe_create_new", "whether a swipe right on the last workspace should create a new one.", true),
        MS<Bool>("gestures:workspace_swipe_direction_lock", "if enabled, switching direction will be locked when you swipe past the direction_lock_threshold.", true),
        MS<Int>("gestures:workspace_swipe_direction_lock_threshold", "in px, the distance to swipe before direction lock activates.", 10, {.min = 0, .max = 200}),
        MS<Bool>("gestures:workspace_swipe_forever", "if enabled, swiping will not clamp at the neighboring workspaces but continue to the further ones.", false),
        MS<Bool>("gestures:workspace_swipe_use_r", "if enabled, swiping will use the r prefix instead of the m prefix for finding workspaces.", false),
        MS<Int>("gestures:close_max_timeout", "Timeout for closing windows with the close gesture, in ms.", 1000, {.min = 10, .max = 2000}),
        MS<Bool>("gestures:scrolling:move_snap_to_grid", "When releasing the scroll move gesture, whether it shoud try to snap to the grid.", true),
        MS<Bool>("gestures:scrolling:move_snap_cursor", "When releasing the scroll move gesture, whether it shoud snap the cursor to the newly focused window.", true),
        /*
         * group:
         */

        MS<Bool>("group:insert_after_current", "whether new windows in a group spawn after current or at group tail", true),
        MS<Bool>("group:focus_removed_window", "whether Hyprland should focus on the window that has just been moved out of the group", true),
        MS<Bool>("group:merge_groups_on_drag", "whether window groups can be dragged into other groups", true),
        MS<Bool>("group:merge_groups_on_groupbar", "whether one group will be merged with another when dragged into its groupbar", true),
        MS<Gradient>("group:col.border_active", "active group border color", CHyprColor{0x66ffff00}, {.refresh = Supplementary::REFRESH_GRADIENTS_GROUPBAR}),
        MS<Gradient>("group:col.border_inactive", "inactive group border color", CHyprColor{0x66777700}, {.refresh = Supplementary::REFRESH_GRADIENTS_GROUPBAR}),
        MS<Gradient>("group:col.border_locked_inactive", "inactive locked group border color", CHyprColor{0x66ff5500}, {.refresh = Supplementary::REFRESH_GRADIENTS_GROUPBAR}),
        MS<Gradient>("group:col.border_locked_active", "active locked group border color", CHyprColor{0x66775500}, {.refresh = Supplementary::REFRESH_GRADIENTS_GROUPBAR}),
        MS<Bool>("group:auto_group", "automatically group new windows", true),
        MS<Int>("group:drag_into_group", "whether dragging a window into a unlocked group will merge them.", 1,
                {.min = 0, .max = 2, .map = OptionMap{{"disabled", 0}, {"enabled", 1}, {"only when dragging into the groupbar", 2}}}),
        MS<Bool>("group:merge_floated_into_tiled_on_groupbar", "whether dragging a floating window into a tiled window groupbar will merge them", false),
        MS<Bool>("group:group_on_movetoworkspace", "whether using movetoworkspace[silent] will merge the window into the workspace's solitary unlocked group", false),

        /*
         * group:groupbar:
         */

        MS<Bool>("group:groupbar:enabled", "enables groupbars", true, {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<Bool>("group:groupbar:disable_when_only", "disable if contains single window. Considered only if enabled == true", false,
                 {.refresh = Supplementary::REFRESH_WINDOW_STATES}),
        MS<String>("group:groupbar:font_family", "font used to display groupbar titles", "[[EMPTY]]"),
        MS<FontWeight>("group:groupbar:font_weight_active", "weight of the font used to display active groupbar titles"),
        MS<FontWeight>("group:groupbar:font_weight_inactive", "weight of the font used to display inactive groupbar titles"),
        MS<Int>("group:groupbar:font_size", "font size of groupbar title", 8, {.min = 2, .max = 64}),
        MS<Bool>("group:groupbar:gradients", "enables gradients", false),
        MS<Int>("group:groupbar:height", "height of the groupbar", 14, {.min = 1, .max = 64}),
        MS<Int>("group:groupbar:indicator_gap", "height of the gap between the groupbar indicator and title", 0, {.min = 0, .max = 64}),
        MS<Int>("group:groupbar:indicator_height", "height of the groupbar indicator", 3, {.min = 1, .max = 64}),
        MS<Bool>("group:groupbar:stacked", "render the groupbar as a vertical stack", false),
        MS<Int>("group:groupbar:priority", "sets the decoration priority for groupbars", 3, {.min = 0, .max = 6}),
        MS<Bool>("group:groupbar:render_titles", "whether to render titles in the group bar decoration", true),
        MS<Bool>("group:groupbar:scrolling", "whether scrolling in the groupbar changes group active window", true),
        MS<Bool>("group:groupbar:middle_click_close", "whether middle clicking the groupbar closes the clicked window", true),
        MS<Int>("group:groupbar:rounding", "how much to round the groupbar", 1, {.min = 0, .max = 20}),
        MS<Float>("group:groupbar:rounding_power", "rounding power of groupbar corners (2 is a circle)", 2, {.min = 2, .max = 10}),
        MS<Int>("group:groupbar:gradient_rounding", "how much to round the groupbar gradient", 2, {.min = 0, .max = 20}),
        MS<Float>("group:groupbar:gradient_rounding_power", "rounding power of groupbar gradient corners (2 is a circle)", 2, {.min = 2, .max = 10}),
        MS<Bool>("group:groupbar:round_only_edges", "if yes, will only round at the groupbar edges", true),
        MS<Bool>("group:groupbar:gradient_round_only_edges", "if yes, will only round at the groupbar gradient edges", true),
        MS<Color>("group:groupbar:text_color", "color for window titles in the groupbar", 0xffffffff),
        MS<Color>("group:groupbar:text_color_inactive", "color for inactive windows' titles in the groupbar", -1),
        MS<Color>("group:groupbar:text_color_locked_active", "color for the active window's title in a locked group", -1),
        MS<Color>("group:groupbar:text_color_locked_inactive", "color for inactive windows' titles in locked groups", -1),
        MS<Gradient>("group:groupbar:col.active", "active group border color", 0x66ffff00),
        MS<Gradient>("group:groupbar:col.inactive", "inactive (out of focus) group border color", 0x66777700),
        MS<Gradient>("group:groupbar:col.locked_active", "active locked group border color", 0x66ff5500),
        MS<Gradient>("group:groupbar:col.locked_inactive", "inactive locked group border color", 0x66775500),
        MS<Int>("group:groupbar:gaps_out", "gap between gradients and window", 2, {.min = 0, .max = 20}),
        MS<Int>("group:groupbar:gaps_in", "gap between gradients", 2, {.min = 0, .max = 20}),
        MS<Bool>("group:groupbar:keep_upper_gap", "keep an upper gap above gradient", true),
        MS<Int>("group:groupbar:text_offset", "set an offset for a text", 0, {.min = -20, .max = 20}),
        MS<Int>("group:groupbar:text_padding", "set horizontal padding for a text", 0, {.min = 0, .max = 22}),
        MS<Bool>("group:groupbar:blur", "enable background blur for groupbars", false),

        /*
         * misc:
         */

        MS<Bool>("misc:disable_hyprland_logo", "disables the random Hyprland logo / anime girl background. :(", false),
        MS<Bool>("misc:disable_splash_rendering", "disables the Hyprland splash rendering.", false),
        MS<Color>("misc:col.splash", "Changes the color of the splash text.", 0x55ffffff),
        MS<String>("misc:font_family", "Set the global default font to render the text.", "Sans"),
        MS<String>("misc:splash_font_family", "Changes the font used to render the splash text.", "[[EMPTY]]"),
        MS<Int>("misc:force_default_wallpaper", "Force any of the 3 default wallpapers. [-1/0/1/2]", -1, {.min = -1, .max = 2}),
        MS<Int>("misc:vrr", "controls the VRR (Adaptive Sync) of your monitors", 0,
                {.min = 0, .max = 3, .map = OptionMap{{"off", 0}, {"on", 1}, {"fullscreen", 2}, {"fullscreen_game", 3}}, .refresh = Supplementary::REFRESH_MONITOR_STATES}),
        MS<Bool>("misc:mouse_move_enables_dpms", "If DPMS is set to off, wake up the monitors if the mouse moves", false),
        MS<Bool>("misc:key_press_enables_dpms", "If DPMS is set to off, wake up the monitors if a key is pressed.", false),
        MS<Bool>("misc:name_vk_after_proc", "Name virtual keyboards after the processes that create them.", true),
        MS<Bool>("misc:always_follow_on_dnd", "Will make mouse focus follow the mouse when drag and dropping.", true),
        MS<Bool>("misc:layers_hog_keyboard_focus", "If true, will make keyboard-interactive layers keep their focus on mouse move.", true),
        MS<Bool>("misc:animate_manual_resizes", "If true, will animate manual window resizes/moves", false),
        MS<Bool>("misc:animate_mouse_windowdragging", "If true, will animate windows being dragged by mouse.", false),
        MS<Bool>("misc:disable_autoreload", "If true, the config will not reload automatically on save.", false, {.refresh = Supplementary::REFRESH_CONFIG_WATCHER}),
        MS<Bool>("misc:enable_swallow", "Enable window swallowing", false),
        MS<String>("misc:swallow_regex", "The class regex to be used for windows that should be swallowed.", STRVAL_EMPTY),
        MS<String>("misc:swallow_exception_regex", "The title regex to be used for windows that should not be swallowed.", STRVAL_EMPTY),
        MS<Bool>("misc:focus_on_activate", "Whether Hyprland should focus an app that requests to be focused.", false),
        MS<Bool>("misc:mouse_move_focuses_monitor", "Whether mouse moving into a different monitor should focus it", true),
        MS<Bool>("misc:allow_session_lock_restore", "if true, will allow you to restart a lockscreen app in case it crashes.", false),
        MS<Bool>("misc:session_lock_xray", "keep rendering workspaces below your lockscreen", false),
        MS<Bool>("misc:session_lock_blur", "Enable blur for lockscreen. You probably want to enable `session_lock_xray`.", false),
        MS<Color>("misc:background_color", "change the background color.", 0xff111111),
        MS<Bool>("misc:close_special_on_empty", "close the special workspace if the last window is removed", true),
        MS<Int>("misc:on_focus_under_fullscreen", "if there is a fullscreen or maximized window, decide whether a tiled window requested to focus should replace it.", 2,
                {.min = 0, .max = 2, .map = OptionMap{{"ignore", 0}, {"take_over", 1}, {"exit_fullscreen", 2}}}),
        MS<Bool>("misc:exit_window_retains_fullscreen", "if true, closing a fullscreen window makes the next focused window fullscreen", false),
        MS<Int>("misc:initial_workspace_tracking", "if enabled, windows will open on the workspace they were invoked on.", 1, {.min = 0, .max = 2}),
        MS<Int>("misc:initial_workspace_token_timeout", "the time in seconds a window has to open on its invoked workspace before the tracking token expires.", 10,
                {.min = 1, .max = 3600}),
        MS<Bool>("misc:middle_click_paste", "whether to enable middle-click-paste (aka primary selection)", true),
        MS<Int>("misc:render_unfocused_fps", "the maximum limit for renderunfocused windows' fps in the background", 15, {.min = 1, .max = 120}),
        MS<Bool>("misc:disable_xdg_env_checks", "disable the warning if XDG environment is externally managed", false),
        MS<Bool>("misc:disable_hyprland_guiutils_check", "disable the warning if hyprland-guiutils is missing", false),
        MS<Bool>("misc:disable_watchdog_warning", "whether to disable the warning about not using start-hyprland.", false),
        MS<Int>("misc:lockdead_screen_delay", "the delay in ms after the lockdead screen appears.", 1000, {.min = 0, .max = 5000}),
        MS<Bool>("misc:enable_anr_dialog", "whether to enable the ANR (app not responding) dialog when your apps hang", true),
        MS<Int>("misc:anr_missed_pings", "number of missed pings before showing the ANR dialog", 5, {.min = 1, .max = 20}),
        MS<Bool>("misc:screencopy_force_8b", "forces 8 bit screencopy", true),
        MS<Bool>("misc:disable_scale_notification", "disables notification popup when a monitor fails to set a suitable scale", false),
        MS<Bool>("misc:size_limits_tiled", "whether to apply minsize and maxsize rules to tiled windows", false),

        /*
         * binds:
         */

        MS<Bool>("binds:pass_mouse_when_bound", "if disabled, will not pass the mouse events to apps / dragging windows around if a keybind has been triggered.", false),
        MS<Int>("binds:scroll_event_delay", "in ms, how many ms to wait after a scroll event to allow passing another one for the binds.", 300, {.min = 0, .max = 2000}),
        MS<Bool>("binds:workspace_back_and_forth", "If enabled, an attempt to switch to the currently focused workspace will instead switch to the previous workspace.", false),
        MS<Bool>("binds:hide_special_on_workspace_change", "If enabled, changing the active workspace will hide the special workspace on the monitor.", false),
        MS<Bool>("binds:allow_workspace_cycles", "If enabled, workspaces don't forget their previous workspace.", false),
        MS<Int>("binds:workspace_center_on", "Whether switching workspaces should center the cursor on the workspace (0) or on the last active window (1)", 1,
                {.min = 0, .max = 1}),
        MS<Int>("binds:focus_preferred_method", "sets the preferred focus finding method when using focuswindow/movewindow/etc with a direction.", 0, {.min = 0, .max = 1}),
        MS<Bool>("binds:ignore_group_lock", "If enabled, dispatchers like moveintogroup, moveoutofgroup and movewindoworgroup will ignore lock per group.", false),
        MS<Bool>("binds:movefocus_cycles_fullscreen", "If enabled, when on a fullscreen window, movefocus will cycle fullscreen.", false),
        MS<Bool>("binds:movefocus_cycles_groupfirst", "If enabled, when in a grouped window, movefocus will cycle windows in the groups first.", false),
        MS<Bool>("binds:disable_keybind_grabbing", "If enabled, apps that request keybinds to be disabled will not be able to do so.", false),
        MS<Bool>("binds:window_direction_monitor_fallback", "If enabled, moving a window or focus over the edge of a monitor with a direction will move it to the next monitor.",
                 true),
        MS<Bool>("binds:allow_pin_fullscreen", "Allows fullscreen to pinned windows, and restore their pinned status afterwards", false),
        MS<Int>("binds:drag_threshold", "Movement threshold in pixels for window dragging and c/g bind flags. 0 to disable.", 0,
                {.min = 0, .max = std::numeric_limits<int>::max()}),

        /*
         * xwayland:
         */

        MS<Bool>("xwayland:enabled", "allow running applications using X11", true),
        MS<Bool>("xwayland:use_nearest_neighbor", "uses the nearest neighbor filtering for xwayland apps, making them pixelated rather than blurry", true),
        MS<Bool>("xwayland:force_zero_scaling", "forces a scale of 1 on xwayland windows on scaled displays.", false),
        MS<Bool>("xwayland:create_abstract_socket", "Create the abstract Unix domain socket for XWayland", false),

        /*
         * opengl:
         */

        MS<Bool>("opengl:nvidia_anti_flicker", "reduces flickering on nvidia at the cost of possible frame drops on lower-end GPUs.", true),

        /*
         * render:
         */

        MS<Int>("render:direct_scanout", "Enables direct scanout.", 0, {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Bool>("render:expand_undersized_textures", "Whether to expand textures that have not yet resized to be larger.", true),
        MS<Bool>("render:xp_mode", "Disable back buffer and bottom layer rendering.", false),
        MS<Int>("render:ctm_animation", "Whether to enable a fade animation for CTM changes.", 2,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Bool>("render:cm_enabled", "Enable Color Management pipelines (requires restart to fully take effect)", true),
        MS<Bool>("render:send_content_type", "Report content type to allow monitor profile autoswitch", true),
        MS<Int>("render:cm_auto_hdr", "Auto-switch to hdr mode when fullscreen app is in hdr", 1,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"hdr", 1}, {"hdredid", 2}}}),
        MS<Bool>("render:new_render_scheduling", "enable new render scheduling, which should improve FPS on underpowered devices.", false),
        MS<Int>("render:non_shader_cm", "Enable CM without shader.", 3, {.min = 0, .max = 3, .map = OptionMap{{"disable", 0}, {"always", 1}, {"ondemand", 2}, {"ignore", 3}}}),
        MS<String>("render:cm_sdr_eotf", "Default transfer function for displaying SDR apps.", "default"),
        MS<Bool>("render:commit_timing_enabled", "Enable commit timing proto. Requires restart", true),
        MS<Bool>("render:icc_vcgt_enabled", "Enable sending VCGT ramps to KMS with ICC profiles", true),
        MS<Bool>("render:use_shader_blur_blend", "Use experimental blurred bg blending", false),
        MS<Int>("render:use_fp16", "Use experimental internal FP16 buffer.", 2, {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Int>("render:keep_unmodified_copy", "Keep umodified SDR frame copy for sreensharing.", 2,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Int>("render:non_shader_cm_interop", "non_shader_cm interaction with ctm proto (hyprsunset and similar).", 2,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Int>("render:fp16_sdr_tf", "Internal workbuffer transfer function for fp16 in SDR mode", 0, {.min = 0, .max = 1, .map = OptionMap{{"monitor", 0}, {"linear", 1}}}),

        /*
         * cursor:
         */

        MS<Bool>("cursor:invisible", "don't render cursors", false),
        MS<Int>("cursor:no_hardware_cursors", "disables hardware cursors.", 2, {.min = 0, .max = 2, .map = OptionMap{{"Disabled", 0}, {"Enabled", 1}, {"Auto", 2}}}),
        MS<Int>("cursor:no_break_fs_vrr", "disables scheduling new frames on cursor movement for fullscreen apps with VRR enabled.", 2,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Int>("cursor:min_refresh_rate", "minimum refresh rate for cursor movement when no_break_fs_vrr is active.", 24, {.min = 10, .max = 500}),
        MS<Int>("cursor:hotspot_padding", "the padding, in logical px, between screen edges and the cursor", 0, {.min = 0, .max = 20}),
        MS<Float>("cursor:inactive_timeout", "in seconds, after how many seconds of cursor's inactivity to hide it. Set to 0 for never.", 0, {.min = 0, .max = 20}),
        MS<Bool>("cursor:no_warps", "if true, will not warp the cursor in many cases", false),
        MS<Bool>("cursor:persistent_warps", "When a window is refocused, the cursor returns to its last position relative to that window.", false),
        MS<Int>("cursor:warp_on_change_workspace", "Move the cursor to the last focused window after changing the workspace.", 0,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"force", 2}}}),
        MS<Int>("cursor:warp_on_toggle_special", "Move the cursor to the last focused window when toggling a special workspace.", 0,
                {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"force", 2}}}),
        MS<String>("cursor:default_monitor", "the name of a default monitor for the cursor to be set to on startup", STRVAL_EMPTY),
        MS<Float>("cursor:zoom_factor", "the factor to zoom by around the cursor. 1 means no zoom.", 1, {.min = 1, .max = 10, .refresh = Supplementary::REFRESH_CURSOR_ZOOMS}),
        MS<Bool>("cursor:zoom_rigid", "whether the zoom should follow the cursor rigidly or loosely", false),
        MS<Bool>("cursor:zoom_disable_aa", "If enabled, when zooming, no antialiasing will be used", false),
        MS<Bool>("cursor:zoom_detached_camera", "Detaches the camera from the mouse when zoomed in", true),
        MS<Bool>("cursor:enable_hyprcursor", "whether to enable hyprcursor support", true),
        MS<Bool>("cursor:hide_on_key_press", "Hides the cursor when you press any key until the mouse is moved.", false),
        MS<Bool>("cursor:hide_on_touch", "Hides the cursor when the last input was a touch input until a mouse input is done.", true),
        MS<Bool>("cursor:hide_on_tablet", "Hides the cursor when the last input was a tablet input until a mouse input is done.", false),
        MS<Int>("cursor:use_cpu_buffer", "Makes HW cursors use a CPU buffer.", 2, {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),
        MS<Bool>("cursor:sync_gsettings_theme", "sync xcursor theme with gsettings", true),
        MS<Bool>("cursor:warp_back_after_non_mouse_input", "warp the cursor back to where it was after using a non-mouse input to move it.", false),

        /*
         * ecosystem:
         */

        MS<Bool>("ecosystem:no_update_news", "disable the popup that shows up when you update hyprland to a new version.", false),
        MS<Bool>("ecosystem:no_donation_nag", "disable the popup that shows up twice a year encouraging to donate.", false),
        MS<Bool>("ecosystem:enforce_permissions", "whether to enable permission control.", false),

        /*
         * debug:
         */

        MS<Bool>("debug:overlay", "print the debug performance overlay.", false),
        MS<Bool>("debug:damage_blink", "flash damaged areas", false),
        MS<Bool>("debug:gl_debugging", "enable OpenGL debugging and error checking.", false),
        MS<Bool>("debug:disable_logs", "disable logging to a file", true),
        MS<Bool>("debug:disable_time", "disables time logging", true),
        MS<Int>("debug:damage_tracking", "redraw only the needed bits of the display.", 2, {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"monitor", 1}, {"full", 2}}}),
        MS<Bool>("debug:enable_stdout_logs", "enables logging to stdout", false),
        MS<Int>("debug:manual_crash", "set to 1 and then back to 0 to crash Hyprland.", 0, {.min = 0, .max = 1}),
        MS<Bool>("debug:suppress_errors", "if true, do not display config file parsing errors.", false),
        MS<Bool>("debug:disable_scale_checks", "disables verification of the scale factors.", false),
        MS<Int>("debug:error_limit", "limits the number of displayed config file parsing errors.", 5, {.min = 0, .max = 20}),
        MS<Int>("debug:error_position", "sets the position of the error bar.", 0, {.min = 0, .max = 1, .map = OptionMap{{"top", 0}, {"bottom", 1}}}),
        MS<Bool>("debug:colored_stdout_logs", "enables colors in the stdout logs.", true),
        MS<Bool>("debug:log_damage", "enables logging the damage.", false),
        MS<Bool>("debug:pass", "enables render pass debugging.", false),
        MS<Bool>("debug:full_cm_proto", "claims support for all cm proto features (requires restart)", false),
        MS<Bool>("debug:ds_handle_same_buffer", "Special case for DS with unmodified buffer", true),
        MS<Bool>("debug:ds_handle_same_buffer_fifo", "Special case for DS with unmodified buffer unlocks fifo", true),
        MS<Bool>("debug:fifo_pending_workaround", "Fifo workaround for empty pending list", false),
        MS<Bool>("debug:render_solitary_wo_damage", "Render solitary window with empty damage", false),
        MS<Bool>("debug:vfr", "controls the VFR status of Hyprland. Do not turn off unless debugging", true),
        MS<Int>("debug:invalidate_fp16", "allow fp16 buffer invalidation.", 1, {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"auto", 2}}}),

        /*
         * layout:
         */

        MS<Vec2>("layout:single_window_aspect_ratio", "If specified, whenever only a single window is open, it will be coerced into the specified aspect ratio.",
                 Config::VEC2{0, 0}, {.validator = vec2Range(0, 0, 1000, 1000), .refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Float>("layout:single_window_aspect_ratio_tolerance", "Minimum distance for single_window_aspect_ratio to take effect.", 0.1F,
                  {.min = 0.F, .max = 1.F, .refresh = Supplementary::REFRESH_LAYOUTS}),

        /*
         * dwindle:
         */

        MS<Int>("dwindle:force_split", "force a split direction for new windows", 0, {.min = 0, .max = 2, .map = OptionMap{{"follow_mouse", 0}, {"left", 1}, {"right", 2}}}),
        MS<Bool>("dwindle:preserve_split", "if enabled, the split will not change regardless of what happens to the container.", false),
        MS<Bool>("dwindle:smart_split", "if enabled, allows a more precise control over the window split direction based on the cursor's position.", false),
        MS<Bool>("dwindle:smart_resizing", "if enabled, resizing direction will be determined by the mouse's position on the window.", true),
        MS<Bool>("dwindle:permanent_direction_override", "if enabled, makes the preselect direction persist.", false),
        MS<Float>("dwindle:special_scale_factor", "specifies the scale factor of windows on the special workspace", 1, {.min = 0, .max = 1}),
        MS<Float>("dwindle:split_width_multiplier", "specifies the auto-split width multiplier", 1, {.min = 0.1F, .max = 3}),
        MS<Bool>("dwindle:use_active_for_splits", "whether to prefer the active window or the mouse position for splits", true),
        MS<Float>("dwindle:default_split_ratio", "the default split ratio on window open.", 1, {.min = 0.1F, .max = 1.9F}),
        MS<Int>("dwindle:split_bias", "specifies which window will receive the split ratio.", 0, {.min = 0, .max = 1, .map = OptionMap{{"directional", 0}, {"current", 1}}}),
        MS<Bool>("dwindle:precise_mouse_move", "if enabled, bindm movewindow will drop the window more precisely depending on where your mouse is.", false),

        /*
         * master:
         */

        MS<Bool>("master:allow_small_split", "enable adding additional master windows in a horizontal split style", false),
        MS<Float>("master:special_scale_factor", "the scale of the special workspace windows.", 1, {.min = 0, .max = 1}),
        MS<Float>("master:mfact", "the size as a percentage of the master window.", 0.55, {.min = 0, .max = 1, .refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<String>("master:new_status", "`master`: new window becomes master; `slave`: new windows are added to slave stack; `inherit`: inherit from focused window", "slave"),
        MS<Bool>("master:new_on_top", "whether a newly open window should be on the top of the stack", false),
        MS<String>("master:new_on_active", "`before`, `after`: place new window relative to the focused window; `none`: place new window according to new_on_top.", "none"),
        MS<String>("master:orientation", "default placement of the master area", "left", {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Int>("master:slave_count_for_center_master", "when using orientation=center, make the master window centered only when at least this many slave windows are open.", 2,
                {.min = 0, .max = 10, .refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<String>("master:center_master_fallback", "Set fallback for center master when slaves are less than slave_count_for_center_master", "left",
                   {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Bool>("master:center_ignores_reserved", "centers the master window on monitor ignoring reserved areas", false, {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Bool>("master:smart_resizing", "if enabled, resizing direction will be determined by the mouse's position on the window.", true),
        MS<Bool>("master:drop_at_cursor", "when enabled, dragging and dropping windows will put them at the cursor position.", true),
        MS<Bool>("master:always_keep_position", "whether to keep the master window in its configured position when there are no slave windows", false,
                 {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Bool>("master:focus_master_on_close", "when enabled, closing a window focuses the master window", false),

        /*
         * scrolling:
         */

        MS<Bool>("scrolling:fullscreen_on_one_column", "when enabled, a single column on a workspace will always span the entire screen.", true,
                 {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Float>("scrolling:column_width", "the default width of a column.", 0.5, {.min = 0.1, .max = 1.0}),
        MS<Int>("scrolling:focus_fit_method", "When a column is focused, what method should be used to bring it into view", 1,
                {.min = 0, .max = 1, .map = OptionMap{{"center", 0}, {"fit", 1}}}),
        MS<Bool>("scrolling:follow_focus", "when a window is focused, should the layout move to bring it into view automatically", true),
        MS<Float>("scrolling:follow_min_visible", "when a window is focused, require that at least a given fraction of it is visible for focus to follow", 0.4,
                  {.min = 0.0, .max = 1.0}),
        MS<String>("scrolling:explicit_column_widths", "A comma-separated list of preconfigured widths for colresize +conf/-conf", "0.333, 0.5, 0.667, 1.0"),
        MS<String>("scrolling:direction", "Direction in which new windows appear and the layout scrolls", "right", {.refresh = Supplementary::REFRESH_LAYOUTS}),
        MS<Bool>("scrolling:wrap_focus", "Determines if column focus wraps around", true),
        MS<Bool>("scrolling:wrap_swapcol", "Determines if column movement wraps around", true),

        /*
         * openxr:
         */

        MS<Bool>("openxr:enabled", "enable the OpenXR integration (session starts when a runtime is available)", false),
        MS<Bool>("openxr:reprobe",
                 "when enabled but no runtime/headset is available yet, keep re-probing in the background (with a backoff) so the session comes up automatically once the "
                 "runtime starts or the headset is donned — no need to toggle openxr:enabled. Also drives auto-reconnect after a session/runtime loss (report-17)",
                 true),
        MS<Int>("openxr:reprobe_interval_ms",
                "base interval for the openxr:reprobe backoff, in milliseconds. Waiting for the runtime grows the delay from this base up to 30s; waiting for the headset "
                "(runtime up, not donned) polls at this fixed cadence",
                2000, {.min = 250, .max = 60000}),
        MS<Bool>("openxr:reprobe_watch",
                 "while dormant in 'unavailable', also inotify-watch $XDG_RUNTIME_DIR for the runtime's IPC socket appearing (monado_comp_ipc / wivrn/comp_ipc) and probe "
                 "immediately when it does, instead of waiting out the reprobe backoff. Kills the up-to-30s stall between donning the headset and the session coming up "
                 "(WiVRn's socket only appears when the headset connects). The reprobe backoff timer stays as fallback",
                 true),
        MS<Bool>("openxr:recenter_on_plug",
                 "on the first time the headset is donned in a session, re-seat anchor:local monitors relative to your current head pose instead of the runtime's (often "
                 "arbitrary, under boundaryless/standby) LOCAL_FLOOR origin — the monitors appear in front of you at their configured height/distance. Multi-monitor layouts "
                 "are recentered rigidly (relative arrangement preserved). A brief doff-and-don within the same session does NOT re-seat (report-20)",
                 true),
        MS<String>("openxr:recenter",
                   "what the HEADSET's own recenter button does to your monitors: hold (default) = the monitors stay where they are IN THE ROOM, so recentering only moves the "
                   "runtime's origin; follow = a runtime reference-space change ALSO re-seats the anchor:local group rigidly to your current head, i.e. the recenter button "
                   "becomes 'bring my monitors to me'. Under follow, INVOLUNTARY reference-space changes (putting the headset back on, a guardian re-derive) re-seat too — the "
                   "runtime does not say which kind it was. Prefer hold plus a keybind on `xrmonitor reseat`, which re-seats only when you ask (doc 03 §8.4)",
                   "hold"),
        MS<String>("openxr:gpu", "DRM render node to use for XR (e.g. /dev/dri/renderD128). Empty = follow Hyprland's primary GPU", ""),
        MS<Bool>("openxr:ignore_kernel_taint",
                 "start XR even when the kernel has already taken an oops this boot (/proc/sys/kernel/tainted bit 7, TAINT_DIE). By default XR refuses to start in that state, "
                 "because bring-up unavoidably initializes every installed GPU vendor driver and entering an already-corrupt one can take the whole machine down. Development "
                 "escape hatch only — the correct fix is to reboot",
                 false),
        MS<String>("openxr:runtime_json",
                   "path to the OpenXR runtime manifest (openxr_*.json) the session should use, overriding XR_RUNTIME_JSON / active_runtime.json for THIS compositor's XR "
                   "session. Empty (default) = leave the login environment untouched (loader default / active_runtime.json). Read on the main thread at each session start; set "
                   "it live with `hyprctl keyword openxr:runtime_json <path>` then `hyprctl openxr disable && hyprctl openxr enable` to re-handshake against it. Clearing it back "
                   "to empty restores the runtime the process launched with. Powers the XREAL flat/XR toggle (pick the xreal monado build without touching WiVRn's active_runtime)",
                   ""),
        MS<String>("openxr:force_linear",
                   "allocate LINEAR buffers for XR monitors so the XR GPU can import them: auto (only when a cross-GPU split is detected, "
                   "default) | on | off. Needed when openxr:gpu is a different GPU than the desktop renders on; costs some compositing throughput",
                   "auto"),
        MS<String>("openxr:blend_mode", "environment blend mode: auto (runtime preferred) | opaque | alpha (passthrough) | additive. Read at session start", "auto"),
        MS<Float>("openxr:floor_offset", "fallback eye height in meters when the runtime lacks LOCAL_FLOOR", 1.5, {.min = 0.0, .max = 3.0}),
        MS<Float>("openxr:default_size", "default width of a new XR monitor quad, in meters", 1.6, {.min = 0.2, .max = 4.0}),
        MS<Float>("openxr:default_distance", "default distance from the viewer for newly placed monitors, in meters", 1.5, {.min = 0.3, .max = 5.0}),
        MS<Float>("openxr:default_monitor_scale",
                  "scale given to an XR-created monitor that has no explicit scale in a matching monitor= rule. Hyprland's headless PPI guess lands such an output at 2.0 "
                  "(cramped through a headset); an explicit `monitor = NAME, ..., <scale>` still wins, and so does the PPI guess on a mode this value cannot divide into "
                  "whole logical pixels. 0 = always keep the PPI guess",
                  1.25, {.min = 0.0, .max = 8.0}),
        MS<Float>("openxr:leash_response", "head/body leash spring response time in seconds (smaller = snappier)", 0.35, {.min = 0.01, .max = 5.0}),
        MS<Float>("openxr:leash_deadzone_angle", "head/body leash angular deadzone in degrees", 15.0, {.min = 0.0, .max = 180.0}),
        MS<Float>("openxr:leash_deadzone_distance", "head/body leash positional deadzone in meters", 0.25, {.min = 0.0, .max = 5.0}),
        MS<Bool>("openxr:body_leash_follow_height", "body-leashed monitors also follow vertical head movement", false),
        // Adaptive anchoring (research/13): an anchor:local monitor picks itself up and head/body-follows
        // when you walk away from the desk seat, then re-docks when you return, with an eased blend. All
        // read per-frame (hot-tunable like the leash vars). Enable per-monitor with `adaptive:on` on an
        // anchor:local xrmonitor line, or the `openxr adaptive on` verb.
        MS<Float>("openxr:adaptive_leave_radius", "adaptive anchoring: horizontal (XZ) distance from the desk seat, in meters, before a docked monitor undocks and follows (R_out)",
                  1.5, {.min = 0.1, .max = 10.0}),
        MS<Float>("openxr:adaptive_return_radius",
                  "adaptive anchoring: horizontal (XZ) distance back within the desk seat, in meters, before a roaming monitor re-docks (R_in; keep < leave_radius for hysteresis)",
                  1.0, {.min = 0.1, .max = 10.0}),
        MS<Int>("openxr:adaptive_leave_dwell_ms", "adaptive anchoring: how long the head must stay beyond leave_radius before undocking, in milliseconds (anti-flap)", 400,
                {.min = 0, .max = 10000}),
        MS<Int>("openxr:adaptive_return_dwell_ms", "adaptive anchoring: how long the head must stay within return_radius before re-docking, in milliseconds (anti-flap)", 800,
                {.min = 0, .max = 10000}),
        MS<Int>("openxr:adaptive_transition_ms", "adaptive anchoring: dock<->roam eased pose-blend duration, in milliseconds", 700, {.min = 0, .max = 5000}),
        MS<String>("openxr:adaptive_transition_ease", "adaptive anchoring: easing curve for the dock<->roam blend: smoothstep (default) | linear | ease_out", "smoothstep"),
        MS<String>("openxr:adaptive_roam_mode", "adaptive anchoring: default follow behavior while roaming: body (yaw-only at a comfortable height, default) | head (pinned to gaze)",
                   "body"),
        MS<Bool>("openxr:adaptive_use_height", "adaptive anchoring: include vertical (Y) head movement in the geofence distance, so standing up/lying down can also trigger", false),
        MS<Bool>("openxr:adaptive_carry_offset", "adaptive anchoring: roam at the head-relative offset captured when undocking, instead of the configured comfortable roam offset",
                 false),
        MS<Bool>("openxr:pointer", "enable the XR ray pointer device", true),
        MS<Float>("openxr:pointer_trigger_threshold", "trigger analog value that presses the pointer button", 0.7, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:pointer_trigger_threshold_release", "trigger analog value that releases the pointer button (hysteresis)", 0.4, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:grab_threshold", "squeeze analog value that starts a grab", 0.7, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:grab_threshold_release", "squeeze analog value that ends a grab (hysteresis)", 0.4, {.min = 0.0, .max = 1.0}),
        MS<Bool>("openxr:grab_anywhere",
                 "let a squeeze/grip on a monitor's CONTENT body move it (the controller convenience). true keeps grab-anywhere for controllers; false confines moving "
                 "to the bottom move-bar and resizing to the corner handles. The bar and corners always grab regardless. Read per-frame (hot-toggles)",
                 true),
        MS<String>("openxr:hand_grab",
                   "hand-tracking grab gesture (XR_EXT_hand_interaction): pinch (thumb-index, default; anchors to the stable pinch pose so opening the pinch to release "
                   "doesn't lurch the window) | grasp (fist curl; anchors to the wrist) | both (either). Hands always grab from the move-bar/corners only (never the body, "
                   "regardless of grab_anywhere); controllers are unaffected. Read per-frame (hot-toggles)",
                   "both"),
        MS<String>("openxr:hand_grab_anywhere",
                   "which hand-tracking grab GESTURE may move a monitor from its CONTENT body (the hand analog of grab_anywhere, keyed on the gesture that triggered THIS "
                   "grab, not the hand_grab mode): none (hands only grab the move-bar/corners) | grasp (default; a fist grabs anywhere, a pinch stays chrome-only and keeps "
                   "its click) | pinch | both. CAVEAT for pinch/both: a body pinch will BOTH click (pointer press) and grab. Bar/corners always grab regardless; controllers "
                   "use grab_anywhere instead. Read per-frame (hot-toggles). Pairs with hand_grab=both (default) so a fist grabs anywhere while a pinch stays chrome-only "
                   "+ keeps its click — the live-validated scheme",
                   "grasp"),
        MS<Int>("openxr:grab_release_latency_ms",
                "on grab release, re-anchor from the quad pose sampled this many ms BEFORE the release edge, to reject the release-motion lurch (the grip/hand "
                "swing on the release frame). 0 = use the release frame. ~100 clears the perturbation; raise it if releases still lurch, lower it if a quick "
                "flick-and-let-go feels like it snaps back too far",
                100, {.min = 0, .max = 500}),
        MS<Float>("openxr:grab_release_velocity_reject",
                  "release-jerk rejection as a RATIO (not m/s): if the quad's peak speed in the ~80ms release window exceeds this multiple of its typical carry speed "
                  "(trimmed mean of the faster half of the preceding ~500ms of samples, so a flick started from rest is judged by its flick pace), re-anchor from the last "
                  "carry-paced sample instead of the fixed latency window — catches a fast fist-open/flick at the release edge. Because it is relative, a uniformly fast "
                  "flick (release speed ≈ carry speed) is NOT rewound, unlike the old absolute threshold. 0 disables (latency window only)",
                  3.0, {.min = 0.0, .max = 50.0}),
        MS<Bool>("openxr:grab_filter",
                 "smooth a move-grab carry with a 1€ low-pass filter (Casiez CHI'12) to remove tracking jitter; adds ~1 frame of latency in exchange for steadiness. On by "
                 "default (live-validated on Quest 3). See grab_filter_scope for hands-vs-controllers and min_cutoff/beta to tune. Read per-frame (hot-toggles)",
                 true),
        MS<String>("openxr:grab_filter_scope",
                   "which devices the 1€ move-grab carry filter (grab_filter) applies to: all (default; hands AND controllers — controllers reported carry jitter too) | "
                   "hands (hands only, controllers keep the zero-latency device-space late-latch). Filtering a device drops its runtime late-latch (submits the smoothed "
                   "pose in LOCAL_FLOOR, +~1 frame latency), so scope=all trades a frame of controller latency for steadiness. Read per-frame (hot-toggles)",
                   "all"),
        MS<Float>("openxr:grab_filter_min_cutoff",
                  "1€ filter minimum cutoff frequency (Hz) for the hand move-grab carry. Lower = more smoothing when the panel is nearly still (also more lag). Casiez's "
                  "suggested starting point is 1.0; drop toward 0.1-0.5 if a held-still panel still jitters",
                  1.0, {.min = 0.01, .max = 10.0}),
        MS<Float>("openxr:grab_filter_beta",
                  "1€ filter speed coefficient for the hand move-grab carry. Higher = the cutoff rises faster with motion, so fast moves lag less (but jitter returns "
                  "sooner). Casiez's suggested starting point is 0.007; default 0.025 (live-tuned on Quest 3). Raise it (e.g. 0.05-0.5) if moving a panel feels laggy, lower "
                  "it if fast moves overshoot/jitter",
                  0.025, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:scroll_speed", "multiplier for thumbstick scrolling on XR monitors", 1.0, {.min = 0.0, .max = 100.0}),
        MS<Float>("openxr:chrome_margin",
                  "transparent chrome margin added around each XR monitor's content, in meters. The quad grows by this on the top/left/right (and the bottom, which also "
                  "holds the move-bar); the desktop still blits into the inner content rect so no pixel is covered. size: stays CONTENT meters. 0 disables chrome",
                  0.10, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:chrome_bar_height", "height of the XR monitor move-bar, in meters, added below the content inside the bottom margin (WP-G2 draws it, WP-G3 grabs it)",
                  0.08, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:chrome_bar_width_frac", "XR monitor move-bar width as a fraction of the content width (centered horizontally under the content)", 0.8,
                  {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:chrome_corner_size", "size of each XR monitor corner resize handle, in meters (square, clamped to the chrome margin so it never covers content)", 0.09,
                  {.min = 0.0, .max = 1.0}),
        MS<Bool>("openxr:chrome_enabled",
                 "master toggle for XR monitor chrome (auto-hiding move-bar + corner resize handles). When 0 the margins collapse to 0 and no chrome is drawn — identical to "
                 "chrome_margin=0. Hot-reloadable (recreates swapchains)",
                 true),
        MS<Int>("openxr:chrome_hide_delay_ms", "delay before the XR monitor chrome fades out after the ray stops hovering/grabbing it, in milliseconds", 1500,
                {.min = 0, .max = 60000}),
        MS<Int>("openxr:chrome_fade_ms", "XR monitor chrome fade in/out duration, in milliseconds", 150, {.min = 0, .max = 5000}),
        MS<Color>("openxr:chrome_col_idle", "XR monitor chrome color at rest (visible while hovering the quad; premultiplied over passthrough)", 0x66aaaaaa),
        MS<Color>("openxr:chrome_col_hover", "XR monitor chrome color for the element (bar or corner) the ray is pointing at", 0xcc66aaff),
        MS<Color>("openxr:chrome_col_grab", "XR monitor chrome color while the quad is grabbed", 0xff66aaff),
        // ---- stereo CONTENT on an XR monitor: the quad pair (research/24 §5.1, WP X1) ----
        MS<Bool>("openxr:stereo_quad_pair",
                 "present a stereo-declared window (windowrule = stereo <layout>) that is fullscreen on an XR monitor as a PAIR of quad layers — one per eye, each cropped to "
                 "that eye's half of the packed frame — so the headset shows real stereo instead of a doubled image. Needs a runtime that honors eyeVisibility and "
                 "subImage.imageRect (Monado and WiVRn do). Turn it off to fall back to the single flattened quad if a runtime mishandles either",
                 true),
        // ---- the DEPTH DESKTOP on XR monitors (research/24 §6, WP X3/X4) ----
        MS<Bool>("openxr:depth_desktop",
                 "composite every XR monitor ONCE PER EYE so the shipped depth rules (decoration:depth_focused / depth_unfocused / depth_layers, windowrule = depth, layerrule "
                 "= depth) read as real 3D depth in the headset: a focused window floats in front of the wallpaper, bars sit above both. The monitor's declared size is "
                 "unchanged and stays PER EYE — an xrmonitor at 2560x1440 still gives you a 2560x1440 desktop, now scanned out as a 5120x1440 pair. Costs one extra composite "
                 "per XR monitor per frame, and only while something on it actually has depth (a flat desktop composites once and shows the same pane to both eyes). Turn it "
                 "off for the cheapest possible XR presentation, or while measuring",
                 true),
        // ---- luma-keyed transparency, "black-as-alpha" (docs/openxr/research/archive/09-monitor-transparency.md) ----
        MS<Float>("openxr:black_alpha",
                  "luma-keyed transparency: the alpha given to PURE BLACK content pixels, so a dark desktop turns into an AR-style overlay you can see the room through. 1.0 "
                  "(default) = feature off, every pixel stays opaque; 0.0 = black is fully transparent; in between = translucent black. Brighter pixels stay opaque (see "
                  "black_alpha_knee). Only has an effect when the session's environment blend mode is alpha (passthrough) or additive — under opaque the runtime paints black "
                  "behind us, so it is force-disabled with a warning. Chrome (move-bar/handles) is never keyed",
                  1.0, {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:black_alpha_knee",
                  "luma-keyed transparency: the Rec.709 luma at which content becomes FULLY opaque. Pure black gets openxr:black_alpha, luma >= this is untouched, with a "
                  "smoothstep ramp between. Lower = only near-black dissolves (dark themes keep their contrast); higher = dark greys go see-through too",
                  0.10, {.min = 0.001, .max = 1.0}),
        MS<Int>("openxr:transparency_blend_ms",
                "how long a per-monitor transparency change (uniform alpha or luma key, from an `xrrule` match, a manual `hyprctl openxr alpha` or a config reload) takes to "
                "ease in. Nothing ever pops: the value rides a smoothstep envelope, and an interrupted transition restarts from wherever it currently is. 0 = snap instantly",
                600, {.min = 0, .max = 5000}),
        // ---- ray aim / cursor / hover assist (docs/openxr/research/INTERACTION.md, report 14) ----
        MS<Bool>("openxr:cursor",
                 "draw a small endpoint cursor where each hand's aim ray hits an XR monitor (report 14: HypXRland drew no ray/cursor, the dominant 'hard to aim' cause). "
                 "Zero cost when no ray hovers a quad. Read per-frame (hot-toggles)",
                 true),
        MS<Float>("openxr:cursor_size", "endpoint cursor diameter in meters (constant physical size; appears round on the quad regardless of aspect)", 0.02,
                  {.min = 0.001, .max = 0.2}),
        MS<Float>("openxr:cursor_press_scale", "endpoint cursor growth factor while a button/pinch is pressed (Quest-style press feedback)", 1.6, {.min = 1.0, .max = 4.0}),
        MS<Color>("openxr:cursor_col_idle", "endpoint cursor color over a monitor's content/margin (premultiplied over the quad)", 0x80ffffff),
        MS<Color>("openxr:cursor_col_grabbable", "endpoint cursor color over a grab handle (move-bar or corner) — a squeeze would grab here", 0xcc66aaff),
        MS<Color>("openxr:cursor_col_press", "endpoint cursor color while a button/pinch is pressed", 0xffffffff),
        MS<Color>("openxr:cursor_col_grab", "endpoint cursor color reserved for an active grab (the ray is normally suppressed while grabbing)", 0xff66aaff),
        MS<Bool>("openxr:cursor_per_hand_tint", "tint the LEFT hand's cursor cooler (red down / blue up) so two simultaneous rays are distinguishable", true),
        MS<Float>("openxr:cursor_redraw_epsilon",
                  "endpoint-cursor movement dead-band in uv units: a hover redraws (and so re-encodes) the swapchain only once the cursor moves this far. Absorbs at-rest hand "
                  "tremor so holding still over a static desktop costs zero re-encode instead of a full-frame redraw every runtime frame (report 14 live: idle-hover IDR-drop storm). "
                  "Read per-frame (hot-toggle); 0 disables the dead-band",
                  0.0015, {.min = 0.0, .max = 0.05}),
        MS<Float>("openxr:hover_hysteresis",
                  "sticky-hover exit margin in meters (report 14 Stage A2): once the ray lands a grab handle, keep it highlighted/grab-eligible until the ray leaves the handle "
                  "expanded by this margin (or lands a different handle). Stops highlight flicker at handle boundaries. 0 disables stickiness. Read per-frame (hot-toggles)",
                  0.03, {.min = 0.0, .max = 0.5}),
        MS<Int>("openxr:hover_dropout_frames", "sticky-hover dropout tolerance: hold the highlighted handle across up to this many consecutive frames where the ray misses every quad",
                2, {.min = 0, .max = 30}),
        MS<Bool>("openxr:magnet",
                 "chrome-only aim magnetism (report 14 Stage C): when the ray narrowly MISSES a grab handle but the handle is within magnet_angle, treat it as hovering the "
                 "handle so the highlight/grab-eligibility turns on exactly when a squeeze (which already forgives the same cone) would grab. Never magnetizes CONTENT — desktop "
                 "clicks stay pixel-precise. Read per-frame (hot-toggles)",
                 true),
        MS<Float>("openxr:magnet_angle", "aim-magnetism cone half-angle in degrees: how far off a grab handle the ray may point and still snap to it (chrome only)", 2.0,
                  {.min = 0.0, .max = 8.0}),
        MS<Bool>("openxr:haptics", "master toggle for controller haptics (grab/press ticks + the hover tick). Hands have no actuator and are unaffected. Read per-frame (hot-toggles)",
                 true),
        MS<Bool>("openxr:haptic_hover", "fire a short controller haptic tick when the ray first enters a grab handle (move-bar/corner) — the tactile 'you are on the handle' cue",
                 true),
        MS<Float>("openxr:haptic_amplitude", "controller haptic tick amplitude (0-1) for the grab/press/hover ticks", 0.5, {.min = 0.0, .max = 1.0}),
        MS<Bool>("openxr:aim_filter",
                 "1€-filter the aim pose before hit-testing (report 14 Stage B) to steady the ray/cursor. Applies to controllers AND hands; adds ~1 frame of latency, "
                 "imperceptible for pointing. On by default. Read per-frame (hot-toggles)",
                 true),
        MS<Float>("openxr:aim_filter_min_cutoff", "aim 1€ filter minimum cutoff frequency (Hz). Lower = steadier when aiming slowly (also more lag). Casiez's starting point is 1.0",
                  1.5, {.min = 0.01, .max = 10.0}),
        MS<Float>("openxr:aim_filter_beta", "aim 1€ filter speed coefficient. Higher = the cutoff rises faster with motion so fast sweeps lag less (jitter returns sooner)", 0.01,
                  {.min = 0.0, .max = 1.0}),
        MS<Float>("openxr:aim_pinch_damping",
                  "aim min-cutoff multiplier while a press/pinch is RAMPING UP (below the trigger threshold): lower = more smoothing so committing a click/grab doesn't yank the "
                  "aim off-target on the commit frame (the documented Meta behavior). 1 disables onset damping",
                  0.4, {.min = 0.0, .max = 1.0}),
        MS<String>("openxr:inhibit_idle",
                   "when a live XR session inhibits idle (hypridle etc.). off = never; focused = while the session has input focus (the pre-research/20 behavior — a runtime "
                   "dashboard in front does NOT inhibit); present (default) = while the headset is actually WORN, which also covers worn-but-not-focused (runtime dashboard, "
                   "overlay mode). present needs XR_EXT_user_presence: with a presence-capable runtime it requires the session VISIBLE and the user present (both, because "
                   "WiVRn's presence sticks 'present' in standby), and with a runtime that has no presence signal it falls back to focused. Legacy 0/false => off, 1/true => "
                   "focused (an explicit opt-in keeps its old meaning)",
                   "present"),
        MS<String>("openxr:monitors_follow_session",
                   "when XR-created virtual monitors behave like UNPLUGGED external monitors (held disabled — workspaces evacuate to the remaining monitors exactly like a "
                   "physical unplug, then return by name when re-plugged). off = never (the old always-present behavior); session = while no OpenXR session exists; visible "
                   "(default) = while the session is not VISIBLE/FOCUSED, so a doffed/standby headset (whose runtime keeps a session alive) reads as unplugged. Unplugging on a "
                   "visibility drop waits out monitor_unplug_grace_ms (anti-flap). When the runtime exposes XR_EXT_user_presence (e.g. WiVRn), visible gates on the real "
                   "donned/doffed signal instead of session visibility — so a session created with the headset on the shelf never plugs. Legacy 0/false => off, 1/true => "
                   "session. See also destroy_monitors_on_stop (research/18)",
                   "visible"),
        MS<Int>("openxr:monitor_unplug_grace_ms",
                "with monitors_follow_session = visible, how long the headset must stay doffed/standby (not present / session not VISIBLE) before the XR monitors unplug and "
                "their workspaces evacuate, in milliseconds. Absorbs a brief doff-and-don and proximity-sensor churn so a quick glance away never rearranges workspaces. Donning "
                "re-plugs immediately regardless",
                20000, {.min = 0, .max = 600000}),
        MS<Int>("openxr:monitor_plug_settle_ms",
                "with monitors_follow_session = visible AND a runtime that does NOT expose XR_EXT_user_presence, how long the session must stay continuously VISIBLE before the "
                "FIRST plug of the session, in milliseconds. Suppresses the session-create visibility blip (some runtimes sprint to FOCUSED at startup even while doffed) without "
                "slowing a real don. Ignored once presence is available (presence is blip-proof by construction) and after the first plug",
                1500, {.min = 0, .max = 60000}),
        MS<Bool>("openxr:destroy_monitors_on_stop",
                 "destroy XR-created virtual monitors when the session stops, instead of keeping them (unplugged when monitors_follow_session != off, plain headless outputs "
                 "otherwise). Declared xrmonitors re-materialize on the next session start",
                 false),
        MS<Bool>("openxr:overlay",
                 "run as an XR_EXTX_overlay session so monitors composite ON TOP of another XR client (a game, or hypxrpaper). Requires a runtime that "
                 "advertises XR_EXTX_overlay (Monado/WiVRn); ignored with a warning otherwise. Read at session start (disable/enable to change)",
                 false),
        MS<Int>("openxr:overlay_z", "overlay composition placement (XR_EXTX_overlay sessionLayersPlacement); higher composites later / on top. Read at session start", 1,
                {.min = 0, .max = 1000000}),

        // ---- conditional hand input (research/16 Part A). Controllers are NEVER gated by this. ----
        MS<String>("openxr:hand_input",
                   "when hand tracking drives the XR pointer/grabs: on (always) | off (never; controllers still work) | auto (default). auto enables hands only when you are "
                   "AWAY from the keyboard (openxr:hand_input_idle_s of keyboard silence, or roaming if hand_input_roam_enables) — so typing never fires false hand gestures. Use "
                   "the `xrmonitor handinput toggle` dispatcher (bind a key-chord) to force hands on while at the keyboard. Hot-applies.",
                   "auto"),
        MS<Float>("openxr:hand_input_idle_s", "hand_input=auto: seconds of physical-keyboard silence before hands re-enable; any keypress re-gates them immediately", 15.0,
                  {.min = 0.0, .max = 600.0}),
        MS<Bool>("openxr:hand_input_roam_enables",
                 "hand_input=auto: also enable hands whenever the head is beyond the adaptive seat geofence (roaming), so wandering the house always has hands even mid-typing-timeout", true),

        // ---- gaze grab: keyboard-driven monitor manipulation steered by head gaze (research/16 Part B) ----
        MS<String>("openxr:gaze_source", "gaze ray source: view (center-FOV head-forward vector, the only source on eye-tracking-less HMDs like Quest 3) | eye (reserved; auto-falls-back to view)", "view"),
        MS<Bool>("openxr:gaze_filter", "1€ low-pass the gaze pose before hit-testing, to steady the dwell selection (research/16 §3.1)", true),
        MS<Float>("openxr:gaze_filter_min_cutoff", "gaze 1€ filter minimum cutoff frequency (Hz); lower = steadier when looking slowly (more lag)", 1.5, {.min = 0.01, .max = 10.0}),
        MS<Float>("openxr:gaze_filter_beta", "gaze 1€ filter speed coefficient; higher = the cutoff rises faster with head motion so fast looks lag less", 0.01, {.min = 0.0, .max = 1.0}),
        MS<Int>("openxr:gaze_dwell_ms", "how long the gaze must rest on a monitor before it becomes the grab-eligible candidate (anti-saccade; also the selection hysteresis)", 200,
                {.min = 0, .max = 2000}),
        MS<Float>("openxr:gaze_hysteresis_deg", "sticky-selection exit margin in degrees: the currently-selected monitor is hit-tested with this much extra angular slack so adjacent boundaries don't flicker", 3.0,
                  {.min = 0.0, .max = 15.0}),
        MS<Bool>("openxr:gaze_follow", "while gaze-grabbed the monitor rides the live gaze ray (default). false = it detaches at grab and only the push/pull keys move it along the grab-time ray", true),
        MS<Float>("openxr:gaze_dist_step", "default push/pull step for `xrmonitor gazepush` with no argument, in meters (bind with binde for stepped repeats)", 0.1, {.min = 0.01, .max = 1.0}),
        MS<Bool>("openxr:gaze_cursor", "draw a distinct cursor dot at the gaze point on the carried monitor while a gaze carry is active (the dwell candidate is shown via the chrome highlight)", true),
        MS<Color>("openxr:gaze_cursor_col", "color of the gaze cursor dot (premultiplied over the quad)", 0xffcc66ff),

        // ---- 2D-plane sync (docs/openxr/research/archive/12-spatial-2d-layout.md) ----
        MS<Bool>("openxr:layout2d:enabled",
                 "derive Hyprland's 2D monitor layout from where the XR quads actually float, so mouse crossing and directional focus match spatial intuition (a monitor to your "
                 "upper-right in the headset sits upper-right in the 2D plane). Off = the historic behavior, XR monitors appended flush-right in creation order. Recomputed only "
                 "on discrete events (monitor add/remove/plug, grab RELEASE, dock/undock, place/distance verbs, config reload), never per-frame and never mid-carry",
                 true),
        MS<Float>("openxr:layout2d:px_per_degree",
                  "angular -> pixel scale used to place monitor CENTERS in the 2D plane. 35 is a 1080p reference monitor at the default distance/size, so quads that are "
                  "angularly adjacent in 3D come out nearly edge-touching before compaction. Compaction is authoritative for adjacency, so this mostly sets how proportional the "
                  "spacing feels",
                  35.0, {.min = 1.0, .max = 500.0}),
        MS<String>("openxr:layout2d:vertical",
                   "how a monitor's row placement is derived: elevation (angular, a clean spherical unwrap with one constant — default) | world_height (metric, better when the "
                   "monitors sit at wildly different distances)",
                   "elevation"),
        MS<Float>("openxr:layout2d:px_per_meter", "openxr:layout2d:vertical=world_height only: metres of world height per layout pixel of vertical offset", 1000.0,
                  {.min = 1.0, .max = 20000.0}),
        MS<String>("openxr:layout2d:attach",
                   "where the XR block attaches to the physical monitors — physical outputs have no room pose, so XR monitors are arranged among themselves and the block is "
                   "attached at a seam: right (flush right of the physical layout, vertically centered — default) | around (the XR block IS the layout; auto-selected anyway when "
                   "no non-XR monitor is enabled)",
                   "right"),
        MS<Int>("openxr:layout2d:min_overlap_px",
                "minimum perpendicular overlap forced between neighbouring monitors. Hyprland's directional focus needs the facing edges within 2px AND a non-zero overlap on the "
                "other axis; a real minimum also stops the cursor having to thread a hairline seam",
                64, {.min = 0, .max = 4000}),
        MS<Float>("openxr:layout2d:row_merge_deg", "elevation window, in degrees, within which monitors are treated as one row/tier", 10.0, {.min = 0.0, .max = 90.0}),
        MS<Float>("openxr:layout2d:reorder_hysteresis_deg",
                  "how far (degrees) a monitor must move before the 2D layout is allowed to change. Below this it keeps its previous placement, so a slightly bumped quad never "
                  "reshuffles your mouse mapping. 0 disables the dead band",
                  4.0, {.min = 0.0, .max = 45.0}),
        MS<Int>("openxr:layout2d:debounce_ms", "coalescing window for 2D-plane-sync recompute triggers, in milliseconds — a burst of grab releases costs one relayout", 300,
                {.min = 0, .max = 5000}),
        MS<String>("openxr:cursor_crossing",
                   "how the cursor decides where to land when it pushes past an XR monitor's edge: raycast (default — cast from your head through the exit point and land on the "
                   "monitor you actually SEE over there, so depth/elevation/yaw are respected) | layout (the 2D-plane sync's grid adjacency alone). raycast falls back to layout "
                   "whenever it cannot answer: no XR session, a stale head pose, a non-XR source monitor, or a ray that meets nothing",
                   "raycast"),

        /*
         * experimental:
         */

        MS<Bool>("experimental:wp_cm_1_2", "Allow wp-cm-v1 version 2", true),

        /*
		 * input_capture: 
		 */
        MS<Bool>("input-capture:capture_modifiers", "If enabled, modifiers are also captured and sent to the program", false),
        MS<Bool>("input-capture:enforce_barriers", "If enabled, throw a wayland error when a invalid barrier is received", true),

        /*
         * quirks:
         */

        MS<Int>("quirks:prefer_hdr", "Prefer HDR mode.", 0, {.min = 0, .max = 2, .map = OptionMap{{"disable", 0}, {"enable", 1}, {"gamescope_only", 2}}}),
        MS<Bool>("quirks:skip_non_kms_dmabuf_formats", "Do not report dmabuf formats which cannot be imported into KMS", false),
    };

#undef MS
}
