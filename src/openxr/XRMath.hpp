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
#include <cstddef>
#include <array>
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

    // Forward (aim) vector of a pose/orientation: OpenXR forward is -Z. Used by the gaze ray
    // (VIEW-space head-forward, research/16 §2.1) and any other "which way is this facing" query.
    inline Vec3 poseForward(const Quat& q) {
        return qRotate(q, Vec3{0.F, 0.F, -1.F});
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

    // ---- reference-space change reconstruction (doc 03 §8.1, research/22 §4.3) ----
    //
    // XrEventDataReferenceSpaceChangePending MAY arrive with poseValid == XR_FALSE, and monado (so
    // WiVRn, so every session on this box) ALWAYS does: u_space_overseer.c pushes pose_valid=false
    // with an identity pose even though recenter_local_spaces just computed the exact delta. Without
    // a reconstruction the old handling had nothing to apply, so every anchor kept coordinates in a
    // frame that no longer exists and the monitors teleported by the whole frame shift — measured in
    // one live session as 8.25 m and ~155 deg of yaw across a single recenter.
    //
    // The head is the one physical object observable on BOTH sides of the swap. The origin moves
    // instantaneously and the skull does not, so the same head sampled just before the event
    // (`headOld`, old space) and on the first frame after it (`headNew`, new space) pins the
    // transform exactly:
    //
    //     headOld = M ∘ headNew   =>   M = headOld ∘ inv(headNew)
    //
    // which is precisely what `poseInPreviousSpace` would have carried: the new origin expressed in
    // the old space, ready for CXRAnchor::onReferenceSpaceChanged.
    //
    // Both frames are gravity-aligned, so the true delta is 4-DoF — yaw and translation (research/22
    // §2.2). Only the YAW of each head sample is used: the head's pitch and roll genuinely differ
    // between the two samples (a real head keeps moving), and feeding that difference in would tilt
    // the entire monitor group off the horizon rather than cancel.
    inline SXRPose solveReferenceSpaceChangeFromHead(const SXRPose& headOld, const SXRPose& headNew) {
        // A head staring straight up or down has no observable yaw (doc 03 §1.5). Rather than let
        // qYawOf's fallback invent one, treat the rotation as unchanged and correct translation only.
        const auto observable = [](const Quat& q) {
            const Vec3 f = qRotate(q, Vec3{0.F, 0.F, -1.F});
            return std::sqrt(f.x * f.x + f.z * f.z) >= 1e-4F;
        };
        const Quat R = observable(headOld.rot) && observable(headNew.rot) ? qFromYaw(qYawOf(headOld.rot, 0.F) - qYawOf(headNew.rot, 0.F)) : Quat{};
        return SXRPose{headOld.pos - qRotate(R, headNew.pos), R};
    }

    // What to do about a reference-space change, decided from what is actually knowable.
    enum class eXRRecenterFix : uint8_t {
        XR_RECENTER_APPLY_RUNTIME_POSE = 0, // poseValid: the runtime told us the delta; use it verbatim
        XR_RECENTER_SOLVE_FROM_HEAD,        // no delta, but a fresh head sample straddles the change
        XR_RECENTER_RESEAT_TO_HEAD,         // no delta and no usable head: the old frame is unrecoverable
    };

    // `headOldAgeNs` is how long before the post-change head sample the last pre-change one was
    // taken. One frame (~11 ms) is the normal case and the head cannot have moved meaningfully in
    // it. A large age means tracking was lost across the change — typically the headset was off, and
    // the wearer may be standing somewhere else entirely — so the old sample proves nothing about
    // where the room went and the group has to be re-seated to the head instead of "corrected" onto
    // a stale guess.
    inline constexpr int64_t XR_RECENTER_HEAD_MAX_AGE_NS = 500'000'000; // 0.5 s

    inline constexpr eXRRecenterFix xrRecenterFix(bool poseValid, bool headOldValid, int64_t headOldAgeNs, int64_t maxAgeNs = XR_RECENTER_HEAD_MAX_AGE_NS) {
        if (poseValid)
            return eXRRecenterFix::XR_RECENTER_APPLY_RUNTIME_POSE;
        if (headOldValid && headOldAgeNs >= 0 && headOldAgeNs <= maxAgeNs)
            return eXRRecenterFix::XR_RECENTER_SOLVE_FROM_HEAD;
        return eXRRecenterFix::XR_RECENTER_RESEAT_TO_HEAD;
    }

    // A reconstructed delta this small is the runtime telling us the frame did not really move
    // (or the solve landing on the identity it should). Applying it would still be correct, but it
    // would warp every anchor's spring state for nothing, so the caller skips it.
    inline bool xrRecenterIsNoOp(const SXRPose& m, float posEpsM = 1e-3F, float rotEpsRad = 1e-3F) {
        return m.pos.length() <= posEpsM && qAngleBetween(m.rot, Quat{}) <= rotEpsRad;
    }

    // ---- the wearer's frame (doc 03 §8.2, §8.3) ----
    //
    // The yaw-only frame standing on the floor under a head pose. Everything that re-seats a monitor
    // group "to the user" composes into THIS frame: building it at y = 0 keeps a pose's y a
    // floor-relative height, and taking yaw only keeps the group level however the head is tilted.
    inline SXRPose xrHeadFrame(const SXRPose& head) {
        return SXRPose{Vec3{head.pos.x, 0.F, head.pos.z}, qFromYaw(qYawOf(head.rot, 0.F))};
    }

    // A world (LOCAL_FLOOR) pose re-expressed in that frame — the exact inverse of the composition
    // recenterLocalToHead performs, so the two round-trip to the float.
    //
    // This is the DURABLE form of a monitor's placement. A LOCAL anchor pose names a point relative
    // to the runtime's origin, and that origin does not outlive the session (nor even survive a
    // mid-session recenter, §8.1); the same placement named relative to the WEARER means the same
    // thing in any reference space, because the wearer is the same person in all of them. Capturing
    // it while the user is actually wearing the headset is what lets a NEW session put the room back
    // the way they left it rather than replay coordinates from a frame that no longer exists.
    inline SXRPose xrPoseInHeadFrame(const SXRPose& head, const SXRPose& world) {
        return poseCompose(poseInverse(xrHeadFrame(head)), world);
    }

    // hypxrvoice GAP 2: the LOCAL_FLOOR quad pose for `place <name> at x,y,z`. The quad center sits at
    // `point`; orientation faces the head (yaw+pitch toward `headPos`, no roll). When head tracking is
    // invalid, or the head is directly above/below `point` (degenerate lookAt), it keeps `fallbackRot`
    // (the quad's current facing). Pure so it is unit-testable without a live runtime.
    inline SXRPose placeAtFacing(const Vec3& point, const Vec3& headPos, bool headValid, const Quat& fallbackRot) {
        SXRPose W;
        W.pos = point;
        W.rot = headValid ? lookAtNoRoll(point, headPos, fallbackRot) : fallbackRot;
        return W;
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

    // ---- gaze-vector monitor selection (docs/openxr/research/archive/16-gaze-grab.md §3) ----
    //
    // A raw head/eye ray jitters, so the instantaneous nearest-hit monitor is NOT a safe grab
    // target: a saccade/head-flick at the keypress frame would grab the wrong monitor. The fix
    // (research/16 §3.1, lifted from research/14) is DWELL + HYSTERESIS: a monitor becomes the
    // "dwell-stable candidate" only after the gaze has rested on it continuously for gaze_dwell_ms,
    // and switching to a different monitor (or losing the target to passthrough) likewise requires
    // that new state to persist for the dwell. This dwell-to-switch machine IS the hysteresis: an
    // adjacent-boundary flicker never accumulates enough continuous dwell to flip the selection.
    // Pure POD + one float accumulator (gtested in tests/xr/gaze_select.cpp); the 1€ pre-filter on
    // the gaze POSE (research/16 §3.1 stage B) lives on the caller (frame thread), applied before
    // the ray cast — this only consumes the resulting per-frame nearest-hit id.
    struct SXRGazeSelect {
        int64_t stable  = -1;   // the published dwell-stable candidate (-1 = none / looking at passthrough)
        int64_t pending = -1;   // the id currently accumulating dwell toward becoming `stable`
        float   dwell   = 0.F;  // seconds `pending` has been the continuous nearest hit

        void    reset() {
            stable  = -1;
            pending = -1;
            dwell   = 0.F;
        }
    };

    // Advance the gaze selection. `rawHit` is this frame's nearest-hit monitor id (-1 = the gaze
    // ray missed every quad), `dt` seconds since the last step, `dwellSec` the required rest time.
    // Returns the new dwell-stable selection. Semantics:
    //   - rawHit == stable  -> reaffirm (clear any pending switch); stays selected with no dwell.
    //   - rawHit != stable  -> accumulate dwell toward rawHit (incl. rawHit == -1, i.e. looking
    //                          away must also persist dwellSec before the selection clears). When
    //                          the accumulator reaches dwellSec, commit: stable = rawHit.
    // dwellSec <= 0 makes selection instantaneous (stable = rawHit every frame).
    inline int64_t stepGazeSelect(SXRGazeSelect& s, int64_t rawHit, float dt, float dwellSec) {
        if (rawHit == s.stable) {
            s.pending = s.stable;
            s.dwell   = 0.F;
            return s.stable;
        }
        if (dwellSec <= 0.F) {
            s.stable  = rawHit;
            s.pending = rawHit;
            s.dwell   = 0.F;
            return s.stable;
        }
        if (rawHit != s.pending) {
            s.pending = rawHit;
            s.dwell   = 0.F;
        }
        s.dwell += dt > 0.F ? dt : 0.F;
        if (s.dwell >= dwellSec) {
            s.stable = s.pending;
            s.dwell  = 0.F;
        }
        return s.stable;
    }

    // hypxrvoice GAP 4. The reported gaze candidate is the DWELL-STABLE id, but the cheap hit the
    // selection pass keeps is the NEAREST one (`rawId`), which differs from the stable id exactly
    // during a dwell transition. So the hit point belonging to the REPORTED candidate has to be
    // picked between the two intersections the pass saw this frame. stepGazeSelect() can only leave
    // `stable` at one of two values — the pre-step stable, or (on a commit) `pending`, which IS
    // `rawId` — so those two candidates are sufficient; no third quad ever needs re-intersecting.
    //
    // Pure so the pick is gtestable without a runtime (tests/xr/gaze_hit.cpp). `t` is the ray
    // parameter (distance along a unit direction); the caller turns it into a world point.
    struct SXRGazeHitPick {
        bool  valid = false;
        float t     = 0.F;
    };

    inline SXRGazeHitPick pickGazeHitT(int64_t stable, int64_t rawId, bool rawValid, float rawT, int64_t prevStable, bool prevValid, float prevT) {
        SXRGazeHitPick out;
        if (stable < 0)
            return out; // looking at passthrough — nothing is selected, so nothing was hit
        if (rawValid && stable == rawId) {
            out.valid = true;
            out.t     = rawT;
            return out;
        }
        if (prevValid && stable == prevStable) {
            out.valid = true;
            out.t     = prevT;
        }
        return out;
    }

    // ---- timestamped head-pose / gaze history ring (hypxrvoice WP-V1) ----
    //
    // A voice daemon (docs/openxr/research/VOICE-CONTROL.md) resolves deixis like "drop this
    // monitor HERE" against where the user's head was pointed AT THE MOMENT THEY SPOKE — speech
    // recognition takes 1-3s, so by parse time the head has moved on (often to the feedback HUD),
    // and pose-at-parse-time is a systematic bug. The compositor therefore keeps a short rolling
    // ring of per-frame head poses + gaze candidates that the daemon queries by monotonic
    // timestamp (`hyprctl openxr gaze at <ms>`).
    //
    // CLOCK CONTRACT: `timestampMs` is milliseconds on the SAME monotonic clock the daemon reads
    // with clock_gettime(CLOCK_MONOTONIC) — the compositor stamps it via Time::millis(
    // Time::steadyNow()), and std::chrono::steady_clock == CLOCK_MONOTONIC on Linux/libstdc++
    // (see the note in src/helpers/time/Time.cpp). So a daemon can capture `clock_gettime(
    // CLOCK_MONOTONIC)` at the instant speech begins, convert to ms, and pass it straight to
    // `gaze at <ms>` with no clock-domain translation.
    //
    // The struct is a plain POD (no strings, no refcounts) so the single writer (the XR frame
    // thread) can push it under a plain mutex without touching hyprutils refcounts or config
    // strings (the frame-thread rules in XRMonitorLayer.hpp). Names are resolved from the stored
    // MONITORID on the MAIN thread at query time.
    struct SXRPoseSample {
        int64_t timestampMs   = 0;     // CLOCK_MONOTONIC ms at capture (see CLOCK CONTRACT above)
        bool    viewValid     = false; // the head/view pose was locatable this frame
        Vec3    headPos;               // head position in LOCAL_FLOOR space (meters)
        Quat    headRot;               // head orientation ([x,y,z,w])
        int64_t gazeMonitorId = -1;    // dwell-stable gazed-at monitor id (-1 = passthrough / none)
        int64_t gazeRawId     = -1;    // instantaneous nearest-hit monitor id this frame (pre-dwell)
        float   gazeDwell     = 0.F;   // seconds accumulated toward the pending dwell switch
        // hypxrvoice GAP 4: where the gaze ray actually MET the dwell-stable monitor's quad, in the
        // same LOCAL_FLOOR meters `openxr place <name> at x,y,z` consumes. Plain values captured on
        // the frame thread by the pass that already does the intersection — the main thread never
        // recomputes it (that would mean reading live quad poses off refcounted layers). gazeHitValid
        // is false whenever the ray is not currently ON the stable monitor: looking at passthrough,
        // or mid-dwell while the selection still names the monitor the user has looked AWAY from.
        bool    gazeHitValid  = false;
        Vec3    gazeHitPoint;          // LOCAL_FLOOR meters
        float   gazeHitDist   = 0.F;   // ray distance (meters) from the gaze origin to gazeHitPoint
    };

    // Fixed-capacity single-writer ring. Power-of-two capacity so the index math is a mask.
    // `count` is the monotonic total pushed; the live window is the last min(count,N) samples in
    // nondecreasing-timestamp order. Pure POD + index math (no threading here — the manager owns
    // the mutex), so it is gtestable with no runtime (tests/xr/pose_ring.cpp).
    template <size_t N>
    struct SXRPoseRing {
        static_assert(N > 0 && (N & (N - 1)) == 0, "SXRPoseRing capacity must be a power of two");

        std::array<SXRPoseSample, N> buf{};
        uint64_t                     count = 0;

        void   push(const SXRPoseSample& s) {
            buf[count & (N - 1)] = s;
            ++count;
        }
        bool   empty() const {
            return count == 0;
        }
        size_t size() const {
            return count < N ? (size_t)count : N;
        }
        // Logical index i in [0,size): 0 == oldest live sample, size-1 == newest.
        const SXRPoseSample& at(size_t i) const {
            const uint64_t start = count - size(); // first live absolute index
            return buf[(start + i) & (N - 1)];
        }
        const SXRPoseSample& newest() const {
            return buf[(count - 1) & (N - 1)];
        }
    };

    // Nearest-in-time lookup. Returns false (and leaves `out` untouched) iff the ring is empty.
    // Otherwise `out` is the sample whose timestamp is closest to `targetMs`, CLAMPED to the
    // retained range: a target older than the oldest sample returns the oldest, newer than the
    // newest returns the newest. The caller detects staleness / clamping by comparing
    // out.timestampMs to targetMs (the IPC surface reports matchedTimestampMs + ageMs). Ties
    // (equidistant) resolve to the NEWER sample. Timestamps are nondecreasing (one writer, a
    // monotonic clock), so this is a binary search over the logical window.
    template <size_t N>
    inline bool poseRingNearest(const SXRPoseRing<N>& ring, int64_t targetMs, SXRPoseSample& out) {
        const size_t sz = ring.size();
        if (sz == 0)
            return false;
        // lower_bound: first index whose timestamp >= targetMs.
        size_t lo = 0, hi = sz;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (ring.at(mid).timestampMs < targetMs)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == 0) {
            out = ring.at(0); // target at/older than oldest -> clamp to oldest
            return true;
        }
        if (lo == sz) {
            out = ring.at(sz - 1); // target newer than newest -> clamp to newest
            return true;
        }
        const SXRPoseSample& hiS = ring.at(lo);     // timestamp >= target
        const SXRPoseSample& loS = ring.at(lo - 1); // timestamp <  target
        // Tie -> newer (hiS). |hi - target| <= |target - lo| picks hiS on equality.
        out = (hiS.timestampMs - targetMs) <= (targetMs - loS.timestampMs) ? hiS : loS;
        return true;
    }

    // ---- conditional hand-input gate (research/16 Part A) ----
    //
    // Pure decision for whether hand input is live, so the truth table is gtestable
    // (tests/xr/gaze_select.cpp) apart from the manager's atomics/clock plumbing.
    enum eXRHandPolicyV : uint8_t { XR_HANDPOL_AUTO = 0, XR_HANDPOL_ON, XR_HANDPOL_OFF };
    enum eXRHandForceV : uint8_t { XR_HANDFORCE_NONE = 0, XR_HANDFORCE_ON, XR_HANDFORCE_OFF };
    enum eXRHandGate : uint8_t {
        XR_HANDGATE_ACTIVE = 0, // hands live
        XR_HANDGATE_KBD,        // AUTO, gated because the user is typing (keyboard active)
        XR_HANDGATE_MANUAL,     // AUTO, forced off by the manual toggle latch
        XR_HANDGATE_OFF,        // policy off
    };

    // policy/force are eXRHandPolicyV/eXRHandForceV. awayFromKbd = keyboard has been idle >= the
    // threshold; roaming = the head is beyond the adaptive seat geofence; roamEnables = the
    // openxr:hand_input_roam_enables OR-term is on. force is consulted only in AUTO.
    inline eXRHandGate handInputGate(uint8_t policy, uint8_t force, bool awayFromKbd, bool roaming, bool roamEnables) {
        if (policy == XR_HANDPOL_ON)
            return XR_HANDGATE_ACTIVE;
        if (policy == XR_HANDPOL_OFF)
            return XR_HANDGATE_OFF;
        // AUTO
        if (force == XR_HANDFORCE_ON)
            return XR_HANDGATE_ACTIVE;
        if (force == XR_HANDFORCE_OFF)
            return XR_HANDGATE_MANUAL;
        return (awayFromKbd || (roamEnables && roaming)) ? XR_HANDGATE_ACTIVE : XR_HANDGATE_KBD;
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

    // The exact inverse of rayQuadIntersect's UV map: the world point at (u,v) on a quad of pose
    // `Q` and size w x h metres. Same convention — u = 0 left / 1 right, v = 0 top / 1 bottom — and
    // deliberately NOT clamped, so a UV outside [0,1] gives the corresponding point on the quad's
    // EXTENDED plane. Ray-cast cursor crossing (XRCursorCross.hpp) depends on that: the cursor's
    // overshoot past a monitor edge is exactly a UV past [0,1], and turning it back into a world
    // point is what aims the ray. Round-trips with rayQuadIntersect for any in-bounds (u,v).
    inline Vec3 quadPointFromUV(const SXRPose& Q, float w, float h, float u, float v) {
        return Q.pos + qRotate(Q.rot, Vec3{(u - 0.5F) * w, (0.5F - v) * h, 0.F});
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
    // BAR always MOVEs; each CORNER always RESIZEs (from that corner); MARGIN / NONE never grab;
    // BODY grabs conditionally. gtest-covered truth table (tests/xr/grab_gating.cpp).
    //
    // BODY grab gating:
    //   • controller (handActive==false): MOVEs iff openxr:grab_anywhere (the grip-anywhere
    //     convenience). `handBodyGrab` is ignored.
    //   • hand      (handActive==true):  MOVEs iff `handBodyGrab` — the caller has already checked
    //     openxr:hand_grab_anywhere against THIS grab's active gesture (see xrHandBodyGrabAllowed).
    //     grab_anywhere does not apply to hands. Default caller value false ⇒ WP-G5's original
    //     "hands never body-grab" behavior.
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

    inline eXRGrabAction grabActionForRegion(eXRQuadRegion region, bool grabAnywhere, bool handActive, bool handBodyGrab = false) {
        switch (region) {
            case XR_REGION_BAR: return XR_GRAB_ACTION_MOVE;
            case XR_REGION_CORNER_TL: return XR_GRAB_ACTION_RESIZE_TL;
            case XR_REGION_CORNER_TR: return XR_GRAB_ACTION_RESIZE_TR;
            case XR_REGION_CORNER_BL: return XR_GRAB_ACTION_RESIZE_BL;
            case XR_REGION_CORNER_BR: return XR_GRAB_ACTION_RESIZE_BR;
            case XR_REGION_BODY:
                if (handActive)
                    return handBodyGrab ? XR_GRAB_ACTION_MOVE : XR_GRAB_ACTION_NONE;
                return grabAnywhere ? XR_GRAB_ACTION_MOVE : XR_GRAB_ACTION_NONE;
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

    // ---- hand body-grab gating (openxr:hand_grab_anywhere) ----
    //
    // Which hand grab GESTURE (if any) may move a monitor from its CONTENT body — the hand analog of
    // openxr:grab_anywhere, but keyed on the gesture that actually crossed the grab Schmitt this grab
    // (not the openxr:hand_grab MODE). The gesture identity is xrHandGrabUsesPinch(mode, ...): true ⇒
    // this grab is a PINCH, false ⇒ a GRASP (fist). So with hand_grab=both, a fist-triggered body grab
    // is honored under GRASP/BOTH but a pinch-triggered one is not — the trigger, not the mode, decides.
    //   NONE  → hands never body-grab (original WP-G5 behavior; the bar/corners still grab).
    //   GRASP → only a fist body-grabs; a pinch stays chrome-only (and keeps its click). DEFAULT.
    //   PINCH → only a pinch body-grabs. CAVEAT: a body pinch will BOTH click (pointer press) AND grab.
    //   BOTH  → either gesture body-grabs (same pinch click+grab caveat).
    enum eXRHandGrabAnywhere : uint8_t {
        XR_HANDGRAB_ANY_NONE = 0,
        XR_HANDGRAB_ANY_GRASP,
        XR_HANDGRAB_ANY_PINCH,
        XR_HANDGRAB_ANY_BOTH,
    };

    inline eXRHandGrabAnywhere xrParseHandGrabAnywhere(const std::string& s) {
        if (s == "none")
            return XR_HANDGRAB_ANY_NONE;
        if (s == "pinch")
            return XR_HANDGRAB_ANY_PINCH;
        if (s == "both")
            return XR_HANDGRAB_ANY_BOTH;
        return XR_HANDGRAB_ANY_GRASP; // default + explicit "grasp"
    }

    inline const char* xrHandGrabAnywhereName(eXRHandGrabAnywhere m) {
        switch (m) {
            case XR_HANDGRAB_ANY_NONE: return "none";
            case XR_HANDGRAB_ANY_PINCH: return "pinch";
            case XR_HANDGRAB_ANY_BOTH: return "both";
            default: return "grasp";
        }
    }

    // Whether a hand grab whose triggering gesture was a pinch (gestureIsPinch==true) or a grasp
    // (false) is permitted to move a monitor from its BODY, under the hand_grab_anywhere config.
    inline bool xrHandBodyGrabAllowed(eXRHandGrabAnywhere cfg, bool gestureIsPinch) {
        switch (cfg) {
            case XR_HANDGRAB_ANY_NONE: return false;
            case XR_HANDGRAB_ANY_GRASP: return !gestureIsPinch;
            case XR_HANDGRAB_ANY_PINCH: return gestureIsPinch;
            case XR_HANDGRAB_ANY_BOTH: return true;
        }
        return false;
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

    // ---- adaptive anchoring: geofence + transition envelope (docs/openxr/research/13-adaptive-anchoring.md) ----
    //
    // Pure, unconditional (gtest-covered in tests/xr/adaptive.cpp): the geofence hysteresis/dwell
    // and the dock<->roam transition envelope for adaptive anchoring. Everything is POD + dt (no
    // clocks), so the whole trigger is deterministic and headless-testable and runs on the frame
    // thread with ZERO hyprutils refcount ops (XRMonitorLayer.hpp rule).

    // Easing curve for the transition envelope parameter (§4.3 / §6.1 openxr:adaptive_transition_ease).
    enum eXREase : uint8_t {
        XR_EASE_LINEAR = 0,
        XR_EASE_SMOOTHSTEP, // t*t*(3-2t) — default
        XR_EASE_OUT,        // 1-(1-t)^2
    };

    inline eXREase xrParseEase(const std::string& s) {
        if (s == "linear")
            return XR_EASE_LINEAR;
        if (s == "ease_out" || s == "easeout")
            return XR_EASE_OUT;
        return XR_EASE_SMOOTHSTEP; // default + explicit "smoothstep"
    }

    inline const char* xrEaseName(eXREase e) {
        switch (e) {
            case XR_EASE_LINEAR: return "linear";
            case XR_EASE_OUT: return "ease_out";
            default: return "smoothstep";
        }
    }

    // Apply an easing curve to a linear parameter t. All three curves fix f(0)=0, f(1)=1 and are
    // monotone non-decreasing on [0,1]; t is clamped so out-of-range inputs stay well-defined.
    inline float easeApply(eXREase e, float t) {
        t = std::clamp(t, 0.F, 1.F);
        switch (e) {
            case XR_EASE_SMOOTHSTEP: return t * t * (3.F - 2.F * t);
            case XR_EASE_OUT: return 1.F - (1.F - t) * (1.F - t);
            default: return t; // linear
        }
    }

    // Advance a transition envelope parameter one frame toward 1 (the chromeFadeAdvance shape with
    // the grace/hide terms dropped, §4.3): ramp at dt/durationSec, snap to 1 if durationSec<=0, hold
    // if dt<=0. The parameter stays LINEAR (easing is applied at sample time) so a reversed
    // transition can remap it as 1-t without distortion (§4.2).
    inline float envAdvance(float t, float dtSec, float durationSec) {
        if (durationSec <= 0.F)
            return 1.F; // no duration -> snap complete
        if (dtSec <= 0.F)
            return std::clamp(t, 0.F, 1.F); // no time elapsed -> hold
        return std::min(1.F, std::clamp(t, 0.F, 1.F) + dtSec / durationSec);
    }

    // Horizontal (XZ-plane) distance between two positions — the DEFAULT geofence metric so that
    // standing up (a change in y only) does not count as walking away (§3.1).
    inline float horizDistXZ(const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x, dz = a.z - b.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    // Full 3D distance — the geofence metric when openxr:adaptive_use_height is set.
    inline float dist3(const Vec3& a, const Vec3& b) {
        return (a - b).length();
    }

    // One dwell-accumulator step (§3.2 anti-flap core). While `condition` holds, add dt to the
    // accumulator; the instant it is false, reset to 0 — so a single spurious sample can never
    // advance the timer. Returns true once the accumulator reaches dwellSec (the condition has held
    // continuously for at least that long). Pure; the caller owns the accumulator.
    inline bool dwellStep(bool condition, float& accumSec, float dtSec, float dwellSec) {
        if (!condition) {
            accumSec = 0.F;
            return false;
        }
        accumSec += std::max(0.F, dtSec);
        return accumSec >= dwellSec;
    }

    // ---- ray aim / cursor / hover assist (docs/openxr/research/INTERACTION.md "Aiming", report 14) ----
    //
    // Pure, unconditional (gtest-covered in tests/xr/ray_assist.cpp): everything the ray-aim
    // pipeline needs that is not already an OpenXR call — the "is this a grab handle" predicate, a
    // per-region uv rect, forgiving (cone-magnetized) classification, the sticky-hover Schmitt, the
    // aim-filter pinch-onset damping, and the endpoint-cursor pack/state/size/tint helpers. All POD
    // + scalars (no clocks, no OpenXR headers), so the frame thread runs them with ZERO hyprutils
    // refcount ops (XRMonitorLayer.hpp rule) and the gtests exercise the full truth tables.

    // A grab HANDLE region — the move-bar or any corner. Body/margin/none are not handles. The
    // endpoint cursor + hover haptic + magnetism all key on this ("you are on something grabbable").
    inline bool xrRegionIsGrabbable(eXRQuadRegion r) {
        return r == XR_REGION_BAR || xrRegionIsCorner(r);
    }

    // The uv rect (top-left origin, v down) of a chrome region within the full quad. Returns false
    // for regions with no rect (body/margin/none). Corner rects match classifyQuadHit's bands
    // exactly (the square just OUTSIDE the content corner), so hysteresis + magnetism agree with the
    // hit test and the drawChrome fills.
    inline bool xrRegionRect(const SXRChromeGeometry& g, eXRQuadRegion r, float& u0, float& v0, float& u1, float& v1) {
        switch (r) {
            case XR_REGION_BAR:
                if (!(g.barU1 > g.barU0 && g.barV1 > g.barV0))
                    return false;
                u0 = g.barU0;
                v0 = g.barV0;
                u1 = g.barU1;
                v1 = g.barV1;
                return true;
            case XR_REGION_CORNER_TL:
                u0 = g.contentU0 - g.cornerU;
                v0 = g.contentV0 - g.cornerV;
                u1 = g.contentU0;
                v1 = g.contentV0;
                break;
            case XR_REGION_CORNER_TR:
                u0 = g.contentU1;
                v0 = g.contentV0 - g.cornerV;
                u1 = g.contentU1 + g.cornerU;
                v1 = g.contentV0;
                break;
            case XR_REGION_CORNER_BL:
                u0 = g.contentU0 - g.cornerU;
                v0 = g.contentV1;
                u1 = g.contentU0;
                v1 = g.contentV1 + g.cornerV;
                break;
            case XR_REGION_CORNER_BR:
                u0 = g.contentU1;
                v0 = g.contentV1;
                u1 = g.contentU1 + g.cornerU;
                v1 = g.contentV1 + g.cornerV;
                break;
            default: return false;
        }
        return g.cornerU > 0.F && g.cornerV > 0.F;
    }

    // Distance from (u,v) to the rect [u0,v0,u1,v1], measured in SLACK units per axis (0 inside).
    // Used to rank magnetism candidates and to test hysteresis exit bounds. A zero slack on an axis
    // makes any outside offset on that axis "infinitely far" (never snaps/holds along a collapsed
    // axis), which is the correct degenerate behavior.
    inline float xrRectSlackDist(float u, float v, float u0, float v0, float u1, float v1, float sU, float sV) {
        const float du = std::max(std::max(u0 - u, u - u1), 0.F);
        const float dv = std::max(std::max(v0 - v, v - v1), 0.F);
        const float nu = du <= 0.F ? 0.F : (sU > 0.F ? du / sU : 1e9F);
        const float nv = dv <= 0.F ? 0.F : (sV > 0.F ? dv / sV : 1e9F);
        return std::sqrt(nu * nu + nv * nv);
    }

    // Forgiving classification (report 14 §4 Stage A4 + Stage C, chrome-only magnetism). Start from
    // the exact classify; an exact BODY / BAR / CORNER hit is returned verbatim so content precision
    // is preserved (BODY is NEVER magnetized) and an exact handle wins. Only a MARGIN hit is upgraded:
    // if a grab HANDLE lies within (slackU,slackV) uv, snap to the NEAREST handle so the highlight
    // (and grab eligibility) turns on exactly when a squeeze — which already forgives the same cone —
    // would grab. slackU/slackV are the cone slack in uv (tan(cone)*t / quadMeters per axis); pass 0
    // to disable magnetism (pure alignment then reduces to the exact classify). The cursor stays at
    // the RAW uv (only the region is forgiven), so the user still sees precisely where they point.
    inline eXRQuadRegion classifyQuadRegionForgiving(float u, float v, const SXRChromeGeometry& g, float slackU, float slackV) {
        const eXRQuadRegion base = classifyQuadHit(u, v, g);
        if (base != XR_REGION_MARGIN)
            return base; // BODY (precision) / BAR / CORNER — never override an exact hit
        if (slackU <= 0.F && slackV <= 0.F)
            return base;

        eXRQuadRegion       best     = XR_REGION_MARGIN;
        float               bestDist = 1.F + 1e-4F; // must be within one slack unit to snap
        const eXRQuadRegion handles[] = {XR_REGION_BAR, XR_REGION_CORNER_TL, XR_REGION_CORNER_TR, XR_REGION_CORNER_BL, XR_REGION_CORNER_BR};
        for (const eXRQuadRegion r : handles) {
            float u0, v0, u1, v1;
            if (!xrRegionRect(g, r, u0, v0, u1, v1))
                continue;
            const float d = xrRectSlackDist(u, v, u0, v0, u1, v1, slackU, slackV);
            if (d < bestDist) {
                bestDist = d;
                best     = r;
            }
        }
        return best;
    }

    // ---- sticky hover (report 14 §4 Stage A2) ----
    //
    // Region transitions are otherwise instantaneous, so the highlight/grab-eligibility flickers at
    // handle boundaries and across brief tracking dropouts — a large part of "hard to grab." This is
    // the region analog of SXRSchmitt: once the ray lands a grab HANDLE, hold that region until the
    // ray leaves the handle rect expanded by a hysteresis margin (or lands a DIFFERENT handle, which
    // wins immediately), tolerating up to `maxMiss` frames where the ray misses every quad.
    struct SXRHoverStick {
        eXRQuadRegion region = XR_REGION_NONE; // published sticky region
        int64_t       mon    = -1;             // MONITORID owning the sticky region (-1 = none)
        int           miss   = 0;              // consecutive frames the ray fell off the sticky target
    };

    // One sticky-hover step. rawRegion/rawMon = this frame's (forgiving) classification on the quad
    // the ray hit (rawMon < 0 ⇒ the ray missed every quad this frame). (u,v) is the raw hit uv on
    // rawMon; g is rawMon's chrome (or the sticky monitor's when holding across a same-monitor
    // non-handle hover). exitU/exitV are the hysteresis exit margins in uv; maxMiss is the dropout
    // tolerance. Returns the region to publish and advances `s`.
    inline eXRQuadRegion stepHoverStick(SXRHoverStick& s, eXRQuadRegion rawRegion, int64_t rawMon, float u, float v, const SXRChromeGeometry& g, float exitU, float exitV,
                                        int maxMiss) {
        // A fresh grab handle always wins immediately (snappy acquisition, no lag on entry).
        if (rawMon >= 0 && xrRegionIsGrabbable(rawRegion)) {
            s.region = rawRegion;
            s.mon    = rawMon;
            s.miss   = 0;
            return s.region;
        }

        // Holding a handle: decide whether to keep it.
        if (xrRegionIsGrabbable(s.region) && s.mon >= 0) {
            if (rawMon == s.mon) {
                // The ray still hits the sticky monitor but on a non-handle region (body/margin).
                // Hold while it is within the handle rect expanded by the exit margin.
                float u0, v0, u1, v1;
                if (xrRegionRect(g, s.region, u0, v0, u1, v1)) {
                    const float d = xrRectSlackDist(u, v, u0, v0, u1, v1, exitU, exitV);
                    if (d <= 1.F) {
                        s.miss = 0;
                        return s.region; // still within the sticky exit bound -> hold
                    }
                }
                // Left the exit bound -> release to the raw region below.
            } else if (rawMon < 0) {
                // The ray missed everything: tolerate a short dropout so a 1-2 frame tracking gap
                // does not drop the highlight.
                if (++s.miss <= maxMiss)
                    return s.region;
            }
            // else: a different monitor -> release below.
        }

        s.region = rawRegion;
        s.mon    = rawMon;
        s.miss   = 0;
        return s.region;
    }

    // ---- aim-pose 1€ filter helpers (report 14 §4 Stage B) ----
    //
    // The aim pose is 1€-filtered before hit-testing (openxr:aim_filter) via the existing
    // oneEuroStepPose. The only extra decision is pinch-onset damping: while a press/pinch is
    // RAMPING UP (analog in the onset band, below the trigger-on threshold), lower the min-cutoff so
    // the commit gesture does not yank the aim off-target on the exact frame the user commits (the
    // documented Meta behavior). Outside the band the base cutoff is used.
    inline float aimFilterMinCutoff(float base, float damping, float analog, float onsetLo, float triggerOn) {
        if (analog > onsetLo && analog < triggerOn)
            return base * std::clamp(damping, 0.F, 1.F);
        return base;
    }

    // ---- endpoint cursor (report 14 §4 Stage A1) ----
    //
    // The cursor is drawn into the hovered quad's own swapchain image at the ray-hit uv (the same
    // pass as drawChrome). Its per-hand sample crosses frame->frame on ONE atomic word per hand on
    // the layer (the XRMonitorLayer.hpp visual-state contract, extended): present(1) state(3) u(14)
    // v(14). u/v are the RAW hit uv clamped to [0,1] (full quad, so the cursor can sit over content,
    // margin, or a handle).
    enum eXRCursorState : uint8_t {
        XR_CURSOR_HIDDEN = 0,
        XR_CURSOR_IDLE,      // over body / margin
        XR_CURSOR_GRABBABLE, // over the move-bar or a corner (a squeeze would grab)
        XR_CURSOR_PRESS,     // a button / pinch is pressed (cursor grows)
        XR_CURSOR_GRAB,      // actively grabbing (ray suppressed; reserved)
    };

    inline uint32_t xrPackCursor(bool present, eXRCursorState st, float u, float v) {
        const uint32_t uq = (uint32_t)std::lround(std::clamp(u, 0.F, 1.F) * 16383.F) & 0x3FFF;
        const uint32_t vq = (uint32_t)std::lround(std::clamp(v, 0.F, 1.F) * 16383.F) & 0x3FFF;
        const uint32_t sp = present ? 1u : 0u;
        return (sp << 31) | (((uint32_t)st & 0x7u) << 28) | (uq << 14) | vq;
    }

    inline void xrUnpackCursor(uint32_t w, bool& present, eXRCursorState& st, float& u, float& v) {
        present = (w >> 31) & 0x1u;
        st      = (eXRCursorState)((w >> 28) & 0x7u);
        u       = (float)((w >> 14) & 0x3FFFu) / 16383.F;
        v       = (float)(w & 0x3FFFu) / 16383.F;
    }

    // Damage dead-band for the endpoint cursor. Returns true iff the cursor's on-screen appearance
    // changed enough to warrant re-compositing the swapchain image. The 14-bit uv packing resolves
    // ~0.15px on a 2560px quad, so at-rest hand/controller tremor (the 1€ aim filter smooths velocity
    // but leaves no dead-band at rest) flips the packed word EVERY frame. Without this gate a bare
    // hover over a static desktop takes the full-swapchain restoreSnapshot + re-release path at the
    // runtime's 90Hz — WiVRn then re-encodes an essentially-static frame every frame, a sustained
    // NVENC/network burst that shows up live as dropped IDR keyframes and macroblock corruption
    // (report: IDR drops cluster exactly in hover windows, silent when the ray leaves the quad).
    //
    // prevDrawn is the packed word we LAST actually drew (compare against it, not the last sample, so
    // slow drift accumulates until it crosses the band instead of being lost). epsilonUV is the
    // per-axis movement dead-band in uv units. Appearance/disappearance and state (color/size) changes
    // always redraw; two hidden cursors never do.
    inline bool xrCursorRedrawNeeded(uint32_t prevDrawn, uint32_t cur, float epsilonUV) {
        bool           pp = false, cp = false;
        eXRCursorState ps = XR_CURSOR_HIDDEN, cs = XR_CURSOR_HIDDEN;
        float          pu = 0.F, pv = 0.F, cu = 0.F, cv = 0.F;
        xrUnpackCursor(prevDrawn, pp, ps, pu, pv);
        xrUnpackCursor(cur, cp, cs, cu, cv);
        if (pp != cp)  // appeared or disappeared -> must draw the dot or erase it
            return true;
        if (!cp)  // both hidden -> nothing on screen either way
            return false;
        if (ps != cs)  // state drives color and press-size -> visible change
            return true;
        return std::fabs(cu - pu) > epsilonUV || std::fabs(cv - pv) > epsilonUV;
    }

    // Endpoint-cursor color for a state, from the openxr:cursor_col_* palette.
    inline uint32_t xrCursorColorFor(eXRCursorState st, uint32_t idle, uint32_t grabbable, uint32_t press, uint32_t grab) {
        switch (st) {
            case XR_CURSOR_GRABBABLE: return grabbable;
            case XR_CURSOR_PRESS: return press;
            case XR_CURSOR_GRAB: return grab;
            default: return idle;
        }
    }

    // Per-hand cursor tint so two simultaneous rays are distinguishable (openxr:cursor_per_hand_tint).
    // The right hand keeps the palette color; the left hand is shifted COOLER (red pulled down, blue
    // pushed up) with alpha + green preserved. Deterministic (gtested); enable=false is identity.
    inline uint32_t xrCursorTint(uint32_t argb, int hand /* 0 = left, 1 = right */, bool enable) {
        if (!enable || hand != 0)
            return argb;
        const uint32_t a = (argb >> 24) & 0xFF;
        const uint32_t r = (argb >> 16) & 0xFF;
        const uint32_t g = (argb >> 8) & 0xFF;
        const uint32_t b = argb & 0xFF;
        const uint32_t rr = (uint32_t)std::lround(r * 0.40F);
        const uint32_t bb = b + (uint32_t)std::lround((255.F - b) * 0.50F);
        return (a << 24) | (rr << 16) | (g << 8) | (bb & 0xFF);
    }

    // Endpoint-cursor uv half-extents from a metric diameter and the FULL quad meters (per-axis so
    // the dot stays metrically round across the quad aspect); grown by pressScale while pressed.
    inline void xrCursorRadiusUV(float diamM, float quadWm, float quadHm, bool pressed, float pressScale, float& ru, float& rv) {
        const float scale = pressed ? std::max(1.F, pressScale) : 1.F;
        ru                = quadWm > 0.F ? 0.5F * diamM * scale / quadWm : 0.F;
        rv                = quadHm > 0.F ? 0.5F * diamM * scale / quadHm : 0.F;
    }

    // ---- luma-keyed transparency, a.k.a. "black-as-alpha" (openxr:black_alpha, report 09 §2.2) ----
    //
    // The blit normally pins content alpha to 1.0 so XRGB garbage alpha can't punch holes under
    // ALPHA_BLEND passthrough. With black_alpha < 1 we instead DERIVE the alpha from the pixel's own
    // Rec.709 luma: pure black gets `blackAlpha`, anything at or above `knee` stays fully opaque, with
    // a smoothstep ramp between — so a dark desktop reads as an AR overlay (the room shows through the
    // black, the pixels stay solid).
    //
    // KEEP IN SYNC with the GLSL in CXRGraphics::initBlitGL — this is the host-side reference the
    // gtests exercise (tests/xr/black_alpha.cpp); the shader must compute the identical curve.

    inline constexpr float XR_BLACK_ALPHA_KNEE_MIN = 0.001F; // avoid a divide-by-zero / infinite step

    // The alpha to write for a pixel of luma `luma`. blackAlpha >= 1 (the default) short-circuits to a
    // fully opaque 1.0, so the feature-off path is bit-identical to the old "force alpha 1" behavior.
    inline float xrLumaKeyAlphaFromLuma(float luma, float blackAlpha, float knee) {
        const float ba = std::clamp(blackAlpha, 0.F, 1.F);
        if (ba >= 1.F)
            return 1.F;
        const float k = std::max(knee, XR_BLACK_ALPHA_KNEE_MIN);
        float       t = std::clamp(luma / k, 0.F, 1.F);
        t             = t * t * (3.F - 2.F * t); // smoothstep(0, knee, luma)
        return ba + (1.F - ba) * t;
    }

    // Rec.709 luma of a 0..1 RGB triple (the same weights the blit shader's dot() uses).
    inline float xrRec709Luma(float r, float g, float b) {
        return 0.2126F * r + 0.7152F * g + 0.0722F * b;
    }

    inline float xrLumaKeyAlpha(float r, float g, float b, float blackAlpha, float knee) {
        return xrLumaKeyAlphaFromLuma(xrRec709Luma(r, g, b), blackAlpha, knee);
    }

    // Premultiplied output for a solid fill of color (r,g,b) under the key. Our quads are submitted
    // PREMULTIPLIED (no XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT), so rgb must be scaled by the
    // computed alpha too — scaling alpha alone leaves rgb > a and the runtime's `src=ONE,
    // dst=ONE_MINUS_SRC_ALPHA` blend then adds the content at full brightness over passthrough (the
    // classic additive-halo bug, report 09 §2.1).
    inline void xrLumaKeyPremultiplied(float r, float g, float b, float blackAlpha, float knee, float& outR, float& outG, float& outB, float& outA) {
        const float a = xrLumaKeyAlpha(r, g, b, blackAlpha, knee);
        outR          = r * a;
        outG          = g * a;
        outB          = b * a;
        outA          = a;
    }

    // Is the key doing anything at all? (Cheap frame-thread guard; also the "feature active" predicate
    // `hyprctl openxr status` reports.)
    inline bool xrBlackKeyActive(float blackAlpha) {
        return blackAlpha < 1.F;
    }
}
