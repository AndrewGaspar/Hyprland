#!/usr/bin/env bash
# In-container session launcher. Run as `dev` inside a real logind session
# (entered by scripts/xr-container.sh `session` via `machinectl shell`). Brings
# up the dev Hyprland with the XR extension, nesting into the bind-mounted host
# wayland socket, against either:
#   windowed  — a vendored windowed Monado (no headset) whose window appears on
#               the host desktop via the host XWayland socket, OR
#   wivrn     — the host WiVRn runtime (real headset). This container NEVER runs
#               wivrn-server; it only talks to the host's over a bind-mounted
#               socket.
#
# The Omarchy autostart chain (waybar/mako/walker/portals via uwsm-app, from the
# base config's `source`) comes up on THIS container's private system/session
# buses — the whole point of the container. Nothing is shimmed.
#
# Env inputs (set inline by the wrapper's `machinectl shell … bash -lc` command):
#   XR_MODE            windowed | wivrn                                (required)
#   XR_GPU_NODE        render node -> openxr:gpu (XR encode side)      (required)
#   NESTED_GPU_NODE    render node -> AQ_DRM_DEVICES (nested compositor
#                      / host-compositor side); default = XR_GPU_NODE
#                      (single-GPU). Split-GPU --wivrn sets this to the
#                      host GPU while XR_GPU_NODE is the encode GPU.
#   XR_RUNTIME_JSON    OpenXR runtime manifest                        (required)
#   HL_WAYLAND_DISPLAY absolute path of the bind-mounted host socket  (required)
#   XR_BASE_CONF       base config to source (default ~/.config/hypr/hyprland.conf)
#   XR_OVERLAY         1 -> openxr:overlay = 1 (auto-forced to 1 when XR_ENV is set)
#   XR_PASSTHROUGH     1 -> openxr:blend_mode = alpha
#   XR_ENV             ambient-background spec (pano | forest | <path>); when set,
#                      hypxrpaper is launched as the PRIMARY OpenXR session and
#                      HypXRland runs as an XR_EXTX_overlay on top of it (the
#                      preview-xr.sh --env model, adapted for the container).
#   HYPXRPAPER_BIN     (XR_ENV) hypxrpaper path (default /build/hypxrpaper/hypxrpaper)
#   MONADO_BIN         (windowed) monado-service path (default /build/monado/…)
#   XR_X_DISPLAY       (windowed) X display for the container-local Xwayland (:9)
#   WIVRN_HOST_SOCK    (wivrn) bind-mounted host wivrn comp_ipc socket
#   HL_PIPEWIRE_SOCK   host PipeWire socket bind-mounted under /hypxrland-host
#                      (native-PipeWire clients); unset => no native share
#   HL_PULSE_SOCK      host PulseAudio native socket bind-mounted under
#                      /hypxrland-host (Pulse-shim clients: chromium, pactl);
#                      unset => no pulse share. Both unset (host --no-audio or no
#                      host PipeWire) => the session runs with no audio.
#   HYPRLAND_BIN       dev Hyprland (default /build/Hyprland)
set -uo pipefail

: "${XR_MODE:?}"; : "${XR_GPU_NODE:?}"; : "${XR_RUNTIME_JSON:?}"; : "${HL_WAYLAND_DISPLAY:?}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# Omarchy's helper scripts (omarchy-launch-walker, omarchy-menu, ...) live in the
# cloned tree's bin/, wired onto PATH by omarchy's shell setup on a real install —
# a host-oriented stage our curated install skips. Every omarchy-* keybind needs
# them, so wire PATH here: Hyprland's exec children inherit it, and the omarchy
# autostart's import-environment propagates it to the systemd user manager for
# uwsm-app-routed launches.
export PATH="$HOME/.local/share/omarchy/bin:$PATH"

# The image's theme install runs with OMARCHY_THEME_SKIP_BACKGROUND=1 (no
# compositor at build time), deferring the wallpaper selection to us: create
# ~/.config/omarchy/current/background (symlink into the theme's backgrounds/)
# if it doesn't exist yet, or swaybg renders a black desktop.
[ -e "$HOME/.config/omarchy/current/background" ] || omarchy-theme-bg-next >/dev/null 2>&1 || true

# --- host audio (PipeWire / PulseAudio) --------------------------------------
# The host audio daemon is shared in via read-only socket bind mounts under
# /hypxrland-host (xr-container.sh cmd_session). We run NO audio daemon in the
# container; instead we symlink the host sockets onto the DEFAULT client
# discovery paths in $XDG_RUNTIME_DIR, so every client resolves them with no env
# plumbing (incl. anything the systemd user manager launches — same $XDG_RUNTIME_DIR):
#   pipewire-0    -> native-PipeWire clients (pw-cli, native apps)
#   pulse/native  -> PulseAudio-shim clients (chromium, pactl, paplay/parecord)
# connect() follows the symlink and reaches the host daemon cross-namespace by the
# socket inode (proven: pactl/pw-cli info work over a :ro mount). The image ships
# pipewire.service/.socket (but NEITHER is enabled, and there is no pipewire-pulse/
# wireplumber daemon pkg) — mask them defensively so a stray socket-activation
# can't bind $XDG_RUNTIME_DIR/pipewire-0 over our symlink. Both socket vars unset
# (host --no-audio or no host PipeWire) => this block no-ops and apps run silent.
HL_PIPEWIRE_SOCK="${HL_PIPEWIRE_SOCK:-}"
HL_PULSE_SOCK="${HL_PULSE_SOCK:-}"
if [[ -n $HL_PIPEWIRE_SOCK || -n $HL_PULSE_SOCK ]]; then
    systemctl --user mask --now \
        pipewire.socket pipewire.service pipewire-pulse.socket \
        pipewire-pulse.service wireplumber.service >/dev/null 2>&1 || true
    audio_linked=()
    if [[ -n $HL_PIPEWIRE_SOCK && -S $HL_PIPEWIRE_SOCK ]]; then
        ln -sf "$HL_PIPEWIRE_SOCK" "$XDG_RUNTIME_DIR/pipewire-0"
        audio_linked+=("pipewire-0")
    fi
    if [[ -n $HL_PULSE_SOCK && -S $HL_PULSE_SOCK ]]; then
        mkdir -p "$XDG_RUNTIME_DIR/pulse"
        ln -sf "$HL_PULSE_SOCK" "$XDG_RUNTIME_DIR/pulse/native"
        audio_linked+=("pulse/native")
    fi
    if ((${#audio_linked[@]})); then
        echo "   audio: host PipeWire shared read-only (in-container daemons masked; linked ${audio_linked[*]})"
    else
        echo "   audio: sockets requested but absent under /hypxrland-host — session runs silent (non-fatal)" >&2
    fi
else
    echo "   audio: not shared (--no-audio or host has no PipeWire) — session runs silent"
fi

# --- nest into the host wayland compositor -----------------------------------
# WAYLAND_DISPLAY is an ABSOLUTE path (starts with /), so libwayland connects to
# it directly and skips the XDG_RUNTIME_DIR 0700-owner check entirely.
export AQ_BACKENDS=wayland
export WAYLAND_DISPLAY="$HL_WAYLAND_DISPLAY"
# Nested compositor renders on the host-compositor GPU (NESTED_GPU_NODE); the XR
# encode side (openxr:gpu, set in the merged config) uses XR_GPU_NODE. On a
# single-GPU run NESTED_GPU_NODE is unset and both collapse to XR_GPU_NODE.
NESTED_GPU_NODE="${NESTED_GPU_NODE:-$XR_GPU_NODE}"
export AQ_DRM_DEVICES="$NESTED_GPU_NODE"
export XR_RUNTIME_JSON
HYPRLAND_BIN="${HYPRLAND_BIN:-/build/Hyprland}"
[[ -x $HYPRLAND_BIN ]] || { echo "!! dev Hyprland missing at $HYPRLAND_BIN — build it first (bash /src/containers/build-in-ctr.sh)" >&2; exit 6; }

echo "== HypXRland container session =="
echo "   mode=$XR_MODE  nested-gpu(AQ)=$NESTED_GPU_NODE  xr-gpu(openxr)=$XR_GPU_NODE"
echo "   nesting into host WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
echo "   XR_RUNTIME_JSON=$XR_RUNTIME_JSON"

# --- merged launch config ----------------------------------------------------
# An ambient background (XR_ENV) means HypXRland composites its monitors ON TOP of
# the hypxrpaper primary session, so it MUST be an overlay — force openxr:overlay=1.
XR_ENV="${XR_ENV:-}"
XR_OVERLAY="${XR_OVERLAY:-0}"
[[ -n $XR_ENV ]] && XR_OVERLAY=1
MERGED="$XDG_RUNTIME_DIR/hyprland-xr-merged.conf"
XR_GPU_NODE="$XR_GPU_NODE" \
XR_OVERLAY="$XR_OVERLAY" \
XR_PASSTHROUGH="${XR_PASSTHROUGH:-0}" \
XR_BASE_CONF="${XR_BASE_CONF:-$HOME/.config/hypr/hyprland.conf}" \
    bash /src/containers/session/merge-conf.sh "$MERGED" >/dev/null
echo "   merged config -> $MERGED"

# --- runtime-specific bring-up ----------------------------------------------
case "$XR_MODE" in
    wivrn)
        # The WiVRn client (Monado IPC) hardcodes $XDG_RUNTIME_DIR/wivrn/comp_ipc.
        # Symlink it to the bind-mounted host socket — connect() follows the
        # symlink, and an AF_UNIX socket is reachable cross-namespace by its inode.
        sock="${WIVRN_HOST_SOCK:-/hypxrland-host/wivrn-comp_ipc}"
        if [[ -S $sock ]]; then
            mkdir -p "$XDG_RUNTIME_DIR/wivrn"
            ln -sf "$sock" "$XDG_RUNTIME_DIR/wivrn/comp_ipc"
            echo "   wivrn socket -> $XDG_RUNTIME_DIR/wivrn/comp_ipc  ($sock)"
        else
            echo "!! WiVRn socket not found at $sock." >&2
            echo "   Is wivrn-server running on the HOST with the headset connected?" >&2
            echo "   (Session creation would fail; exiting cleanly.)" >&2
            exit 3
        fi
        if ldd /usr/lib/wivrn/libopenxr_wivrn.so 2>&1 | grep -qi 'not found'; then
            echo "!! WiVRn client library has unresolved deps:" >&2
            ldd /usr/lib/wivrn/libopenxr_wivrn.so 2>&1 | grep -i 'not found' >&2
        fi
        ;;
    windowed)
        MONADO_BIN="${MONADO_BIN:-/build/monado/src/xrt/targets/service/monado-service}"
        [[ -x $MONADO_BIN ]] || { echo "!! monado-service missing at $MONADO_BIN — run build-in-ctr.sh" >&2; exit 4; }
        command -v Xwayland >/dev/null || { echo "!! Xwayland missing (install xorg-xwayland)" >&2; exit 7; }

        # Monado's windowed compositor needs an X server (its Wayland window backend
        # asserts with 0 swapchain images under Hyprland — XRT_COMPOSITOR_FORCE_XCB).
        # Rather than depend on a host XWayland (lazy; may be down; /tmp bind mounts
        # are shadowed by the container's systemd tmpfs anyway), run a CONTAINER-LOCAL
        # rooted Xwayland that connects to the same host wayland socket — its single X
        # screen appears as one window on the host desktop, and Monado draws into it.
        local_dpy="${XR_X_DISPLAY:-:9}"
        echo ">> starting container Xwayland $local_dpy (rooted, on the host compositor)"
        Xwayland "$local_dpy" -geometry 1600x900 >"$XDG_RUNTIME_DIR/xwayland.log" 2>&1 &
        XWL_PID=$!
        for _ in $(seq 1 20); do
            [[ -S /tmp/.X11-unix/X${local_dpy#:} ]] && break
            kill -0 "$XWL_PID" 2>/dev/null || { echo "!! Xwayland died:" >&2; tail -20 "$XDG_RUNTIME_DIR/xwayland.log" >&2; exit 7; }
            sleep 0.3
        done
        [[ -S /tmp/.X11-unix/X${local_dpy#:} ]] || { echo "!! Xwayland X socket never appeared" >&2; exit 7; }
        echo "   Xwayland up (pid $XWL_PID, DISPLAY=$local_dpy); log $XDG_RUNTIME_DIR/xwayland.log"

        echo ">> starting windowed monado-service (remote driver, DISPLAY=$local_dpy)"
        env P_OVERRIDE_ACTIVE_CONFIG=remote XRT_NO_STDIN=1 XRT_COMPOSITOR_FORCE_XCB=1 \
            DISPLAY="$local_dpy" \
            "$MONADO_BIN" >"$XDG_RUNTIME_DIR/monado.log" 2>&1 &
        MONADO_PID=$!
        sleep 3
        if ! kill -0 "$MONADO_PID" 2>/dev/null; then
            echo "!! monado-service died — last 20 log lines:" >&2
            tail -20 "$XDG_RUNTIME_DIR/monado.log" >&2
            exit 5
        fi
        echo "   monado-service up (pid $MONADO_PID); log $XDG_RUNTIME_DIR/monado.log"
        ;;
    *) echo "unknown XR_MODE: $XR_MODE (want windowed|wivrn)" >&2; exit 2 ;;
esac

# --- optional ambient background (hypxrpaper as the PRIMARY session) ----------
# Launched AFTER the runtime is up (windowed monado / wivrn socket ready) and
# BEFORE Hyprland, so the primary session exists when HypXRland's overlay joins.
# It runs on the SAME runtime (XR_RUNTIME_JSON, exported above) and the XR/encode
# GPU (XR_GPU_NODE — in split mode that's the runtime-compositor GPU, which is
# where a primary session must render; a cross-GPU primary crashes the runtime).
# Backgrounded and PID-tracked here; `podman rm -f` reaps it with the whole tree
# (same as the windowed monado above), so no explicit teardown is needed.
if [[ -n $XR_ENV ]]; then
    PAPER_BIN="${HYPXRPAPER_BIN:-/build/hypxrpaper/hypxrpaper}"
    [[ -x $PAPER_BIN ]] || { echo "!! hypxrpaper missing at $PAPER_BIN — run build-in-ctr.sh (or set HYPXRPAPER_BIN)" >&2; exit 8; }

    # Bundled scenes (e.g. forest-clearing) live in the read-only /src checkout.
    # An out-of-tree /build binary's exe-relative search can't find them, so point
    # hypxrpaper's asset search at the submodule's assets dir explicitly.
    export HYPXRPAPER_ASSET_DIR="${HYPXRPAPER_ASSET_DIR:-/src/subprojects/hypxrpaper/assets}"

    # Map the --env spec to hypxrpaper args (mirrors scripts/preview-xr.sh).
    PAPER_ARGS=()
    case "$XR_ENV" in
        pano)   ;;                                   # no args = built-in gradient sky
        forest) PAPER_ARGS=(--scene forest-clearing) ;;
        *.hdr|*.HDR|*.png|*.PNG|*.jpg|*.JPG|*.jpeg|*.JPEG)
                PAPER_ARGS=("$XR_ENV") ;;            # equirectangular panorama (positional)
        *)      PAPER_ARGS=(--scene "$XR_ENV") ;;    # a .glb/.gltf/scene.json or bundled scene name
    esac
    [[ -n $XR_GPU_NODE ]] && PAPER_ARGS+=(--gpu "$XR_GPU_NODE")

    echo ">> starting hypxrpaper ambient background (--env $XR_ENV; assets=$HYPXRPAPER_ASSET_DIR)"
    "$PAPER_BIN" "${PAPER_ARGS[@]}" >"$XDG_RUNTIME_DIR/hypxrpaper.log" 2>&1 &
    PAPER_PID=$!
    sleep 2
    if ! kill -0 "$PAPER_PID" 2>/dev/null; then
        echo "!! hypxrpaper died — last 20 log lines:" >&2
        tail -20 "$XDG_RUNTIME_DIR/hypxrpaper.log" >&2
        exit 9
    fi
    echo "   hypxrpaper up (pid $PAPER_PID); log $XDG_RUNTIME_DIR/hypxrpaper.log"
fi

echo ">> exec dev Hyprland (--config $MERGED)"
# exec: Hyprland becomes the session leader's foreground process, so the wrapper's
# attached `machinectl shell` terminal shows its logs. Container teardown
# (podman rm -f) reaps the whole tree, including the backgrounded monado above.
exec "$HYPRLAND_BIN" --config "$MERGED"
