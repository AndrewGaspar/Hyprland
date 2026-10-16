#include "XRDonDoff.hpp"

#include <algorithm>
#include <cmath>

#include <hyprutils/memory/Casts.hpp>

using Hyprutils::Memory::sc;

// research/30 — see XRDonDoff.hpp. Pure.

namespace OpenXR {
    static float wrapAngle(float a) {
        while (a > 3.14159265F)
            a -= 2.F * 3.14159265F;
        while (a < -3.14159265F)
            a += 2.F * 3.14159265F;
        return a;
    }

    static float emaAlpha(float dt, float tau) {
        return 1.F - std::exp(-dt / tau);
    }

    // ---- §5.1 ----

    SXRHeadFeatures xrHeadFeatures(const SXRPose& prev, const SXRPose& cur, float dt, const SXRHeadFeatures& prevF, bool posTracked, bool prevValid) {
        SXRHeadFeatures f = prevF;
        f.upY             = xrHeadUpY(cur.rot);
        f.upright         = f.upY >= 0.5F;
        f.pitch           = xrHeadPitch(cur.rot);
        f.level           = f.upright && std::fabs(f.pitch) <= XR_FEAT_LEVEL_PITCH;
        f.yaw             = f.upright ? qYawOf(cur.rot, prevF.yaw) : prevF.yaw;

        if (!prevValid) {
            f.angSpeed  = 0.F;
            f.linSpeed  = 0.F;
            f.angSqEma  = 0.F;
            f.linSqEma  = 0.F;
            f.observedS = 0.F;
            f.primed    = false;
        } else if (dt > 0.F && std::isfinite(dt)) {
            const float d   = std::fabs(prev.rot.x * cur.rot.x + prev.rot.y * cur.rot.y + prev.rot.z * cur.rot.z + prev.rot.w * cur.rot.w);
            const float w   = 2.F * std::acos(std::min(1.F, d)) / dt;
            const float v   = (cur.pos - prev.pos).length() / dt;
            const float wOk = std::isfinite(w) ? w : 0.F;
            const float vOk = std::isfinite(v) ? v : 0.F;
            if (!f.primed) {
                f.angSpeed = wOk;
                f.linSpeed = vOk;
                f.angSqEma = wOk * wOk;
                f.linSqEma = vOk * vOk;
                f.primed   = true;
            } else {
                f.angSpeed += emaAlpha(dt, XR_FEAT_ANG_TAU_S) * (wOk - f.angSpeed);
                f.linSpeed += emaAlpha(dt, XR_FEAT_LIN_TAU_S) * (vOk - f.linSpeed);
                const float ad = emaAlpha(dt, XR_FEAT_DEAD_TAU_S);
                f.angSqEma += ad * (wOk * wOk - f.angSqEma);
                f.linSqEma += ad * (vOk * vOk - f.linSqEma);
            }
            f.observedS += dt;
        }

        f.still     = f.angSpeed < XR_FEAT_STILL_ANG && (!posTracked || f.linSpeed < XR_FEAT_STILL_LIN);
        f.deadStill = f.observedS >= XR_FEAT_DEAD_TAU_S && std::sqrt(f.angSqEma) < XR_FEAT_DEAD_STILL_ANG && (!posTracked || std::sqrt(f.linSqEma) < XR_FEAT_DEAD_STILL_LIN);
        return f;
    }

    // ---- §6.1 ----

    bool xrSampleQuiet(uint32_t flags) {
        constexpr uint32_t NEED = XR_HF_WORN | XR_HF_TRUSTED | XR_HF_UPRIGHT | XR_HF_LEVEL | XR_HF_STILL;
        return (flags & NEED) == NEED;
    }

    SXROnset xrFindMotionOnset(const CXRPoseHistory& h, int64_t tEdgeNs, const SXROnsetParams& p) {
        SXROnset    out;
        const auto& t0      = h.t0();
        bool        inRun   = false;
        int64_t     runEnd  = 0;
        int64_t     runFrom = 0;
        for (size_t k = 0; k < t0.size(); ++k) {
            const auto& s = t0.back(k);
            if (s.tNs > tEdgeNs)
                continue;
            if (s.tNs < tEdgeNs - p.maxBackNs)
                break;
            if (!xrSampleQuiet(s.flags)) {
                if (out.found)
                    break;
                inRun = false;
                continue;
            }
            if (!inRun) {
                inRun  = true;
                runEnd = s.tNs;
            }
            runFrom = s.tNs;
            if (!out.found && runEnd - runFrom >= p.quietMinNs) {
                out.found    = true;
                out.tOnsetNs = runEnd;
            }
            if (out.found)
                out.quietStartNs = runFrom;
        }
        return out;
    }

    // ---- §6.3 ----

    float xrBearingInSeat(const SXRPose& seat, const Vec3& world) {
        const Vec3 off = poseCompose(poseInverse(xrHeadFrame(seat)), SXRPose{world, Quat{}}).pos;
        return std::atan2(off.x, -off.z);
    }

    bool xrBridgeSeat(const CXRPoseHistory& h, uint16_t fromGen, uint16_t toGen, SXRPose& seat) {
        const auto& gens = h.gens();
        uint16_t    g    = fromGen;
        for (size_t steps = 0; g != toGen; ++steps) {
            if (steps >= XR_HIST_GEN_CAP)
                return false;
            ++g;
            const SXRGenRecord* rec = nullptr;
            for (size_t k = 0; k < gens.size(); ++k) {
                if (gens.back(k).gen == g) {
                    rec = &gens.back(k);
                    break;
                }
            }
            if (!rec || rec->bridge != XR_BRIDGE_KNOWN || !xrPoseFinite(rec->M))
                return false;
            seat     = poseCompose(poseInverse(rec->M), seat);
            seat.rot = qFromYaw(qYawOf(seat.rot, 0.F));
        }
        return true;
    }

    SXRSeatCandidate xrSeatFromAccum(const SXREpisodeAccum& a, eXRSeatSource src) {
        SXRSeatCandidate c;
        c.src         = src;
        c.gen         = a.gen;
        c.tStartNs    = a.tStartNs;
        c.tEndNs      = a.tLastNs;
        c.durUs       = a.durUs;
        c.gazeEvalUs  = a.gazeEvalUs;
        c.gazeFrac    = a.gazeEvalUs ? sc<float>(sc<double>(a.gazeOnUs) / sc<double>(a.gazeEvalUs)) : 0.F;
        c.dof         = a.posTracked ? 6 : 3;
        c.spreadM     = std::sqrt((a.posMax.x - a.posMin.x) * (a.posMax.x - a.posMin.x) + (a.posMax.z - a.posMin.z) * (a.posMax.z - a.posMin.z));
        uint64_t best = 0;
        for (size_t i = 0; i < 8; ++i) {
            if (a.gazeIds[i] && a.gazeUs[i] > best) {
                best          = a.gazeUs[i];
                c.primaryGaze = a.gazeIds[i];
            }
        }
        if (a.nPos && a.yawW > 0.0) {
            const double n     = sc<double>(a.nPos);
            const float  yaw   = sc<float>(std::atan2(a.yawWSin, a.yawWCos));
            c.seat             = SXRPose{Vec3{sc<float>(a.posSumX / n), sc<float>(a.posSumY / n), sc<float>(a.posSumZ / n)}, qFromYaw(yaw)};
            c.yawConcentration = sc<float>(std::sqrt(a.yawWCos * a.yawWCos + a.yawWSin * a.yawWSin) / a.yawW);
            c.haveYaw          = std::isfinite(yaw);
        }
        return c;
    }

    static bool strongMemory(uint8_t src) {
        return src == XR_SEAT_DWELL || src == XR_SEAT_EPISODE_OLDER || src == XR_SEAT_RESEAT;
    }

    eXRSeatRefusal xrSeatPlausible(SXRSeatCandidate& c, const CXRPoseHistory& h, const SXRArrangementView& arr, const SXRSeatParams& p, float& detail, uint32_t& primary,
                                   float& primaryBearing, SXRPose& seatNow) {
        detail         = 0.F;
        primary        = 0;
        primaryBearing = 0.F;

        size_t eligible = 0;
        for (size_t i = 0; i < arr.n; ++i) {
            if (arr.m[i].eligible)
                ++eligible;
        }
        if (eligible == 0)
            return XR_SEATREF_NO_MONITORS;

        // P1: real, upright evidence (every vote in the sums was upright by construction)
        if (!c.haveYaw || !xrPoseFinite(c.seat))
            return XR_SEATREF_P1_EVIDENCE;
        if (c.yawConcentration < p.minConcentration) {
            if (!arr.groupSeatValid) {
                detail = c.yawConcentration;
                return XR_SEATREF_P1_EVIDENCE;
            }
            c.yawFromGroup = true;
        }

        // P2: a frame we can still express the seat in
        seatNow = c.seat;
        if (!xrBridgeSeat(h, c.gen, arr.gen, seatNow))
            return XR_SEATREF_P2_FRAME;
        if (c.yawFromGroup)
            seatNow.rot = qFromYaw(qYawOf(arr.groupSeat.rot, 0.F));

        // the primary: most gazed during the candidate, else the one nearest the seat's forward
        const SXRArrangementMonitor* prim = nullptr;
        for (size_t i = 0; i < arr.n; ++i) {
            if (arr.m[i].eligible && c.primaryGaze != 0 && arr.m[i].id == c.primaryGaze)
                prim = &arr.m[i];
        }
        if (!prim) {
            float best = 1e9F;
            for (size_t i = 0; i < arr.n; ++i) {
                if (!arr.m[i].eligible)
                    continue;
                const float b = std::fabs(xrBearingInSeat(seatNow, arr.m[i].world.pos));
                if (b < best) {
                    best = b;
                    prim = &arr.m[i];
                }
            }
        }
        if (!prim)
            return XR_SEATREF_NO_MONITORS;
        primary        = prim->id;
        primaryBearing = xrBearingInSeat(seatNow, prim->world.pos);

        // P3: never commit a seat whose arrangement is behind it (D3)
        if (std::fabs(primaryBearing) > p.facingMaxRad) {
            detail = std::fabs(primaryBearing) / XR_DEG;
            return XR_SEATREF_P3_FACING;
        }

        // P4: reach
        auto horiz = [&](const Vec3& w) {
            const float dx = w.x - seatNow.pos.x, dz = w.z - seatNow.pos.z;
            return std::sqrt(dx * dx + dz * dz);
        };
        if (horiz(prim->world.pos) > p.reachPrimaryM) {
            detail = horiz(prim->world.pos);
            return XR_SEATREF_P4_REACH;
        }
        for (size_t i = 0; i < arr.n; ++i) {
            if (arr.m[i].eligible && horiz(arr.m[i].world.pos) > p.reachAllM) {
                detail = horiz(arr.m[i].world.pos);
                return XR_SEATREF_P4_REACH;
            }
        }

        // P5: only when gaze actually ran long enough to have an opinion
        if (sc<int64_t>(c.gazeEvalUs) * 1000 >= p.gazeMinEvalNs && c.gazeFrac < p.gazeMinFrac) {
            detail = c.gazeFrac;
            return XR_SEATREF_P5_GAZE;
        }

        // the one ranking rule: a weak witness may not overwrite a strong memory it disagrees with
        if (c.src == XR_SEAT_QUIET && prim->memValid && (strongMemory(prim->memSrc) || arr.authoredSinceCommit)) {
            const float memBearing = std::atan2(prim->memOffset.pos.x, -prim->memOffset.pos.z);
            const float diff       = std::fabs(wrapAngle(primaryBearing - memBearing));
            if (diff > p.rankDisagreeRad) {
                detail = diff / XR_DEG;
                return XR_SEATREF_RANK;
            }
        }
        return XR_SEATREF_OK;
    }

    static void accumFromSummary(SXREpisodeAccum& a, const SXRHistSummary& b) {
        if (!a.any) {
            a.any      = true;
            a.tStartNs = b.t0Ns;
            a.tLastNs  = b.t1Ns;
            a.posMin   = b.posMin;
            a.posMax   = b.posMax;
            a.gen      = b.gen;
        }
        a.tStartNs = std::min(a.tStartNs, b.t0Ns);
        a.tLastNs  = std::max(a.tLastNs, b.t1Ns);
        a.durUs += b.wornUs;
        a.levelUs += b.levelUs;
        a.posSumX += b.posSum.x;
        a.posSumY += b.posSum.y;
        a.posSumZ += b.posSum.z;
        a.nPos += b.nLevel;
        a.yawWCos += b.yawWCos;
        a.yawWSin += b.yawWSin;
        a.yawW += b.yawW;
        a.gazeEvalUs += b.gazeEvalUs;
        a.gazeOnUs += b.gazeOnMonitorUs;
        a.posMin      = Vec3{std::min(a.posMin.x, b.posMin.x), std::min(a.posMin.y, b.posMin.y), std::min(a.posMin.z, b.posMin.z)};
        a.posMax      = Vec3{std::max(a.posMax.x, b.posMax.x), std::max(a.posMax.y, b.posMax.y), std::max(a.posMax.z, b.posMax.z)};
        a.angSpeedMax = std::max(a.angSpeedMax, b.angSpeedMax);
        if (b.flagsOr & XR_HF_POS_TRACKED)
            a.posTracked = true;
    }

    SXRSeatVerdict xrSelectSeat(const CXRPoseHistory& h, int64_t tEdgeNs, const SXRArrangementView& arr, const SXRSeatParams& p) {
        SXRSeatVerdict v;
        v.onset             = xrFindMotionOnset(h, tEdgeNs, p.onset);
        const int64_t cut   = v.onset.found ? v.onset.tOnsetNs : tEdgeNs - p.onset.maxBackNs;
        const int64_t since = tEdgeNs - p.lookbackNs;

        auto          refuse = [&](uint8_t src, uint8_t reason, float detail, int64_t tEnd) {
            if (v.nRefused >= v.refused.size())
                return;
            v.refused[v.nRefused++] = SXRSeatRefusalRec{.src = src, .reason = reason, .detail = detail, .tEndNs = tEnd};
        };
        auto attempt = [&](SXRSeatCandidate c) {
            ++v.candidates;
            float          detail = 0.F;
            uint32_t       prim   = 0;
            float          bear   = 0.F;
            SXRPose        now;
            eXRSeatRefusal r = XR_SEATREF_OK;
            if (c.src != XR_SEAT_QUIET && sc<int64_t>(c.durUs) * 1000 < p.dwellMinNs) {
                r      = XR_SEATREF_SHORT;
                detail = sc<float>(c.durUs) * 1e-6F;
            } else
                r = xrSeatPlausible(c, h, arr, p, detail, prim, bear, now);
            if (r != XR_SEATREF_OK) {
                refuse(c.src, r, detail, c.tEndNs);
                return false;
            }
            v.ok             = true;
            v.chosen         = c;
            v.seatNow        = now;
            v.primary        = prim;
            v.primaryBearing = bear;
            return true;
        };

        // 1. the open / most recent episode, cut at the motion onset
        const auto& eps        = h.episodes();
        bool        usedNewest = false;
        {
            SXREpisodeAccum acc;
            bool            have = false;
            if (h.runningEpisodeOpen() && h.runningEpisode().any) {
                acc  = h.runningEpisode();
                have = true;
            } else if (!eps.empty() && eps.back(0).tEndNs >= since) {
                acc        = eps.back(0).acc;
                have       = true;
                usedNewest = true;
            }
            if (have) {
                xrEpisodeCutTail(acc, h.t0(), cut);
                if (attempt(xrSeatFromAccum(acc, XR_SEAT_DWELL)))
                    return v;
            }
        }

        // 2. the quiet window ending at the onset
        if (v.onset.found) {
            SXREpisodeAccum acc;
            const auto&     t0 = h.t0();
            for (size_t k = t0.size(); k-- > 0;) {
                const auto& s = t0.back(k);
                if (s.tNs < v.onset.quietStartNs || s.tNs > v.onset.tOnsetNs)
                    continue;
                const int64_t dt = k + 1 < t0.size() && s.tNs > v.onset.quietStartNs ? xrHistDtNs(t0.back(k + 1).tNs, s.tNs) : 0;
                xrEpisodeAccumulate(acc, s, dt);
            }
            if (acc.any && attempt(xrSeatFromAccum(acc, XR_SEAT_QUIET)))
                return v;
        }

        // 3. older closed episodes
        size_t tries = 0;
        for (size_t k = usedNewest ? 1 : 0; k < eps.size() && tries < 32; ++k) {
            const auto& ep = eps.back(k);
            if (ep.tEndNs < since)
                break;
            if (sc<int64_t>(ep.durUs) * 1000 < p.dwellMinNs)
                continue;
            ++tries;
            SXREpisodeAccum acc = ep.acc;
            xrEpisodeCutTail(acc, h.t0(), cut);
            if (attempt(xrSeatFromAccum(acc, XR_SEAT_EPISODE_OLDER)))
                return v;
        }

        // 4. runs of T2 / T3 buckets that look like a dwell the detector missed
        for (size_t ti = 1; ti <= 2; ++ti) {
            const auto&     ring = h.tier(ti);
            SXREpisodeAccum run;
            size_t          runs  = 0;
            auto            flush = [&]() {
                const bool okRun = run.any && sc<int64_t>(run.durUs) * 1000 >= p.dwellMinNs;
                if (okRun) {
                    ++runs;
                    if (attempt(xrSeatFromAccum(run, XR_SEAT_SUMMARY)))
                        return true;
                }
                run = SXREpisodeAccum{};
                return false;
            };
            for (size_t k = 0; k < ring.size() && runs < 4; ++k) {
                const auto& b = ring.back(k);
                if (b.t1Ns > cut)
                    continue;
                if (b.t1Ns < since)
                    break;
                const int64_t span = b.t1Ns - b.t0Ns;
                bool          q    = span > 0 && b.nLevel > 0 && sc<double>(b.wornUs) * 1000.0 >= 0.9 * sc<double>(span) &&
                    sc<double>(b.levelUs) * 1000.0 >= p.summaryFrac * sc<double>(span) && sc<double>(b.stillUs) * 1000.0 >= p.summaryFrac * sc<double>(span);
                if (q && run.any && b.gen != run.gen)
                    q = false;
                if (q && run.any && (b.flagsOr & XR_HF_POS_TRACKED)) {
                    const float ex = std::max(run.posMax.x, b.posMax.x) - std::min(run.posMin.x, b.posMin.x);
                    const float ez = std::max(run.posMax.z, b.posMax.z) - std::min(run.posMin.z, b.posMin.z);
                    q              = std::sqrt(ex * ex + ez * ez) <= 2.F * p.seatRadius;
                }
                if (!q) {
                    if (flush())
                        return v;
                    continue;
                }
                accumFromSummary(run, b);
            }
            if (flush())
                return v;
        }
        return v;
    }

    const char* xrSeatSourceName(uint8_t src) {
        switch (src) {
            case XR_SEAT_DWELL: return "dwell";
            case XR_SEAT_QUIET: return "quiet";
            case XR_SEAT_EPISODE_OLDER: return "older-dwell";
            case XR_SEAT_SUMMARY: return "summary";
            case XR_SEAT_RESEAT: return "reseat";
            case XR_SEAT_CREATED: return "created";
            case XR_SEAT_DECLARED: return "declared";
            case XR_SEAT_LEGACY: return "legacy";
            default: return "none";
        }
    }

    const char* xrSeatRefusalName(uint8_t r) {
        switch (r) {
            case XR_SEATREF_OK: return "ok";
            case XR_SEATREF_SHORT: return "too short";
            case XR_SEATREF_P1_EVIDENCE: return "P1 no upright evidence";
            case XR_SEATREF_P2_FRAME: return "P2 unbridgeable frame";
            case XR_SEATREF_P3_FACING: return "P3 arrangement behind the seat";
            case XR_SEATREF_P4_REACH: return "P4 out of reach";
            case XR_SEATREF_P5_GAZE: return "P5 never looked at a monitor";
            case XR_SEATREF_RANK: return "weaker witness disagrees";
            case XR_SEATREF_NO_MONITORS: return "no eligible monitors";
            default: return "?";
        }
    }

    // ---- §4.3 ----

    SXRDonStep xrDonTransition(eXRDonState s, eXRDonInput in, const SXRDonFacts& f) {
        SXRDonStep out{.next = s, .actions = 0};
        if (in == XR_DIN_SESSION_DOWN) {
            out.next = XR_DON_NO_SESSION;
            if (s == XR_DON_WORN || ((s == XR_DON_UNWORN || s == XR_DON_DOFFING) && !f.committedSinceWorn))
                out.actions |= XR_DA_COMMIT_FROM_HISTORY;
            return out;
        }
        if (in == XR_DIN_AUTHORED) {
            if (s != XR_DON_NO_SESSION)
                out.actions |= XR_DA_MARK_AUTHORED;
            return out;
        }
        switch (s) {
            case XR_DON_NO_SESSION:
                if (in == XR_DIN_SESSION_UP)
                    out.next = XR_DON_UNWORN;
                break;
            case XR_DON_UNWORN:
                if (in == XR_DIN_WORN_UP) {
                    if (f.donGate && !f.decisionCertainResume) {
                        out.next = XR_DON_DONNING;
                        out.actions |= XR_DA_PUBLISH_DONNING;
                    } else
                        out.next = XR_DON_WORN;
                }
                break;
            case XR_DON_DONNING:
                if (in == XR_DIN_HYDRATED)
                    out.next = XR_DON_WORN;
                else if (in == XR_DIN_BACKSTOP) {
                    out.next = XR_DON_WORN;
                    out.actions |= XR_DA_SHOW_IN_PLACE | XR_DA_LOG_WARN;
                } else if (in == XR_DIN_WORN_DOWN)
                    out.next = XR_DON_UNWORN; // nothing was worn long enough to remember
                break;
            case XR_DON_WORN:
                if (in == XR_DIN_WORN_DOWN) {
                    out.next = XR_DON_DOFFING;
                    out.actions |= XR_DA_COMMIT_FROM_HISTORY;
                } else if (in == XR_DIN_RESEAT_DONE)
                    out.actions |= XR_DA_COMMIT_RESEAT;
                else if (in == XR_DIN_DISCONTINUITY && f.donGate) {
                    out.next = XR_DON_DONNING;
                    out.actions |= XR_DA_PUBLISH_DONNING;
                    if (!f.lateDiscontinuity)
                        out.actions |= XR_DA_COMMIT_FROM_HISTORY;
                }
                break;
            case XR_DON_DOFFING:
                if (in == XR_DIN_COMMIT_DONE)
                    out.next = XR_DON_UNWORN;
                else if (in == XR_DIN_WORN_UP)
                    out.next = XR_DON_WORN; // a don raced the commit: the commit already ran once
                break;
        }
        return out;
    }

    const char* xrDonStateName(uint8_t s) {
        switch (s) {
            case XR_DON_NO_SESSION: return "no-session";
            case XR_DON_UNWORN: return "unworn";
            case XR_DON_DONNING: return "donning";
            case XR_DON_WORN: return "worn";
            case XR_DON_DOFFING: return "doffing";
            default: return "?";
        }
    }
}
