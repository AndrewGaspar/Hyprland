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

command -v ccache >/dev/null && export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache
export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"

echo "==> ccache stats (before):"
ccache -s 2>/dev/null || true

echo "==> Configuring Hyprland (Ninja, Debug, tests + XR tests) into $BUILD"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_TESTS=ON \
    -DWITH_XR_TESTS=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

echo "==> Building targets: Hyprland hyprtester"
cmake --build "$BUILD" --target Hyprland hyprtester -j"$(nproc)"

echo "==> Building vendored Monado into $BUILD/monado (redirected out of read-only source)"
MONADO_BUILD="$BUILD/monado" EIGEN_BUILD="$BUILD/eigen" bash "$SRC/scripts/build-monado.sh"

echo "==> ccache stats (after):"
ccache -s 2>/dev/null || true

echo "==> Build artifacts:"
for f in "$BUILD/Hyprland" \
         "$BUILD/hyprtester/hyprtester" \
         "$BUILD/monado/src/xrt/targets/service/monado-service" \
         "$BUILD/monado/openxr_monado-dev.json"; do
    if [[ -e $f ]]; then echo "   OK  $f"; else echo "   MISSING  $f"; fi
done
echo "==> Done."
