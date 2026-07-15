#!/usr/bin/env bash
# Build the vendored Monado (subprojects/monado, pinned to the commit the XR test
# suite's wire ABI is validated against — see hyprtester/src/xr/monado_remote_wire.hpp).
#
# TWO FLAVORS, built into SEPARATE directories so they never clobber each other:
#
#   (default / --null)  the NULL-compositor test flavor the XR suite validates against.
#                       Output: subprojects/monado/build (monado-service, openxr_monado-dev.json).
#                       `hyprtester --xr` and scripts/preview-xr.sh depend on THIS build.
#
#   --xreal             a REAL-compositor flavor for the XREAL Air 2 Ultra 3DoF rig (WP-XR1):
#                       the xreal_air HMD driver + the real Vulkan comp_main with its Wayland
#                       window backend. Output: subprojects/monado/build-xreal
#                       (monado-service, openxr_monado-dev.json). Used by scripts/xreal-mode.sh.
#                       Building this does NOT touch the null build, so the test suite is safe.
#
# One-time setup for `hyprtester --xr` and scripts/preview-xr.sh:
#   git submodule update --init subprojects/monado   (this script does it for you)
#   scripts/build-monado.sh            # null flavor
#   scripts/build-monado.sh --xreal    # xreal flavor (for the glasses)
#
# Re-runs are incremental. Build-directory overrides (for a READ-ONLY source tree, e.g. the repo
# bind-mounted at /src:ro inside the HypXRland container): set MONADO_BUILD and EIGEN_BUILD to point
# the CMake build trees somewhere writable outside the source. Both default to the in-tree paths, so
# existing invocations are unchanged. For the xreal flavor the build tree defaults to
# subprojects/monado/build-xreal (override with MONADO_BUILD).

set -euo pipefail

FLAVOR="null"
for arg in "$@"; do
    case "$arg" in
        --xreal) FLAVOR="xreal" ;;
        --null)  FLAVOR="null" ;;
        -h|--help)
            grep '^#' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "unknown argument: $arg (valid: --null, --xreal)" >&2; exit 2 ;;
    esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB="$REPO/subprojects/monado"
EIGEN="$REPO/subprojects/eigen"
VKH="$REPO/subprojects/vulkan-headers"

# Writable build trees — overridable so a read-only source tree can still build. The two flavors get
# distinct defaults so they coexist.
if [[ $FLAVOR == xreal ]]; then
    MONADO_BUILD="${MONADO_BUILD:-$SUB/build-xreal}"
else
    MONADO_BUILD="${MONADO_BUILD:-$SUB/build}"
fi
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

# ccache launchers (shared cache across the main checkout + worktrees; see MEMORY.md Build).
CCACHE_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

# Flavor-specific CMake flags.
#   null : the test flavor — null compositor + the remote driver whose wire ABI the suite pins.
#   xreal: the xreal_air HMD driver + the REAL comp_main (XRT_MODULE_COMPOSITOR_MAIN, default ON) with
#          its Wayland window backend (XRT_HAVE_WAYLAND, auto-detected from wayland/-protocols/libdrm).
#          hidapi (XRT_HAVE_HIDAPI, the driver's hard dep) must be present — a post-configure check
#          below turns a missing dep into a loud failure instead of a silently driverless service.
if [[ $FLAVOR == xreal ]]; then
    FLAVOR_FLAGS=(
        -DXRT_BUILD_DRIVER_XREAL_AIR=ON
        -DXRT_MODULE_COMPOSITOR_MAIN=ON
        -DXRT_MODULE_COMPOSITOR_NULL=OFF
        -DXRT_BUILD_DRIVER_REMOTE=OFF
        -DXRT_MODULE_MONADO_GUI=OFF
        -DXRT_BUILD_DRIVER_SURVIVE=OFF
    )
else
    FLAVOR_FLAGS=(
        -DXRT_MODULE_COMPOSITOR_NULL=ON
        -DXRT_BUILD_DRIVER_REMOTE=ON
        -DXRT_MODULE_MONADO_GUI=ON
        -DXRT_BUILD_DRIVER_SURVIVE=OFF
    )
fi

echo ">> configuring monado ($FLAVOR flavor) -> $MONADO_BUILD ..."
# -include cstdint: the pinned Monado commit predates GCC 16's stricter libstdc++
# transitive includes (u_extension_list.cpp et al. use std::uint8_t without <cstdint>);
# force-include instead of patching the submodule.
if ! cmake -S "$SUB" -B "$MONADO_BUILD" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    "${CCACHE_ARGS[@]}" \
    -DCMAKE_CXX_FLAGS="-include cstdint" \
    -DEigen3_DIR="$EIGEN_BUILD" \
    -DVulkan_INCLUDE_DIR="$VKH/include" \
    "${FLAVOR_FLAGS[@]}"; then
    cat >&2 <<'EOF'

Monado configure failed. Likely missing build dependencies — on Arch:
  sudo pacman -S --needed vulkan-headers vulkan-icd-loader libdrm \
      libxcb wayland wayland-protocols glslang shaderc sdl2 systemd-libs hidapi
(Adjust for your distro; Monado prints the specific missing package above.)
The --xreal flavor additionally needs hidapi (the xreal_air driver's dependency).
EOF
    exit 1
fi

# For the xreal flavor, fail LOUD if the driver or the Wayland backend didn't actually enable —
# otherwise you get a monado-service that can't see the glasses (no hidapi) or can't open a window on
# DP-5 (no Wayland), and the failure only shows up at runtime.
if [[ $FLAVOR == xreal ]]; then
    _cache="$MONADO_BUILD/CMakeCache.txt"
    _fail=0
    if ! grep -q '^XRT_BUILD_DRIVER_XREAL_AIR:BOOL=ON$' "$_cache"; then
        echo "!! XRT_BUILD_DRIVER_XREAL_AIR is OFF — hidapi (XRT_HAVE_HIDAPI) is likely missing. Install hidapi and re-run." >&2
        _fail=1
    fi
    if ! grep -q '^XRT_HAVE_WAYLAND:BOOL=ON$' "$_cache"; then
        echo "!! XRT_HAVE_WAYLAND is OFF — comp_main can't open its window on DP-5. Install wayland + wayland-protocols + libdrm and re-run." >&2
        _fail=1
    fi
    [[ $_fail -eq 0 ]] || exit 1
fi

# Rebuilding overwrites monado-service, which silently DROPS any file capability (e.g. the
# cap_sys_nice+ep used for a REALTIME GPU queue in XReal direct mode). Snapshot it so we can
# remind the user to re-apply after the build — a lost cap resurfaces weeks later as
# "why is direct mode juddery again?".
_svc_bin=$MONADO_BUILD/src/xrt/targets/service/monado-service
_had_sys_nice=0
if command -v getcap >/dev/null 2>&1 && [[ -e $_svc_bin ]] && getcap "$_svc_bin" 2>/dev/null | grep -q cap_sys_nice; then
    _had_sys_nice=1
fi

echo ">> building monado ($FLAVOR: service + OpenXR client library) ..."
_mj=$(nproc); _mj=${HYPXRLAND_BUILD_JOBS:-$_mj}
if [[ $FLAVOR == xreal ]]; then
    cmake --build "$MONADO_BUILD" --target monado-service openxr_monado -j"$_mj"
    _artifacts=(src/xrt/targets/service/monado-service openxr_monado-dev.json)
else
    cmake --build "$MONADO_BUILD" --target monado-service openxr_monado gui -j"$_mj"
    _artifacts=(src/xrt/targets/service/monado-service src/xrt/targets/gui/monado-gui openxr_monado-dev.json)
fi

for f in "${_artifacts[@]}"; do
    [[ -e $MONADO_BUILD/$f ]] || { echo "expected artifact missing: $MONADO_BUILD/$f" >&2; exit 1; }
done

echo ">> done: $MONADO_BUILD"
if [[ $_had_sys_nice -eq 1 ]]; then
    echo
    echo "   !! the rebuild DROPPED monado-service's cap_sys_nice file capability (REALTIME GPU queue)."
    echo "   !! re-apply it:  sudo setcap cap_sys_nice+ep $_svc_bin"
    echo
fi
if [[ $FLAVOR == xreal ]]; then
    echo "   runtime manifest: $MONADO_BUILD/openxr_monado-dev.json"
    echo "   point the toggle at it:  hyprctl keyword openxr:runtime_json $MONADO_BUILD/openxr_monado-dev.json"
    echo "   (scripts/xreal-mode.sh reads MONADO_XREAL_BUILD / defaults to this path.)"
else
    echo "   hyprtester --xr and scripts/preview-xr.sh will now find this build automatically."
fi
