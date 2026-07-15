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
FLAT_MODE="${XREAL_FLAT_MODE:-1920x1080@90}"
SBS_WIDTH=3840
MONO_WIDTH=1920
# The glasses do NOT re-advertise a 3840-wide EDID mode after the HID 3D switch on every host (on the
# Framework 16 / amdgpu the connector re-probes but only ever exposes its native 1920x1080 DTDs), so we
# FORCE an unadvertised 3840x1080 CVT reduced-blanking modeline. @60Hz / 266.5MHz pixel clock ≈ 6.4Gbit/s
# which is *below* the glasses' proven link budget (native 1920x1080@120 = 297MHz/7.1Gbit/s works), and
# comfortably inside a 2-lane HBR2 (10.8Gbit/s) DP-alt link. The glasses' internal scaler accepts the
# forced signal and hardware-splits it left-half→left eye / right-half→right eye. Overridable; keep it at
# or below 72Hz (90Hz+ at 3840 wide exceeds the 2-lane budget → out-of-range/black).
SBS_MODELINE="${XREAL_SBS_MODELINE:-266.50 3840 3888 3920 4000 1080 1083 1093 1111 +hsync -vsync}"

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

# Map a Hyprland connector name (e.g. "DP-5") to its /sys DRM connector dir (e.g. card2-DP-5).
sys_connector_dir() {
    local mon="$1" d
    for d in /sys/class/drm/card*-"$mon"; do
        [[ -e $d ]] && { printf '%s' "$d"; return 0; }
    done
    return 1
}

# Find the DRM render node (renderD12X) of the GPU that drives a given connector, so we can point
# HypXRland's OpenXR EGL/composite node at the SAME GPU that scans out the glasses (else cross-GPU
# EGL import crashes — on this box DP-5 is the AMD 890M / renderD129, not the NVIDIA renderD128 that
# the WiVRn config pins by default). Prints e.g. "/dev/dri/renderD129"; empty on failure.
render_node_for_connector() {
    local mon="$1" conn card pci r
    conn="$(sys_connector_dir "$mon")" || return 1   # e.g. /sys/class/drm/card2-DP-5
    # The connector's own "device" link resolves to the DRM card, not the PCI device; resolve the PCI
    # device via the CARD (card2/device) so it matches renderD*/device.
    card="${conn##*/}"                                # card2-DP-5
    card="/sys/class/drm/${card%%-*}"                 # /sys/class/drm/card2
    pci="$(readlink -f "$card/device" 2>/dev/null)" || return 1
    [[ -n $pci ]] || return 1
    for r in /sys/class/drm/renderD*; do
        [[ -e $r/device ]] || continue
        if [[ "$(readlink -f "$r/device" 2>/dev/null)" == "$pci" ]]; then
            printf '/dev/dri/%s' "$(basename "$r")"
            return 0
        fi
    done
    return 1
}

# Width Hyprland currently reports for a monitor (0 if absent).
monitor_width() {
    local mon="$1" json
    json="$(hyprctl -j monitors all 2>/dev/null || hyprctl -j monitors 2>/dev/null || true)"
    if have_jq; then
        printf '%s' "$json" | jq -r --arg m "$mon" 'first(.[] | select(.name==$m) | .width) // 0'
    else
        printf '%s' "$json" | tr ',' '\n' | grep -A2 "\"name\": *\"$mon\"" | grep -oE '"width": *[0-9]+' | head -n1 | grep -oE '[0-9]+' || echo 0
    fi
}

# Is the glasses' DP connector physically present ("connected") at the kernel DRM layer?
connector_is_connected() {
    local mon="$1" conn
    conn="$(sys_connector_dir "$mon")" || return 1
    [[ "$(cat "$conn/status" 2>/dev/null)" == "connected" ]]
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
    echo "== xreal-mode xr ($( [[ $MONO -eq 1 ]] && echo 'mono 1920x1080 single-view' || echo 'SBS 3840x1080 stereo' )) =="

    # 1. Require the glasses present (HID). Safe no-op when unplugged.
    if [[ -n $XREAL_CTL ]] && [[ $DRY_RUN -eq 0 ]] && ! "$XREAL_CTL" detect >/dev/null 2>&1; then
        echo "!! no XREAL device detected over HID — plug the glasses in first (nothing changed)." >&2
        exit 1
    fi

    if ! mon="$(detect_monitor)"; then
        echo "!! could not detect the glasses' DP output — set XREAL_MONITOR=<name> and retry." >&2
        exit 1
    fi
    echo "  glasses DP output: $mon"

    # 2. End any active XR session first, then release the HID device by stopping monado, so the HID
    #    display-mode switch below is uncontended AND — crucially — monado re-reads the display mode when
    #    it restarts. The xreal_air driver picks stereo-vs-mono view geometry ONCE, at create time, from
    #    whatever mode the glasses report over HID (control_display_mode → switch_display_mode). So the
    #    HID switch MUST precede the monado (re)start or comp_main comes up with the wrong view count.
    run hyprctl openxr disable
    run systemctl --user stop "$XREAL_SERVICE_UNIT"

    # 3. HID display-mode switch (2D mono <-> 3D SBS). In mono we stay 2D (the driver then collapses the
    #    second view to 1x1 — a genuine single 1920x1080 view, not a squished stereo pack).
    if [[ $MONO -eq 1 ]]; then
        xreal_ctl mode 2d
    else
        xreal_ctl mode 3d
    fi

    # 4. Set the DP output mode. Mono uses the native 1920x1080. SBS FORCES the unadvertised 3840x1080
    #    modeline (the glasses don't re-advertise a wide EDID on this host) and then VERIFIES the connector
    #    actually came up 3840-wide and still connected — if not, it REVERTS to the flat mode and 2D so the
    #    desktop is never left on a dead mode, then bails.
    if [[ $MONO -eq 1 ]]; then
        run hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1"
    else
        echo "  forcing ${SBS_WIDTH}x1080 modeline on $mon (glasses hardware-split L/R)…"
        run hyprctl keyword monitor "$mon,modeline ${SBS_MODELINE},auto,1"
        if [[ $DRY_RUN -eq 0 ]]; then
            local i=0 w=0
            while :; do
                w="$(monitor_width "$mon")"
                connector_is_connected "$mon" && [[ "$w" == "$SBS_WIDTH" ]] && break
                i=$((i+1)); [[ $i -le 12 ]] || {
                    echo "!! $mon did not come up ${SBS_WIDTH}-wide (width=$w, connected=$(connector_is_connected "$mon" && echo yes || echo no))." >&2
                    echo "   Reverting $mon to $FLAT_MODE + glasses to 2D and aborting SBS (desktop restored)." >&2
                    hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1" >/dev/null 2>&1 || true
                    [[ -n $XREAL_CTL ]] && "$XREAL_CTL" mode 2d >/dev/null 2>&1 || true
                    echo "   Retry, or use: xreal-mode.sh xr --mono" >&2
                    exit 1
                }
                sleep 0.25
            done
            echo "  $mon is up ${SBS_WIDTH}x1080 (connected)."
        fi
    fi

    # 5. Nudge the DP output through a dpms off/on cycle to wake the panel (the glasses' panel can sleep
    #    across the mode switch and stay dark until re-driven).
    run hyprctl dispatch dpms off "$mon"
    run hyprctl dispatch dpms on "$mon"

    # 6. Point HypXRland's OpenXR EGL/composite node at the GPU that actually scans out the glasses, and
    #    force an opaque blend mode (the birdbath is additive/see-through by default; without opaque the
    #    black background stays transparent and the surface can't go solitary/direct-scanout). openxr:gpu
    #    is auto-detected from the connector's DRM render node so this does not depend on the running
    #    config's default (the WiVRn config pins the NVIDIA node, which would cross-GPU-crash here).
    local gpu
    if gpu="$(render_node_for_connector "$mon")"; then
        echo "  openxr:gpu -> $gpu (drives $mon)"
        run hyprctl keyword openxr:gpu "$gpu"
    else
        echo "  (could not auto-detect $mon's render node — leaving openxr:gpu at the running config)"
    fi
    run hyprctl keyword openxr:blend_mode opaque

    # 7. (Re)start the xreal monado runtime AFTER the HID mode switch so the driver reads the current mode
    #    and builds comp_main at the matching width. Import the Wayland env first. NEVER touches wivrn.
    run systemctl --user import-environment WAYLAND_DISPLAY XDG_RUNTIME_DIR
    run systemctl --user restart "$XREAL_SERVICE_UNIT"

    # 7b. Wait for monado's IPC socket before enabling — `systemctl restart` returns as soon as the process
    #     is forked, but comp_main takes a beat to init Vulkan and bind $XDG_RUNTIME_DIR/monado_comp_ipc.
    #     Enabling before the socket exists fails with "OpenXR runtime unavailable".
    if [[ $DRY_RUN -eq 0 ]]; then
        local sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/monado_comp_ipc" j=0
        until [[ -S $sock ]]; do
            j=$((j+1)); [[ $j -le 40 ]] || { echo "  (monado_comp_ipc never appeared — enabling anyway)"; break; }
            sleep 0.25
        done
        sleep 0.5   # small extra settle after the socket binds
    fi

    # 8. Point HypXRland at the xreal runtime manifest and enable the session (re-handshakes against it,
    #    bypassing WiVRn's active_runtime).
    run hyprctl keyword openxr:runtime_json "$RUNTIME_JSON"
    run hyprctl openxr enable

    echo "done. Check: hyprctl openxr status   (state should reach 'focused'; runtime json: $RUNTIME_JSON)"
    echo "The monado comp_main window (class 'openxr', title 'Monado') should be fullscreen on $mon at"
    echo "${width}x1080 — verify with: hyprctl clients -j | jq '.[]|select(.class==\"openxr\")|{size,fullscreen}'"
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

    # 4. Restore the DP output to the flat desktop mode (if the connector is present). After coming back
    #    from a forced 3840 SBS modeline the connector re-probes, so set the native mode and verify it
    #    recovered; nudge it through dpms if it looks wedged.
    local mon
    if mon="$(detect_monitor)"; then
        run hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1"
        if [[ $DRY_RUN -eq 0 ]]; then
            sleep 0.5
            local w; w="$(monitor_width "$mon")"
            if [[ "$w" != "$MONO_WIDTH" ]] || ! connector_is_connected "$mon"; then
                echo "  ($mon width=$w — re-issuing $FLAT_MODE + dpms nudge to recover)"
                hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1" >/dev/null 2>&1 || true
                hyprctl dispatch dpms off "$mon" >/dev/null 2>&1 || true
                hyprctl dispatch dpms on "$mon" >/dev/null 2>&1 || true
                sleep 0.5; w="$(monitor_width "$mon")"
            fi
            echo "  $mon width now: $w (connected=$(connector_is_connected "$mon" && echo yes || echo no))"
        fi
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
