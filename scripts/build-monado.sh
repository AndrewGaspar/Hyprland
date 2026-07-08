#!/usr/bin/env bash
# Build the vendored Monado (subprojects/monado, pinned to the commit the XR test
# suite's wire ABI is validated against — see hyprtester/src/xr/monado_remote_wire.hpp).
#
# One-time setup for `hyprtester --xr` and scripts/preview-xr.sh:
#   git submodule update --init subprojects/monado   (this script does it for you)
#   scripts/build-monado.sh
#
# Re-runs are incremental. The result lands in subprojects/monado/build
# (monado-service, monado-gui, openxr_monado-dev.json).
#
# Build-directory overrides (for a READ-ONLY source tree, e.g. the repo
# bind-mounted at /src:ro inside the HypXRland container): set MONADO_BUILD and
# EIGEN_BUILD to point the CMake build trees somewhere writable outside the
# source (e.g. MONADO_BUILD=/build/monado EIGEN_BUILD=/build/eigen). Both default
# to the in-tree paths, so existing invocations are unchanged. The submodule
# sources are still read from subprojects/ (read-only is fine for out-of-tree
# builds); make sure the submodules are already checked out, since this script
# cannot `git submodule update` into a read-only tree.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB="$REPO/subprojects/monado"
EIGEN="$REPO/subprojects/eigen"
VKH="$REPO/subprojects/vulkan-headers"

# Writable build trees — overridable so a read-only source tree can still build.
MONADO_BUILD="${MONADO_BUILD:-$SUB/build}"
EIGEN_BUILD="${EIGEN_BUILD:-$EIGEN/build}"
mkdir -p "$MONADO_BUILD" "$EIGEN_BUILD"

if [[ ! -f $SUB/CMakeLists.txt || ! -f $EIGEN/CMakeLists.txt || ! -d $VKH/include ]]; then
    echo ">> initializing monado/eigen/vulkan-headers submodules..."
    git -C "$REPO" submodule update --init --depth 1 subprojects/monado subprojects/eigen subprojects/vulkan-headers
fi

# Eigen is vendored too (header-only, but Monado finds it in CONFIG mode, so a quick
# configure is needed to generate Eigen3Config.cmake in the build tree — no compilation).
if [[ ! -f $EIGEN_BUILD/Eigen3Config.cmake ]]; then
    echo ">> configuring vendored eigen (header-only, generates the CMake package)..."
    cmake -S "$EIGEN" -B "$EIGEN_BUILD" -DBUILD_TESTING=OFF -DEIGEN_BUILD_DOC=OFF >/dev/null
fi

# The three features the XR test harness and desktop preview require. Everything else
# is Monado's default auto-detection. Making these explicit turns a missing dependency
# into a loud configure failure instead of a silent SKIP at test time.
# Header-only deps come from the vendored submodules (Eigen3 config package above;
# Vulkan headers via the FindVulkan hint — the ICD loader library is still a system dep).
echo ">> configuring monado..."
# -include cstdint: the pinned Monado commit predates GCC 16's stricter libstdc++
# transitive includes (u_extension_list.cpp et al. use std::uint8_t without <cstdint>);
# force-include instead of patching the submodule.
if ! cmake -S "$SUB" -B "$MONADO_BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS="-include cstdint" \
    -DEigen3_DIR="$EIGEN_BUILD" \
    -DVulkan_INCLUDE_DIR="$VKH/include" \
    -DXRT_MODULE_COMPOSITOR_NULL=ON \
    -DXRT_BUILD_DRIVER_REMOTE=ON \
    -DXRT_MODULE_MONADO_GUI=ON \
    -DXRT_BUILD_DRIVER_SURVIVE=OFF; then
    cat >&2 <<'EOF'

Monado configure failed. Likely missing build dependencies — on Arch:
  sudo pacman -S --needed vulkan-headers vulkan-icd-loader libdrm \
      libxcb wayland wayland-protocols glslang shaderc sdl2 systemd-libs
(Adjust for your distro; Monado prints the specific missing package above.)
EOF
    exit 1
fi

echo ">> building monado (service + OpenXR client library + gui)..."
cmake --build "$MONADO_BUILD" --target monado-service openxr_monado gui -j"$(nproc)"

for f in src/xrt/targets/service/monado-service src/xrt/targets/gui/monado-gui openxr_monado-dev.json; do
    [[ -e $MONADO_BUILD/$f ]] || { echo "expected artifact missing: $MONADO_BUILD/$f" >&2; exit 1; }
done

echo ">> done: $MONADO_BUILD"
echo "   hyprtester --xr and scripts/preview-xr.sh will now find this build automatically."
