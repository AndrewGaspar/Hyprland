#!/usr/bin/env bash
# HypXRland desktop preview — no headset needed.
#
# Launches:
#   1. monado-service in WINDOWED mode (no XRT_COMPOSITOR_NULL) with the
#      remote driver: a "Monado" window appears showing the rendered XR space.
#   2. A nested dev Hyprland (build-debug) with the XR extension enabled
#      (scripts/preview-xr.conf): its virtual monitors show up as floating
#      quads inside the Monado window.
#
# Then drive the fake head/controllers interactively with:
#   /home/ajg/code/monado/build/src/xrt/targets/gui/monado-gui remote
#
# Ctrl-C here stops everything. Kills ONLY the PIDs it spawned.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MONADO_BUILD="${MONADO_BUILD:-/home/ajg/code/monado/build}"
HYPRLAND_BIN="$REPO/build-debug/Hyprland"
CONF="$REPO/scripts/preview-xr.conf"
LOGDIR="${TMPDIR:-/tmp}/hypxrland-preview-$$"
mkdir -p "$LOGDIR"

[[ -x $HYPRLAND_BIN ]] || { echo "missing $HYPRLAND_BIN — build first: cmake --build build-debug --target Hyprland"; exit 1; }
[[ -x $MONADO_BUILD/src/xrt/targets/service/monado-service ]] || { echo "missing monado-service under $MONADO_BUILD"; exit 1; }

MONADO_PID=""
HL_PID=""
cleanup() {
    # PID-targeted only — never kill by name (the host compositor is also "Hyprland").
    [[ -n $HL_PID ]] && kill "$HL_PID" 2>/dev/null && sleep 1 && kill -9 "$HL_PID" 2>/dev/null
    [[ -n $MONADO_PID ]] && kill "$MONADO_PID" 2>/dev/null && sleep 1 && kill -9 "$MONADO_PID" 2>/dev/null
    echo "stopped. logs in $LOGDIR"
}
trap cleanup EXIT INT TERM

echo ">> starting windowed monado-service (remote driver, TCP 4242)..."
# XRT_COMPOSITOR_FORCE_XCB: Monado's Wayland window backend asserts with 0
# swapchain images under Hyprland (surface not configured before swapchain
# creation); the X11-via-XWayland window path works.
env P_OVERRIDE_ACTIVE_CONFIG=remote XRT_NO_STDIN=1 XRT_COMPOSITOR_FORCE_XCB=1 \
    "$MONADO_BUILD/src/xrt/targets/service/monado-service" \
    >"$LOGDIR/monado.log" 2>&1 &
MONADO_PID=$!
sleep 3
kill -0 "$MONADO_PID" 2>/dev/null || { echo "monado-service died, see $LOGDIR/monado.log"; exit 1; }

echo ">> starting nested dev Hyprland with XR enabled..."
# Snapshot existing instance signatures so we can spot the new one.
HYPR_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/hypr"
mapfile -t SIGS_BEFORE < <(ls -1 "$HYPR_DIR" 2>/dev/null)

env XR_RUNTIME_JSON="$MONADO_BUILD/openxr_monado-dev.json" \
    "$HYPRLAND_BIN" --config "$CONF" \
    >"$LOGDIR/hyprland.log" 2>&1 &
HL_PID=$!

# Find the nested instance's signature (new dir in $HYPR_DIR).
SIG=""
for _ in $(seq 1 20); do
    sleep 0.5
    kill -0 "$HL_PID" 2>/dev/null || { echo "nested Hyprland died, see $LOGDIR/hyprland.log"; exit 1; }
    for d in "$HYPR_DIR"/*/; do
        b=$(basename "$d")
        [[ " ${SIGS_BEFORE[*]} " == *" $b "* ]] || { SIG=$b; break 2; }
    done
done
[[ -n $SIG ]] || echo "warning: could not detect nested instance signature; use 'hyprctl instances'"

cat <<EOF

  Two new windows should appear on your desktop:
    - the nested Hyprland (normal desktop view of the virtual monitors)
    - the Monado compositor window (the 3D XR view with floating quads)

  Nested instance signature: ${SIG:-<run 'hyprctl instances'>}

  Drive the fake HMD and controllers (third window, run in another terminal):
    $MONADO_BUILD/src/xrt/targets/gui/monado-gui remote
      -> tick 'active' on left/right controller, then use the pose sliders,
         trigger (select = click), squeeze (grab = move a quad).

  Talk to the nested instance (paste-ready):
    hyprctl -i $SIG openxr status
    hyprctl -i $SIG dispatch xrmonitor create XR-demo 1280x720 anchor:local pos:0.8,1.4,-1.4
    hyprctl -i $SIG openxr layout

  Watch the socket2 events (bar integration surface):
    socat - UNIX-CONNECT:$HYPR_DIR/$SIG/.socket2.sock | grep -E 'openxr|xrmonitor'

  Known env quirk on this box (dual GPU): the Monado session can die with heap
  corruption after some minutes — just Ctrl-C and rerun.

  Ctrl-C to stop.
EOF

wait "$HL_PID"
HL_PID=""
