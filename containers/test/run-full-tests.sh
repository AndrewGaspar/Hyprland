#!/usr/bin/env bash
# In-container hermetic runner for the FULL (non-XR) `hyprtester` suite — the 178
# cases in tests/{main,clients,misc}, 108 of which spawn kitty. Runs AS `dev`
# inside a real logind session (entered by scripts/xr-container.sh `test --full`
# via `machinectl shell dev@.host`), nested into a headless labwc exactly like its
# sibling run-xr-tests.sh.
#
# Why this exists: the host cannot run this suite. It has no kitty, and a non-`--xr`
# hyprtester on the host can select the developer's LIVE compositor. The container
# has kitty (containers/omarchy-install-ctr.sh) and no live session to hit, so it is
# the only place the kitty-dependent set runs at all.
#
# Why nested (and how it differs from --xr):
#   Rootless podman is seatless, so aquamarine's DRM/headless path fails at
#   CBackend::create() — the same WP2 finding run-xr-tests.sh documents. runXrSuite
#   handles that itself (it retries nested); the NORMAL suite has no such fallback,
#   it launches once and gives up. Two things make the nest happen anyway:
#     * Hyprland no longer reads HYPRLAND_HEADLESS_ONLY (Compositor.cpp asks for
#       HEADLESS mandatory / DRM if-available / WAYLAND fallback), so hyprtester
#       setting it does not pin the launch to headless, and
#     * the compositor inherits our environment, so exporting WAYLAND_DISPLAY at
#       labwc's socket is enough for the wayland fallback backend to take.
#   Nothing in hyprtester needs patching; we just have to hand it a display.
#
# Config knobs (env, all optional; the wrapper sets them):
#   XR_GPU_NODE         GPU render node to pin (labwc's WLR_RENDER_DRM_DEVICE). If
#                       unset, resolved by vendor scan (scripts/lib/gpu.sh, amd).
#   BUILD_DIR           in-container build tree (default /build).
#   SRC_DIR             overlay-mounted repo (default /src).
#   FULL_TEST_SENTINEL  path the real exit code is written to (machinectl shell
#                       always exits 0) — default /tmp/hypxrland-full-tests.exit.
#   FULL_TEST_LOG       combined run log (default /tmp/hypxrland-full-tests.log).
# Positional args are passed through as a hyprtester test-name filter (subset).

set -uo pipefail

if [[ -z ${XR_GPU_NODE:-} ]]; then
    if [[ -r /src/scripts/lib/gpu.sh ]]; then
        # shellcheck source=../../scripts/lib/gpu.sh
        source /src/scripts/lib/gpu.sh
        XR_GPU_NODE="$(resolve_render_node amd 2>/dev/null || true)"
    fi
fi
BUILD="${BUILD_DIR:-/build}"
SRC="${SRC_DIR:-/src}"
SENTINEL="${FULL_TEST_SENTINEL:-/tmp/hypxrland-full-tests.exit}"
RUN_LOG="${FULL_TEST_LOG:-/tmp/hypxrland-full-tests.log}"
LABWC_UNIT="hypxrland-labwc"
TEST_NAMES=("$@")

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"

log() { printf '[full-tests] %s\n' "$*"; }

FINAL_RC=99
finish() {
    systemctl --user stop "$LABWC_UNIT"        2>/dev/null || true
    systemctl --user reset-failed "$LABWC_UNIT" 2>/dev/null || true
    echo "$FINAL_RC" >"$SENTINEL"
    log "wrote sentinel $SENTINEL = $FINAL_RC"
}
trap finish EXIT INT TERM

rm -f "$SENTINEL"
: >"$RUN_LOG"

# --- sanity --------------------------------------------------------------------
command -v labwc >/dev/null || { log "FATAL: labwc not installed"; FINAL_RC=2; exit 2; }
command -v kitty >/dev/null || log "WARNING: kitty not installed — 108 of the 178 cases will fail"
[[ -x "$BUILD/hyprtester/hyprtester" ]] || { log "FATAL: hyprtester missing at $BUILD/hyprtester/hyprtester (build first)"; FINAL_RC=3; exit 3; }
[[ -x "$BUILD/Hyprland" ]]              || { log "FATAL: Hyprland missing at $BUILD/Hyprland (build first)"; FINAL_RC=3; exit 3; }
[[ -f "$SRC/hyprtester/plugin/hyprtestplugin.so" ]] || { log "FATAL: hyprtestplugin.so missing (build first)"; FINAL_RC=3; exit 3; }
[[ -e "$XR_GPU_NODE" ]] || log "WARNING: GPU node $XR_GPU_NODE does not exist inside the container"

# --- 1. bring up labwc (headless nesting host, GPU-pinned) ---------------------
# Clear stale sockets first: a compositor under test is SIGKILLed at the end of a
# run and leaves its wayland-N behind, so "the first wayland-* in the dir" can name
# a socket nobody is listening on — which looks exactly like a nesting failure.
systemctl --user stop "$LABWC_UNIT"        2>/dev/null || true
systemctl --user reset-failed "$LABWC_UNIT" 2>/dev/null || true
rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true

log "starting labwc headless (WLR_RENDER_DRM_DEVICE=$XR_GPU_NODE)"
systemd-run --user --quiet --unit="$LABWC_UNIT" \
    --setenv=XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    --setenv=WLR_BACKENDS=headless \
    --setenv=WLR_LIBINPUT_NO_DEVICES=1 \
    --setenv=WLR_RENDERER_ALLOW_SOFTWARE=1 \
    --setenv=WLR_RENDER_DRM_DEVICE="$XR_GPU_NODE" \
    labwc

WL=""
for _ in $(seq 1 30); do
    WL=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
    [[ -n $WL ]] && break
    if [[ "$(systemctl --user is-active "$LABWC_UNIT" 2>/dev/null)" == "failed" ]]; then
        log "FATAL: labwc failed to start:"
        journalctl --user -u "$LABWC_UNIT" --no-pager 2>/dev/null | tail -20
        FINAL_RC=4; exit 4
    fi
    sleep 0.5
done
[[ -n $WL ]] || { log "FATAL: labwc never created a wayland socket"; FINAL_RC=4; exit 4; }
log "labwc up on socket: $WL"

# --- 2. run the suite, nested into labwc ---------------------------------------
# hyprtester resolves test.lua and the plugin relative to CWD, so run from the
# hyprtester source dir (overlay-mounted, writable — tests rewrite config in place).
export WAYLAND_DISPLAY="$WL"

cd "$SRC/hyprtester" || { log "FATAL: cannot cd $SRC/hyprtester"; FINAL_RC=5; exit 5; }
log "running: hyprtester --binary $BUILD/Hyprland ${TEST_NAMES[*]:-<all>}"
set +e
"$BUILD/hyprtester/hyprtester" --binary "$BUILD/Hyprland" \
    --config ./test.lua --plugin ./plugin/hyprtestplugin.so \
    "${TEST_NAMES[@]}" 2>&1 | tee -a "$RUN_LOG"
rc="${PIPESTATUS[0]}"
log "hyprtester exited rc=$rc"
FINAL_RC="$rc"
exit "$rc"
