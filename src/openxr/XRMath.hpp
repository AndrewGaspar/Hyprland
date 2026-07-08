#pragma once

// XRMath — pure Vec3/Quat/pose helpers for the OpenXR extension (docs/openxr/03-anchoring.md §1).
//
// This header is compiled UNCONDITIONALLY (no HAVE_OPENXR guard, no OpenXR headers) so the
// anchor/config math and their gtests are always buildable, exactly like its pure-math
// siblings XRAnchor.{hpp,cpp} and XRMonitorConfig.{hpp,cpp}. Do not include any OpenXR/EGL/GLES
// headers here.
//
// Conventions (OpenXR): right-handed, +Y up, -Z forward, +X right, meters. Quaternions are
// {x,y,z,w}, identity {0,0,0,1}; qMul(a,b) applies b first (qRotate(qMul(a,b),v) ==
// qRotate(a, qRotate(b, v))).
//
// DEVIATION from doc 03 §1.1 (which suggested global scope like Vector2D): these types live in
// the OpenXR namespace to match the rest of src/openxr/ (XRMonitorConfig etc.) and to avoid
// leaking the very common names Vec3/Quat into global scope. Noted for WP13.

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <string>

namespace OpenXR {
    struct Vec3 {
        float x = 0.F, y = 0.F, z = 0.F;

        constexpr Vec3() = default;
        constexpr Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}

        Vec3 operator+(const Vec3& o) const {
            return {x + o.x, y + o.y, z + o.z};
        }
        Vec3 operator-(const Vec3& o) const {
            return {x - o.x, y - o.y, z - o.z};
        }
        Vec3 operator-() const {
            return {-x, -y, -z};
        }
        Vec3 operator*(float s) const {
            return {x * s, y * s, z * s};
        }
        Vec3 operator/(float s) const {
            return {x / s, y / s, z / s};
        }
        Vec3& operator+=(const Vec3& o) {
            x += o.x;
            y += o.y;
            z += o.z;
            return *this;
        }
        Vec3& operator-=(const Vec3& o) {
            x -= o.x;
            y -= o.y;
            z -= o.z;
            return *this;
        }
        Vec3& operator*=(float s) {
            x *= s;
            y *= s;
            z *= s;
            return *this;
        }

        float dot(const Vec3& o) const {
            return x * o.x + y * o.y + z * o.z;
        }
        Vec3 cross(const Vec3& o) const {
            return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
        }
        float length() const {
            return std::sqrt(dot(*this));
        }
        Vec3 normalized() const {
            const float l = length();
            return l > 0.F ? (*this) / l : Vec3{};
        }
    };

    inline Vec3 operator*(float s, const Vec3& v) {
        return v * s;
    }

    struct Quat {
        // OpenXR convention: [x, y, z, w], identity = (0, 0, 0, 1).
        float x = 0.F, y = 0.F, z = 0.F, w = 1.F;
    };

    struct SXRPose {
        Vec3 pos;
        Quat rot;
    };

    // ---- quaternion helpers ----

    // Hamilton product; b is applied first (doc 03 §1.2 formula).
    inline Quat qMul(const Quat& a, const Quat& b) {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }

    inline Quat qConjugate(const Quat& q) {
        return {-q.x, -q.y, -q.z, q.w};
    }

    inline Quat qInverse(const Quat& q) {
        return qConjugate(q); // unit quats only
    }

    // v' = v + 2w(u×v) + 2u×(u×v), u = (x,y,z)
    inline Vec3 qRotate(const Quat& q, const Vec3& v) {
        const Vec3 u{q.x, q.y, q.z};
        const Vec3 t = 2.F * u.cross(v);
        return v + q.w * t + u.cross(t);
    }

    inline Quat qNormalize(const Quat& q) {
        const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (l <= 0.F)
            return {0.F, 0.F, 0.F, 1.F};
        return {q.x / l, q.y / l, q.z / l, q.w / l};
    }

    // shortest-arc slerp (negate b if dot < 0), nlerp fallback near-parallel.
    inline Quat qSlerp(const Quat& a, Quat b, float t) {
        float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0.F) {
            b = {-b.x, -b.y, -b.z, -b.w};
            d = -d;
        }
        if (d > 0.9995F) {
            const Quat r{a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w)};
            return qNormalize(r);
        }
        const float th = std::acos(d);
        const float s  = std::sin(th);
        const float wa = std::sin((1.F - t) * th) / s;
        const float wb = std::sin(t * th) / s;
        return {wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
    }

    inline Quat qFromAxisAngle(const Vec3& axisNormalized, float rad) {
        const float h = rad * 0.5F;
        const float s = std::sin(h);
        return {axisNormalized.x * s, axisNormalized.y * s, axisNormalized.z * s, std::cos(h)};
    }

    inline Quat qFromYaw(float rad) {
        const float h = rad * 0.5F;
        return {0.F, std::sin(h), 0.F, std::cos(h)};
    }

    inline Quat qFromPitch(float rad) {
        const float h = rad * 0.5F;
        return {std::sin(h), 0.F, 0.F, std::cos(h)};
    }

    // yaw about +Y such that yaw 0 faces -Z; returns fallback near vertical (doc 03 §1.5).
    inline float qYawOf(const Quat& q, float fallback) {
        const Vec3 f = qRotate(q, Vec3{0.F, 0.F, -1.F});
        if (std::sqrt(f.x * f.x + f.z * f.z) < 1e-4F)
            return fallback;
        return std::atan2(-f.x, -f.z);
    }

    inline float qAngleBetween(const Quat& a, const Quat& b) {
        float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        d       = std::clamp(d, -1.F, 1.F);
        return 2.F * std::acos(d);
    }

    // Orientation whose +Z points from `from` toward `to`, +X horizontal (roll removed).
    // Returns fallback when the direction is near-vertical (roll undefined). (doc 03 §1.4)
    inline Quat lookAtNoRoll(const Vec3& from, const Vec3& to, const Quat& fallback) {
        const Vec3 z = (to - from).normalized();
        const Vec3 up{0.F, 1.F, 0.F};
        const Vec3 xc = up.cross(z);
        if (xc.length() < 1e-3F)
            return fallback;
        const Vec3 x = xc.normalized();
        const Vec3 y = z.cross(x);

        // rotation matrix with columns x, y, z; Shepperd's method -> quaternion.
        const float m00 = x.x, m01 = y.x, m02 = z.x;
        const float m10 = x.y, m11 = y.y, m12 = z.y;
        const float m20 = x.z, m21 = y.z, m22 = z.z;
        const float tr = m00 + m11 + m22;

        Quat        q;
        if (tr > 0.F) {
            const float s = 2.F * std::sqrt(tr + 1.F);
            q.w           = s / 4.F;
            q.x           = (m21 - m12) / s;
            q.y           = (m02 - m20) / s;
            q.z           = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            const float s = 2.F * std::sqrt(1.F + m00 - m11 - m22);
            q.x           = s / 4.F;
            q.w           = (m21 - m12) / s;
            q.y           = (m01 + m10) / s;
            q.z           = (m02 + m20) / s;
        } else if (m11 > m22) {
            const float s = 2.F * std::sqrt(1.F + m11 - m00 - m22);
            q.y           = s / 4.F;
            q.w           = (m02 - m20) / s;
            q.x           = (m01 + m10) / s;
            q.z           = (m12 + m21) / s;
        } else {
            const float s = 2.F * std::sqrt(1.F + m22 - m00 - m11);
            q.z           = s / 4.F;
            q.w           = (m10 - m01) / s;
            q.x           = (m02 + m20) / s;
            q.y           = (m12 + m21) / s;
        }
        return qNormalize(q);
    }

    // ---- pose helpers ----

    // a ∘ b : world(x) = a(b(x)). (doc 03 §1.3)
    inline SXRPose poseCompose(const SXRPose& a, const SXRPose& b) {
        return {a.pos + qRotate(a.rot, b.pos), qMul(a.rot, b.rot)};
    }

    inline SXRPose poseInverse(const SXRPose& p) {
        const Quat ci = qConjugate(p.rot);
        return {-qRotate(ci, p.pos), ci};
    }

    // ---- 1€ filter (grabbable-borders WP-G6, research 04-grabbable-borders.md §4/§5.4) ----
    //
    // Casiez, Roussel & Vogel, "1€ Filter: A Simple Speed-based Low-pass Filter for Noisy Input in
    // Interactive Systems," CHI '12. Reference implementation: github.com/casiez/OneEuroFilter
    // (1eurofilter.cc). This is a faithful, allocation-free transcription of that algorithm as a POD
    // step function so it is gtest-covered (tests/xr/one_euro.cpp asserts bit-for-bit against a
    // direct port of the reference C++) and usable on the frame thread with ZERO hyprutils refcount
    // ops (XRMonitorLayer.hpp rule). Used ONLY for the optional hand-grab carry filter — a first-order
    // low-pass whose cutoff rises with the signal speed, so it kills jitter when the panel is nearly
    // still (low cutoff) yet does not lag when it is moved fast (high cutoff).
    //
    // Mapping to the reference: a OneEuroFilter owns two LowPassFilters, `x` (value) and `dx`
    // (derivative). xPrevRaw == x->lastRawValue() (x.y), xHat == x->hatxprev (x.s), dxHat ==
    // dx->hatxprev (dx.s). Both LowPassFilters become "initialized" on the SAME first filter() call,
    // so one `init` flag suffices.

    constexpr float XR_ONEEURO_DCUTOFF = 1.0F; // reference default derivative cutoff (Hz)

    struct SXROneEuro {
        bool  init     = false;
        float xPrevRaw = 0.F; // last RAW input        (reference LowPassFilter x.y)
        float xHat     = 0.F; // last FILTERED value    (reference LowPassFilter x.s)
        float dxHat    = 0.F; // last FILTERED derivative (reference LowPassFilter dx.s)

        void  reset() {
            *this = SXROneEuro{};
        }
    };

    // One 1€ step for a scalar signal. `dt` is seconds since the previous step (the reference
    // recovers freq = 1/(t_now - t_prev) from timestamps; te = 1/freq = dt). minCutoff/beta are the
    // two user parameters; dCutoff is the derivative cutoff (XR_ONEEURO_DCUTOFF). Returns the
    // filtered value and advances `s`. First sample passes through (seeds state); a non-positive dt
    // holds the last filtered value (guards the reference's 1/dt against a zero/negative step).
    inline float oneEuroStep(SXROneEuro& s, float value, float dt, float minCutoff, float beta, float dCutoff = XR_ONEEURO_DCUTOFF) {
        constexpr float PI = 3.14159265358979323846F;
        // alpha(cutoff) = 1 / (1 + tau/te), tau = 1/(2*pi*cutoff), te = dt.
        auto alpha = [&](float cutoff) -> float {
            const float tau = 1.F / (2.F * PI * cutoff);
            return 1.F / (1.F + tau / dt);
        };

        if (!s.init) {
            s.init     = true;
            s.xPrevRaw = value;
            s.xHat     = value;
            s.dxHat    = 0.F; // dx->filterWithAlpha(0, ...) on the first call returns 0 in the reference
            return value;
        }
        if (dt <= 0.F)
            return s.xHat; // no time advanced -> hold (avoid the reference's 1/dt blow-up)

        const float dvalue  = (value - s.xPrevRaw) / dt;               // (value - lastRaw) * freq
        const float aD      = alpha(dCutoff);
        const float edvalue = aD * dvalue + (1.F - aD) * s.dxHat;      // dx low-pass
        const float cutoff  = minCutoff + beta * std::fabs(edvalue);
        const float aC      = alpha(cutoff);
        const float result  = aC * value + (1.F - aC) * s.xHat;        // x low-pass at speed-adaptive cutoff

        s.xPrevRaw = value;
        s.xHat     = result;
        s.dxHat    = edvalue;
        return result;
    }

    // Per-axis 1€ filter state for a full pose: xyz position + the 4 quaternion components.
    struct SXROneEuroPose {
        SXROneEuro px, py, pz;     // position
        SXROneEuro qx, qy, qz, qw; // orientation components

        void       reset() {
            *this = SXROneEuroPose{};
        }
    };

    // 1€-filter a pose. Position filters each axis independently. Orientation: the reference OneEuro
    // repo has no canonical quaternion path, and its guidance for rotations is to low-pass the
    // components and renormalize; we do exactly that, first flipping the incoming quat into the same
    // hemisphere as the last filtered quat (q and -q are the same rotation, but a component-wise
    // low-pass across the antipode would collapse toward zero). The small per-frame rotation deltas
    // of a hand carry keep the component-wise result faithful, and qNormalize restores a unit quat.
    inline SXRPose oneEuroStepPose(SXROneEuroPose& s, const SXRPose& pose, float dt, float minCutoff, float beta, float dCutoff = XR_ONEEURO_DCUTOFF) {
        SXRPose out;
        out.pos.x = oneEuroStep(s.px, pose.pos.x, dt, minCutoff, beta, dCutoff);
        out.pos.y = oneEuroStep(s.py, pose.pos.y, dt, minCutoff, beta, dCutoff);
        out.pos.z = oneEuroStep(s.pz, pose.pos.z, dt, minCutoff, beta, dCutoff);

        Quat q = pose.rot;
        if (s.qw.init) {
            const float d = q.x * s.qx.xHat + q.y * s.qy.xHat + q.z * s.qz.xHat + q.w * s.qw.xHat;
            if (d < 0.F)
                q = Quat{-q.x, -q.y, -q.z, -q.w};
        }
        Quat f;
        f.x     = oneEuroStep(s.qx, q.x, dt, minCutoff, beta, dCutoff);
        f.y     = oneEuroStep(s.qy, q.y, dt, minCutoff, beta, dCutoff);
        f.z     = oneEuroStep(s.qz, q.z, dt, minCutoff, beta, dCutoff);
        f.w     = oneEuroStep(s.qw, q.w, dt, minCutoff, beta, dCutoff);
        out.rot = qNormalize(f);
        return out;
    }

    // ---- ray -> quad intersection (docs/openxr/04-input.md §3) ----
    //
    // Pure, unconditional (no OpenXR headers): the ray-pointer hit test and its UV mapping, so
    // tests/xr/ray_intersect.cpp can exercise it with no runtime present.
    struct SXRQuadHit {
        bool  hit = false;
        float t   = 0.F; // ray parameter (distance along dir if dir is unit length)
        float u   = 0.F; // 0 at the quad's left edge, 1 at the right
        float v   = 0.F; // 0 at the top edge, 1 at the bottom (surface v grows downward)
    };

    // Intersect a ray (world origin `o`, world direction `d`) with a quad at world pose `Q`,
    // width `w` and height `h` in meters (the quad lies in its local x-y plane, centered at
    // origin, +X right / +Y up). `slack` expands the half-extents for the grab cone forgiveness
    // (doc 04 §6, WP8). Transform the ray into quad-local space, intersect z = 0, bounds-test.
    inline SXRQuadHit rayQuadIntersect(const SXRPose& Q, const Vec3& o, const Vec3& d, float w, float h, float slack = 0.F) {
        SXRQuadHit out;

        const Quat qi = qInverse(Q.rot);
        const Vec3 lo = qRotate(qi, o - Q.pos); // ray origin in quad-local space
        const Vec3 ld = qRotate(qi, d);         // ray direction in quad-local space

        if (std::fabs(ld.z) < 1e-6F)
            return out; // ray parallel to the quad plane

        const float t = -lo.z / ld.z;
        if (t <= 0.F)
            return out; // behind the ray origin

        const Vec3 p = lo + ld * t;
        if (std::fabs(p.x) > w * 0.5F + slack || std::fabs(p.y) > h * 0.5F + slack)
            return out; // outside the quad bounds

        out.hit = true;
        out.t   = t;
        out.u   = p.x / w + 0.5F;
        out.v   = 0.5F - p.y / h;
        return out;
    }

    // ---- chrome margin geometry + hit-region classification (docs/openxr/research/04-grabbable-borders.md §8, WP-G1) ----
    //
    // Per §8 the submitted XrCompositionLayerQuad is expanded with a TRANSPARENT alpha margin
    // around the desktop content: swapchain = content + margins, the desktop blits into the inner
    // content rect (alpha 1), and the margin renders transparent (alpha 0). This keeps `size:`
    // meaning CONTENT meters while giving chrome (a bottom move-bar + corner resize handles) a
    // place to live that never covers a desktop pixel.
    //
    // SXRChromeGeometry describes that layout PURELY as normalized fractions of the FULL quad
    // (u,v in [0,1], v growing downward like rayQuadIntersect). Storing fractions — not meters or
    // pixels — makes the blit inset (px = frac * swapchainPx), the submitted quad size
    // (quadMeters = contentMeters / contentFrac) and this classifier all derive from ONE source,
    // so a ray hit classified BODY here remaps to exactly the desktop pixel the content blit wrote,
    // and stays consistent through a live meters-resize with no swapchain churn (fractions fixed).
    // Pure + unconditional (gtest-covered in tests/xr/chrome_hit.cpp); no OpenXR headers.

    enum eXRQuadRegion : uint8_t {
        XR_REGION_NONE = 0,  // ray missed every quad (input-layer sentinel; classify never returns it)
        XR_REGION_BODY,      // over the desktop content — the only region that moves/clicks the pointer
        XR_REGION_BAR,       // bottom move-bar (WP-G2 draws it, WP-G3 grabs from it)
        XR_REGION_CORNER_TL, // corner resize handles (WP-G3)
        XR_REGION_CORNER_TR,
        XR_REGION_CORNER_BL,
        XR_REGION_CORNER_BR,
        XR_REGION_MARGIN, // transparent dead margin (hover-only, no events)
    };

    inline bool xrRegionIsCorner(eXRQuadRegion r) {
        return r == XR_REGION_CORNER_TL || r == XR_REGION_CORNER_TR || r == XR_REGION_CORNER_BL || r == XR_REGION_CORNER_BR;
    }

    // ---- grab-region gating (docs/openxr/research/04-grabbable-borders.md §5.2, WP-G3) ----
    //
    // Pure decision helper: given the chrome region a grab gesture landed on, what does it do?
    // BAR always MOVEs; each CORNER always RESIZEs (from that corner); BODY MOVEs only when
    // openxr:grab_anywhere is set (the controller-grip-anywhere convenience) AND hands are not the
    // active device; MARGIN / NONE never grab. gtest-covered truth table (tests/xr/grab_gating.cpp).
    //
    // `handActive` is the WP-G5 slot: when hands are the active device the fist/pinch is forced to
    // the bar/corners (no whole-content grab, per the vendor UX in §2-§3), so BODY never grabs
    // regardless of grab_anywhere. WP-G3 always passes false — flipping G5 on is a one-argument
    // change with no rework here.
    enum eXRGrabAction : uint8_t {
        XR_GRAB_ACTION_NONE = 0,
        XR_GRAB_ACTION_MOVE,
        XR_GRAB_ACTION_RESIZE_TL,
        XR_GRAB_ACTION_RESIZE_TR,
        XR_GRAB_ACTION_RESIZE_BL,
        XR_GRAB_ACTION_RESIZE_BR,
    };

    inline bool xrGrabActionIsResize(eXRGrabAction a) {
        return a == XR_GRAB_ACTION_RESIZE_TL || a == XR_GRAB_ACTION_RESIZE_TR || a == XR_GRAB_ACTION_RESIZE_BL || a == XR_GRAB_ACTION_RESIZE_BR;
    }

    inline eXRGrabAction grabActionForRegion(eXRQuadRegion region, bool grabAnywhere, bool handActive) {
        switch (region) {
            case XR_REGION_BAR: return XR_GRAB_ACTION_MOVE;
            case XR_REGION_CORNER_TL: return XR_GRAB_ACTION_RESIZE_TL;
            case XR_REGION_CORNER_TR: return XR_GRAB_ACTION_RESIZE_TR;
            case XR_REGION_CORNER_BL: return XR_GRAB_ACTION_RESIZE_BL;
            case XR_REGION_CORNER_BR: return XR_GRAB_ACTION_RESIZE_BR;
            case XR_REGION_BODY: return (grabAnywhere && !handActive) ? XR_GRAB_ACTION_MOVE : XR_GRAB_ACTION_NONE;
            default: return XR_GRAB_ACTION_NONE; // XR_REGION_MARGIN / XR_REGION_NONE
        }
    }

    // Corner signs in the CONTENT's world frame: sx = +1 right edge / -1 left edge; sy = +1 top
    // edge (+world-up) / -1 bottom edge. (u grows right, v grows DOWN, so top corners are +up.)
    inline void xrCornerSigns(eXRQuadRegion corner, float& sx, float& sy) {
        sx = (corner == XR_REGION_CORNER_TR || corner == XR_REGION_CORNER_BR) ? 1.F : -1.F;
        sy = (corner == XR_REGION_CORNER_TL || corner == XR_REGION_CORNER_TR) ? 1.F : -1.F;
    }

    inline const char* xrRegionName(eXRQuadRegion r) {
        switch (r) {
            case XR_REGION_BODY: return "body";
            case XR_REGION_BAR: return "bar";
            case XR_REGION_CORNER_TL: return "corner-tl";
            case XR_REGION_CORNER_TR: return "corner-tr";
            case XR_REGION_CORNER_BL: return "corner-bl";
            case XR_REGION_CORNER_BR: return "corner-br";
            case XR_REGION_MARGIN: return "margin";
            default: return "none";
        }
    }

    // ---- hand-interaction active-device detection + gesture selection (WP-G5, research §5.3/§8) ----
    //
    // Pure, unconditional (gtest-covered in tests/xr/hand_grab.cpp): no OpenXR headers. The frame
    // thread caches each hand's current interaction profile path (xrGetCurrentInteractionProfile,
    // refreshed on XrEventDataInteractionProfileChanged) and maps it here to an input KIND. "Hands
    // are active" ⟺ the profile is the ext/hand_interaction_ext profile we suggest pinch/grasp
    // bindings for — the only profile that yields a stable pinch pose. Every controller profile
    // (incl. microsoft/hand_interaction, which we bind no hand actions for) maps to CONTROLLER so
    // the squeeze/grip path is used unchanged.

    enum eXRInputKind : uint8_t {
        XR_INPUT_CONTROLLER = 0,
        XR_INPUT_HANDS,
    };

    inline constexpr const char* XR_PROFILE_HAND_INTERACTION = "/interaction_profiles/ext/hand_interaction_ext";

    inline eXRInputKind xrInputKindForProfile(const std::string& profilePath) {
        return profilePath == XR_PROFILE_HAND_INTERACTION ? XR_INPUT_HANDS : XR_INPUT_CONTROLLER;
    }

    inline const char* xrInputKindName(eXRInputKind k) {
        return k == XR_INPUT_HANDS ? "hands" : "controllers";
    }

    // Which hand gesture(s) start a grab when hands are the active device (openxr:hand_grab). PINCH
    // (default, research §8) disables the pose-perturbing fist for hands entirely; GRASP restores the
    // old fist behavior; BOTH accepts either. Controllers ignore this — they always squeeze.
    enum eXRHandGrab : uint8_t {
        XR_HANDGRAB_PINCH = 0,
        XR_HANDGRAB_GRASP,
        XR_HANDGRAB_BOTH,
    };

    inline eXRHandGrab xrParseHandGrab(const std::string& s) {
        if (s == "grasp")
            return XR_HANDGRAB_GRASP;
        if (s == "both")
            return XR_HANDGRAB_BOTH;
        return XR_HANDGRAB_PINCH; // default + explicit "pinch"
    }

    inline const char* xrHandGrabName(eXRHandGrab m) {
        switch (m) {
            case XR_HANDGRAB_GRASP: return "grasp";
            case XR_HANDGRAB_BOTH: return "both";
            default: return "pinch";
        }
    }

    // The analog grab value fed to the grab Schmitt when hands are active: pinch strength, fist-curl
    // strength, or the max of the two (BOTH), per the hand_grab mode.
    inline float xrHandGrabValue(eXRHandGrab mode, float pinchVal, float graspVal) {
        switch (mode) {
            case XR_HANDGRAB_GRASP: return graspVal;
            case XR_HANDGRAB_BOTH: return std::max(pinchVal, graspVal);
            default: return pinchVal; // XR_HANDGRAB_PINCH
        }
    }

    // Whether a hand grab under `mode` (with these strengths at the trigger instant) should anchor
    // to the stable pinch pose (pinch_ext/pose) rather than the wrist grip pose. PINCH → always pinch;
    // GRASP → always grip (grasp_ext has no pose); BOTH → whichever gesture is the stronger contributor
    // (pinch on a tie, since the pinch pose is the steadier anchor). Callers additionally require a
    // valid pinch pose this frame before honoring a true result.
    inline bool xrHandGrabUsesPinch(eXRHandGrab mode, float pinchVal, float graspVal) {
        switch (mode) {
            case XR_HANDGRAB_GRASP: return false;
            case XR_HANDGRAB_BOTH: return pinchVal >= graspVal;
            default: return true; // XR_HANDGRAB_PINCH
        }
    }

    struct SXRChromeGeometry {
        // Content rect within the full quad, in [0,1] uv (from the top-left; v grows down).
        float contentU0 = 0.F, contentV0 = 0.F, contentU1 = 1.F, contentV1 = 1.F;
        // Bottom (default) move-bar rect, uv. Zero-area when the bar is disabled.
        float barU0 = 0.F, barV0 = 0.F, barU1 = 0.F, barV1 = 0.F;
        // Corner handle extents in uv (per-axis so a handle stays metrically square across aspect).
        float cornerU = 0.F, cornerV = 0.F;

        bool  hasContentRect() const {
            return contentU1 > contentU0 && contentV1 > contentV0;
        }
        // Whether any drawable chrome element exists (a move-bar with area, or corner handles).
        // WP-G2 uses this to gate the chrome draw pass + auto-hide entirely: with chrome disabled
        // (margin 0 + bar 0) the geometry is a full-quad content rect and this returns false, so
        // the draw pass is skipped and there is zero visual change (the disable mechanism, doc §8).
        bool  hasChrome() const {
            return (barU1 > barU0 && barV1 > barV0) || (cornerU > 0.F && cornerV > 0.F);
        }
        float contentFracW() const {
            return contentU1 - contentU0;
        }
        float contentFracH() const {
            return contentV1 - contentV0;
        }
        float contentCenterU() const {
            return 0.5F * (contentU0 + contentU1);
        }
        float contentCenterV() const {
            return 0.5F * (contentV0 + contentV1);
        }
    };

    // Build the chrome layout from CONTENT dimensions (meters) + the openxr:chrome_* config (all
    // meters/fractions). marginL=R=T = margin; the bottom margin additionally holds the bar
    // (marginB = margin + barHeight). The bar is centered on the content, `barWidthFrac` of the
    // content width, directly below the content. Corner handles are squares of `cornerSize` meters
    // at the content's outer corners, living entirely in the margin (clamped to the margin so they
    // never eat into the content/BODY). With margin==0 && barHeight==0 the result is a full-quad
    // content rect (chrome disabled) and classify always returns BODY.
    inline SXRChromeGeometry makeChromeGeometry(float contentW, float contentH, float margin, float barHeight, float barWidthFrac, float cornerSize) {
        SXRChromeGeometry g;
        margin       = std::max(0.F, margin);
        barHeight    = std::max(0.F, barHeight);
        barWidthFrac = std::clamp(barWidthFrac, 0.F, 1.F);
        cornerSize   = std::clamp(cornerSize, 0.F, margin); // stays inside the margin frame
        if (contentW <= 0.F || contentH <= 0.F)
            return g; // degenerate; leave the full-quad identity rect

        const float mL = margin, mR = margin, mT = margin, mB = margin + barHeight;
        const float quadW = contentW + mL + mR;
        const float quadH = contentH + mT + mB;

        g.contentU0 = mL / quadW;
        g.contentU1 = (mL + contentW) / quadW;
        g.contentV0 = mT / quadH;
        g.contentV1 = (mT + contentH) / quadH;

        if (barHeight > 0.F && barWidthFrac > 0.F) {
            const float barW    = contentW * barWidthFrac;
            const float centerX = mL + contentW * 0.5F; // content center x, meters from left
            const float barTop  = mT + contentH;        // content bottom, meters from top
            g.barU0             = (centerX - barW * 0.5F) / quadW;
            g.barU1             = (centerX + barW * 0.5F) / quadW;
            g.barV0             = barTop / quadH;
            g.barV1             = (barTop + barHeight) / quadH;
        }

        g.cornerU = cornerSize / quadW;
        g.cornerV = cornerSize / quadH;
        return g;
    }

    // Classify a full-quad ray hit (u,v in [0,1] from rayQuadIntersect). Never returns
    // XR_REGION_NONE (assumes an actual hit). Content interior wins so no click pixel is stolen;
    // then corner handles, then the bar, else the transparent margin.
    inline eXRQuadRegion classifyQuadHit(float u, float v, const SXRChromeGeometry& g) {
        const bool inContentX = u >= g.contentU0 && u <= g.contentU1;
        const bool inContentY = v >= g.contentV0 && v <= g.contentV1;
        if (inContentX && inContentY)
            return XR_REGION_BODY;

        // Corner handles (only when the handle has area): squares just OUTSIDE the content corner.
        if (g.cornerU > 0.F && g.cornerV > 0.F) {
            const bool leftBand   = u >= g.contentU0 - g.cornerU && u <= g.contentU0;
            const bool rightBand  = u >= g.contentU1 && u <= g.contentU1 + g.cornerU;
            const bool topBand    = v >= g.contentV0 - g.cornerV && v <= g.contentV0;
            const bool bottomBand = v >= g.contentV1 && v <= g.contentV1 + g.cornerV;
            if (leftBand && topBand)
                return XR_REGION_CORNER_TL;
            if (rightBand && topBand)
                return XR_REGION_CORNER_TR;
            if (leftBand && bottomBand)
                return XR_REGION_CORNER_BL;
            if (rightBand && bottomBand)
                return XR_REGION_CORNER_BR;
        }

        // Move-bar.
        if (g.barU1 > g.barU0 && g.barV1 > g.barV0 && u >= g.barU0 && u <= g.barU1 && v >= g.barV0 && v <= g.barV1)
            return XR_REGION_BAR;

        return XR_REGION_MARGIN;
    }

    // Remap a full-quad uv to CONTENT uv (what the desktop pointer path needs). Returns false when
    // (u,v) lands outside the content rect (caller treats it as a non-body hit); cu/cv are only
    // meaningful when it returns true.
    inline bool remapToContentUV(float u, float v, const SXRChromeGeometry& g, float& cu, float& cv) {
        if (!g.hasContentRect())
            return false;
        cu = (u - g.contentU0) / g.contentFracW();
        cv = (v - g.contentV0) / g.contentFracH();
        return cu >= 0.F && cu <= 1.F && cv >= 0.F && cv <= 1.F;
    }

    // The submitted quad's geometric center differs from the CONTENT center whenever the margins
    // are asymmetric (the bottom margin holds the bar). Given the content pose (the anchor solve's
    // output — pose of the content center), return the pose of the QUAD center to submit so the
    // content stays exactly where the anchor placed it. quadW/quadH are the full quad meters.
    inline SXRPose contentPoseToQuadCenter(const SXRPose& contentPose, const SXRChromeGeometry& g, float quadW, float quadH) {
        // Content center within the quad, quad-local (+X right, +Y up; v grows down so +y = -v).
        const float xLocal = (g.contentCenterU() - 0.5F) * quadW;
        const float yLocal = (0.5F - g.contentCenterV()) * quadH;
        const Vec3  off    = qRotate(contentPose.rot, Vec3{xLocal, yLocal, 0.F});
        return SXRPose{contentPose.pos - off, contentPose.rot};
    }

    // ---- chrome auto-hide fade envelope (docs/openxr/research/04-grabbable-borders.md §8, WP-G2) ----
    //
    // Pure, unconditional (gtest-covered in tests/xr/chrome_fade.cpp): advance a layer's chrome
    // fade alpha one frame toward its visibility target, so the move-bar/corner handles fade IN
    // when the ray hovers the quad (or it is grabbed) and fade OUT `hideDelay` seconds after the
    // last such interaction. Driven on the FRAME thread from xrWaitFrame predicted-display-time
    // deltas (no wall clock — see the frame loop), which is why it takes explicit dt/since-active
    // seconds rather than reading a clock.
    //
    //   cur            current alpha in [0,1]
    //   activeNow      the ray hovers ANY region of this quad this frame, or it is grabbed
    //   dtSec          seconds since the previous advance (predicted-time delta, clamped by caller)
    //   sinceActiveSec seconds since `activeNow` was last true (0 when activeNow)
    //   fadeSec        full fade duration in seconds (openxr:chrome_fade_ms)
    //   hideDelaySec   idle grace before fading out (openxr:chrome_hide_delay_ms)
    //
    // The visibility target is 1 while active or within the hide-delay grace, else 0; alpha ramps
    // toward it at a constant dt/fadeSec rate (linear fade). Returns the new alpha in [0,1].
    inline float chromeFadeAdvance(float cur, bool activeNow, float dtSec, float sinceActiveSec, float fadeSec, float hideDelaySec) {
        const bool  visible = activeNow || sinceActiveSec < hideDelaySec;
        const float target  = visible ? 1.F : 0.F;
        if (fadeSec <= 0.F)
            return target; // no fade duration -> snap
        if (dtSec <= 0.F)
            return std::clamp(cur, 0.F, 1.F); // no time elapsed -> hold
        const float step = dtSec / fadeSec;
        if (target > cur)
            return std::min(target, cur + step);
        return std::max(target, cur - step);
    }
}
