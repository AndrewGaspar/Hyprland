#!/usr/bin/env bash
# xreal-mode.sh — toggle the XREAL Air 2 Ultra between FLAT (ordinary head-locked DP monitor) and XR
# (3DoF OpenXR display via the xreal-flavor Monado) on the running Hyprland/HypXRland session (WP-XR1).
#
#   xreal-mode.sh xr        HID->3D SBS, set the DP output to 3840x1080, start the xreal monado
#                           runtime, point HypXRland at it, and enable the XR session.
#   xreal-mode.sh flat      reverse it: disable the XR session (only if the xreal runtime is active),
#                           stop the xreal runtime, HID->2D, restore the DP output to 1920x1080.
#   xreal-mode.sh status    print detection + current DP mode + `hyprctl openxr status`.
#
# Options:
#   --dry-run    print every action without executing anything.
#   --mono       for `xr`: use 2D mono (single 1920x1080 head-tracked panel) instead of SBS stereo —
#                the fallback if SBS bring-up misbehaves (research doc §4 "highest risk").
#
# Machine-agnostic. The glasses' DP connector is AUTO-DETECTED from `hyprctl monitors` (matches a
# description of "Air 2 Ultra" / "Nreal" / "XREAL"); override with XREAL_MONITOR=DP-5. The xreal monado
# runtime manifest is MONADO_XREAL_BUILD/openxr_monado-dev.json (default subprojects/monado/build-xreal);
# override MONADO_XREAL_BUILD. Idempotent and safe when the glasses are unplugged. NEVER touches
# wivrn.service or any process by name.
#
# Prereqs (one-time, see docs/openxr/07-xreal.md): the udev rule (contrib/xreal/70-xreal.rules), the
# xreal-ctl helper (contrib/xreal, on PATH or set XREAL_CTL), the xreal monado build
# (scripts/build-monado.sh --xreal), and the systemd unit (contrib/xreal/monado-xreal.service).

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

DRY_RUN=0
MONO=0
CMD=""
for arg in "$@"; do
    case "$arg" in
        xr|flat|status) CMD="$arg" ;;
        --dry-run) DRY_RUN=1 ;;
        --mono) MONO=1 ;;
        -h|--help) grep -E '^#( |$)' "$0" | sed 's/^#\ \?//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; echo "usage: xreal-mode.sh {xr|flat|status} [--dry-run] [--mono]" >&2; exit 2 ;;
    esac
done
[[ -n $CMD ]] || { echo "usage: xreal-mode.sh {xr|flat|status} [--dry-run] [--mono]" >&2; exit 2; }

# ---- configuration (all overridable for machine-agnosticism) ----
MONADO_XREAL_BUILD="${MONADO_XREAL_BUILD:-$REPO/subprojects/monado/build-xreal}"
RUNTIME_JSON="${XREAL_RUNTIME_JSON:-$MONADO_XREAL_BUILD/openxr_monado-dev.json}"
XREAL_CTL="${XREAL_CTL:-}"
XREAL_SERVICE_UNIT="${XREAL_SERVICE_UNIT:-monado-xreal.service}"
FLAT_MODE="${XREAL_FLAT_MODE:-1920x1080@60}"
SBS_WIDTH=3840
MONO_WIDTH=1920

# Resolve xreal-ctl: explicit $XREAL_CTL, then PATH, then the in-repo build.
if [[ -z $XREAL_CTL ]]; then
    if command -v xreal-ctl >/dev/null 2>&1; then
        XREAL_CTL="$(command -v xreal-ctl)"
    elif [[ -x $REPO/contrib/xreal/xreal-ctl ]]; then
        XREAL_CTL="$REPO/contrib/xreal/xreal-ctl"
    fi
fi

# ---- helpers ----
run() {
    # Execute (or, in --dry-run, just print) a command.
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] %s\n' "$*"
    else
        printf '  + %s\n' "$*"
        "$@"
    fi
}

have_jq() { command -v jq >/dev/null 2>&1; }

# Auto-detect the glasses' DP connector name from hyprctl monitors (all outputs, incl. disabled).
detect_monitor() {
    if [[ -n ${XREAL_MONITOR:-} ]]; then
        printf '%s' "$XREAL_MONITOR"
        return 0
    fi
    local json name
    json="$(hyprctl -j monitors all 2>/dev/null || hyprctl -j monitors 2>/dev/null || true)"
    [[ -n $json ]] || return 1
    if have_jq; then
        name="$(printf '%s' "$json" | jq -r '
            [ .[] | select((.description // "") | test("Air 2 Ultra|Nreal|XREAL|MRG"; "i")) ] | (.[0].name // empty)')"
    else
        # jq-less fallback: pair up "name"/"description" and match. Best-effort.
        name="$(printf '%s' "$json" | tr ',' '\n' | grep -iE '"(name|description)"' \
            | paste - - 2>/dev/null | grep -iE 'Air 2 Ultra|Nreal|XREAL|MRG' | grep -oE '"name": *"[^"]+"' | head -n1 | sed -E 's/.*"name": *"([^"]+)".*/\1/')"
    fi
    [[ -n $name ]] || return 1
    printf '%s' "$name"
}

# Does the given monitor currently advertise a mode >= SBS_WIDTH wide? (availableModes like 3840x1080@60)
monitor_has_wide_mode() {
    local mon="$1" json
    json="$(hyprctl -j monitors all 2>/dev/null || hyprctl -j monitors 2>/dev/null || true)"
    [[ -n $json ]] || return 1
    if have_jq; then
        printf '%s' "$json" | jq -e --arg m "$mon" --argjson w "$SBS_WIDTH" '
            any(.[]; .name == $m and (.availableModes // [] | any(. as $md | ($md | split("x")[0] | tonumber) >= $w)))' >/dev/null
    else
        printf '%s' "$json" | grep -oE '"[0-9]+x[0-9]+@[0-9.]+Hz"' | grep -qE "\"${SBS_WIDTH}x"
    fi
}

xreal_ctl() {
    [[ -n $XREAL_CTL ]] || { echo "!! xreal-ctl not found (build contrib/xreal, put it on PATH, or set XREAL_CTL)" >&2; return 1; }
    run "$XREAL_CTL" "$@"
}

# Is the currently-active XR session using OUR xreal runtime manifest? (so `flat` only disables ours)
xr_session_is_xreal() {
    local status
    status="$(hyprctl openxr status 2>/dev/null || true)"
    [[ -n $status ]] || return 1
    printf '%s' "$status" | grep -qiE "^runtime json: .*$(basename "$(dirname "$RUNTIME_JSON")")/$(basename "$RUNTIME_JSON")$" \
        || printf '%s' "$status" | grep -qF "$RUNTIME_JSON"
}

# ---- commands ----
cmd_status() {
    local mon
    echo "== xreal-mode status =="
    if mon="$(detect_monitor)"; then
        echo "glasses DP output: $mon"
        local cur
        cur="$(hyprctl -j monitors all 2>/dev/null | { have_jq && jq -r --arg m "$mon" '.[] | select(.name==$m) | "\(.width)x\(.height)@\(.refreshRate)"' || cat; } 2>/dev/null | head -n1)"
        echo "current DP mode:   ${cur:-<unknown>}"
    else
        echo "glasses DP output: <not detected> (unplugged, or set XREAL_MONITOR)"
    fi
    if [[ -n $XREAL_CTL ]]; then
        echo -n "device HID:        "; "$XREAL_CTL" detect 2>&1 || true
    else
        echo "device HID:        xreal-ctl not found"
    fi
    echo "xreal runtime:     $RUNTIME_JSON $( [[ -f $RUNTIME_JSON ]] && echo '(built)' || echo '(NOT built — scripts/build-monado.sh --xreal)')"
    echo "systemd unit:      $XREAL_SERVICE_UNIT -> $(systemctl --user is-active "$XREAL_SERVICE_UNIT" 2>/dev/null; true)"
    echo "--- hyprctl openxr status ---"
    hyprctl openxr status 2>/dev/null || echo "(no HypXRland / openxr status unavailable)"
}

cmd_xr() {
    local width mon
    if [[ $MONO -eq 1 ]]; then width=$MONO_WIDTH; else width=$SBS_WIDTH; fi
    echo "== xreal-mode xr ($( [[ $MONO -eq 1 ]] && echo 'mono 1920x1080' || echo 'SBS 3840x1080' )) =="

    # 1. Require the glasses present (HID). Safe no-op when unplugged.
    if [[ -n $XREAL_CTL ]] && [[ $DRY_RUN -eq 0 ]] && ! "$XREAL_CTL" detect >/dev/null 2>&1; then
        echo "!! no XREAL device detected over HID — plug the glasses in first (nothing changed)." >&2
        exit 1
    fi

    # 2. HID display-mode switch (2D mono <-> 3D SBS). In mono we stay 2D.
    if [[ $MONO -eq 1 ]]; then
        xreal_ctl mode 2d
    else
        xreal_ctl mode 3d
    fi

    # 3. Detect the DP connector and set its mode. For SBS, wait for the 3840-wide mode to appear after
    #    the HID switch (the glasses re-present a wider EDID); fall back with a clear message if it never
    #    shows (then the user can retry with --mono).
    if ! mon="$(detect_monitor)"; then
        echo "!! could not detect the glasses' DP output — set XREAL_MONITOR=<name> and retry." >&2
        exit 1
    fi
    echo "  glasses DP output: $mon"
    if [[ $MONO -eq 0 ]]; then
        if [[ $DRY_RUN -eq 0 ]]; then
            local i=0
            until monitor_has_wide_mode "$mon"; do
                i=$((i+1)); [[ $i -le 20 ]] || { echo "!! $mon never advertised a ${SBS_WIDTH}-wide mode after the HID 3D switch." >&2
                    echo "   The DP link did not pick up SBS. Retry, or use: xreal-mode.sh xr --mono" >&2; exit 1; }
                sleep 0.25
            done
        fi
        run hyprctl keyword monitor "$mon,${SBS_WIDTH}x1080,auto,1"
    else
        run hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1"
    fi

    # 4. Start the xreal monado runtime via its systemd user unit (à la wivrn). Import the Wayland env
    #    first so comp_main can connect to Hyprland. NEVER touches wivrn.service.
    run systemctl --user import-environment WAYLAND_DISPLAY XDG_RUNTIME_DIR
    run systemctl --user start "$XREAL_SERVICE_UNIT"

    # 5. Point HypXRland at the xreal runtime manifest and (re)enable the session. Setting runtime_json
    #    then disable+enable re-handshakes against it, bypassing WiVRn's active_runtime.
    run hyprctl keyword openxr:runtime_json "$RUNTIME_JSON"
    run hyprctl openxr disable
    run hyprctl openxr enable

    echo "done. Check: hyprctl openxr status   (state should reach 'active'; runtime json should be $RUNTIME_JSON)"
    echo "The monado comp_main window (app-id 'openxr') should fullscreen on $mon — see example/xreal.conf for the window rules."
}

cmd_flat() {
    echo "== xreal-mode flat =="
    # 1. Disable the XR session ONLY if it is our xreal runtime (leave WiVRn/Quest sessions alone).
    if [[ $DRY_RUN -eq 1 ]] || xr_session_is_xreal; then
        run hyprctl openxr disable
    else
        echo "  (XR session is not the xreal runtime — leaving openxr untouched)"
    fi

    # 2. Stop the xreal monado runtime. Idempotent; never touches wivrn.service.
    run systemctl --user stop "$XREAL_SERVICE_UNIT"

    # 3. HID back to 2D mono. Safe no-op when unplugged (xreal-ctl exits non-zero, which we tolerate).
    if [[ -n $XREAL_CTL ]]; then
        if [[ $DRY_RUN -eq 1 ]]; then
            xreal_ctl mode 2d
        else
            "$XREAL_CTL" mode 2d || echo "  (glasses not present over HID — skipping 2D switch)"
        fi
    fi

    # 4. Restore the DP output to the flat desktop mode (if the connector is present).
    local mon
    if mon="$(detect_monitor)"; then
        run hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1"
    else
        echo "  (glasses DP output not present — nothing to restore)"
    fi
    echo "done. $( [[ -n ${mon:-} ]] && echo "$mon is back to $FLAT_MODE." )"
}

case "$CMD" in
    status) cmd_status ;;
    xr)     cmd_xr ;;
    flat)   cmd_flat ;;
esac
