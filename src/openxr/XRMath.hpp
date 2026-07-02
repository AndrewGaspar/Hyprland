#pragma once

// XRMath — pure Vec3/Quat/pose helpers for the OpenXR extension.
//
// This header is compiled UNCONDITIONALLY (no HAVE_OPENXR guard, no OpenXR headers)
// so the anchor/config math and their gtests are always buildable, exactly like its
// pure-math siblings XRAnchor.{hpp,cpp} and XRMonitorConfig.{hpp,cpp}. Do not include
// any OpenXR/EGL/GLES headers here.
//
// This is a WP1 stub — only the minimal types are declared. The full leash/pose math
// lands with the anchoring engine (see docs/openxr/03-anchoring.md, WP5).

namespace OpenXR {
    struct SVec3 {
        float x = 0.F;
        float y = 0.F;
        float z = 0.F;
    };

    struct SQuat {
        // OpenXR convention: [x, y, z, w], identity = (0, 0, 0, 1).
        float x = 0.F;
        float y = 0.F;
        float z = 0.F;
        float w = 1.F;
    };

    struct SPose {
        SVec3 position;
        SQuat orientation;
    };
}
