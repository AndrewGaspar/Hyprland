#include "XRDonSettle.hpp"
#include "XRPoseHistory.hpp"

#include <algorithm>
#include <cmath>

namespace OpenXR {
    float xrHeadRoll(const Quat& q) {
        const Vec3 right = qRotate(q, Vec3{1.F, 0.F, 0.F});
        return std::asin(std::clamp(-right.y, -1.F, 1.F));
    }

    float xrDonFadeAlpha(int64_t sinceNs, int64_t fadeNs) {
        if (fadeNs <= 0)
            return 1.F;
        if (sinceNs <= 0)
            return 0.F;
        const float t = std::clamp(static_cast<float>(static_cast<double>(sinceNs) / static_cast<double>(fadeNs)), 0.F, 1.F);
        return t * t * (3.F - 2.F * t);
    }

    void CXRSettleDetector::reset(int64_t tEdgeNs, const SXRSettleParams& p) {
        m_head         = 0;
        m_n            = 0;
        m_active       = true;
        m_params       = p;
        m_tEdgeNs      = tEdgeNs;
        m_best         = SWindow{};
        m_haveReclined = false;
        m_reclined     = SSample{};
        m_blockCounts.fill(0);
    }

    bool CXRSettleDetector::active() const {
        return m_active;
    }

    void CXRSettleDetector::noteBlock(eXRSettleBlock b) {
        if (b < m_blockCounts.size())
            ++m_blockCounts[b];
    }

    CXRSettleDetector::SWindow CXRSettleDetector::trailingWindow(int64_t spanNs, bool usableOnly) const {
        SWindow w;
        if (m_n == 0)
            return w;
        const int64_t tNewest = m_ring[(m_head + RING - 1) % RING].tNs;
        Vec3          sumPos;
        float         sumAng = 0.F, maxAng = 0.F, sumPitch = 0.F, sumRoll = 0.F, sx = 0.F, sz = 0.F;
        size_t        n       = 0;
        bool          covered = false;
        for (size_t k = 0; k < m_n; ++k) {
            const auto& s = m_ring[(m_head + RING - 1 - k) % RING];
            if (!(usableOnly ? s.usable : s.ok))
                break;
            sumPos += s.pos;
            sumAng += s.ang;
            maxAng = std::max(maxAng, s.ang);
            sumPitch += s.pitch;
            sumRoll += s.roll;
            sx += std::sin(s.yaw);
            sz += std::cos(s.yaw);
            ++n;
            if (s.tNs <= tNewest - spanNs) {
                covered = true;
                break;
            }
        }
        if (!covered || n == 0)
            return w;
        const float fn = static_cast<float>(n);
        w.valid        = true;
        w.pos          = sumPos / fn;
        w.yaw          = std::atan2(sx, sz);
        w.angMean      = sumAng / fn;
        w.angMax       = maxAng;
        w.pitch        = sumPitch / fn;
        w.roll         = sumRoll / fn;
        return w;
    }

    SXRSettleResult CXRSettleDetector::step(const SXRSettleInput& in) {
        SXRSettleResult r;
        if (!m_active)
            return r;

        // ---- classify this sample ----
        SSample s;
        s.tNs              = in.tNs;
        s.pos              = in.head.pos;
        const bool upright = in.valid && xrHeadUpright(in.head.rot);
        s.pitch            = in.valid ? xrHeadPitch(in.head.rot) : 0.F;
        s.roll             = in.valid ? xrHeadRoll(in.head.rot) : 0.F;
        s.yaw              = upright ? qYawOf(in.head.rot, 0.F) : 0.F;
        s.ang              = in.angSpeed;
        const bool level   = upright && std::fabs(s.pitch) <= XR_SETTLE_PITCH && std::fabs(s.roll) <= XR_SETTLE_ROLL;
        const bool posOk   = !m_params.needPos || in.posTracked;
        const bool moving  = in.angSpeed >= XR_SETTLE_ANG_MAX || (in.posTracked && in.linSpeed >= XR_SETTLE_LIN_MAX);
        s.usable           = in.valid && in.trusted && level;

        eXRSettleBlock block = XR_SBLK_NONE;
        if (!in.valid || !in.trusted)
            block = XR_SBLK_NO_VIEW;
        else if (!posOk)
            block = XR_SBLK_NO_POSITION;
        else if (m_params.mode == XR_SETTLE_POSE) {
            if (!upright)
                block = XR_SBLK_NOT_UPRIGHT;
            else if (!level)
                block = XR_SBLK_NOT_LEVEL;
            else if (moving)
                block = XR_SBLK_MOVING;
        }
        s.ok = block == XR_SBLK_NONE;
        noteBlock(block);

        m_ring[m_head] = s;
        m_head         = (m_head + 1) % RING;
        m_n            = std::min(m_n + 1, RING);

        // ---- fallback bookkeeping (rungs 1 and 2) ----
        if (s.usable) {
            const auto sub = trailingWindow(XR_SETTLE_BEST_SUB_NS, true);
            if (sub.valid && (!m_best.valid || sub.angMean < m_best.angMean))
                m_best = sub;
        }
        if (in.valid && in.trusted && upright && std::fabs(s.pitch) <= XR_SETTLE_RECLINED_PITCH) {
            m_haveReclined = true;
            m_reclined     = s;
        }

        const int64_t since = in.tNs - m_tEdgeNs;
        r.settleNs          = since;

        // ---- settled? ----
        if (since >= XR_SETTLE_MIN_DELAY_NS) {
            const bool pose = m_params.mode == XR_SETTLE_POSE;
            const auto w    = trailingWindow(pose ? XR_SETTLE_HOLD_NS : XR_SETTLE_TRACK_HOLD_NS, false);
            if (w.valid && (!pose || w.angMean < XR_SETTLE_ANG_MEAN)) {
                r.state    = XR_SETTLE_SETTLED;
                r.rung     = XR_RUNG_SETTLED;
                r.havePose = pose;
                r.pose     = SXRPose{w.pos, qFromYaw(w.yaw)};
                r.angMax   = w.angMax;
                r.angMean  = w.angMean;
                r.pitch    = w.pitch;
                r.roll     = w.roll;
                m_active   = false;
                return r;
            }
        }

        if (since < m_params.timeoutNs)
            return r;

        // ---- timeout: the §5.2 ladder ----
        r.state  = XR_SETTLE_TIMEOUT;
        m_active = false;
        // the reason the hold kept breaking: the commonest block since the edge
        size_t   best  = XR_SBLK_NONE;
        uint32_t bestN = 0;
        for (size_t b = 1; b < m_blockCounts.size(); ++b) {
            if (m_blockCounts[b] <= bestN)
                continue;
            best  = b;
            bestN = m_blockCounts[b];
        }
        r.block = static_cast<eXRSettleBlock>(best);
        if (m_params.mode == XR_SETTLE_TRACKING) {
            // nothing was going to move: show in place either way
            r.rung = XR_RUNG_IN_PLACE;
            return r;
        }
        if (m_best.valid) {
            r.rung     = XR_RUNG_BEST_WINDOW;
            r.havePose = true;
            r.pose     = SXRPose{m_best.pos, qFromYaw(m_best.yaw)};
            r.angMax   = m_best.angMax;
            r.angMean  = m_best.angMean;
            r.pitch    = m_best.pitch;
            r.roll     = m_best.roll;
            return r;
        }
        if (m_haveReclined) {
            r.rung     = XR_RUNG_LAST_UPRIGHT;
            r.havePose = true;
            r.pose     = SXRPose{m_reclined.pos, qFromYaw(m_reclined.yaw)};
            r.angMax   = m_reclined.ang;
            r.angMean  = m_reclined.ang;
            r.pitch    = m_reclined.pitch;
            r.roll     = m_reclined.roll;
            return r;
        }
        r.rung = XR_RUNG_IN_PLACE;
        return r;
    }
}
