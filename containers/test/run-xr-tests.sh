#!/usr/bin/env bash
# In-container hermetic `hyprtester --xr` runner (WP2). Runs AS `dev` inside a
# real logind session (entered by scripts/xr-container.sh `test` via
# `machinectl shell dev@.host`). Brings up a headless labwc as the nesting host,
# then runs the XR suite nested into it — nothing from the host crosses in.
#
# Why labwc (and not the stock headless path)?
#   WP2 empirical finding (verified live): HYPRLAND_HEADLESS_ONLY=1 does NOT work
#   in this rootless-podman container. Even with real systemd + logind there is
#   no seat, so aquamarine's DRM/headless backend fails at CBackend::create().
#   hyprtester's runXrSuite (hyprtester/src/main.cpp) tries that stock headless
#   path FIRST, then falls back to nesting into $WAYLAND_DISPLAY. We stand up
#   labwc's wayland-0 and point the fallback at it. labwc is the only light
#   compositor that advertises BOTH protocols aquamarine's nested backend
#   hard-requires: xdg_wm_base >= v6 and zwp_linux_dmabuf_v1 (weston/sway expose
#   only xdg_wm_base v5). So: labwc is the DEFAULT and only working path here;
#   the stock headless attempt is expected to fail fast before the nest succeeds.
#
# Real exit code: `machinectl shell` always exits 0, so the true result is
# written to the sentinel file ($XR_TEST_SENTINEL) — the host wrapper reads that.
#
# Config knobs (env, all optional; the wrapper sets them):
#   XR_GPU_NODE       GPU render node to pin (default /dev/dri/renderD129 = AMD).
#                     Fans out to WLR_RENDER_DRM_DEVICE + HYPRTESTER_XR_GPU.
#   BUILD_DIR         in-container build tree (default /build).
#   SRC_DIR           overlay-mounted repo (default /src).
#   MONADO_SERVICE    monado-service binary (default derived from BUILD_DIR).
#   XR_TEST_SENTINEL  path the real exit code is written to (default
#                     /tmp/hypxrland-xr-tests.exit).
#   XR_TEST_LOG       combined run log (default /tmp/hypxrland-xr-tests.log).
#   HYPRTESTER_HYPXRPAPER  if set, forwarded (enables xr_overlay_composition).
# Positional args are passed through as a hyprtester test-name filter (subset).

set -uo pipefail

XR_GPU_NODE="${XR_GPU_NODE:-/dev/dri/renderD129}"
BUILD="${BUILD_DIR:-/build}"
SRC="${SRC_DIR:-/src}"
MONADO_SERVICE="${MONADO_SERVICE:-$BUILD/monado/src/xrt/targets/service/monado-service}"
SENTINEL="${XR_TEST_SENTINEL:-/tmp/hypxrland-xr-tests.exit}"
RUN_LOG="${XR_TEST_LOG:-/tmp/hypxrland-xr-tests.log}"
LABWC_UNIT="hypxrland-labwc"
TEST_NAMES=("$@")

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"

log() { printf '[xr-tests] %s\n' "$*"; }

# Record the true exit code to the sentinel on every exit path, and stop labwc.
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
[[ -x "$BUILD/hyprtester/hyprtester" ]] || { log "FATAL: hyprtester missing at $BUILD/hyprtester/hyprtester (build first)"; FINAL_RC=3; exit 3; }
[[ -x "$BUILD/Hyprland" ]]              || { log "FATAL: Hyprland missing at $BUILD/Hyprland (build first)"; FINAL_RC=3; exit 3; }
[[ -x "$MONADO_SERVICE" ]] || log "WARNING: monado-service not at $MONADO_SERVICE — XR tests will SKIP (runtime absent)"
[[ -e "$XR_GPU_NODE" ]]    || log "WARNING: GPU node $XR_GPU_NODE does not exist inside the container"

# --- 1. bring up labwc (headless nesting host, GPU-pinned) ---------------------
log "starting labwc headless (WLR_RENDER_DRM_DEVICE=$XR_GPU_NODE)"
systemctl --user reset-failed "$LABWC_UNIT" 2>/dev/null || true
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

# --- 2. verify the protocols aquamarine's nested backend needs -----------------
if command -v wayland-info >/dev/null; then
    proto=$(WAYLAND_DISPLAY="$WL" wayland-info 2>/dev/null | grep -iE 'xdg_wm_base|zwp_linux_dmabuf_v1' || true)
    log "labwc protocol advertisement (aquamarine nesting requirement):"
    printf '%s\n' "$proto" | sed 's/^/[xr-tests]     /'
    printf '%s\n' "$proto" | grep -q 'xdg_wm_base'         || log "WARNING: xdg_wm_base not advertised — nesting will fail"
    printf '%s\n' "$proto" | grep -q 'zwp_linux_dmabuf_v1' || log "WARNING: zwp_linux_dmabuf_v1 not advertised — nesting will fail"
else
    log "wayland-info not installed; skipping protocol verification"
fi

# --- 3. run the suite, nested into labwc ---------------------------------------
# hyprtester resolves its config (xr-test.conf) relative to CWD, so run from the
# hyprtester source dir (overlay-mounted, writable — xr_config_declared rewrites
# and restores xr-test.conf in place).
export WAYLAND_DISPLAY="$WL"
export HYPRTESTER_MONADO_SERVICE="$MONADO_SERVICE"
export HYPRTESTER_XR_GPU="$XR_GPU_NODE"

cd "$SRC/hyprtester" || { log "FATAL: cannot cd $SRC/hyprtester"; FINAL_RC=5; exit 5; }
log "running: hyprtester --xr --binary $BUILD/Hyprland ${TEST_NAMES[*]:-<all>}"
set +e
"$BUILD/hyprtester/hyprtester" --xr --binary "$BUILD/Hyprland" "${TEST_NAMES[@]}" 2>&1 | tee -a "$RUN_LOG"
rc="${PIPESTATUS[0]}"
log "hyprtester exited rc=$rc"
FINAL_RC="$rc"
exit "$rc"
