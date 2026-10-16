#pragma once

// tests/xr/donoff_trace.hpp — a synthetic head-trace builder for the don/doff gtests
// (docs/openxr/research/30-donoff-intent.md §13.1). It derives features and flags exactly the way
// COpenXRManager's frame thread does, and pushes into a CXRPoseHistory.

#include <openxr/XRDonDoff.hpp>
#include <openxr/XRPoseHistory.hpp>

#include <cmath>
#include <memory>

namespace DonOffTest {
    using namespace OpenXR;

    constexpr float DEG = 3.14159265F / 180.F;

    inline Quat     headRot(float yaw, float pitch) {
        return qMul(qFromYaw(yaw), qFromPitch(pitch));
    }

    struct STrace {
        std::unique_ptr<CXRPoseHistory> h = std::make_unique<CXRPoseHistory>();
        SXRHeadFeatures                 f;
        SXRPose                         prev;
        bool                            prevValid  = false;
        int64_t                         tNs        = 1'000'000'000'000LL;
        uint16_t                        gen        = 1;
        bool                            posTracked = true;
        bool                            worn       = true;
        bool                            gazeEval   = false;
        float                           hz         = 90.F;
        uint32_t                        epoch      = 0;
        // monitors recorded in every sample (ids + world poses)
        uint8_t            nMon = 0;
        SXRHistMonitorPose mons[XR_HIST_MAX_MONITORS];

        void               step(const SXRPose& head, uint32_t gaze = 0) {
            const float dt = 1.F / hz;
            tNs += sc(dt);
            f = xrHeadFeatures(prev, head, dt, f, posTracked, prevValid);
            SXRHistSample s;
            s.tNs          = tNs;
            s.headPos      = head.pos;
            s.headRot      = head.rot;
            s.angSpeed     = f.angSpeed;
            s.linSpeed     = f.linSpeed;
            s.gen          = gen;
            s.arrangeEpoch = epoch;
            s.flags        = XR_HF_VALID | XR_HF_TRACKED | XR_HF_TRUSTED | XR_HF_VISIBLE;
            if (posTracked)
                s.flags |= XR_HF_POS_TRACKED;
            if (worn)
                s.flags |= XR_HF_WORN;
            if (f.upright)
                s.flags |= XR_HF_UPRIGHT;
            if (f.level)
                s.flags |= XR_HF_LEVEL;
            if (f.still)
                s.flags |= XR_HF_STILL;
            if (f.deadStill)
                s.flags |= XR_HF_DEAD_STILL;
            if (gazeEval) {
                s.flags |= XR_HF_GAZE_EVAL;
                if (gaze) {
                    s.flags |= XR_HF_GAZE_ON_MONITOR;
                    s.gazeMonitorId = gaze;
                }
            }
            s.nMonitors = nMon;
            for (size_t i = 0; i < nMon; ++i) {
                s.monitors[i] = mons[i];
            }
            h->push(s);
            prev      = head;
            prevValid = true;
        }

        static int64_t sc(float s) {
            return static_cast<int64_t>(std::llround(static_cast<double>(s) * 1e9));
        }

        // hold a pose for `seconds`, with a physiological tremor (~1°/s) so it is never dead-still
        void hold(const Vec3& pos, float yaw, float pitch, float seconds, uint32_t gaze = 0) {
            const int n = static_cast<int>(seconds * hz);
            for (int i = 0; i < n; ++i) {
                const float tr = 0.15F * DEG * std::sin(static_cast<float>(i) * 0.37F);
                step(SXRPose{pos, headRot(yaw + tr, pitch + 0.5F * tr)}, gaze);
            }
        }

        // linear interpolation of position / yaw / pitch over `seconds`
        void move(const Vec3& p0, float y0, float pi0, const Vec3& p1, float y1, float pi1, float seconds, uint32_t gaze = 0) {
            const int n = std::max(1, static_cast<int>(seconds * hz));
            for (int i = 1; i <= n; ++i) {
                const float a = static_cast<float>(i) / static_cast<float>(n);
                step(SXRPose{p0 + (p1 - p0) * a, headRot(y0 + (y1 - y0) * a, pi0 + (pi1 - pi0) * a)}, gaze);
            }
        }
    };

    // three monitors in a shallow arc 1.5 m in front of a seat at the origin facing -Z
    inline SXRArrangementView desk(const Vec3& seat = Vec3{0.F, 0.F, 0.F}, float seatYaw = 0.F) {
        SXRArrangementView v;
        v.gen             = 1;
        const float az[3] = {0.F, 40.F * DEG, -40.F * DEG};
        for (size_t i = 0; i < 3; ++i) {
            const float yaw = seatYaw + az[i];
            auto&       m   = v.m[i];
            m.id            = static_cast<uint32_t>(i + 1);
            m.eligible      = true;
            const Vec3 fwd  = poseForward(qFromYaw(yaw));
            m.world         = SXRPose{Vec3{seat.x + fwd.x * 1.5F, 1.5F, seat.z + fwd.z * 1.5F}, qFromYaw(yaw)};
        }
        v.n = 3;
        return v;
    }
}
