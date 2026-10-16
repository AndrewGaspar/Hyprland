#include "XRPoseHistory.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <hyprutils/memory/Casts.hpp>

using Hyprutils::Memory::sc;

// research/30 §8 — see XRPoseHistory.hpp. Pure; no OpenXR, no logging, no allocation.

namespace OpenXR {
    static constexpr float XR_HIST_BIG = 1e30F;

    static Quat            canonicalQuat(const Quat& q) {
        return q.w < 0.F ? Quat{-q.x, -q.y, -q.z, -q.w} : q;
    }

    static void minInto(Vec3& a, const Vec3& b) {
        a = Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
    }

    static void maxInto(Vec3& a, const Vec3& b) {
        a = Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
    }

    float xrHeadUpY(const Quat& q) {
        return qRotate(q, Vec3{0.F, 1.F, 0.F}).y;
    }

    bool xrHeadUpright(const Quat& q) {
        // upY >= 0.5: the head is within 60° of upright, pitch and roll combined. Past that the yaw of
        // the forward vector stops describing which way the PERSON faces (at 90° it is undefined, past
        // it it is mirrored), so nothing durable may read it.
        return xrHeadUpY(q) >= 0.5F;
    }

    float xrHeadPitch(const Quat& q) {
        const Vec3 f = poseForward(q);
        return std::asin(std::clamp(f.y, -1.F, 1.F));
    }

    int64_t xrHistDtNs(int64_t prevNs, int64_t curNs) {
        return std::clamp<int64_t>(curNs - prevNs, 0, XR_HIST_MAX_DT_NS);
    }

    float xrStillnessWeight(int64_t dtNs, float angSpeed) {
        const float dt = sc<float>(std::clamp<int64_t>(dtNs, 0, XR_HIST_MAX_DT_NS)) * 1e-9F;
        const float w  = std::isfinite(angSpeed) ? std::max(0.F, angSpeed) : 0.F;
        return dt / (1.F + w / XR_STILL_OMEGA0);
    }

    // ---- summaries ----

    SXRHistSummary xrHistSummaryOf(const SXRHistSample& s, int64_t dtNs) {
        SXRHistSummary out;
        const uint32_t us   = sc<uint32_t>(std::max<int64_t>(0, dtNs) / 1000);
        const uint32_t f    = s.flags;
        out.t0Ns            = s.tNs - std::max<int64_t>(0, dtNs);
        out.t1Ns            = s.tNs;
        out.n               = 1;
        out.wornUs          = (f & XR_HF_WORN) ? us : 0;
        out.trustedUs       = (f & XR_HF_TRUSTED) ? us : 0;
        out.levelUs         = (f & XR_HF_LEVEL) ? us : 0;
        out.stillUs         = (f & XR_HF_STILL) ? us : 0;
        out.dwellUs         = (f & XR_HF_IN_DWELL) ? us : 0;
        out.gazeEvalUs      = (f & XR_HF_GAZE_EVAL) ? us : 0;
        out.gazeOnMonitorUs = (f & XR_HF_GAZE_ON_MONITOR) ? us : 0;
        out.posMin          = Vec3{XR_HIST_BIG, XR_HIST_BIG, XR_HIST_BIG};
        out.posMax          = Vec3{-XR_HIST_BIG, -XR_HIST_BIG, -XR_HIST_BIG};
        if (f & XR_HF_VALID) {
            out.posMin = s.headPos;
            out.posMax = s.headPos;
        }
        if ((f & XR_HF_LEVEL) && (f & XR_HF_TRUSTED)) {
            out.posSum   = s.headPos;
            out.nLevel   = 1;
            out.pitchSum = xrHeadPitch(s.headRot);
            if (f & XR_HF_UPRIGHT) {
                const float w   = xrStillnessWeight(dtNs, s.angSpeed);
                const float yaw = qYawOf(s.headRot, 0.F);
                out.yawWCos     = w * std::cos(yaw);
                out.yawWSin     = w * std::sin(yaw);
                out.yawW        = w;
            }
        }
        out.angSpeedMax      = s.angSpeed;
        out.linSpeedMax      = s.linSpeed;
        out.arrangeEpochLast = s.arrangeEpoch;
        out.flagsOr          = f;
        out.flagsAnd         = f;
        out.gen              = s.gen;
        out.monitorsDropped  = s.monitorsDropped ? 1 : 0;
        out.nMonitors        = std::min<uint8_t>(s.nMonitors, sc<uint8_t>(XR_HIST_MAX_MONITORS));
        for (size_t i = 0; i < out.nMonitors; ++i) {
            const auto& m = s.monitors[i];
            auto&       o = out.monitors[i];
            o.id          = m.id;
            o.n           = 1;
            o.posSum      = m.pos;
            o.posMin      = m.pos;
            o.posMax      = m.pos;
            o.rotSum      = canonicalQuat(m.rot);
            o.mode        = m.mode;
            o.flagsOr     = m.flags;
            o.flagsAnd    = m.flags;
        }
        return out;
    }

    void xrHistMonitorMerge(SXRHistMonitorSummary* dst, uint8_t& nDst, uint8_t& dropped, const SXRHistMonitorSummary* src, uint8_t nSrc) {
        for (size_t i = 0; i < nSrc; ++i) {
            const auto& b   = src[i];
            bool        hit = false;
            for (size_t j = 0; j < nDst; ++j) {
                auto& a = dst[j];
                if (a.id != b.id)
                    continue;
                a.n += b.n;
                a.posSum += b.posSum;
                minInto(a.posMin, b.posMin);
                maxInto(a.posMax, b.posMax);
                a.rotSum = Quat{a.rotSum.x + b.rotSum.x, a.rotSum.y + b.rotSum.y, a.rotSum.z + b.rotSum.z, a.rotSum.w + b.rotSum.w};
                a.mode   = b.mode; // the newer mode wins (b follows a)
                a.flagsOr |= b.flagsOr;
                a.flagsAnd &= b.flagsAnd;
                hit = true;
                break;
            }
            if (hit)
                continue;
            if (nDst < XR_HIST_MAX_MONITORS) {
                dst[nDst++] = b;
                continue;
            }
            // Overflow: keep the entries covering the most samples, and say that something was dropped.
            dropped     = 1;
            size_t minJ = 0;
            for (size_t j = 1; j < nDst; ++j) {
                if (dst[j].n < dst[minJ].n)
                    minJ = j;
            }
            if (b.n > dst[minJ].n)
                dst[minJ] = b;
        }
    }

    void xrHistMerge(SXRHistSummary& a, const SXRHistSummary& b) {
        if (a.n == 0) {
            a = b;
            return;
        }
        if (b.n == 0)
            return;
        a.t0Ns = std::min(a.t0Ns, b.t0Ns);
        a.t1Ns = std::max(a.t1Ns, b.t1Ns);
        a.n += b.n;
        a.wornUs += b.wornUs;
        a.trustedUs += b.trustedUs;
        a.levelUs += b.levelUs;
        a.stillUs += b.stillUs;
        a.dwellUs += b.dwellUs;
        a.gazeOnMonitorUs += b.gazeOnMonitorUs;
        a.gazeEvalUs += b.gazeEvalUs;
        a.posSum += b.posSum;
        a.nLevel += b.nLevel;
        minInto(a.posMin, b.posMin);
        maxInto(a.posMax, b.posMax);
        a.yawWCos += b.yawWCos;
        a.yawWSin += b.yawWSin;
        a.yawW += b.yawW;
        a.pitchSum += b.pitchSum;
        a.angSpeedMax      = std::max(a.angSpeedMax, b.angSpeedMax);
        a.linSpeedMax      = std::max(a.linSpeedMax, b.linSpeedMax);
        a.arrangeEpochLast = b.arrangeEpochLast;
        a.flagsOr |= b.flagsOr;
        a.flagsAnd &= b.flagsAnd;
        a.closedEarly = std::max(a.closedEarly, b.closedEarly);
        a.monitorsDropped |= b.monitorsDropped;
        xrHistMonitorMerge(a.monitors, a.nMonitors, a.monitorsDropped, b.monitors, b.nMonitors);
    }

    bool xrHistMeanYaw(const SXRHistSummary& s, float& yaw, float& concentration) {
        if (s.yawW <= 0.F)
            return false;
        yaw           = std::atan2(s.yawWSin, s.yawWCos);
        concentration = std::sqrt(s.yawWCos * s.yawWCos + s.yawWSin * s.yawWSin) / s.yawW;
        return true;
    }

    // ---- episodes ----

    void xrEpisodeAccumulate(SXREpisodeAccum& a, const SXRHistSample& s, int64_t dtNs) {
        const uint64_t us = sc<uint64_t>(std::max<int64_t>(0, dtNs) / 1000);
        const uint32_t f  = s.flags;
        if (!a.any) {
            a.any        = true;
            a.tStartNs   = s.tNs;
            a.posMin     = s.headPos;
            a.posMax     = s.headPos;
            a.epochStart = s.arrangeEpoch;
            a.gen        = s.gen;
        }
        a.tLastNs = s.tNs;
        a.durUs += us;
        a.epochEnd = s.arrangeEpoch;
        if (f & XR_HF_POS_TRACKED)
            a.posTracked = true;
        minInto(a.posMin, s.headPos);
        maxInto(a.posMax, s.headPos);
        a.angSpeedMax = std::max(a.angSpeedMax, s.angSpeed);
        if (f & XR_HF_LEVEL) {
            a.levelUs += us;
            a.posSumX += s.headPos.x;
            a.posSumY += s.headPos.y;
            a.posSumZ += s.headPos.z;
            ++a.nPos;
            if (f & XR_HF_UPRIGHT) {
                const double w   = xrStillnessWeight(dtNs, s.angSpeed);
                const double yaw = qYawOf(s.headRot, 0.F);
                a.yawWCos += w * std::cos(yaw);
                a.yawWSin += w * std::sin(yaw);
                a.yawW += w;
            }
        }
        if (f & XR_HF_GAZE_EVAL) {
            a.gazeEvalUs += us;
            if (s.gazeMonitorId != 0) {
                a.gazeOnUs += us;
                size_t slot = 8;
                for (size_t i = 0; i < 8; ++i) {
                    if (a.gazeIds[i] == s.gazeMonitorId || (slot == 8 && a.gazeIds[i] == 0))
                        slot = i;
                    if (a.gazeIds[i] == s.gazeMonitorId)
                        break;
                }
                if (slot == 8) {
                    // full: evict the least-gazed (a ninth monitor in one dwell is a rounding error)
                    slot = 0;
                    for (size_t i = 1; i < 8; ++i) {
                        if (a.gazeUs[i] < a.gazeUs[slot])
                            slot = i;
                    }
                    a.gazeUs[slot] = 0;
                }
                a.gazeIds[slot] = s.gazeMonitorId;
                a.gazeUs[slot] += us;
            }
        }
    }

    void xrEpisodeSubtract(SXREpisodeAccum& a, const SXREpisodeAccum& tail) {
        if (!tail.any)
            return;
        a.durUs      = a.durUs > tail.durUs ? a.durUs - tail.durUs : 0;
        a.levelUs    = a.levelUs > tail.levelUs ? a.levelUs - tail.levelUs : 0;
        a.gazeEvalUs = a.gazeEvalUs > tail.gazeEvalUs ? a.gazeEvalUs - tail.gazeEvalUs : 0;
        a.gazeOnUs   = a.gazeOnUs > tail.gazeOnUs ? a.gazeOnUs - tail.gazeOnUs : 0;
        a.posSumX -= tail.posSumX;
        a.posSumY -= tail.posSumY;
        a.posSumZ -= tail.posSumZ;
        a.nPos = a.nPos > tail.nPos ? a.nPos - tail.nPos : 0;
        a.yawWCos -= tail.yawWCos;
        a.yawWSin -= tail.yawWSin;
        a.yawW = std::max(0.0, a.yawW - tail.yawW);
        for (size_t i = 0; i < 8; ++i) {
            if (!tail.gazeIds[i])
                continue;
            for (size_t j = 0; j < 8; ++j) {
                if (a.gazeIds[j] != tail.gazeIds[i])
                    continue;
                a.gazeUs[j] = a.gazeUs[j] > tail.gazeUs[i] ? a.gazeUs[j] - tail.gazeUs[i] : 0;
            }
        }
        // min/max/angSpeedMax are not subtractable: they stay as an over-estimate (diagnostic only)
    }

    void xrEpisodeCutTail(SXREpisodeAccum& acc, const SXRHistRing<SXRHistSample, XR_HIST_T0_CAP>& t0, int64_t cutNs) {
        if (!acc.any || cutNs >= acc.tLastNs)
            return;
        SXREpisodeAccum tail;
        for (size_t k = 0; k + 1 < t0.size(); ++k) {
            const auto& cur = t0.back(k);
            if (cur.tNs > acc.tLastNs)
                continue;
            if (cur.tNs <= cutNs || cur.tNs < acc.tStartNs)
                break;
            if (!(cur.flags & XR_HF_EP_ACCUM))
                continue;
            xrEpisodeAccumulate(tail, cur, xrHistDtNs(t0.back(k + 1).tNs, cur.tNs));
        }
        xrEpisodeSubtract(acc, tail);
        acc.tLastNs = std::max(acc.tStartNs, cutNs);
    }

    SXRDwellEpisode xrEpisodeFinalize(const SXREpisodeAccum& a, uint8_t endReason) {
        SXRDwellEpisode e;
        e.acc       = a;
        e.tStartNs  = a.tStartNs;
        e.tEndNs    = a.tLastNs;
        e.endReason = endReason;
        e.durUs     = sc<uint32_t>(std::min<uint64_t>(a.durUs, std::numeric_limits<uint32_t>::max()));
        if (a.nPos)
            e.seatPos = Vec3{sc<float>(a.posSumX / sc<double>(a.nPos)), sc<float>(a.posSumY / sc<double>(a.nPos)), sc<float>(a.posSumZ / sc<double>(a.nPos))};
        if (a.yawW > 0.0) {
            e.seatYaw          = sc<float>(std::atan2(a.yawWSin, a.yawWCos));
            e.yawConcentration = sc<float>(std::sqrt(a.yawWCos * a.yawWCos + a.yawWSin * a.yawWSin) / a.yawW);
        }
        e.posMin            = a.posMin;
        e.posMax            = a.posMax;
        e.levelFrac         = a.durUs ? sc<float>(sc<double>(a.levelUs) / sc<double>(a.durUs)) : 0.F;
        e.gazeEvalUs        = sc<uint32_t>(std::min<uint64_t>(a.gazeEvalUs, std::numeric_limits<uint32_t>::max()));
        e.gazeFrac          = a.gazeEvalUs ? sc<float>(sc<double>(a.gazeOnUs) / sc<double>(a.gazeEvalUs)) : 0.F;
        e.angSpeedMax       = a.angSpeedMax;
        e.gen               = a.gen;
        e.dof               = a.posTracked ? 6 : 3;
        e.arrangeEpochStart = a.epochStart;
        e.arrangeEpochEnd   = a.epochEnd;
        // top 4 gazed, by time
        std::array<size_t, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
        std::ranges::sort(order, [&](size_t l, size_t r) { return a.gazeUs[l] > a.gazeUs[r]; });
        for (size_t i = 0; i < 4; ++i) {
            if (!a.gazeIds[order[i]] || !a.gazeUs[order[i]])
                break;
            e.gazeIds[i] = a.gazeIds[order[i]];
            e.gazeUs[i]  = sc<uint32_t>(std::min<uint64_t>(a.gazeUs[order[i]], std::numeric_limits<uint32_t>::max()));
        }
        return e;
    }

    // ---- CXRPoseHistory ----

    void CXRPoseHistory::push(SXRHistSample s) {
        const int64_t dt = m_haveLast ? xrHistDtNs(m_lastNs, s.tNs) : 0;
        if (m_haveLast && s.gen != m_lastGen) {
            closeAllTiers(1);
            if (m_ep.any)
                finishEpisode(m_ep.tLastNs, XR_EP_GEN);
        }
        s.flags &= ~(sc<uint32_t>(XR_HF_IN_DWELL) | sc<uint32_t>(XR_HF_EP_ACCUM));
        stepEpisode(s, dt);
        m_t0.push(s);
        foldIntoTier(0, xrHistSummaryOf(s, dt));
        m_lastNs   = s.tNs;
        m_lastGen  = s.gen;
        m_haveLast = true;
    }

    void CXRPoseHistory::endSession(int64_t tNs) {
        closeAllTiers(2);
        if (m_ep.any)
            finishEpisode(std::min(tNs, m_ep.tLastNs), XR_EP_SESSION);
        m_haveLast = false;
    }

    void CXRPoseHistory::closeEpisode(int64_t tEndNs, uint8_t reason) {
        if (!m_ep.any)
            return;
        finishEpisode(std::min(tEndNs, m_ep.tLastNs), reason);
    }

    void CXRPoseHistory::recordGen(const SXRGenRecord& g) {
        m_gens.push(g);
    }

    void CXRPoseHistory::journal(const SXRJournalEntry& e) {
        m_journal.push(e);
    }

    const SXRHistRing<SXRHistSample, XR_HIST_T0_CAP>& CXRPoseHistory::t0() const {
        return m_t0;
    }

    const SXRHistRing<SXRHistSummary, XR_HIST_TIER_CAP>& CXRPoseHistory::tier(size_t k) const {
        return m_tiers[std::min(k, XR_HIST_TIERS - 1)];
    }

    const SXRHistSummary& CXRPoseHistory::openBucket(size_t k) const {
        return m_open[std::min(k, XR_HIST_TIERS - 1)];
    }

    bool CXRPoseHistory::openBucketValid(size_t k) const {
        return m_openValid[std::min(k, XR_HIST_TIERS - 1)];
    }

    const SXRHistRing<SXRDwellEpisode, XR_HIST_EPISODE_CAP>& CXRPoseHistory::episodes() const {
        return m_episodes;
    }

    const SXRHistRing<SXRGenRecord, XR_HIST_GEN_CAP>& CXRPoseHistory::gens() const {
        return m_gens;
    }

    const SXRHistRing<SXRJournalEntry, XR_HIST_JOURNAL_CAP>& CXRPoseHistory::journalRing() const {
        return m_journal;
    }

    const SXREpisodeAccum& CXRPoseHistory::runningEpisode() const {
        return m_ep;
    }

    bool CXRPoseHistory::runningEpisodeOpen() const {
        return m_epOpen;
    }

    void CXRPoseHistory::foldIntoTier(size_t k, const SXRHistSummary& s) {
        if (!m_openValid[k]) {
            m_open[k]      = s;
            m_openValid[k] = true;
        } else
            xrHistMerge(m_open[k], s);
        if (m_open[k].t1Ns - m_open[k].t0Ns >= XR_HIST_TIER_RES_NS[k])
            closeTier(k, 0);
    }

    void CXRPoseHistory::closeTier(size_t k, uint8_t reason) {
        if (!m_openValid[k])
            return;
        if (reason)
            m_open[k].closedEarly = reason;
        m_tiers[k].push(m_open[k]);
        m_openValid[k]            = false;
        const SXRHistSummary done = m_open[k];
        m_open[k]                 = SXRHistSummary{};
        if (k + 1 < XR_HIST_TIERS)
            foldIntoTier(k + 1, done);
    }

    void CXRPoseHistory::closeAllTiers(uint8_t reason) {
        for (size_t k = 0; k < XR_HIST_TIERS; ++k) {
            closeTier(k, reason);
        }
    }

    void CXRPoseHistory::resetCandidate(const SXRHistSample& s) {
        m_ep          = SXREpisodeAccum{};
        m_epOpen      = false;
        m_epAnchor    = s.headPos;
        m_epOutsideNs = 0;
    }

    void CXRPoseHistory::finishEpisode(int64_t tEndNs, uint8_t reason) {
        if (m_epOpen) {
            SXREpisodeAccum acc = m_ep;
            xrEpisodeCutTail(acc, m_t0, tEndNs);
            auto ep = xrEpisodeFinalize(acc, reason);
            // the arrangement as it stood at the end: the newest T0 sample at or before tEndNs
            for (size_t k = 0; k < m_t0.size(); ++k) {
                const auto& cur = m_t0.back(k);
                if (cur.tNs > tEndNs)
                    continue;
                ep.nMonitors = cur.nMonitors;
                for (size_t i = 0; i < cur.nMonitors && i < XR_HIST_MAX_MONITORS; ++i) {
                    ep.arrangement[i] = cur.monitors[i];
                }
                break;
            }
            m_episodes.push(ep);
        }
        m_ep          = SXREpisodeAccum{};
        m_epOpen      = false;
        m_epOutsideNs = 0;
    }

    void CXRPoseHistory::stepEpisode(SXRHistSample& s, int64_t dtNs) {
        const uint32_t f = s.flags;
        if (!(f & XR_HF_WORN)) {
            if (m_ep.any)
                finishEpisode(m_ep.tLastNs, XR_EP_DOFF);
            return;
        }
        if (!(f & XR_HF_TRUSTED))
            return; // a tracking blink neither votes nor ends the dwell
        if (!m_ep.any)
            resetCandidate(s);

        if (f & XR_HF_POS_TRACKED) {
            const float dx = s.headPos.x - m_epAnchor.x, dz = s.headPos.z - m_epAnchor.z;
            if (std::sqrt(dx * dx + dz * dz) > m_params.seatRadius) {
                if (m_epOutsideNs == 0)
                    m_epOutsideNs = s.tNs;
                if (s.tNs - m_epOutsideNs <= m_params.walkOutNs)
                    return; // stepped away, maybe back: keep the seat's sums clean meanwhile
                finishEpisode(m_ep.tLastNs, XR_EP_WALK);
                resetCandidate(s);
            } else
                m_epOutsideNs = 0;
        }

        xrEpisodeAccumulate(m_ep, s, dtNs);
        s.flags |= XR_HF_EP_ACCUM;
        if (m_ep.nPos)
            m_epAnchor = Vec3{sc<float>(m_ep.posSumX / sc<double>(m_ep.nPos)), sc<float>(m_ep.posSumY / sc<double>(m_ep.nPos)), sc<float>(m_ep.posSumZ / sc<double>(m_ep.nPos))};
        if (!m_epOpen && sc<int64_t>(m_ep.durUs) * 1000 >= m_params.dwellMinNs && sc<double>(m_ep.levelUs) >= m_params.minLevel * sc<double>(m_ep.durUs))
            m_epOpen = true;
        if (m_epOpen)
            s.flags |= XR_HF_IN_DWELL;
    }
}
