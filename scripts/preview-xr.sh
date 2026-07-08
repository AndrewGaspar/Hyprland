#!/usr/bin/env bash
# HypXRland desktop preview — no headset needed.
#
# Launches:
#   1. monado-service in WINDOWED mode (no XRT_COMPOSITOR_NULL) with the
#      remote driver: a "Monado" window appears showing the rendered XR space.
#   2. (optional, with --env) hypxrpaper as the PRIMARY session, drawing an
#      ambient background (gradient sky / panorama / 3D scene). HypXRland then
#      runs as an XR_EXTX_overlay session composited on top of it.
#   3. A nested dev Hyprland (build-debug) with the XR extension enabled
#      (scripts/preview-xr.conf): its virtual monitors show up as floating
#      quads inside the Monado window.
#
# Then drive the fake head/controllers interactively with:
#   subprojects/monado/build/src/xrt/targets/gui/monado-gui remote
#
# Ctrl-C here stops everything. Kills ONLY the PIDs it spawned.
#
# Usage: preview-xr.sh [--wivrn] [--passthrough] [--env <spec>]
#   --wivrn                 use the system WiVRn runtime (real headset!) instead of
#                           launching the vendored windowed monado-service. Requires
#                           wivrn-server running with the headset connected.
#   --passthrough           set openxr:blend_mode = alpha so monitors composite over
#                           the real world (needs a runtime/HMD with passthrough,
#                           e.g. Quest 3 via --wivrn; Monado null only does opaque).
#   --env pano              gradient-sky panorama background (hypxrpaper, no args)
#   --env forest            bundled 'forest-clearing' 3D scene (--scene forest-clearing)
#   --env <path>            *.hdr/*.png/*.jpg -> equirect panorama; else -> --scene <path>
# When --env is given, hypxrpaper is discovered via $HYPXRPAPER_BIN, then
# `hypxrpaper` on PATH, and openxr:overlay is enabled in the generated config.

set -euo pipefail

ENV_SPEC=""
USE_WIVRN=0
PASSTHROUGH=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wivrn)
            USE_WIVRN=1; shift ;;
        --passthrough)
            PASSTHROUGH=1; shift ;;
        --env)
            [[ $# -ge 2 ]] || { echo "--env requires an argument (pano | forest | <path>)"; exit 2; }
            ENV_SPEC="$2"; shift 2 ;;
        --env=*)
            ENV_SPEC="${1#--env=}"; shift ;;
        -h|--help)
            grep -E '^# ' "${BASH_SOURCE[0]}" | sed 's/^# //'; exit 0 ;;
        *)
            echo "unknown argument: $1"; echo "usage: preview-xr.sh [--wivrn] [--passthrough] [--env <spec>]"; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MONADO_BUILD="${MONADO_BUILD:-$REPO/subprojects/monado/build}"
HYPRLAND_BIN="$REPO/build-debug/Hyprland"
LOGDIR="${TMPDIR:-/tmp}/hypxrland-preview-$$"
mkdir -p "$LOGDIR"

[[ -x $HYPRLAND_BIN ]] || { echo "missing $HYPRLAND_BIN — build first: cmake --build build-debug --target Hyprland"; exit 1; }

if [[ $USE_WIVRN -eq 1 ]]; then
    # Real-headset mode: talk to the system WiVRn runtime instead of our vendored Monado.
    RUNTIME_JSON="${WIVRN_RUNTIME_JSON:-/usr/share/openxr/1/openxr_wivrn.json}"
    [[ -f $RUNTIME_JSON ]] || { echo "WiVRn runtime manifest not found at $RUNTIME_JSON (set WIVRN_RUNTIME_JSON)"; exit 1; }
    pgrep -x wivrn-server >/dev/null || {
        echo "wivrn-server is not running — start it (wivrn-dashboard) and connect the headset first"
        exit 1
    }
    echo ">> WiVRn mode: using $RUNTIME_JSON (make sure the headset client is CONNECTED,"
    echo "   or session creation will fail)"
else
    [[ -x $MONADO_BUILD/src/xrt/targets/service/monado-service ]] || {
        echo "missing monado-service under $MONADO_BUILD — run scripts/build-monado.sh first (or set MONADO_BUILD)"
        exit 1
    }
    RUNTIME_JSON="$MONADO_BUILD/openxr_monado-dev.json"
fi

# Merged launch config: the tracked preview config, an optional untracked machine-local
# override (same file the test harness uses), and an optional XR_GPU env pin — machine
# specifics stay out of the tracked files (dual-GPU: Hyprland must match Monado's GPU).
CONF="$LOGDIR/preview-merged.conf"
{
    echo "source = $REPO/scripts/preview-xr.conf"
    [[ -f $REPO/hyprtester/xr-test-local.conf ]] && echo "source = $REPO/hyprtester/xr-test-local.conf"
    [[ -n ${XR_GPU:-} ]] && echo "openxr:gpu = $XR_GPU"
    # Overlay mode ONLY when an ambient background is requested: HypXRland then composites its
    # monitors on top of the hypxrpaper primary session instead of owning the whole view.
    [[ -n $ENV_SPEC ]] && echo "openxr:overlay = 1"
    [[ $PASSTHROUGH -eq 1 ]] && echo "openxr:blend_mode = alpha"
    true
} > "$CONF"

if [[ $PASSTHROUGH -eq 1 && -n $ENV_SPEC ]]; then
    echo "note: --passthrough + --env: the ambient background is an opaque primary session," \
         "so it will cover the real-world view; passthrough only shows through where nothing renders."
fi

# Resolve the GPU render node the merged config pins Hyprland to (dual-GPU boxes), so we can hand
# hypxrpaper the SAME node via --gpu — a cross-GPU primary would crash Monado at swapchain time.
# Prefer $XR_GPU, else scrape openxr:gpu out of the untracked local override.
XR_GPU_NODE="${XR_GPU:-}"
if [[ -z $XR_GPU_NODE && -f $REPO/hyprtester/xr-test-local.conf ]]; then
    XR_GPU_NODE="$(sed -nE 's/^[[:space:]]*openxr:gpu[[:space:]]*=[[:space:]]*(\S+).*/\1/p' \
        "$REPO/hyprtester/xr-test-local.conf" | tail -n1)"
fi

MONADO_PID=""
HL_PID=""
PAPER_PID=""
cleanup() {
    # PID-targeted only — never kill by name (the host compositor is also "Hyprland").
    [[ -n $HL_PID ]] && kill "$HL_PID" 2>/dev/null && sleep 1 && kill -9 "$HL_PID" 2>/dev/null
    [[ -n $PAPER_PID ]] && kill "$PAPER_PID" 2>/dev/null && sleep 1 && kill -9 "$PAPER_PID" 2>/dev/null
    [[ -n $MONADO_PID ]] && kill "$MONADO_PID" 2>/dev/null && sleep 1 && kill -9 "$MONADO_PID" 2>/dev/null
    echo "stopped. logs in $LOGDIR"
}
trap cleanup EXIT INT TERM

if [[ $USE_WIVRN -eq 0 ]]; then
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
fi

# Optional ambient background: hypxrpaper as the PRIMARY OpenXR session, under HypXRland's overlay.
if [[ -n $ENV_SPEC ]]; then
    PAPER_BIN="${HYPXRPAPER_BIN:-$(command -v hypxrpaper || true)}"
    [[ -n $PAPER_BIN && -x $PAPER_BIN ]] || {
        echo "hypxrpaper not found — set \$HYPXRPAPER_BIN to its path or put 'hypxrpaper' on \$PATH"
        exit 1
    }

    # Map the --env spec to hypxrpaper args.
    PAPER_ARGS=()
    case "$ENV_SPEC" in
        pano)   ;; # no args = built-in gradient sky
        forest) PAPER_ARGS=(--scene forest-clearing) ;;
        *.hdr|*.HDR|*.png|*.PNG|*.jpg|*.JPG|*.jpeg|*.JPEG)
                PAPER_ARGS=("$ENV_SPEC") ;;         # equirectangular panorama (positional)
        *)      PAPER_ARGS=(--scene "$ENV_SPEC") ;; # a .glb/.gltf/scene.json or bundled scene name
    esac
    # Same GPU node the merged config pins Hyprland to (cross-GPU primary crashes Monado).
    [[ -n $XR_GPU_NODE ]] && PAPER_ARGS+=(--gpu "$XR_GPU_NODE")

    echo ">> starting hypxrpaper ambient background (--env $ENV_SPEC)..."
    env XR_RUNTIME_JSON="$RUNTIME_JSON" \
        "$PAPER_BIN" "${PAPER_ARGS[@]}" \
        >"$LOGDIR/hypxrpaper.log" 2>&1 &
    PAPER_PID=$!
    sleep 2
    kill -0 "$PAPER_PID" 2>/dev/null || { echo "hypxrpaper died, see $LOGDIR/hypxrpaper.log"; exit 1; }
fi

echo ">> starting nested dev Hyprland with XR enabled..."
# Snapshot existing instance signatures so we can spot the new one.
HYPR_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/hypr"
mapfile -t SIGS_BEFORE < <(ls -1 "$HYPR_DIR" 2>/dev/null)

env XR_RUNTIME_JSON="$RUNTIME_JSON" \
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

if [[ $USE_WIVRN -eq 1 ]]; then
cat <<EOF

  WiVRn mode: put the headset ON — the floating monitors render there, not in
  a desktop window. The nested Hyprland window on your desktop is the same
  content in flat form (and where keyboard/mouse input goes).

  If nothing appears in the headset, check:
    grep -iE 'openxr|session|instance' $LOGDIR/hyprland.log
  (a headset that isn't connected, or a runtime without XR_MNDX_egl_enable,
  fails at instance/session creation with a clear error there)
EOF
else
cat <<EOF

  Two new windows should appear on your desktop:
    - the nested Hyprland (normal desktop view of the virtual monitors)
    - the Monado compositor window (the 3D XR view with floating quads)

  Drive the fake HMD and controllers (third window, run in another terminal):
    $MONADO_BUILD/src/xrt/targets/gui/monado-gui remote
      -> tick 'active' on left/right controller, then use the pose sliders,
         trigger (select = click), squeeze (grab = move a quad).

  Known env quirk on this box (dual GPU): the Monado session can die with heap
  corruption after some minutes — just Ctrl-C and rerun.
EOF
fi

cat <<EOF
${ENV_SPEC:+"
  Ambient background: hypxrpaper (--env $ENV_SPEC) is the PRIMARY session; HypXRland
  runs as an XR_EXTX_overlay on top of it (openxr:overlay = 1). Its monitors should
  float over the ambient scene instead of a black void. Confirm the overlay took:
    hyprctl -i \$SIG openxr status    # -> overlay: yes
  (hypxrpaper log: $LOGDIR/hypxrpaper.log)
"}
  Nested instance signature: ${SIG:-<run 'hyprctl instances'>}

  Talk to the nested instance (paste-ready):
    hyprctl -i $SIG openxr status
    hyprctl -i $SIG dispatch xrmonitor create XR-demo 1280x720 anchor:local pos:0.8,1.4,-1.4
    hyprctl -i $SIG openxr layout

  Watch the socket2 events (bar integration surface):
    socat - UNIX-CONNECT:$HYPR_DIR/$SIG/.socket2.sock | grep -E 'openxr|xrmonitor'

  Ctrl-C to stop.
EOF

wait "$HL_PID"
HL_PID=""
