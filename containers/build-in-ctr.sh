#!/usr/bin/env bash
# In-container build of the dev Hyprland + hyprtester + vendored Monado.
# Run as `dev` inside the HypXRland container (from `xr-container.sh shell`, or
# scripted). Source is the overlay-mounted /src (host tree untouched); ALL build
# output goes to the /build volume (never the source tree, never the host's
# build-debug/).
#
#   source:  /src            (repo, overlay mount)
#   build:   /build          (hypxrland-build volume)
#   ccache:  ~/.cache/ccache (hypxrland-ccache volume)
#
# Monado is redirected out of its in-source subprojects/monado/build into
# /build/monado (MONADO_BUILD) + /build/eigen (EIGEN_BUILD) — see the overrides
# added to scripts/build-monado.sh.

set -euo pipefail

SRC=/src
BUILD=/build

# Build discipline (this box froze under uncoordinated heavy load): cap parallel
# jobs. Override with HYPXRLAND_BUILD_JOBS. Exported so build-monado.sh sees it.
_np=$(nproc); JOBS=${HYPXRLAND_BUILD_JOBS:-8}; [[ $JOBS -gt $_np ]] && JOBS=$_np
export HYPXRLAND_BUILD_JOBS="$JOBS"

command -v ccache >/dev/null && export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"

echo "==> ccache stats (before):"
ccache -s 2>/dev/null || true

# The image's Arch mirror is Omarchy's FROZEN snapshot, which may ship deps older
# (wayland-protocols < 1.49) or more broken (hyprutils 0.13.1 virtual-inheritance
# casts) than the tree requires — configure then depends on the vendored
# subprojects/{wayland-protocols,hyprutils} submodules. /src is an overlay that
# snapshots at container CREATE, so a submodule checked out on the host after
# the container was created is invisible here; fail early with the fix.
for sub in wayland-protocols/stable hyprutils/include; do
    if [[ ! -e $SRC/subprojects/$sub ]]; then
        echo "!! subprojects/${sub%%/*} not present in /src — on the HOST run:" >&2
        echo "     git submodule update --init subprojects/${sub%%/*}" >&2
        echo "   then recreate this container (the /src overlay snapshots at create)." >&2
        echo "   Continuing; configure will fail (or the build will crash at runtime)" >&2
        echo "   if the image's system copy is too old." >&2
    fi
done

echo "==> Configuring Hyprland (Ninja, Debug, tests + XR tests) into $BUILD"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_TESTS=ON \
    -DWITH_XR_TESTS=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# hyprtester spawns small Wayland helper CLIENTS (hyprtester/CMakeLists.txt `clientNew()`) as
# separate executables out of the same build dir; nothing links them into `hyprtester`, so a
# --target list that omits them leaves them missing. Tests that need one then SKIP — and a skip
# counts as a PASS in this harness, so the suite stays green while silently losing coverage
# (xr_idle_inhibit / xr_idle_inhibit_modes need `idle-notify`, xr_plugged_* need `pointer-scroll`).
# Build them explicitly — this list must stay in sync with `clientNew()` in that CMakeLists, or
# the missing ones go back to skipping silently.
HYPRTESTER_CLIENTS=(pointer-warp surface-scale-transform pointer-scroll child-window xdg-interactive shortcut-inhibitor keyboard-modifiers idle-notify layer-surface output-info screencopy-crop screencopy-probe)

echo "==> Building targets: Hyprland hyprtester ${HYPRTESTER_CLIENTS[*]}"
cmake --build "$BUILD" --target Hyprland hyprtester "${HYPRTESTER_CLIENTS[@]}" -j"$JOBS"

echo "==> Building vendored Monado into $BUILD/monado (redirected out of read-only source)"
MONADO_BUILD="$BUILD/monado" EIGEN_BUILD="$BUILD/eigen" bash "$SRC/scripts/build-monado.sh"

# hypxrpaper (ambient XR backgrounds) — the primary OpenXR session the container
# `session --env` mode composites HypXRland's monitors over. Built OUT OF TREE into
# $BUILD/hypxrpaper because /src is a read-only overlay (same reason monado is
# redirected above). Its deps (openxr/egl/glesv2/gbm/libdrm) are already in the
# image's Hyprland dep set; stb_image + cgltf are vendored in the submodule's
# third_party/. Assets (the bundled forest-clearing scene) are NOT copied — they
# ride along in the read-only /src checkout and are located at session time via
# HYPXRPAPER_ASSET_DIR (see containers/session/session-launch.sh); an out-of-tree
# $BUILD binary's exe-relative search would never find /src's assets.
HYPXRPAPER_SRC="$SRC/subprojects/hypxrpaper"
HYPXRPAPER_BUILD="$BUILD/hypxrpaper"
if [[ -f $HYPXRPAPER_SRC/CMakeLists.txt ]]; then
    echo "==> Configuring hypxrpaper into $HYPXRPAPER_BUILD (out-of-tree; read-only /src)"
    cmake -S "$HYPXRPAPER_SRC" -B "$HYPXRPAPER_BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    echo "==> Building hypxrpaper"
    cmake --build "$HYPXRPAPER_BUILD" --target hypxrpaper -j"$JOBS"
else
    echo "!! hypxrpaper submodule not checked out at $HYPXRPAPER_SRC — run:" >&2
    echo "     git submodule update --init subprojects/hypxrpaper" >&2
    echo "   (skipping; container 'session --env' will be unavailable)" >&2
fi

echo "==> ccache stats (after):"
ccache -s 2>/dev/null || true

echo "==> Build artifacts:"
for f in "$BUILD/Hyprland" \
         "$BUILD/hyprtester/hyprtester" \
         "$BUILD/monado/src/xrt/targets/service/monado-service" \
         "$BUILD/monado/openxr_monado-dev.json" \
         "$BUILD/hypxrpaper/hypxrpaper"; do
    if [[ -e $f ]]; then echo "   OK  $f"; else echo "   MISSING  $f"; fi
done
echo "==> Done."
