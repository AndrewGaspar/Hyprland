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
#   --direct     for `xr`: LEASE the glasses' DP connector to Monado (DRM-lease / direct mode, V2.2)
#                instead of the default fullscreen-window path. HypXRland stops driving DP-5 as a desktop
#                and offers it via wp_drm_lease_v1; Monado's direct-wayland backend leases it and owns the
#                flip (one fewer compositor hop → lower latency). Requires `monitor=DP-5,...,lease` support
#                in the running HypXRland binary (build + relog). WINDOW mode is the default/fallback; run
#                `xreal-mode.sh flat` to reclaim DP-5 as a normal desktop monitor. See docs 07-xreal.md.
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
DIRECT=0
CMD=""
for arg in "$@"; do
    case "$arg" in
        xr|flat|status) CMD="$arg" ;;
        --dry-run) DRY_RUN=1 ;;
        --mono) MONO=1 ;;
        --direct) DIRECT=1 ;;
        -h|--help) grep -E '^#( |$)' "$0" | sed 's/^#\ \?//'; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; echo "usage: xreal-mode.sh {xr|flat|status} [--dry-run] [--mono] [--direct]" >&2; exit 2 ;;
    esac
done
[[ -n $CMD ]] || { echo "usage: xreal-mode.sh {xr|flat|status} [--dry-run] [--mono] [--direct]" >&2; exit 2; }

# ---- configuration (all overridable for machine-agnosticism) ----
MONADO_XREAL_BUILD="${MONADO_XREAL_BUILD:-$REPO/subprojects/monado/build-xreal}"
RUNTIME_JSON="${XREAL_RUNTIME_JSON:-$MONADO_XREAL_BUILD/openxr_monado-dev.json}"
XREAL_CTL="${XREAL_CTL:-}"
XREAL_SERVICE_UNIT="${XREAL_SERVICE_UNIT:-monado-xreal.service}"
# systemd drop-in that flips the monado unit from window mode (its default Environment=XRT_COMPOSITOR_FORCE_
# WAYLAND=1) to DRM-lease direct mode. Written by `xr --direct`, removed by window mode / `flat`, so the
# default (no drop-in) is always the window path.
DROPIN_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/${XREAL_SERVICE_UNIT}.d"
DIRECT_DROPIN="$DROPIN_DIR/10-xreal-direct.conf"
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

# Does the given connector's KERNEL DRM mode list advertise a mode >= SBS_WIDTH wide? Reads
# /sys/class/drm/card*-<mon>/modes — the GROUND TRUTH for what Monado's direct-mode Vulkan
# VkDisplayModeKHR enumeration will see on the LEASED connector. After the HID 3D switch the glasses
# drop the DP link (~0.5s) and re-present a native EDID that advertises ONLY the 3840x1080 SBS mode; we
# must gate the lease flip on THIS (not just hyprctl availableModes) so Monado leases a connector whose
# mode list already contains 3840 and its auto mode-select (max pixels) picks 3840, not a stale 1920.
connector_has_wide_mode() {
    local mon="$1" conn
    conn="$(sys_connector_dir "$mon")" || return 1
    [[ "$(cat "$conn/status" 2>/dev/null)" == "connected" ]] || return 1
    awk -F x -v w="$SBS_WIDTH" '$1+0 >= w {found=1} END{exit !found}' "$conn/modes" 2>/dev/null
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

# DRM-lease direct mode: write / remove the systemd drop-in that selects Monado's direct-wayland backend
# (which leases the named connector as a wp_drm_lease_v1 client). Both are idempotent + dry-run aware.
enable_direct_dropin() {
    local mon="$1"
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] write %s (FORCE_WAYLAND_DIRECT=1, WAYLAND_CONNECTOR=%s) + daemon-reload\n' "$DIRECT_DROPIN" "$mon"
        return 0
    fi
    mkdir -p "$DROPIN_DIR"
    cat > "$DIRECT_DROPIN" <<EOF
# Auto-generated by scripts/xreal-mode.sh --direct. Removed by window mode / \`flat\`.
# DRM-lease direct mode: unset FORCE_WAYLAND (window backend) and select the direct-wayland backend,
# which leases connector $mon via wp_drm_lease_v1 and owns the flip. See docs/openxr/07-xreal.md.
#
# NOTE: the base unit sets Environment=XRT_COMPOSITOR_FORCE_WAYLAND=1. We must UNSET it, not set it
# empty: monado reads options with getenv() (u_debug.c get_option_raw), so an EMPTY var is still a
# non-NULL string, and debug_string_to_bool("") returns TRUE (empty matches none of the "false"/"0"
# cases). An empty FORCE_WAYLAND therefore stays TRUE, and comp_settings.c checks force_wayland AFTER
# force_wayland_direct — so it clobbers target_identifier back to "wayland" (the windowed backend) and
# direct mode never engages. UnsetEnvironment= removes it entirely so getenv() returns NULL → false.
[Service]
UnsetEnvironment=XRT_COMPOSITOR_FORCE_WAYLAND
Environment=XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1
Environment=XRT_COMPOSITOR_WAYLAND_CONNECTOR=$mon
# Direct-scanout pacing (fake pacer; RADV exposes no VK_GOOGLE_display_timing so the
# non-feedback pacer is the only option — it phase-locks to the VK_EXT_display_control
# FIRST_PIXEL_OUT vblank events on the leased connector):
#  - present->display offset: with the xreal_air driver reporting rolling-scanout info the
#    compositor samples a begin-of-scanout pose (photon time of row 0 ~= the first-pixel-out
#    vblank the pacer locks to) and an end-of-scanout pose (+16.0ms), so the old mid-frame
#    guess (default 4ms, "based on Index") is wrong here; 1ms ~= DP link + panel row latency
#    (and is the pacer's documented minimum).
#  - min compositor time: the wake->flip budget. The default 3.33ms (20% of 16.7ms) has no
#    headroom when the iGPU is contended by desktop/XR rendering (monado only gets a MEDIUM
#    global-priority queue without CAP_SYS_NICE), and every overrun is a FIFO flip slipping a
#    full 60Hz period = a visible stutter. 8ms covers the measured contention tails (worst ~6.5ms under a saturated iGPU); the
#    pose for the frame is still *predicted* for its display time, so the cost is prediction
#    horizon, not extra perceived lag.
Environment=U_PACING_COMP_PRESENT_TO_DISPLAY_OFFSET_MS=1.0
Environment=U_PACING_COMP_MIN_TIME_MS=8.0
EOF
    printf '  + wrote %s (direct-wayland, connector %s)\n' "$DIRECT_DROPIN" "$mon"
    systemctl --user daemon-reload
}
disable_direct_dropin() {
    [[ -f $DIRECT_DROPIN ]] || return 0
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] rm %s + daemon-reload (revert to window mode)\n' "$DIRECT_DROPIN"
        return 0
    fi
    run rm -f "$DIRECT_DROPIN"
    systemctl --user daemon-reload
}

# True iff Monado is CURRENTLY running in DRM-lease direct mode — i.e. it holds the glasses' DP connector's
# DRM lease. Proxy: the direct drop-in is installed AND the monado unit is active. When this is true, Monado
# owns the leased CRTC, so it MUST be stopped (lease released) before anything disables the XR session or
# modesets that connector — see stop_monado_for_lease_release().
direct_mode_active() {
    [[ -f $DIRECT_DROPIN ]] || return 1
    systemctl --user is-active --quiet "$XREAL_SERVICE_UNIT"
}

# Stop Monado and let Hyprland observe the DRM-lease release BEFORE the caller disables the XR session or
# reclaims/modesets the connector. CRITICAL anti-deadlock ordering for direct mode:
#   Disabling the XR session (hyprctl openxr disable) or reclaiming the DP connector to a desktop WHILE
#   Monado still holds the lease deadlocks the compositor — Hyprland's main thread blocks on a synchronous
#   OpenXR IPC teardown (xrDestroySession/xrDestroyInstance) against a Monado that is itself blocked on its
#   leased DP flip / lease-release (which needs Hyprland's wayland thread — the very thread that is blocked).
#   That cross-process deadlock hangs the WHOLE session (eDP included → hard reboot).
# `systemctl --user stop` blocks until the unit's process is reaped; reaping closes BOTH the OpenXR IPC
# socket (so a later openxr-disable hits a dead socket and returns fast, never blocking) AND the DRM-lease
# fd (so the kernel revokes the lease). We then wait for the unit to report inactive and give Hyprland's
# wayland loop a beat to process Monado's disconnect and clear the connector's leased state.
stop_monado_for_lease_release() {
    echo "  direct mode active — stopping Monado FIRST to release the DP lease (anti-deadlock ordering)."
    run systemctl --user stop "$XREAL_SERVICE_UNIT"
    [[ $DRY_RUN -eq 1 ]] && return 0
    local i=0
    while [[ $i -lt 20 ]]; do
        systemctl --user is-active --quiet "$XREAL_SERVICE_UNIT" || break
        i=$((i+1)); sleep 0.1
    done
    sleep 0.5   # let Hyprland's wayland loop drop the lease (monitor m_isBeingLeased -> false) before we proceed
}

# Is the currently-active XR session using OUR xreal runtime manifest? (so `flat` only disables ours)
xr_session_is_xreal() {
    local status
    status="$(hyprctl openxr status 2>/dev/null || true)"
    [[ -n $status ]] || return 1
    printf '%s' "$status" | grep -qiE "^runtime json: .*$(basename "$(dirname "$RUNTIME_JSON")")/$(basename "$RUNTIME_JSON")$" \
        || printf '%s' "$status" | grep -qF "$RUNTIME_JSON"
}

# Robustly land the Monado comp_main window (Wayland app-id "openxr", title "Monado") FULLSCREEN on the
# glasses' DP output. The window is a floating toplevel; a windowrule alone is unreliable (it only fires
# at map time, and only if this profile's xreal.conf windowrule is actually sourced), so we ALSO move it
# to the monitor and fullscreen it explicitly, by address. Idempotent; safe to call repeatedly.
place_openxr_window() {
    local mon="$1" addr i m
    # Session-scoped windowrule so any freshly-created openxr window also lands correctly.
    run hyprctl keyword windowrule "monitor $mon, fullscreen 1, border_size 0, no_anim 1, no_blur 1, immediate 1, match:class ^(openxr)\$"
    [[ $DRY_RUN -eq 1 ]] && return 0
    have_jq || { echo "  (jq not found — relying on the windowrule to place the openxr window)"; return 0; }
    # Wait for the openxr window to exist (comp_main opens its toplevel a beat after the socket binds).
    addr=""
    for i in $(seq 1 40); do
        addr="$(hyprctl -j clients 2>/dev/null | jq -r '.[]|select(.class=="openxr")|.address' | head -n1)"
        [[ -n $addr && $addr != null ]] && break
        addr=""; sleep 0.25
    done
    [[ -n $addr ]] || { echo "  (openxr window never appeared — check: hyprctl clients)"; return 1; }
    # Move to the glasses' output, then fullscreen deterministically (fullscreenstate 2 2 = full).
    hyprctl dispatch focuswindow "address:$addr" >/dev/null 2>&1 || true
    hyprctl dispatch movewindow "mon:$mon" >/dev/null 2>&1 || true
    sleep 0.4
    hyprctl dispatch focuswindow "address:$addr" >/dev/null 2>&1 || true
    hyprctl dispatch fullscreenstate 2 2 >/dev/null 2>&1 || true
    sleep 0.4
    # Report where it landed (monitor is an ID in clients json; map it to a name).
    m="$(hyprctl -j clients 2>/dev/null | jq -r --arg a "$addr" '.[]|select(.address==$a)|.monitor')"
    m="$(hyprctl -j monitors all 2>/dev/null | jq -r --arg id "$m" '.[]|select((.id|tostring)==$id)|.name')"
    echo "  openxr window placed on: ${m:-<unknown>} (want $mon), fullscreen requested"
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
    echo "== xreal-mode xr ($( [[ $MONO -eq 1 ]] && echo 'mono 1920x1080 single-view' || echo 'SBS 3840x1080 stereo' ), $( [[ $DIRECT -eq 1 ]] && echo 'DRM-lease DIRECT' || echo 'fullscreen WINDOW' ) mode) =="

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

    # 2. If we are ALREADY in DRM-lease direct mode, Monado holds the DP lease — release it FIRST (stop
    #    Monado and wait for Hyprland to drop the lease) BEFORE disabling the XR session. Disabling the
    #    session while the lease is held deadlocks the compositor (full-system hang); this also makes
    #    re-running `xr`/`xr --direct` while already in direct mode a SAFE clean teardown, not a re-entry hang.
    if direct_mode_active; then
        stop_monado_for_lease_release
    fi
    # End any active XR session, then release the HID device by stopping monado, so the HID display-mode
    # switch below is uncontended AND — crucially — monado re-reads the display mode when it restarts. The
    # xreal_air driver picks stereo-vs-mono view geometry ONCE, at create time, from whatever mode the glasses
    # report over HID (control_display_mode → switch_display_mode). So the HID switch MUST precede the monado
    # (re)start or comp_main comes up with the wrong view count. (In direct mode the lease was already released
    # just above, so this openxr-disable runs with no live lease and cannot deadlock.)
    run hyprctl openxr disable
    run systemctl --user stop "$XREAL_SERVICE_UNIT"

    # 3. HID display-mode switch (2D mono <-> 3D SBS). In mono we stay 2D (the driver then collapses the
    #    second view to 1x1 — a genuine single 1920x1080 view, not a squished stereo pack).
    if [[ $MONO -eq 1 ]]; then
        xreal_ctl mode 2d
    else
        xreal_ctl mode 3d
    fi

    # 4. Bring up the glasses' output. DIRECT (lease) mode: flip the HypXRland `lease` monitor-rule flag so
    #    HypXRland stops driving $mon as a desktop and offers the connector via wp_drm_lease_v1 — Monado's
    #    direct-wayland backend then leases it and does its own modeset (so the resolution field here is a
    #    placeholder Hyprland won't apply; the glasses' post-HID-3d native mode is what Monado picks up).
    #    WINDOW mode (default): set the DP output mode ourselves (mono native 1920x1080, or the forced/native
    #    3840x1080 SBS) and VERIFY it, reverting to flat + 2D if it never comes up (desktop never left dead).
    if [[ $DIRECT -eq 1 ]]; then
        # CRITICAL ORDERING: Monado's direct-wayland backend leases $mon and reads the display mode off the
        # LEASED connector's own VkDisplayModeKHR list. After the HID 3D switch the glasses drop the DP link
        # and re-present a native EDID advertising the 3840-wide SBS mode ~1s later. If we flip to `lease`
        # BEFORE that settles, Monado leases a connector whose only mode is the stale 1920x1080 and clamps the
        # SBS frame into 1920 ("Ignoring given extent 3840x1080 and using 1920x1080 from mode") — both eyes
        # squished into one buffer, no per-eye split. So WAIT for the connector's kernel DRM mode list to
        # actually advertise ${SBS_WIDTH}-wide before offering the lease. (Gate on /sys, the exact list Monado
        # enumerates — not hyprctl availableModes.) With 3840 present, Monado's auto mode-select (max pixels)
        # picks 3840x1080 and the glasses hardware-split the side-by-side frame per eye.
        if [[ $DRY_RUN -eq 0 ]]; then
            echo "  waiting for $mon's DRM connector to advertise a native ${SBS_WIDTH}-wide mode after the HID 3D switch…"
            local i=0
            while :; do
                connector_has_wide_mode "$mon" && break
                i=$((i+1)); [[ $i -le 40 ]] || {   # ~10s
                    echo "!! $mon never advertised a ${SBS_WIDTH}-wide mode; leasing anyway would clamp to 1920." >&2
                    echo "   Current DRM modes: $(tr '\n' ' ' < "$(sys_connector_dir "$mon")/modes" 2>/dev/null)" >&2
                    echo "   Reverting glasses to 2D and aborting (desktop restored). Retry, or replug if the glasses wedged." >&2
                    [[ -n $XREAL_CTL ]] && "$XREAL_CTL" mode 2d >/dev/null 2>&1 || true
                    hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1" >/dev/null 2>&1 || true
                    exit 1
                }
                sleep 0.25
            done
            echo "  $mon advertises ${SBS_WIDTH}-wide (DRM connector) — offering the lease."
        fi
        echo "  flipping $mon to LEASE — offered to Monado direct mode; it leaves the desktop."
        run hyprctl keyword monitor "$mon,preferred,auto,1,lease"
    elif [[ $MONO -eq 1 ]]; then
        run hyprctl keyword monitor "$mon,${FLAT_MODE},auto,1"
    elif [[ $DRY_RUN -eq 1 ]]; then
        run hyprctl keyword monitor "$mon,${SBS_WIDTH}x1080@60,auto,1.0"
    else
        # After the HID 3D switch the glasses re-present a NATIVE 3840-wide EDID mode, but the connector
        # first drops and returns (~2s). PREFER that native mode; fall back to a forced CVT modeline only
        # if it never shows up. (The forced modeline is a slightly-off 59.855 Hz AND makes the glasses'
        # internal L/R scaler mis-sample → diagonal striping + no stereo alignment; the native 60 Hz mode
        # is what the scaler expects.) Scale is pinned to 1.0 below: any fractional scale resamples the SBS
        # frame (1 buffer px must map to 1 physical px, else the left/right split softens and can skew).
        echo "  waiting for $mon to re-present a native ${SBS_WIDTH}-wide mode after the HID 3D switch…"
        local i=0 have_wide=""
        while :; do
            if monitor_has_wide_mode "$mon"; then have_wide=1; break; fi
            i=$((i+1)); [[ $i -le 20 ]] || break   # ~5s
            sleep 0.25
        done
        if [[ -n $have_wide ]]; then
            local modestr=""
            if have_jq; then
                modestr="$(hyprctl -j monitors all 2>/dev/null | jq -r --arg m "$mon" --argjson w "$SBS_WIDTH" \
                    '[.[]|select(.name==$m)|.availableModes[]|select((split("x")[0]|tonumber)>=$w)][0] // empty' | sed 's/Hz$//')"
            fi
            [[ -n $modestr ]] || modestr="${SBS_WIDTH}x1080@60"
            echo "  native wide mode advertised ($modestr) — using it."
            run hyprctl keyword monitor "$mon,${modestr},auto,1.0"
        else
            echo "  no native ${SBS_WIDTH} mode appeared — forcing the CVT modeline (glasses hardware-split L/R)…"
            run hyprctl keyword monitor "$mon,modeline ${SBS_MODELINE},auto,1.0"
        fi
        # Verify the connector actually came up ${SBS_WIDTH}-wide and still connected; else revert + bail.
        i=0; local w=0
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

    # 5. Nudge the DP output through a dpms off/on cycle to wake the panel (the glasses' panel can sleep
    #    across the mode switch and stay dark until re-driven). WINDOW mode only — in DIRECT mode HypXRland
    #    no longer owns $mon (it is leased), so Monado drives the panel; a dpms on a leased output is a no-op.
    if [[ $DIRECT -eq 0 ]]; then
        run hyprctl dispatch dpms off "$mon"
        run hyprctl dispatch dpms on "$mon"
    fi

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

    # 7. Select the Monado compositor backend for this mode, then (re)start the xreal monado runtime AFTER
    #    the HID mode switch so the driver reads the current mode. DIRECT writes a drop-in selecting the
    #    direct-wayland (lease) backend; WINDOW removes it so the unit's default FORCE_WAYLAND window backend
    #    applies. Import the Wayland env first. NEVER touches wivrn.
    if [[ $DIRECT -eq 1 ]]; then
        enable_direct_dropin "$mon"
    else
        disable_direct_dropin
    fi
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

    # 9. WINDOW mode: land the Monado comp_main window (app-id "openxr") fullscreen on the glasses' output.
    #    DIRECT mode has NO window — Monado scans out to the leased connector directly — so there is nothing
    #    to place; instead confirm $mon left the desktop (it should be absent from `hyprctl monitors`).
    if [[ $DIRECT -eq 0 ]]; then
        place_openxr_window "$mon"
        echo "done. Check: hyprctl openxr status   (state should reach 'focused'; runtime json: $RUNTIME_JSON)"
        echo "The monado comp_main window (class 'openxr', title 'Monado') should be fullscreen on $mon at"
        echo "${width}x1080 — verify with: hyprctl clients -j | jq '.[]|select(.class==\"openxr\")|{size,fullscreen}'"
    else
        echo "done (DIRECT/lease mode). Verify:"
        echo "  - $mon is NOT a desktop output:  hyprctl monitors | grep -q '$mon' && echo STILL-DESKTOP || echo left-desktop-OK"
        echo "  - it is offered/leased:          hyprctl monitors all -j | jq -r '.[]|select(.name==\"$mon\")|.name'"
        echo "  - Monado holds the lease:        journalctl --user -u $XREAL_SERVICE_UNIT -e | grep -i 'connector id'"
        echo "  - session reaches focused:       hyprctl openxr status   (runtime json: $RUNTIME_JSON)"
        echo "If the lease never grants (no HypXRland offer), the running binary lacks the 'lease' rule flag —"
        echo "rebuild + relog, or fall back to window mode: xreal-mode.sh flat && xreal-mode.sh xr"
    fi
}

cmd_flat() {
    echo "== xreal-mode flat =="
    # 0. If we are in DRM-lease direct mode, Monado holds the DP lease. Release it FIRST — stop Monado and
    #    wait for Hyprland to drop the lease — BEFORE disabling the XR session or reclaiming the connector to
    #    a desktop. Tearing down (openxr disable / DP modeset) while the lease is held deadlocks the compositor
    #    (full-system hang). Once the lease is released the reclaim/modeset in step 4 is safe. See cmd_xr.
    if direct_mode_active; then
        stop_monado_for_lease_release
    fi
    # 1. Disable the XR session ONLY if it is our xreal runtime (leave WiVRn/Quest sessions alone).
    if [[ $DRY_RUN -eq 1 ]] || xr_session_is_xreal; then
        run hyprctl openxr disable
    else
        echo "  (XR session is not the xreal runtime — leaving openxr untouched)"
    fi

    # 2. Stop the xreal monado runtime (releases any DRM lease it holds on $mon). Idempotent; never touches
    #    wivrn.service. Then remove the direct-mode drop-in so a subsequent start is back to window mode.
    run systemctl --user stop "$XREAL_SERVICE_UNIT"
    disable_direct_dropin

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
