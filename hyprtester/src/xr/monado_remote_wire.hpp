#pragma once

// =============================================================================
//  VENDORED Monado remote-driver wire protocol structs.
//
//  Source of truth: subprojects/monado/src/xrt/drivers/remote/r_interface.h
//                   (+ the xrt_defines.h PODs it embeds)
//  Upstream: https://gitlab.freedesktop.org/monado/monado
//  PINNED TO Monado commit: c2ddab59dc41366fe520dc4e8abcfea257ecf0b8
//  (the subprojects/monado submodule is pinned to the SAME commit — re-pin both together)
//
//  These are self-contained POD copies — we deliberately do NOT include or link
//  against Monado (that would drag its headers/libs into every hyprtester build).
//  This is the SOLE sanctioned vendoring in the tree and never ships in the
//  compositor (it lives under hyprtester/, behind WITH_XR_TESTS).
//
//  Re-pinning is a deliberate maintenance task: bump the commit above, re-copy
//  the structs, and re-verify the static_asserts below. Never re-pin silently.
//
//  The compile-time static_asserts below are the first half of ABI-drift
//  protection (layout). The runtime half is RemoteClient's connect handshake,
//  which SKIPs (not FAILs) the suite when a locally-installed Monado has evolved
//  the wire format past this pin.
// =============================================================================

#ifdef WITH_XR_TESTS

#include <cstdint>
#include <array>
#include <bit>

namespace MonadoWire {

    // --- xrt_defines.h PODs (minimal, inlined) --------------------------------

    struct xrt_vec1 {
        float x;
    };

    struct xrt_vec2 {
        float x, y;
    };

    struct xrt_vec3 {
        float x, y, z;
    };

    struct xrt_quat {
        float x, y, z, w;
    };

    struct xrt_pose {
        xrt_quat orientation;
        xrt_vec3 position;
    };

    struct xrt_fov {
        float angle_left, angle_right, angle_up, angle_down;
    };

    // --- r_interface.h wire structs -------------------------------------------

    // r_interface.h:74-107. Layout comment from source:
    //   active(2) + bools(11) + pad(3) = 16
    struct r_remote_controller_data {
        xrt_pose pose;
        xrt_vec3 linear_velocity;
        xrt_vec3 angular_velocity;

        float    hand_curl[5];

        xrt_vec1 trigger_value;
        xrt_vec1 squeeze_value;
        xrt_vec1 squeeze_force;
        xrt_vec2 thumbstick;
        xrt_vec1 trackpad_force;
        xrt_vec2 trackpad;

        bool     hand_tracking_active;
        bool     active;

        bool     system_click;
        bool     system_touch;
        bool     a_click;
        bool     a_touch;
        bool     b_click;
        bool     b_touch;
        bool     trigger_click;
        bool     trigger_touch;
        bool     thumbstick_click;
        bool     thumbstick_touch;
        bool     trackpad_touch;
        bool     _pad0; // load-bearing for layout — keep
        bool     _pad1;
        bool     _pad2;
    };

    // r_interface.h:109-131. Layout comments from source:
    //   per-view: fov(16) + pose(16 + 12) + 4 = 48
    //   tail:     pose(16 + 12) bool(1) + pad(3) = 32
    struct r_head_data {
        struct {
            xrt_fov  fov;
            xrt_pose pose;
            uint32_t _pad;
        } views[2];

        xrt_pose center;

        bool     per_view_data_valid;
        bool     _pad0, _pad1, _pad2;
    };

    // r_interface.h:138-145. The one struct written per tick.
    struct r_remote_data {
        uint64_t                 header;
        r_head_data              head;
        r_remote_controller_data left, right;
    };

    // Monado: #define R_HEADER_VALUE (*(uint64_t *)"mndrmt3\0")
    // i.e. the little-endian u64 of the bytes 'm','n','d','r','m','t','3','\0'.
    inline constexpr uint64_t R_HEADER_VALUE = 0x0033746D72646E6DULL;

    // Do not hand-trust the hex: derive it from the string literal and compare.
    inline constexpr std::array<char, 8> R_HEADER_BYTES = {'m', 'n', 'd', 'r', 'm', 't', '3', '\0'};
    static_assert(std::bit_cast<uint64_t>(R_HEADER_BYTES) == R_HEADER_VALUE, "R_HEADER_VALUE hex does not match the bytes \"mndrmt3\\0\" — fix the constant");

    // Compile-time ABI-drift guard. Sizes verified against the pinned checkout
    // (throwaway TU compiled with the real Monado headers on x86-64):
    //   sizeof(r_remote_controller_data) == 120
    //   sizeof(r_head_data)              == 128
    //   sizeof(r_remote_data)            == 376
    //   sizeof(one r_head_data view)     == 48
    static_assert(sizeof(r_remote_controller_data) == 120, "r_remote_controller_data layout drift vs pinned Monado");
    static_assert(sizeof(r_head_data) == 128, "r_head_data layout drift vs pinned Monado");
    static_assert(sizeof(r_remote_data) == 376, "r_remote_data layout drift vs pinned Monado");
    static_assert(sizeof(reinterpret_cast<r_head_data*>(0)->views[0]) == 48, "r_head_data view layout drift vs pinned Monado");

} // namespace MonadoWire

#endif // WITH_XR_TESTS
