# Shared helpers for pushing config at a running Hyprland, across both front ends.
#
# Hyprland's Lua config manager dropped `hyprctl keyword`:
#
#     $ hyprctl keyword general:gaps_in 5
#     keyword can't work with non-legacy parsers. Use eval.
#
# and it still exits 0 while doing nothing, so a stale call fails SILENTLY -
# no error, no effect, no clue. These helpers probe the running session and
# emit the form that works, which keeps these scripts correct both here (Lua)
# and on machines still on the classic .conf front end.
#
# Source with:  . "$(dirname "$0")/lib/hypr-config.sh"

hypr_config_is_lua() {
    hyprctl systeminfo 2>/dev/null | grep -q 'configProvider: lua'
}

# hypr_set_option <classic:key:path> <lua-literal-value>
#
# The value is emitted verbatim as a Lua literal, so booleans and numbers pass
# through as-is; a string value needs its own quotes.
#
#   hypr_set_option input:touchpad:disable_while_typing false
hypr_set_option() {
    if hypr_config_is_lua; then
        hyprctl eval "hl.config({ [\"$(printf '%s' "$1" | tr ':' '.')\"] = $2 })" >/dev/null
    else
        hyprctl keyword "$1" "$2" >/dev/null
    fi
}

# hypr_set_monitor <output> <mode> <position> <scale> [stereo]
#
#   hypr_set_monitor "desc:$DESC" 3840x1080@60 auto 1 sbs
hypr_set_monitor() {
    local output=$1 mode=$2 position=$3 scale=$4 stereo=${5:-}

    if hypr_config_is_lua; then
        local spec="output = [[$output]], mode = [[$mode]], position = [[$position]], scale = [[$scale]]"
        [ -n "$stereo" ] && spec="$spec, stereo = [[$stereo]]"
        hyprctl eval "hl.monitor({ $spec })" >/dev/null
    else
        local value="$output, $mode, $position, $scale"
        [ -n "$stereo" ] && value="$value, stereo:$stereo"
        hyprctl keyword monitor "$value" >/dev/null
    fi
}

# hypr_disable_monitor <output>
#
# Turn an output off without touching the config file. The two front ends spell
# this completely differently - Lua takes a `disabled` field on the rule, the
# classic parser takes a bare `disable` in place of the whole mode/position/scale
# tail - so it cannot ride on hypr_set_monitor's argument list.
#
# Re-enable by asserting an ordinary rule again (hypr_set_monitor), or by
# `hyprctl reload`, which re-applies whatever the config file says.
#
#   hypr_disable_monitor "desc:Nreal Air 2 Ultra 0x88888800"
hypr_disable_monitor() {
    local output=$1

    if hypr_config_is_lua; then
        hyprctl eval "hl.monitor({ output = [[$output]], disabled = true })" >/dev/null
    else
        hyprctl keyword monitor "$output, disable" >/dev/null
    fi
}
