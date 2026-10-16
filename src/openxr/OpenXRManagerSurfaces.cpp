#include "OpenXRManager.hpp"
#ifdef HAVE_OPENXR

// research/32 surfaces, C0-C2 — COpenXRManager's adapter around the pure pieces in XRSurfaces /
// XRPlaneSource and the XR_EXT_plane_detection consumer (XRPlaneDetection). Every decision is a pure
// function; this file gathers inputs, takes locks, publishes, logs.
//
// Threads (XRMonitorLayer.hpp rules): the MAIN thread owns the sources, reads the openxr:surfaces*
// strings, polls and publishes POD snapshots under m_planesMu. The FRAME thread copies a snapshot
// when its generation moves and its epoch is current, and does everything else with plain values —
// raw references into `active` (no PXRLAYER copy), no config reads.

#include <openxr/openxr.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

#include "XRMonitorLayer.hpp"
#include "XRAnchor.hpp"
#include "XRInput.hpp"
#include "XRSession.hpp"
#include "XRPlaneDetection.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/eventLoop/EventLoopTimer.hpp"
#include "../helpers/time/Time.hpp"

#include <hyprutils/memory/Casts.hpp>

using Hyprutils::Memory::sc;

static constexpr float SURF_DEG = 3.14159265F / 180.F;

// ---- main thread ----

void COpenXRManager::publishSurfaceTuning() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:surfaces");
    static auto PDETECT  = CConfigValue<Hyprlang::INT>("openxr:surfaces_detect");
    static auto PSYNTH   = CConfigValue<std::string>("openxr:surfaces_synthetic");
    static auto PSNAP    = CConfigValue<Hyprlang::FLOAT>("openxr:surfaces_snap_distance");
    static auto PTILT    = CConfigValue<Hyprlang::FLOAT>("openxr:surfaces_stand_tilt");
    static auto PWALLS   = CConfigValue<Hyprlang::INT>("openxr:surfaces_walls");
    static auto PPOLL    = CConfigValue<Hyprlang::INT>("openxr:surfaces_poll_ms");

    if (!m_surfaceSources)
        m_surfaceSources = makeUnique<OpenXR::CXRSurfaceSources>();

    const bool enabled = *PENABLED != 0;
    m_surfacesDetect   = *PDETECT != 0;
    m_surfacePollMs    = std::clamp(sc<int>(*PPOLL), 100, 10000);

    const std::string spec = *PSYNTH;
    if (!m_surfaceSources->setSyntheticSpec(spec))
        Log::logger->log(Log::ERR, "[OPENXR] surfaces: {}", m_surfaceSources->syntheticStatus());

    OpenXR::SXRSurfaceTuning t;
    t.snapDist     = std::clamp(sc<float>(*PSNAP), 0.01F, 0.5F);
    t.exitDist     = t.snapDist * 1.5F;
    t.standTiltRad = std::clamp(sc<float>(*PTILT), 0.F, 45.F) * SURF_DEG;
    t.walls        = *PWALLS != 0;
    {
        std::scoped_lock lock(m_planesMu);
        m_surfaceTuningPub = t;
        m_planesPubGen.fetch_add(1, std::memory_order_release); // the frame thread re-reads the tuning
    }

    if (enabled != m_surfacesEnabled)
        Log::logger->log(Log::DEBUG, "[OPENXR] surfaces {}", enabled ? "enabled" : "disabled");
    m_surfacesEnabled = enabled;
    m_surfacesOn.store(enabled, std::memory_order_release);

    if (!enabled) {
        // Off: nothing is armed or previewed from the next frame on (the frame pass checks the flag),
        // and nothing is published. Attachments stay on their layers but no longer follow.
        cancelSurfaceTimer();
        m_surfaceSources->clear();
        publishPlanes(OpenXR::SXRPlaneSnapshot{}, m_surfaceEpoch.load(std::memory_order_acquire));
        return;
    }
    // A changed spec must not wait for the next tick to be published.
    m_surfaceSources->invalidate();
    if (m_running.load())
        armSurfaceTimer(0);
}

void COpenXRManager::armSurfaceTimer(int ms) {
    if (!g_pEventLoopManager)
        return;
    const auto dur = std::chrono::milliseconds(std::max(0, ms));
    if (!m_surfaceTimer) {
        m_surfaceTimer = makeShared<CEventLoopTimer>(
            dur,
            [](SP<CEventLoopTimer>, void*) {
                // capture nothing; re-check the global (requestEffectEval discipline)
                if (g_pOpenXRManager)
                    g_pOpenXRManager->onSurfacePoll();
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_surfaceTimer);
    } else
        m_surfaceTimer->updateTimeout(dur);
}

void COpenXRManager::cancelSurfaceTimer() {
    if (m_surfaceTimer)
        m_surfaceTimer->updateTimeout(std::nullopt);
}

void COpenXRManager::publishPlanes(const OpenXR::SXRPlaneSnapshot& s, uint32_t epoch) {
    std::scoped_lock lock(m_planesMu);
    m_planesPub      = s;
    m_planesPubEpoch = epoch;
    m_planesPubGen.fetch_add(1, std::memory_order_release);
}

void COpenXRManager::onSurfacePoll() {
    surfaceTraceDrain();
    if (!m_surfacesEnabled || !m_running.load() || !m_session || !m_surfaceSources) {
        cancelSurfaceTimer();
        return;
    }

    OpenXR::SXRPlanePollContext ctx;
    ctx.xrTime = m_lastDisplayTime.load(std::memory_order_acquire);
    if (!m_session->m_usingLocalFloor) {
        static auto PFLOOR = CConfigValue<Hyprlang::FLOAT>("openxr:floor_offset");
        ctx.floorShift     = sc<float>(*PFLOOR);
    }
    OpenXR::SXRPoseSample head;
    if (newestPoseSample(head) && head.viewValid) {
        ctx.head      = head.headPos;
        ctx.headValid = true;
    }

    // The epoch is read BEFORE the sources locate anything: a reference-space change racing this poll
    // makes the frame thread refuse the result rather than adopt planes located in a dead space.
    const uint32_t epoch = m_surfaceEpoch.load(std::memory_order_acquire);
    if (epoch != m_surfaceEpochSeen) {
        m_surfaceEpochSeen = epoch;
        m_surfaceSources->invalidate();
    }

    const auto               t0 = Time::steadyNow();
    OpenXR::SXRPlaneSnapshot snap;
    const bool               changed = m_surfaceSources->poll(ctx, snap);
    const auto               ms      = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - t0).count();
    if (ms > 100)
        Log::logger->log(Log::WARN, "[OPENXR] surfaces: a plane poll held the main thread {}ms ({})", ms, m_surfaceSources->runtimeStatus());

    if (changed) {
        publishPlanes(snap, epoch);
        Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: published {} plane(s), generation {} ({}; synthetic: {})", snap.n, snap.generation, m_surfaceSources->runtimeStatus(),
                         m_surfaceSources->syntheticStatus());
    }
    armSurfaceTimer(m_surfacePollMs);
}

void COpenXRManager::surfacesSessionUp() {
    if (!m_surfaceSources)
        m_surfaceSources = makeUnique<OpenXR::CXRSurfaceSources>();
    if (m_session)
        m_surfaceSources->setRuntimeSource(makeUnique<CXRPlaneDetectionSource>(m_session->m_instance, m_session->m_session, m_session->m_refSpace, m_session->m_hasPlaneDetection,
                                                                               m_session->m_planeDetectionAdvertised, m_session->m_planeDetectionFeatures));
    Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: runtime plane source {}", m_surfaceSources->runtimeStatus());
    if (m_surfacesEnabled)
        armSurfaceTimer(0);
}

void COpenXRManager::surfacesSessionDown(bool lost) {
    cancelSurfaceTimer();
    if (m_surfaceSources) {
        m_surfaceSources->detachRuntime(lost);
        m_surfaceSources->clear();
    }
    publishPlanes(OpenXR::SXRPlaneSnapshot{}, m_surfaceEpoch.load(std::memory_order_acquire));
    m_lastDisplayTime.store(0, std::memory_order_release);
}

void COpenXRManager::onSurfaceSnapped(const SXRStateEvent& e) {
    m_surfaceSnapMonitor = e.str;
    SXRSurfaceSnapRecord rec;
    {
        std::scoped_lock lock(m_surfaceStatusMu);
        rec = m_surfaceSnapRec;
    }
    Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: {} snapped onto {} plane {:#x} ({}) at [{:.2f}, {:.2f}, {:.2f}]", e.str, OpenXR::xrPlaneLabelName(rec.label), rec.planeId,
                     OpenXR::xrSnapKindName(rec.kind), rec.contact.x, rec.contact.y, rec.contact.z);
    if (!e.str.empty() && g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorsurface", std::format("{},{},{}", e.str, OpenXR::xrSnapKindName(rec.kind), OpenXR::xrPlaneLabelName(rec.label))});
}

COpenXRManager::SXRSurfaceStatus COpenXRManager::surfaceStatus() {
    SXRSurfaceStatus st;
    st.enabled = m_surfacesEnabled;
    if (m_surfaceSources) {
        st.runtime    = m_surfaceSources->runtimeStatus();
        st.synthetic  = m_surfaceSources->syntheticStatus();
        const auto& m = m_surfaceSources->merged();
        st.planes.assign(m.planes.begin(), m.planes.begin() + std::min<size_t>(m.n, m.planes.size()));
    }
    SXRSurfaceSnapRecord rec;
    {
        std::scoped_lock lock(m_surfaceStatusMu);
        rec = m_surfaceSnapRec;
    }
    if (rec.valid) {
        st.haveSnap    = true;
        st.snapMonitor = m_surfaceSnapMonitor;
        st.snapPlane   = rec.planeId;
        st.snapLabel   = OpenXR::xrPlaneLabelName(rec.label);
        st.snapKind    = OpenXR::xrSnapKindName(rec.kind);
        st.snapContact = rec.contact;
        st.snapPos     = rec.pos;
        st.snapAgeMs   = std::max<int64_t>(0, Time::millis(Time::steadyNow()) - rec.timeMs);
    }
    return st;
}

void COpenXRManager::surfaceTraceDrain() {
    std::array<SXRSurfaceTraceRec, 4> recs;
    size_t                            n    = 0;
    uint32_t                          lost = 0;
    {
        std::scoped_lock lock(m_surfaceTraceMu);
        n                  = m_surfaceTraceCount;
        const size_t first = (m_surfaceTraceHead + m_surfaceTrace.size() - n) % m_surfaceTrace.size();
        for (size_t i = 0; i < n; ++i)
            recs[i] = m_surfaceTrace[(first + i) % m_surfaceTrace.size()];
        lost                = m_surfaceTraceLost;
        m_surfaceTraceCount = 0;
        m_surfaceTraceLost  = 0;
    }
    if (lost)
        Log::logger->log(Log::DEBUG, "[OPENXR] surfaces trace: {} record(s) dropped before this drain", lost);

    for (size_t r = 0; r < n; ++r) {
        const auto& rec = recs[r];
        const auto& q   = rec.query;
        // The planes nearest the window's bottom first: those are the ones that matter. Clutter and
        // planes far from it are counted, not listed.
        std::array<const SXRSurfaceTraceRec::SPlane*, OpenXR::XR_PLANE_MAX> order{};
        const size_t                                                        np = std::min<size_t>(rec.nPlanes, order.size());
        for (size_t i = 0; i < np; ++i)
            order[i] = &rec.planes[i];
        std::sort(order.begin(), order.begin() + np, [](const auto* a, const auto* b) { return std::fabs(a->diag.lowest) < std::fabs(b->diag.lowest); });

        std::string planes;
        size_t      listed = 0;
        for (size_t i = 0; i < np; ++i) {
            const auto& p = *order[i];
            if (listed >= 6)
                break;
            planes += std::format(" | {:#x} {} {} h{:.2f}: lowest {:+.3f} center {:+.3f} {} -> {}", p.id, OpenXR::xrPlaneLabelName(p.label), OpenXR::xrPlaneOrientName(p.orient),
                                  p.height, p.diag.lowest, p.diag.centerH, p.diag.contained ? "over" : "not over", OpenXR::xrSurfaceVerdictName(p.verdict));
            ++listed;
        }
        if (np > listed)
            planes += std::format(" | +{} more", np - listed);
        const std::string name = rec.monitor.data(); // NUL-terminated: copied with room for it
        Log::logger->log(Log::DEBUG, "[OPENXR] surfaces trace: {} {} at [{:.2f}, {:.2f}, {:.2f}] halfW {:.2f} bottom {:.2f} head {}[{:.2f}, {:.2f}, {:.2f}]: {} plane(s), {}{}",
                         name, rec.settle ? "nudged" : "carried", q.content.pos.x, q.content.pos.y, q.content.pos.z, q.halfWidth, q.bottomExt, q.headValid ? "" : "(invalid) ",
                         q.head.x, q.head.y, q.head.z, rec.nPlanes,
                         rec.picked ? std::format("snap {:#x}", rec.picked) : (rec.armed ? std::format("armed {:#x}", rec.armed) : std::string("nothing qualifies")), planes);
    }
}

void COpenXRManager::surfaceLogGazeRelease(const CXRMonitorLayer& l) {
    // MAIN, under m_layersMu, just before endGazeGrab: what the release will re-decide against.
    if (!m_surfacesEnabled)
        return;
    surfaceTraceDrain(); // the carry's last records first, so the log reads in order
    const OpenXR::SXRSurfaceArm& arm = l.m_anchor.surfaceArm();
    if (!arm.havePlane)
        Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: gaze release of {} with nothing armed — it stays where it is", l.m_monitorName);
    else
        Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: gaze release of {} with {} plane {:#x} {}", l.m_monitorName, OpenXR::xrPlaneLabelName(arm.plane.label), arm.plane.id,
                         arm.preview ? "previewed" : "armed (not previewed: it must be back within the entry distance)");
}

// ---- frame thread ----

void COpenXRManager::surfaceTraceRecord(const CXRMonitorLayer& l, const OpenXR::SXRSurfaceQuery& q, bool settle, uint64_t picked, uint64_t armed, uint64_t sticky) {
    // POD only: the record is filled on the stack and copied into the ring.
    SXRSurfaceTraceRec rec;
    std::copy_n(l.m_monitorName.data(), std::min(l.m_monitorName.size(), rec.monitor.size() - 1), rec.monitor.data());
    rec.settle                          = settle;
    rec.query                           = q;
    rec.picked                          = picked;
    rec.armed                           = armed;
    const OpenXR::SXRSurfaceTuning& tun = m_surfaceTuningFrame;
    const size_t                    np  = std::min<size_t>(m_planesFrame.n, m_planesFrame.planes.size());
    rec.nPlanes                         = sc<uint8_t>(np);
    for (size_t i = 0; i < np; ++i) {
        const OpenXR::SXRPlane& p     = m_planesFrame.planes[i];
        const bool              stick = sticky != 0 && p.id == sticky;
        auto&                   out   = rec.planes[i];
        OpenXR::xrSurfaceEvaluate(p, q, tun, stick ? std::max(tun.exitDist, tun.snapDist) : tun.snapDist, &out.diag);
        out.id      = p.id;
        out.label   = p.label;
        out.orient  = p.orient;
        out.verdict = out.diag.verdict;
        out.height  = p.pose.pos.y;
    }
    std::scoped_lock lock(m_surfaceTraceMu);
    m_surfaceTrace[m_surfaceTraceHead] = rec;
    m_surfaceTraceHead                 = (m_surfaceTraceHead + 1) % m_surfaceTrace.size();
    if (m_surfaceTraceCount < m_surfaceTrace.size())
        ++m_surfaceTraceCount;
    else
        ++m_surfaceTraceLost;
}

// The window as the snap sees it: `content` (the content pose before any preview), the visible
// half-width and the distance from the content center down to the visible bottom edge (content
// half-height + bottom chrome margin + move-bar). The ray is the gaze (view forward); a hand carry
// replaces it with the aim ray.
static OpenXR::SXRSurfaceQuery surfaceQueryFor(const CXRMonitorLayer& l, const OpenXR::SXRSolveResult& res, const OpenXR::SXRPose& content, const OpenXR::SXRPose& view,
                                               bool viewValid) {
    const OpenXR::SXRChromeGeometry& chrome = l.m_chrome;
    const float                      quadW  = res.widthMeters / (chrome.contentFracW() > 0.F ? chrome.contentFracW() : 1.F);
    const float                      quadH  = res.heightMeters / (chrome.contentFracH() > 0.F ? chrome.contentFracH() : 1.F);
    OpenXR::SXRSurfaceQuery          q;
    q.content   = content;
    q.halfWidth = quadW * 0.5F;
    q.bottomExt = res.heightMeters * 0.5F + quadH * std::max(0.F, 1.F - chrome.contentV1);
    q.head      = view.pos;
    q.headValid = viewValid;
    if (viewValid) {
        q.haveRay   = true;
        q.rayOrigin = view.pos;
        q.rayDir    = OpenXR::poseForward(view.rot);
    }
    return q;
}

void COpenXRManager::surfaceReexpress(const OpenXR::SXRPose& m) {
    // Called from applyReferenceSpaceChange, inside its m_layersMu scope for the attachments.
    OpenXR::xrPlanesReexpress(m_planesFrame, m);
    const OpenXR::SXRPose invM = OpenXR::poseInverse(m);
    for (auto& l : m_layers) {
        if (l->m_surfaceAttach.valid)
            l->m_surfaceAttach.placed = OpenXR::poseCompose(invM, l->m_surfaceAttach.placed);
    }
    m_surfaceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

void COpenXRManager::surfaceDiscontinuity() {
    // Nothing describes how far the world moved: the frame copy describes nowhere. The next poll
    // (fresh epoch) re-publishes from the sources; until then nothing snaps.
    m_planesFrame = OpenXR::SXRPlaneSnapshot{};
    m_surfaceEpoch.fetch_add(1, std::memory_order_acq_rel);
}

void COpenXRManager::surfaceFramePass(const std::vector<PXRLAYER>& active, const std::vector<OpenXR::SXRSolveResult>& results, const std::vector<bool>& solved,
                                      const OpenXR::SXRPose& view, bool viewValid, float floorShift) {
    // 1. Adopt a newly published set — only if it was located in the space we are in now.
    bool adopted = false;
    if (const uint32_t gen = m_planesPubGen.load(std::memory_order_acquire); gen != m_planesFrameGen) {
        std::scoped_lock lock(m_planesMu);
        m_planesFrameGen     = m_planesPubGen.load(std::memory_order_acquire);
        m_surfaceTuningFrame = m_surfaceTuningPub;
        if (m_planesPubEpoch == m_surfaceEpoch.load(std::memory_order_acquire) && m_planesPub.generation != m_planesFrame.generation) {
            m_planesFrame = m_planesPub;
            adopted       = true;
        }
    }

    const bool                      on      = m_surfacesOn.load(std::memory_order_acquire);
    const OpenXR::SXRSurfaceTuning& tun     = m_surfaceTuningFrame;
    const OpenXR::SXRPlane*         planes  = m_planesFrame.planes.data();
    const size_t                    nPlanes = std::min<size_t>(m_planesFrame.n, m_planesFrame.planes.size());

    // the carry trace's rate limit: one record per carried window per XR_SURFACE_TRACE_MS
    const int64_t    nowMs    = Time::millis(Time::steadyNow());
    const bool       traceDue = nowMs - m_surfaceTraceLastMs >= XR_SURFACE_TRACE_MS;
    bool             traced   = false;

    std::scoped_lock lock(m_layersMu);
    for (size_t i = 0; i < active.size(); ++i) {
        CXRMonitorLayer&   l = *active[i];
        OpenXR::CXRAnchor& a = l.m_anchor;

        // 1b. A keyboard nudge (gazepush on a monitor nobody carries) moved a world-placed window:
        //     a one-step carry, so it settles onto a surface it now meets exactly as a release would.
        if (a.takeSurfaceSettle() && on && solved[i] && !a.grabbed() && !a.gazeGrabbed() && a.state().mode == OpenXR::XR_ANCHOR_LOCAL &&
            (!a.adaptiveEnabled() || a.adaptivePhase() == OpenXR::XRAD_DOCKED)) {
            const OpenXR::SXRSurfaceQuery q = surfaceQueryFor(l, results[i], a.state().anchorPose, view, viewValid);
            const auto                    c = OpenXR::xrSurfacePick(planes, nPlanes, q, tun, 0);
            surfaceTraceRecord(l, q, true, c.ok ? c.planeId : 0, 0, 0); // every nudge: they are discrete
            if (c.ok)
                a.surfaceSettleAt(c);
        }

        // 2. A release this frame (or a gaze release on the main thread since the last one) snapped.
        OpenXR::SXRSurfaceCandidate snap;
        if (a.takeSurfaceSnap(snap)) {
            const OpenXR::SXRPlane* p = OpenXR::xrFindPlane(m_planesFrame, snap.planeId);
            l.m_surfaceAttach         = OpenXR::SXRSurfaceAttach{};
            if (p) {
                l.m_surfaceAttach.valid   = true;
                l.m_surfaceAttach.planeId = snap.planeId;
                l.m_surfaceAttach.kind    = snap.kind;
                l.m_surfaceAttach.local   = OpenXR::xrPoseToPlane(*p, a.state().anchorPose);
                l.m_surfaceAttach.placed  = a.state().anchorPose;
            }
            {
                std::scoped_lock slock(m_surfaceStatusMu);
                m_surfaceSnapRec = SXRSurfaceSnapRecord{
                    .valid   = true,
                    .planeId = snap.planeId,
                    .label   = p ? p->label : OpenXR::XR_PLANE_LABEL_UNKNOWN,
                    .kind    = snap.kind,
                    .contact = snap.contact,
                    .pos     = a.state().anchorPose.pos,
                    .timeMs  = Time::millis(Time::steadyNow()),
                };
            }
            enqueue(SXRStateEvent{.type = eXRStateEventType::SURFACE_SNAP, .a = sc<int32_t>(snap.kind), .str = l.m_monitorName});
        }

        const bool moveGrab = a.grabbed() && a.grabKind() == OpenXR::XR_GRABKIND_MOVE;
        const bool carried  = moveGrab || a.gazeGrabbed();
        const bool wasPrev  = l.m_surfacePreview.load(std::memory_order_relaxed);

        if (!on || !carried || !solved[i]) {
            // 3. Not carried (or the feature is off): nothing armed, nothing previewed.
            if (a.surfaceArm().havePlane)
                a.clearSurfaceArm();
            if (wasPrev) {
                l.m_surfacePreview.store(false, std::memory_order_relaxed);
                l.m_surfacePreviewPlane.store(0, std::memory_order_relaxed);
            }
            if (!on || carried || !l.m_surfaceAttach.valid)
                continue;

            // 4. Attached: something else moved it (a verb, a re-seat, a mode change) -> the relation
            //    is over; otherwise follow a refined plane, hold when the plane is gone, detach on a jump.
            auto& att = l.m_surfaceAttach;
            if (a.state().mode != OpenXR::XR_ANCHOR_LOCAL || !OpenXR::xrPoseIdentical(a.state().anchorPose, att.placed)) {
                Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: {} moved off its surface — no longer plane-relative", l.m_monitorName);
                att = OpenXR::SXRSurfaceAttach{};
                continue;
            }
            if (!adopted)
                continue;
            const OpenXR::SXRPlane* p    = OpenXR::xrFindPlane(m_planesFrame, att.planeId);
            const bool              lost = p == nullptr;
            if (lost != att.planeLost)
                Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: {}'s plane {:#x} {}", l.m_monitorName, att.planeId, lost ? "vanished — holding its world pose" : "is back");
            att.planeLost = lost;
            OpenXR::SXRPose W;
            switch (OpenXR::xrAttachStep(att, p, W)) {
                case OpenXR::XR_ATTACH_FOLLOW:
                    if (a.adaptiveEnabled() && a.adaptivePhase() != OpenXR::XRAD_DOCKED)
                        break; // roaming away from the desk: the desk pose follows once docked
                    a.placeLocalAt(W);
                    att.placed = W;
                    break;
                case OpenXR::XR_ATTACH_DETACH:
                    Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: {}'s plane {:#x} jumped — detaching, the window stays put", l.m_monitorName, att.planeId);
                    att = OpenXR::SXRSurfaceAttach{};
                    break;
                case OpenXR::XR_ATTACH_HOLD: break;
            }
            continue;
        }

        // 5. Carried: picking a window up ends any surface relation it had.
        if (l.m_surfaceAttach.valid)
            l.m_surfaceAttach = OpenXR::SXRSurfaceAttach{};

        OpenXR::SXRSurfaceQuery q = surfaceQueryFor(l, results[i], a.carryWorld(), view, viewValid);
        if (moveGrab && m_input) {
            const auto& hs = m_input->hand(a.grabHand());
            q.haveRay      = hs.aim.has_value();
            if (hs.aim) {
                q.rayOrigin = hs.aim->pos + OpenXR::Vec3{0.F, floorShift, 0.F};
                q.rayDir    = OpenXR::poseForward(hs.aim->rot);
            }
            // the controller trigger held at release lays the window flat (a hand's select is its pinch)
            q.flat = !m_input->handActive(a.grabHand()) && hs.select >= 0.5F;
        }

        const OpenXR::SXRSurfaceArm& prev   = a.surfaceArm();
        const uint64_t               sticky = prev.havePlane && prev.preview ? prev.plane.id : 0;
        const auto                   c      = OpenXR::xrSurfacePick(planes, nPlanes, q, tun, sticky);

        OpenXR::SXRSurfaceArm        arm;
        arm.query  = q;
        arm.tuning = tun;
        if (c.ok) {
            arm.havePlane   = true;
            arm.preview     = true;
            arm.plane       = planes[c.index];
            arm.previewPose = c.pose;
            arm.previewKind = c.kind;
        } else if (prev.havePlane) {
            // Keep the last plane ARMED (not previewed) while the window is still near it, so a release
            // whose latched pose sits back inside the threshold — the jerk carried the live pose out —
            // still lands.
            if (const OpenXR::SXRPlane* p = OpenXR::xrFindPlane(m_planesFrame, prev.plane.id);
                p && OpenXR::xrSurfaceEvaluate(*p, q, tun, 2.F * std::max(tun.exitDist, tun.snapDist)).ok) {
                arm.havePlane = true;
                arm.plane     = *p;
            }
        }
        a.setSurfaceArm(arm);
        if (traceDue || arm.preview != wasPrev) { // ...and always on a preview edge: the why of an exit
            surfaceTraceRecord(l, q, false, c.ok ? arm.plane.id : 0, !c.ok && arm.havePlane ? arm.plane.id : 0, sticky);
            traced = true;
        }

        if (arm.preview != wasPrev || (arm.preview && l.m_surfacePreviewPlane.load(std::memory_order_relaxed) != arm.plane.id)) {
            if (arm.preview)
                Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: {} previewing a {} on {} plane {:#x} ({:.3f}m away)", l.m_monitorName, OpenXR::xrSnapKindName(c.kind),
                                 OpenXR::xrPlaneLabelName(arm.plane.label), arm.plane.id, c.dist);
            else
                Log::logger->log(Log::DEBUG, "[OPENXR] surfaces: {} left the surface", l.m_monitorName);
        }
        l.m_surfacePreview.store(arm.preview, std::memory_order_relaxed);
        l.m_surfacePreviewPlane.store(arm.preview ? arm.plane.id : 0, std::memory_order_relaxed);
    }
    if (traced)
        m_surfaceTraceLastMs = nowMs;
}

#endif // HAVE_OPENXR
