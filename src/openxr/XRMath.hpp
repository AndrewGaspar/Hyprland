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
#include <algorithm>

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
}
