#include "OpenXRManager.hpp"
#ifdef HAVE_OPENXR

// research/30 don/doff intent, Phases 0/1 — COpenXRManager's thin adapter around the pure pieces in
// XRPoseHistory / XRDonDoff: the history push (frame thread), the FSM feed, the doff commit (legacy +
// shadow selection, or the intent selection), the immediate re-seat commit, `forget`, the `donoff`
// status / dump, and opt-in trace recording. Every decision is made by a pure function; this file
// only gathers inputs, takes locks, logs and writes the results.

#include <openxr/openxr.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>

#include "XRMonitorLayer.hpp"
#include "XRAnchor.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/eventLoop/EventLoopTimer.hpp"
#include "../helpers/time/Time.hpp"
#include "../render/Renderer.hpp"

#include <hyprutils/memory/Casts.hpp>

using Hyprutils::Memory::sc;

// m_wearBits
static constexpr uint8_t XR_WB_VISIBLE            = 1u << 0;
static constexpr uint8_t XR_WB_PRESENCE_SUPPORTED = 1u << 1;
static constexpr uint8_t XR_WB_PRESENCE_KNOWN     = 1u << 2;
static constexpr uint8_t XR_WB_PRESENT            = 1u << 3;
static constexpr uint8_t XR_WB_WORN               = 1u << 4;

static constexpr float   RAD2DEG = 180.F / 3.14159265F;

static std::string       fmtDur(double s) {
    if (s < 0)
        s = 0;
    if (s < 60)
        return std::format("{:.1f}s", s);
    const int tot = sc<int>(s);
    if (tot < 3600)
        return std::format("{}m{:02}s", tot / 60, tot % 60);
    return std::format("{}h{:02}m", tot / 3600, (tot % 3600) / 60);
}

static float wrapDeg(float d) {
    while (d > 180.F)
        d -= 360.F;
    while (d < -180.F)
        d += 360.F;
    return d;
}

int64_t COpenXRManager::steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Time::steadyNow().time_since_epoch()).count();
}

bool COpenXRManager::placementMemoryIntent() const {
    // MAIN THREAD (a STRING config read). `legacy` (default in Phases 0/1) keeps the staging + drift
    // gate and only SHADOWS the selection; `intent` commits from the selection.
    static auto PMEM = CConfigValue<std::string>("openxr:placement_memory");
    return std::string{*PMEM} == "intent";
}

void COpenXRManager::publishWearBits(bool worn) {
    uint8_t b = 0;
    if (sessionVisible())
        b |= XR_WB_VISIBLE;
    if (m_userPresenceSupported)
        b |= XR_WB_PRESENCE_SUPPORTED;
    if (m_presenceKnown)
        b |= XR_WB_PRESENCE_KNOWN;
    if (m_userPresent)
        b |= XR_WB_PRESENT;
    if (worn)
        b |= XR_WB_WORN;
    m_wearBits.store(b, std::memory_order_relaxed);
}

// ---- frame thread ----

void COpenXRManager::updateHistFeatures(const OpenXR::SXRPose& view, bool viewValid, bool posTracked, float dt) {
    // FRAME THREAD, once per frame right after the head is located: the settle detector (Phase 2) and
    // the history sample both read this frame's features.
    if (viewValid) {
        m_histFeatures  = OpenXR::xrHeadFeatures(m_histPrevHead, view, dt, m_histFeatures, posTracked, m_histPrevValid);
        m_histPrevHead  = view;
        m_histPrevValid = true;
    } else
        m_histPrevValid = false;
    if (viewValid && posTracked && !m_posTrackedSeen.load(std::memory_order_relaxed))
        m_posTrackedSeen.store(true, std::memory_order_relaxed);
}

void COpenXRManager::recordHistorySample(const OpenXR::SXRPose& view, bool viewValid, bool viewTracked, bool posTracked, bool trustworthy, const SXRHistFrameMonitors& mons,
                                         bool gazeEvaluated, bool roaming) {
    // FRAME THREAD. POD only: no refcounts, no strings, no config (the tuning is all constants). The
    // features were derived earlier this frame (updateHistFeatures).

    const uint8_t         wb = m_wearBits.load(std::memory_order_relaxed);
    OpenXR::SXRHistSample s;
    s.tNs          = steadyNs();
    s.headPos      = view.pos;
    s.headRot      = view.rot;
    s.angSpeed     = viewValid ? m_histFeatures.angSpeed : 0.F;
    s.linSpeed     = viewValid ? m_histFeatures.linSpeed : 0.F;
    s.arrangeEpoch = m_arrangeEpoch.load(std::memory_order_relaxed);
    s.gen          = m_originGen.load(std::memory_order_relaxed);
    uint32_t f     = 0;
    if (viewValid) {
        f |= OpenXR::XR_HF_VALID;
        if (m_histFeatures.upright)
            f |= OpenXR::XR_HF_UPRIGHT;
        if (m_histFeatures.level)
            f |= OpenXR::XR_HF_LEVEL;
        if (m_histFeatures.still)
            f |= OpenXR::XR_HF_STILL;
        if (m_histFeatures.deadStill)
            f |= OpenXR::XR_HF_DEAD_STILL;
    }
    if (viewTracked)
        f |= OpenXR::XR_HF_TRACKED;
    if (posTracked)
        f |= OpenXR::XR_HF_POS_TRACKED;
    if (trustworthy)
        f |= OpenXR::XR_HF_TRUSTED;
    if (wb & XR_WB_VISIBLE)
        f |= OpenXR::XR_HF_VISIBLE;
    if (wb & XR_WB_PRESENT)
        f |= OpenXR::XR_HF_PRESENT;
    if (wb & XR_WB_PRESENCE_KNOWN)
        f |= OpenXR::XR_HF_PRESENCE_KNOWN;
    // §4.1: without a presence sensor, a headset lying dead still on the desk while the session is
    // still VISIBLE is not being worn, whatever the visibility says.
    const bool deadStillUnworn = viewValid && m_histFeatures.deadStill && !(wb & XR_WB_PRESENCE_SUPPORTED);
    if ((wb & XR_WB_WORN) && !deadStillUnworn)
        f |= OpenXR::XR_HF_WORN;
    if (mons.anyGrab)
        f |= OpenXR::XR_HF_GRAB;
    if (roaming)
        f |= OpenXR::XR_HF_ROAMING;

    s.nMonitors       = mons.n;
    s.monitorsDropped = mons.dropped;
    for (size_t i = 0; i < mons.n; ++i) {
        s.monitors[i] = mons.mons[i];
    }
    if (gazeEvaluated) {
        f |= OpenXR::XR_HF_GAZE_EVAL;
        if (m_gazeHitId >= 0) {
            for (size_t i = 0; i < mons.nMap; ++i) {
                if (mons.monitorIds[i] != m_gazeHitId)
                    continue;
                s.gazeMonitorId = mons.histIds[i];
                f |= OpenXR::XR_HF_GAZE_ON_MONITOR;
                break;
            }
            for (size_t i = 0; i < s.nMonitors; ++i) {
                if (s.gazeMonitorId && s.monitors[i].id == s.gazeMonitorId)
                    s.monitors[i].flags |= OpenXR::XR_HMF_GAZED;
            }
        }
    }
    s.flags = f;

    std::scoped_lock lock(m_historyMu);
    m_history.push(s);
}

// ---- either thread ----

void COpenXRManager::bumpOriginGen(OpenXR::eXRGenBridge bridge, OpenXR::eXRGenCause cause, const OpenXR::SXRPose& M) {
    const uint16_t       gen = sc<uint16_t>(m_originGen.fetch_add(1, std::memory_order_relaxed) + 1);
    OpenXR::SXRGenRecord g;
    g.tNs    = steadyNs();
    g.M      = M;
    g.gen    = gen;
    g.bridge = bridge;
    g.cause  = cause;
    {
        std::scoped_lock lock(m_historyMu);
        m_history.recordGen(g);
        OpenXR::SXRJournalEntry e;
        e.tNs  = g.tNs;
        e.kind = OpenXR::XR_J_GEN;
        e.a    = gen;
        e.b    = bridge;
        m_history.journal(e);
    }
}

void COpenXRManager::journal(OpenXR::eXRJournalKind kind, uint32_t a, uint32_t b, float x, float y) {
    OpenXR::SXRJournalEntry e;
    e.tNs  = steadyNs();
    e.kind = kind;
    e.a    = a;
    e.b    = b;
    e.x    = x;
    e.y    = y;
    std::scoped_lock lock(m_historyMu);
    m_history.journal(e);
}

// ---- main thread ----

void COpenXRManager::donDoffInput(OpenXR::eXRDonInput in, const char* why) {
    static auto         PRECENTER = CConfigValue<Hyprlang::INT>("openxr:recenter_on_plug");
    OpenXR::SXRDonFacts facts;
    facts.committedSinceWorn = m_committedSinceWorn;
    facts.recenterOnPlug     = *PRECENTER != 0;
    facts.donGate            = donGateEnabled();

    // research/30 Phase 2 (§5.4 as far as Phase 2 decides it): at the don edge, does anything have to
    // be hidden? Either the hydration will MOVE monitors (the session's recenter-on-plug RESTORE is
    // still owed), or nothing moves but the runtime is known to track position and is not doing so
    // yet (XREAL SLAM re-acquiring after a doff: the room would swim with every head translation
    // until it locks). Everything else — the brief in-session doff on a tracked headset — is a
    // certain RESUME and shows at once, with no added latency.
    bool       restoreDue = false, needPos = false, trackingPending = false;
    const auto prev = m_donState;
    if (in == OpenXR::XR_DIN_WORN_UP && prev == OpenXR::XR_DON_UNWORN && facts.donGate) {
        restoreDue                  = facts.recenterOnPlug && (m_donRestore == XR_DONR_PENDING || (m_donRestore == XR_DONR_NONE && !m_everPlugged));
        needPos                     = expectSixDof();
        trackingPending             = needPos && !newestPosTracked();
        facts.decisionCertainResume = !restoreDue && !trackingPending;
    }

    const auto step     = OpenXR::xrDonTransition(prev, in, facts);
    auto       setState = [&](OpenXR::eXRDonState next) {
        if (next == m_donState)
            return;
        Log::logger->log(Log::DEBUG, "[OPENXR] don/doff: {} -> {} ({})", OpenXR::xrDonStateName(m_donState), OpenXR::xrDonStateName(next), why);
        m_donState      = next;
        m_donStateSince = Time::steadyNow();
    };
    setState(step.next);

    if (step.actions & OpenXR::XR_DA_PUBLISH_DONNING)
        enterDonning(restoreDue, needPos, why,
                     restoreDue ? (trackingPending ? "re-seat owed (first don of the session), position not tracked yet" : "re-seat owed (first don of the session)") :
                                  "position tracking not back yet");
    if (in == OpenXR::XR_DIN_WORN_UP && !facts.donGate && m_donRestore == XR_DONR_PENDING) {
        // the gate deferred the first plug's RESTORE and has since been switched off: run it the
        // legacy way (first valid frame) rather than lose it
        m_donRestore = XR_DONR_DONE;
        armReseat(OpenXR::XR_RESEAT_ARM_RESTORE);
        Log::logger->log(Log::DEBUG, "[OPENXR] don/doff: the deferred recenter-on-plug re-seat runs now (the don gate is off)");
    }
    if (prev == OpenXR::XR_DON_DONNING && (step.next == OpenXR::XR_DON_UNWORN || step.next == OpenXR::XR_DON_NO_SESSION)) {
        // taken off (or the session ended) before the head settled: show the monitors where they are
        // (nobody is looking; the next don hides them again) and keep the owed re-seat owed
        releaseDonGate();
        if (m_donRestore == XR_DONR_CLAIMED)
            m_donRestore = step.next == OpenXR::XR_DON_UNWORN ? XR_DONR_PENDING : XR_DONR_NONE;
        m_lastDon.haveLastDon   = true;
        m_lastDon.lastDonSettle = "cancelled";
        Log::logger->log(Log::DEBUG, "[OPENXR] don settle abandoned ({}) — nothing was placed", why);
    }
    if (step.actions & OpenXR::XR_DA_SHOW_IN_PLACE) {
        // the backstop (§5.2): no hydration arrived, so no valid view did either. Show in place; an
        // owed RESTORE is armed the legacy way and runs on the first frame with a plausible head.
        releaseDonGate();
        if (m_donRestore == XR_DONR_CLAIMED) {
            m_donRestore = XR_DONR_DONE;
            armReseat(OpenXR::XR_RESEAT_ARM_RESTORE);
        }
        m_lastDon.haveLastDon     = true;
        m_lastDon.lastDonSettle   = "backstop";
        m_lastDon.lastDonSettleMs = (steadyNs() - m_donGateEdgeNs.load(std::memory_order_relaxed)) / 1'000'000;
        Log::logger->log(Log::WARN, "[OPENXR] don settle produced nothing in {} ms (no valid head pose) — showing the monitors where they are{}", m_lastDon.lastDonSettleMs,
                         m_donKind == XR_DONG_RESTORE ? "; the re-seat runs on the first plausible head" : "");
    }
    if (in == OpenXR::XR_DIN_SESSION_DOWN && m_posTrackedSeen.load(std::memory_order_relaxed))
        expectSixDof(); // remembers this system as a 6DoF one for the next session
    // reaching WORN — straight from the edge, or through a DONNING settle — starts a new wearing
    if (in == OpenXR::XR_DIN_SESSION_UP || (step.next == OpenXR::XR_DON_WORN && prev != OpenXR::XR_DON_WORN))
        m_committedSinceWorn = false;
    if (step.actions & OpenXR::XR_DA_MARK_AUTHORED)
        ++m_authoredSinceCommit;
    if (step.actions & OpenXR::XR_DA_COMMIT_FROM_HISTORY) {
        doffCommit(why);
        m_committedSinceWorn = true;
        if (m_donState == OpenXR::XR_DON_DOFFING)
            setState(OpenXR::xrDonTransition(m_donState, OpenXR::XR_DIN_COMMIT_DONE, facts).next);
    }
}

// ---- research/30 Phase 2: settle, hide, fade on don ----

static const char* settleBlockName(uint8_t b) {
    switch (b) {
        case OpenXR::XR_SBLK_NO_VIEW: return "no trusted head pose";
        case OpenXR::XR_SBLK_NO_POSITION: return "position never tracked";
        case OpenXR::XR_SBLK_NOT_UPRIGHT: return "never upright";
        case OpenXR::XR_SBLK_NOT_LEVEL: return "never level";
        case OpenXR::XR_SBLK_MOVING: return "kept moving";
        default: return "-";
    }
}

bool COpenXRManager::donGateEnabled() const {
    // MAIN THREAD (a STRING config read). Phase 2 rides the research/30 rollout switch: under the
    // default `legacy` the don path is byte-identical to before, and don_settle_timeout_ms = 0 is the
    // documented "today's timing" under `intent` too (§11).
    static auto PTIMEOUT = CConfigValue<Hyprlang::INT>("openxr:don_settle_timeout_ms");
    return *PTIMEOUT > 0 && placementMemoryIntent();
}

bool COpenXRManager::expectSixDof() {
    // MAIN THREAD. Position tracking is EXPECTED when this session has delivered it, or when the same
    // system did earlier in this compositor's life (a fresh XREAL SLAM session starts in its 3DoF hold
    // and would otherwise never be waited for on its first don). A system that then fails to deliver
    // it is forgotten again at the timeout (onDonHydrated), so a 3DoF session pays one wait at most.
    const bool seen  = m_posTrackedSeen.load(std::memory_order_relaxed);
    const bool known = !m_systemName.empty() && std::ranges::find(m_posTrackedSystems, m_systemName) != m_posTrackedSystems.end();
    if (seen && !known && !m_systemName.empty())
        m_posTrackedSystems.push_back(m_systemName);
    return seen || known;
}

bool COpenXRManager::newestPosTracked() {
    std::scoped_lock lock(m_historyMu);
    const auto&      t0 = m_history.t0();
    return !t0.empty() && (t0.back(0).flags & OpenXR::XR_HF_POS_TRACKED);
}

void COpenXRManager::enterDonning(bool restore, bool needPos, const char* why, const char* reason) {
    // MAIN THREAD. Publish the settle for the frame thread: everything first, the token last.
    static auto PTIMEOUT = CConfigValue<Hyprlang::INT>("openxr:don_settle_timeout_ms");
    static auto PFADE    = CConfigValue<Hyprlang::INT>("openxr:don_fade_ms");
    const int   timeout  = sc<int>(std::max<int64_t>(1, *PTIMEOUT));
    m_donKind            = restore ? XR_DONG_RESTORE : XR_DONG_RESUME;
    m_donNeedPos         = needPos;
    if (restore)
        m_donRestore = XR_DONR_CLAIMED;
    m_donGateKind.store(m_donKind, std::memory_order_relaxed);
    m_donGateNeedPos.store(needPos, std::memory_order_relaxed);
    m_donGateTimeoutMs.store(timeout, std::memory_order_relaxed);
    m_donGateFadeMs.store(sc<int32_t>(std::max<int64_t>(0, *PFADE)), std::memory_order_relaxed);
    m_donGateEdgeNs.store(steadyNs(), std::memory_order_relaxed);
    m_donGateToken.store(++m_donToken, std::memory_order_release);

    m_lastDon.haveLastDon     = true;
    m_lastDon.lastDonSettle   = "pending";
    m_lastDon.lastDonSettleMs = 0;
    m_lastDon.lastDonRung     = 0;
    m_lastDon.lastDonReason   = reason;
    m_lastDon.lastDonKind     = restore ? "rehydrate" : "resume";
    m_lastDon.lastDonNeedPos  = needPos;

    // §5.3: the fade re-applies its alpha from the content snapshot, so it needs one taken from the
    // CURRENT content — damage the hidden monitors once so a fresh buffer arrives while they are
    // hidden (the frame thread snapshots every hidden monitor's fresh blits).
    std::vector<PHLMONITOR> damage;
    int                     hidden = 0;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire) || l->m_anchor.state().mode != OpenXR::XR_ANCHOR_LOCAL)
                continue;
            ++hidden;
            if (auto mon = l->m_monitor.lock())
                damage.push_back(mon);
        }
    }
    for (auto& mon : damage) {
        if (g_pHyprRenderer)
            g_pHyprRenderer->damageMonitor(mon);
    }

    // the main-thread backstop (§5.2): the frame thread can only decide on frames it renders
    const auto dur = std::chrono::milliseconds(timeout + OpenXR::XR_SETTLE_BACKSTOP_EXTRA_MS);
    if (!m_donBackstopTimer) {
        m_donBackstopTimer = makeShared<CEventLoopTimer>(dur, [this](SP<CEventLoopTimer> self, void*) { onDonBackstop(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_donBackstopTimer);
    } else
        m_donBackstopTimer->updateTimeout(dur);

    Log::logger->log(Log::DEBUG, "[OPENXR] don/doff: hiding {} anchor:local monitor(s) until the head settles ({}; {}{}; at most {} ms)", hidden, why, reason,
                     needPos ? ", 6DoF expected" : "", timeout);
}

void COpenXRManager::releaseDonGate() {
    // MAIN THREAD: show the current settle's monitors in place, no hydration
    m_donGateRelease.store(m_donToken, std::memory_order_release);
    if (m_donBackstopTimer)
        m_donBackstopTimer->updateTimeout(std::nullopt);
}

bool COpenXRManager::donClaimFirstPlugRestore() {
    // MAIN THREAD, at the first plug of a session with recenter_on_plug. Under the gate the RESTORE
    // belongs to the don settle (it re-seats from the SETTLED head, not the one 35 ms after the edge).
    if (!donGateEnabled())
        return false;
    switch (m_donRestore) {
        case XR_DONR_CLAIMED:
        case XR_DONR_DONE: return true; // the don edge already claimed it (the usual order: worn, then plug)
        case XR_DONR_NONE:
        case XR_DONR_PENDING: break;
    }
    if (m_donState == OpenXR::XR_DON_DONNING) {
        // a settle is running for a RESUME: restart it as the RESTORE
        enterDonning(true, m_donNeedPos, "first plug of the session", "re-seat owed (first don of the session)");
        return true;
    }
    if (m_donState == OpenXR::XR_DON_WORN) {
        // already shown: re-seat the legacy way rather than hide a room the user is looking at
        m_donRestore = XR_DONR_DONE;
        return false;
    }
    m_donRestore = XR_DONR_PENDING; // not worn yet (monitors_follow_session = off | session): the next don runs it
    Log::logger->log(Log::DEBUG, "[OPENXR] first plug of session — the recenter-on-plug re-seat is deferred to the don settle");
    return true;
}

void COpenXRManager::onDonHydrated(uint32_t token) {
    // MAIN THREAD, from DON_HYDRATED (or the backstop finding the record already written)
    if (token != m_donToken || m_donState != OpenXR::XR_DON_DONNING)
        return; // a superseded or abandoned settle
    SXRDonHydration h;
    {
        std::scoped_lock lock(m_donHydMu);
        h = m_donHyd;
    }
    if (h.token != token)
        return;
    if (m_donBackstopTimer)
        m_donBackstopTimer->updateTimeout(std::nullopt);

    const auto&     r           = h.r;
    const bool      restore     = h.kind == XR_DONG_RESTORE;
    constexpr float R2D         = RAD2DEG;
    m_lastDon.haveLastDon       = true;
    m_lastDon.lastDonSettle     = r.state == OpenXR::XR_SETTLE_SETTLED ? "settled" : "timeout";
    m_lastDon.lastDonSettleMs   = r.settleNs / 1'000'000;
    m_lastDon.lastDonRung       = r.rung;
    m_lastDon.lastDonAngMaxDeg  = r.angMax * R2D;
    m_lastDon.lastDonAngMeanDeg = r.angMean * R2D;
    m_lastDon.lastDonPitchDeg   = r.pitch * R2D;
    m_lastDon.lastDonRollDeg    = r.roll * R2D;
    if (r.state == OpenXR::XR_SETTLE_TIMEOUT)
        m_lastDon.lastDonReason += std::format("; timed out: {}", settleBlockName(r.block));

    if (restore) {
        m_donRestore = XR_DONR_DONE;
        // report 12 §3a: the group was just re-seated, so the 2D plane's latched desk orientation
        // describes an arrangement that no longer exists (the first plug used to drop it; under the
        // gate the re-seat happens here instead)
        m_l2dRef = OpenXR::SXRLayout2DRef{};
        m_l2dPrev.clear();
        requestLayout2DSync();
    }
    // a system that was expected to track position and never did this session is not a 6DoF one now
    if (r.block == OpenXR::XR_SBLK_NO_POSITION && !m_posTrackedSeen.load(std::memory_order_relaxed))
        std::erase(m_posTrackedSystems, m_systemName);

    const char* what = restore ? (r.havePose ? "re-seated from the settled head" : "re-seat armed for the first upright head") : "shown in place";
    if (r.state == OpenXR::XR_SETTLE_SETTLED)
        Log::logger->log(Log::DEBUG, "[OPENXR] don settled after {} ms (ω max {:.0f}°/s mean {:.0f}°/s, pitch {:.0f}°, roll {:.0f}°) — {}", m_lastDon.lastDonSettleMs,
                         m_lastDon.lastDonAngMaxDeg, m_lastDon.lastDonAngMeanDeg, m_lastDon.lastDonPitchDeg, m_lastDon.lastDonRollDeg, what);
    else
        Log::logger->log(Log::WARN, "[OPENXR] don settle timed out after {} ms ({}) — rung {}: {}", m_lastDon.lastDonSettleMs, settleBlockName(r.block), sc<int>(r.rung),
                         r.rung == OpenXR::XR_RUNG_BEST_WINDOW      ? std::format("{}, from the calmest moment it looked ahead", what) :
                             r.rung == OpenXR::XR_RUNG_LAST_UPRIGHT ? std::format("{}, from the last upright head (reclined?)", what) :
                                                                      std::format("{}; nothing moved on our own account", what));
    journal(OpenXR::XR_J_DON_SETTLED, token, r.rung, sc<float>(m_lastDon.lastDonSettleMs) * 1e-3F);
    donDoffInput(OpenXR::XR_DIN_HYDRATED, "the head settled");
}

void COpenXRManager::onDonBackstop() {
    // MAIN THREAD, timeout + 1 s after the DONNING edge
    if (m_donState != OpenXR::XR_DON_DONNING)
        return;
    uint32_t written = 0;
    {
        std::scoped_lock lock(m_donHydMu);
        written = m_donHyd.token;
    }
    if (written == m_donToken) {
        onDonHydrated(written); // the event is still in the queue: the frame thread did decide
        return;
    }
    donDoffInput(OpenXR::XR_DIN_BACKSTOP, "the settle backstop fired");
}

// FRAME THREAD. POD + atomics only (no refcounts, no strings, no config reads).
void COpenXRManager::donGateFrameStep(int64_t pdt, const OpenXR::SXRPose& view, bool viewValid, bool trusted, bool posTracked) {
    auto&          F   = m_donFrame;
    const uint32_t tok = m_donGateToken.load(std::memory_order_acquire);
    if (tok != F.seenToken) {
        F.seenToken = tok;
        if (tok) {
            F.kind = m_donGateKind.load(std::memory_order_relaxed);
            OpenXR::SXRSettleParams p;
            p.mode      = F.kind == XR_DONG_RESTORE ? OpenXR::XR_SETTLE_POSE : OpenXR::XR_SETTLE_TRACKING;
            p.timeoutNs = sc<int64_t>(m_donGateTimeoutMs.load(std::memory_order_relaxed)) * 1'000'000LL;
            p.needPos   = m_donGateNeedPos.load(std::memory_order_relaxed);
            F.fadeNs    = sc<int64_t>(m_donGateFadeMs.load(std::memory_order_relaxed)) * 1'000'000LL;
            F.det.reset(m_donGateEdgeNs.load(std::memory_order_relaxed), p);
            F.hidden = true;
            F.fading = false;
        }
    }
    if (!F.hidden)
        return;
    auto startFade = [&] {
        F.hidden       = false;
        F.fading       = F.fadeNs > 0;
        F.fadeStartPdt = pdt;
    };
    if (m_donGateRelease.load(std::memory_order_acquire) == F.seenToken) {
        startFade();
        return;
    }
    OpenXR::SXRSettleInput in;
    in.tNs        = steadyNs();
    in.head       = view;
    in.valid      = viewValid;
    in.trusted    = trusted;
    in.posTracked = posTracked;
    in.angSpeed   = viewValid ? m_histFeatures.angSpeed : 0.F;
    in.linSpeed   = viewValid ? m_histFeatures.linSpeed : 0.F;
    const auto r  = F.det.step(in);
    if (r.state == OpenXR::XR_SETTLE_WAITING)
        return;
    {
        std::scoped_lock lock(m_donHydMu);
        m_donHyd = SXRDonHydration{.token = F.seenToken, .kind = F.kind, .r = r};
    }
    enqueue(SXRStateEvent{.type = eXRStateEventType::DON_HYDRATED, .a = sc<int32_t>(F.seenToken)});
    startFade();
    if (F.kind != XR_DONG_RESTORE)
        return;
    // the RESTORE runs through the ordinary consume path, this frame if the view is valid; without a
    // pose (rung 3) it is the legacy arm, which waits for the first plausible head
    F.restorePoseValid = r.havePose;
    F.restorePose      = r.pose;
    armReseat(OpenXR::XR_RESEAT_ARM_RESTORE);
}

OpenXR::SXRArrangementView COpenXRManager::arrangementView() {
    OpenXR::SXRArrangementView   arr;
    std::vector<OpenXR::SXRPose> worlds, rigs;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire) || arr.n >= arr.m.size())
                continue;
            const auto& st = l->m_anchor.state();
            auto&       m  = arr.m[arr.n++];
            m.id           = l->m_histId;
            m.world        = st.anchorPose;
            // the rule the legacy staging applies, as one predicate: anchor:local, its frame
            // re-established since the last discontinuity, and (adaptive) docked at its desk pose
            m.eligible = st.mode == OpenXR::XR_ANCHOR_LOCAL && OpenXR::xrAnchorFrameReestablished(l->m_anchorFrameStale, st.anchorPose, l->m_anchorFrameRef) &&
                !(l->m_anchor.adaptiveEnabled() && l->m_anchor.adaptivePhase() != OpenXR::XRAD_DOCKED) && OpenXR::xrPoseFinite(st.anchorPose);
            m.memValid  = l->m_restoreValid;
            m.memOffset = l->m_restoreOffset;
            m.memSrc    = l->m_restoreProv.src;
            if (m.eligible)
                worlds.push_back(st.anchorPose);
            if (l->m_declaredRig && l->m_declaredAnchor.mode == OpenXR::XR_ANCHOR_LOCAL)
                rigs.push_back(l->m_declaredAnchor.anchorPose);
        }
    }
    static auto PDEFDIST    = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
    const float authored    = OpenXR::xrGroupAuthoredDistance(rigs.data(), rigs.size(), sc<float>(*PDEFDIST));
    const auto  seat        = OpenXR::xrGroupSeatFrame(worlds.data(), worlds.size(), authored);
    arr.groupSeatValid      = seat.valid;
    arr.groupSeat           = seat.frame;
    arr.gen                 = m_originGen.load(std::memory_order_relaxed);
    arr.authoredSinceCommit = m_authoredSinceCommit > 0;
    return arr;
}

int COpenXRManager::commitSeat(const OpenXR::SXRSeatVerdict& v, const OpenXR::SXRArrangementView& arr) {
    // MAIN THREAD, at the edge (I11). One rigid set: one seat, every eligible monitor (§6.3).
    int              n = 0;
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers) {
        if (l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        bool eligible = false;
        for (size_t i = 0; i < arr.n; ++i) {
            if (arr.m[i].id == l->m_histId)
                eligible = arr.m[i].eligible;
        }
        if (!eligible)
            continue;
        const auto off = OpenXR::xrPoseInHeadFrame(v.seatNow, l->m_anchor.state().anchorPose);
        if (!OpenXR::xrPoseFinite(off))
            continue;
        l->m_restoreOffset = off;
        l->m_restoreValid  = true;
        l->m_restoreProv   = OpenXR::SXRRestoreProvenance{
            .src              = v.chosen.src,
            .committedAtNs    = steadyNs(),
            .seatStartNs      = v.chosen.tStartNs,
            .seatEndNs        = v.chosen.tEndNs,
            .yawConcentration = v.chosen.yawConcentration,
            .gazeFrac         = v.chosen.gazeFrac,
        };
        ++n;
    }
    return n;
}

void COpenXRManager::doffCommit(const char* why) {
    // MAIN THREAD, on the doff / session-end edge (I11).
    const int64_t tEdge  = steadyNs();
    const bool    intent = placementMemoryIntent();

    // The selection runs on a snapshot, against the arrangement (and the memory) as it stood BEFORE
    // this edge's commit, which is what the ranking rule must compare with.
    const auto arr = arrangementView();
    if (!m_historySnap)
        m_historySnap = makeUnique<OpenXR::CXRPoseHistory>();
    {
        std::scoped_lock lock(m_historyMu);
        *m_historySnap = m_history;
    }
    const auto v = OpenXR::xrSelectSeat(*m_historySnap, tEdge, arr, OpenXR::SXRSeatParams{});
    // E should hold the dwell without the take-off motion: close the running episode at the onset.
    if (v.onset.found) {
        std::scoped_lock lock(m_historyMu);
        m_history.closeEpisode(v.onset.tOnsetNs, OpenXR::XR_EP_DOFF);
    }

    if (intent) {
        // the legacy staging values are not memories under `intent`; drop them so status is honest
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            l->m_restoreStagedValid = false;
        }
    } else
        commitRestorePlacements(why); // legacy: unchanged (now with the upright-gated staging)

    // status record
    SXRDonOffStatus& d  = m_lastDoff;
    d.haveLastDoff      = true;
    d.lastDoffShadow    = !intent;
    d.lastSeatSource    = v.ok ? OpenXR::xrSeatSourceName(v.chosen.src) : "none";
    d.onsetBeforeEdgeMs = v.onset.found ? (tEdge - v.onset.tOnsetNs) / 1'000'000 : -1;
    d.seatDurationS     = v.ok ? sc<float>(v.chosen.durUs) * 1e-6F : 0.F;
    d.yawConcentration  = v.ok ? v.chosen.yawConcentration : 0.F;
    d.gazeFrac          = v.ok ? v.chosen.gazeFrac : 0.F;
    d.seatYawDeg        = v.ok ? OpenXR::qYawOf(v.seatNow.rot, 0.F) * RAD2DEG : 0.F;
    d.refusals.clear();
    d.committed = 0;
    d.shadowSummary.clear();

    const char* prefix = intent ? "" : "shadow: ";
    const auto  logLvl = intent ? Log::WARN : Log::DEBUG;
    for (size_t i = 0; i < v.nRefused; ++i) {
        const auto& r      = v.refused[i];
        const auto  detail = r.reason == OpenXR::XR_SEATREF_P3_FACING || r.reason == OpenXR::XR_SEATREF_RANK ? std::format(" ({:.0f}°)", r.detail) :
            r.reason == OpenXR::XR_SEATREF_P4_REACH                                                          ? std::format(" ({:.1f} m)", r.detail) :
            r.reason == OpenXR::XR_SEATREF_SHORT                                                             ? std::format(" ({})", fmtDur(r.detail)) :
                                                                                                               std::string{};
        d.refusals.push_back(std::format("{}: {}{}", OpenXR::xrSeatSourceName(r.src), OpenXR::xrSeatRefusalName(r.reason), detail));
        Log::logger->log(logLvl, "[OPENXR] {}doff seat candidate refused: {} ending {:.1f}s before the edge — {}{}; trying the next", prefix, OpenXR::xrSeatSourceName(r.src),
                         sc<double>(tEdge - r.tEndNs) * 1e-9, OpenXR::xrSeatRefusalName(r.reason), detail);
    }

    const std::string onsetStr =
        v.onset.found ? std::format("motion onset {:.2f}s before the edge", sc<double>(tEdge - v.onset.tOnsetNs) * 1e-9) : "no quiet window in the last 8s";
    std::string seatStr;
    if (v.ok)
        seatStr = std::format("{} {} (yaw {:.0f}°{}, R̄ {:.2f}, spread {:.2f} m, gaze-on-monitor {:.0f}%, primary {} at bearing {:.0f}°)", OpenXR::xrSeatSourceName(v.chosen.src),
                              fmtDur(sc<double>(v.chosen.durUs) * 1e-6), d.seatYawDeg, v.chosen.yawFromGroup ? " from the group frame" : "", v.chosen.yawConcentration,
                              v.chosen.spreadM, v.chosen.gazeFrac * 100.F, histMonitorName(v.primary), v.primaryBearing * RAD2DEG);

    if (intent) {
        if (v.ok) {
            d.committed           = commitSeat(v, arr);
            m_authoredSinceCommit = 0;
            Log::logger->log(Log::DEBUG, "[OPENXR] doff seat ({}; {}): {} — remembered {} monitor(s)", why, onsetStr, seatStr, d.committed);
        } else
            Log::logger->log(Log::WARN, "[OPENXR] doff ({}): no trustworthy seat in 30 min of history — keeping the previous memory", why);
        journal(OpenXR::XR_J_DOFF_COMMIT, v.ok ? v.chosen.src : 0, sc<uint32_t>(d.committed), d.onsetBeforeEdgeMs * 1e-3F);
        return;
    }

    // ---- shadow: compare what the selection WOULD remember with what the legacy gate DID ----
    if (!v.ok) {
        d.shadowSummary = "no trustworthy seat; legacy memory stands";
        Log::logger->log(Log::DEBUG, "[OPENXR] shadow: doff ({}): no trustworthy seat in 30 min of history ({}) — the intent path would keep the previous memory", why, onsetStr);
        journal(OpenXR::XR_J_SHADOW, 0, 0, 0.F);
        return;
    }
    struct SCmp {
        std::string     name;
        OpenXR::SXRPose would, legacy;
        bool            legacyValid = false;
        uint8_t         legacySrc   = OpenXR::XR_SEAT_NONE;
    };
    std::vector<SCmp> cmps;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;
            bool eligible = false;
            for (size_t i = 0; i < arr.n; ++i) {
                if (arr.m[i].id == l->m_histId)
                    eligible = arr.m[i].eligible;
            }
            if (!eligible)
                continue;
            cmps.push_back(SCmp{.name        = l->m_monitorName,
                                .would       = OpenXR::xrPoseInHeadFrame(v.seatNow, l->m_anchor.state().anchorPose),
                                .legacy      = l->m_restoreOffset,
                                .legacyValid = l->m_restoreValid,
                                .legacySrc   = l->m_restoreProv.src});
        }
    }
    int         agree  = 0;
    float       worstM = 0.F, worstDeg = 0.F;
    std::string worstName = "-";
    for (const auto& c : cmps) {
        const float dx = c.would.pos.x - c.legacy.pos.x, dz = c.would.pos.z - c.legacy.pos.z;
        const float dm = c.legacyValid ? std::sqrt(dx * dx + dz * dz) : 0.F;
        const float bw = std::atan2(c.would.pos.x, -c.would.pos.z) * RAD2DEG;
        const float bl = std::atan2(c.legacy.pos.x, -c.legacy.pos.z) * RAD2DEG;
        const float db = c.legacyValid ? std::fabs(wrapDeg(bw - bl)) : 0.F;
        const bool  ok = c.legacyValid && dm <= 0.25F && db <= 10.F;
        if (ok)
            ++agree;
        if (c.legacyValid && (dm > worstM || db > worstDeg)) {
            worstM    = std::max(worstM, dm);
            worstDeg  = std::max(worstDeg, db);
            worstName = c.name;
        }
        if (!ok)
            Log::logger->log(c.legacyValid ? Log::WARN : Log::DEBUG,
                             "[OPENXR] shadow:   {}: would remember [{:.2f}, {:.2f}, {:.2f}] vs memory ({}) [{:.2f}, {:.2f}, {:.2f}] (Δ {:.2f} m, bearing Δ {:.0f}°)", c.name,
                             c.would.pos.x, c.would.pos.y, c.would.pos.z, c.legacyValid ? OpenXR::xrSeatSourceName(c.legacySrc) : "none", c.legacy.pos.x, c.legacy.pos.y,
                             c.legacy.pos.z, dm, db);
    }
    d.committed = sc<int>(cmps.size());
    d.shadowSummary =
        std::format("would remember {} monitor(s); legacy agrees for {}/{} (worst {}: {:.2f} m, {:.0f}°)", cmps.size(), agree, cmps.size(), worstName, worstM, worstDeg);
    Log::logger->log(Log::DEBUG, "[OPENXR] shadow: doff seat ({}; {}): {} — would remember {} monitor(s); legacy memory agrees for {}/{} (worst {}: {:.2f} m, {:.0f}°)", why,
                     onsetStr, seatStr, cmps.size(), agree, cmps.size(), worstName, worstM, worstDeg);
    journal(OpenXR::XR_J_SHADOW, v.chosen.src, sc<uint32_t>(agree), worstDeg);
}

void COpenXRManager::commitReseatDone() {
    // MAIN THREAD. research/30 §7 / D4: the planted offsets ARE the memory, immediately and without
    // any gate — the user just told us where the room goes. This is what lets one SUPER+CTRL+Home
    // repair the flipped memory of 2026-10-13..15, under either placement_memory mode.
    int n = 0;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (!l->m_reseatPlantedPending)
                continue;
            l->m_reseatPlantedPending = false;
            if (l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;
            l->m_restoreOffset = l->m_reseatPlanted;
            l->m_restoreValid  = true;
            l->m_restoreProv   = OpenXR::SXRRestoreProvenance{.src = OpenXR::XR_SEAT_RESEAT, .committedAtNs = steadyNs()};
            // the live staging measures exactly this from now on; keep it from re-committing a
            // pre-re-seat value at the next doff under legacy
            l->m_restoreStaged      = l->m_reseatPlanted;
            l->m_restoreStagedValid = true;
            ++n;
        }
    }
    if (!n)
        return;
    m_arrangeEpoch.fetch_add(1, std::memory_order_relaxed);
    m_authoredSinceCommit = 0;
    journal(OpenXR::XR_J_RESEAT_COMMIT, 0, sc<uint32_t>(n));
    donDoffInput(OpenXR::XR_DIN_RESEAT_DONE, "re-seat");
    Log::logger->log(Log::DEBUG, "[OPENXR] re-seat committed as the new memory for {} monitor(s) (provenance reseat)", n);
}

void COpenXRManager::noteAuthored(uint32_t histId, const char* why) {
    const uint32_t epoch = m_arrangeEpoch.fetch_add(1, std::memory_order_relaxed) + 1;
    journal(OpenXR::XR_J_AUTHORED, histId, epoch);
    donDoffInput(OpenXR::XR_DIN_AUTHORED, why);
}

std::string COpenXRManager::histMonitorName(uint32_t id) {
    if (id == 0)
        return "-";
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_histId == id)
                return l->m_monitorName;
        }
    }
    for (const auto& [hid, name] : m_histNames) {
        if (hid == id)
            return name + " (destroyed)";
    }
    return std::format("#{}", id);
}

std::expected<std::string, std::string> COpenXRManager::cmdForget(const std::string& args) {
    std::string target = args;
    while (!target.empty() && std::isspace(sc<unsigned char>(target.back())))
        target.pop_back();
    while (!target.empty() && std::isspace(sc<unsigned char>(target.front())))
        target.erase(target.begin());
    const bool all = target.empty() || target == "all";
    int        n   = 0;
    uint32_t   id  = 0;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (!all && l->m_monitorName != target)
                continue;
            if (!all)
                id = l->m_histId;
            if (l->m_restoreValid)
                ++n;
            l->m_restoreValid       = false;
            l->m_restoreStagedValid = false;
            l->m_restoreProv        = OpenXR::SXRRestoreProvenance{};
            if (!all)
                break;
        }
    }
    if (!all && id == 0)
        return std::unexpected<std::string>("forget: no XR monitor named '" + target + "'");
    journal(OpenXR::XR_J_FORGET, id, sc<uint32_t>(n));
    Log::logger->log(Log::DEBUG, "[OPENXR] forgot {} remembered placement(s) ({}) — the next session re-seats them from their declared rig", n, all ? "all" : target);
    return std::format("forgot {} remembered placement{} — the next session re-seats {} from the declared rig", n, n == 1 ? "" : "s", all ? "them" : "it");
}

COpenXRManager::SXRDonOffStatus COpenXRManager::donoffStatus() {
    SXRDonOffStatus s     = m_lastDoff;
    s.state               = OpenXR::xrDonStateName(m_donState);
    s.sinceMs             = sc<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_donStateSince).count());
    s.wearSignal          = m_userPresenceSupported ? (m_presenceKnown ? "presence" : "visibility (presence pending)") : "visibility";
    s.originGen           = m_originGen.load(std::memory_order_relaxed);
    s.placementMemory     = placementMemoryIntent() ? "intent" : "legacy";
    s.authoredSinceCommit = m_authoredSinceCommit;
    s.recording           = m_donoffRecording;
    s.donGate             = donGateEnabled();
    s.hidden              = m_donState == OpenXR::XR_DON_DONNING;
    s.haveLastDon         = m_lastDon.haveLastDon;
    s.lastDonKind         = m_lastDon.lastDonKind;
    s.lastDonSettle       = m_lastDon.lastDonSettle;
    s.lastDonReason       = m_lastDon.lastDonReason;
    s.lastDonSettleMs     = m_lastDon.lastDonSettleMs;
    s.lastDonRung         = m_lastDon.lastDonRung;
    s.lastDonAngMaxDeg    = m_lastDon.lastDonAngMaxDeg;
    s.lastDonAngMeanDeg   = m_lastDon.lastDonAngMeanDeg;
    s.lastDonPitchDeg     = m_lastDon.lastDonPitchDeg;
    s.lastDonRollDeg      = m_lastDon.lastDonRollDeg;
    s.lastDonNeedPos      = m_lastDon.lastDonNeedPos;
    std::scoped_lock lock(m_historyMu);
    const auto&      t0 = m_history.t0();
    s.dof               = !t0.empty() && (t0.back(0).flags & OpenXR::XR_HF_POS_TRACKED) ? 6 : 3;
    if (t0.size() >= 2)
        s.spanS[0] = sc<float>(sc<double>(t0.back(0).tNs - t0.at(0).tNs) * 1e-9);
    for (size_t k = 0; k < OpenXR::XR_HIST_TIERS; ++k) {
        const auto& r = m_history.tier(k);
        if (!r.empty())
            s.spanS[k + 1] = sc<float>(sc<double>(r.back(0).t1Ns - r.at(0).t0Ns) * 1e-9);
    }
    s.episodes     = m_history.episodes().size();
    const auto& ep = m_history.runningEpisode();
    s.dwellOpen    = m_history.runningEpisodeOpen();
    s.dwellS       = ep.any ? sc<float>(ep.durUs) * 1e-6F : 0.F;
    s.dwellGaze    = ep.gazeEvalUs ? sc<float>(sc<double>(ep.gazeOnUs) / sc<double>(ep.gazeEvalUs)) : 0.F;
    return s;
}

static const char* journalKindName(uint8_t k) {
    switch (k) {
        case OpenXR::XR_J_PRESENCE: return "presence";
        case OpenXR::XR_J_VISIBILITY: return "visibility";
        case OpenXR::XR_J_SESSION: return "session";
        case OpenXR::XR_J_WORN: return "worn";
        case OpenXR::XR_J_DOFF_COMMIT: return "doff-commit";
        case OpenXR::XR_J_RESEAT_COMMIT: return "reseat-commit";
        case OpenXR::XR_J_AUTHORED: return "authored";
        case OpenXR::XR_J_MONITOR_CREATED: return "monitor-created";
        case OpenXR::XR_J_MONITOR_DESTROYED: return "monitor-destroyed";
        case OpenXR::XR_J_MONITOR_RENAMED: return "monitor-renamed";
        case OpenXR::XR_J_FORGET: return "forget";
        case OpenXR::XR_J_GEN: return "origin-gen";
        case OpenXR::XR_J_SHADOW: return "shadow";
        case OpenXR::XR_J_DON_SETTLED: return "don-settled";
        default: return "?";
    }
}

static const char* episodeEndName(uint8_t r) {
    switch (r) {
        case OpenXR::XR_EP_WALK: return "walk";
        case OpenXR::XR_EP_DOFF: return "doff";
        case OpenXR::XR_EP_GEN: return "gen";
        case OpenXR::XR_EP_SESSION: return "session";
        default: return "open";
    }
}

std::string COpenXRManager::donoffDump() {
    if (!m_historySnap)
        m_historySnap = makeUnique<OpenXR::CXRPoseHistory>();
    {
        std::scoped_lock lock(m_historyMu);
        *m_historySnap = m_history;
    }
    const auto&   h   = *m_historySnap;
    const int64_t now = steadyNs();
    const auto    st  = donoffStatus();
    std::string   out = std::format("don/doff: {} for {} (wear signal: {}, {}DoF, origin gen {}, placement_memory = {})\n", st.state, fmtDur(sc<double>(st.sinceMs) * 1e-3),
                                    st.wearSignal, st.dof, st.originGen, st.placementMemory);
    if (st.haveLastDoff)
        out += std::format("last doff{}: seat {} ({}, R̄ {:.2f}, gaze {:.0f}%, onset {} ms before the edge){}\n", st.lastDoffShadow ? " (shadow)" : "", st.lastSeatSource,
                           fmtDur(st.seatDurationS), st.yawConcentration, st.gazeFrac * 100.F, st.onsetBeforeEdgeMs,
                           st.shadowSummary.empty() ? std::string{} : " — " + st.shadowSummary);
    out += std::format("history: T0 {:.1f}s, T1 {}, T2 {}, T3 {}, T4 {}; {} episode(s); running dwell {} ({})\n", st.spanS[0], fmtDur(st.spanS[1]), fmtDur(st.spanS[2]),
                       fmtDur(st.spanS[3]), fmtDur(st.spanS[4]), st.episodes, fmtDur(st.dwellS), st.dwellOpen ? "open" : "candidate");

    out += "episodes (newest first):\n";
    const auto& eps = h.episodes();
    for (size_t k = 0; k < eps.size() && k < 10; ++k) {
        const auto& e = eps.back(k);
        out += std::format("  {} ago: {} at [{:.2f}, {:.2f}, {:.2f}] yaw {:.0f}° R̄ {:.2f} spread {:.2f} m, gaze {:.0f}%, primary {}, {}DoF, gen {}, ended by {}\n",
                           fmtDur(sc<double>(now - e.tEndNs) * 1e-9), fmtDur(sc<double>(e.durUs) * 1e-6), e.seatPos.x, e.seatPos.y, e.seatPos.z, e.seatYaw * RAD2DEG,
                           e.yawConcentration, std::hypot(e.posMax.x - e.posMin.x, e.posMax.z - e.posMin.z), e.gazeFrac * 100.F, histMonitorName(e.gazeIds[0]), e.dof, e.gen,
                           episodeEndName(e.endReason));
    }
    out += "journal (newest first):\n";
    const auto& j = h.journalRing();
    for (size_t k = 0; k < j.size() && k < 32; ++k) {
        const auto& e = j.back(k);
        out += std::format("  {} ago: {} a={} b={} x={:.2f}\n", fmtDur(sc<double>(now - e.tNs) * 1e-9), journalKindName(e.kind), e.a, e.b, e.x);
    }
    out += "origin generations:\n";
    const auto& g = h.gens();
    for (size_t k = 0; k < g.size() && k < 16; ++k) {
        const auto& r = g.back(k);
        out += std::format("  gen {} ({} ago): {} — bridge {}\n", r.gen, fmtDur(sc<double>(now - r.tNs) * 1e-9),
                           r.cause == OpenXR::XR_GENC_SESSION_START ? "session start" : (r.cause == OpenXR::XR_GENC_RECENTER_KNOWN ? "recenter (delta known)" : "discontinuity"),
                           r.bridge == OpenXR::XR_BRIDGE_KNOWN ? "known" : (r.bridge == OpenXR::XR_BRIDGE_SAME ? "same" : "unknown"));
    }
    return out;
}

// ---- opt-in trace recording (§13.4) ----

static std::filesystem::path traceDir() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && *xdg)
        return std::filesystem::path{xdg} / "hypr" / "xr-traces";
    const char* home = std::getenv("HOME");
    return std::filesystem::path{home ? home : "/tmp"} / ".local" / "state" / "hypr" / "xr-traces";
}

std::expected<std::string, std::string> COpenXRManager::cmdDonoffRecord(const std::string& args) {
    std::string a = args;
    while (!a.empty() && std::isspace(sc<unsigned char>(a.back())))
        a.pop_back();
    if (a == "on") {
        m_donoffRecording = true;
        Log::logger->log(Log::WARN,
                         "[OPENXR] don/doff trace recording ON — head-pose traces (±15 s around each wear edge) are being written to {} until `donoff record off` or session end",
                         traceDir().string());
        return std::format("recording don/doff traces to {} (head poses around each wear edge; off at session end)", traceDir().string());
    }
    if (a == "off") {
        if (m_donoffRecording && m_donoffTraceEdgeNs)
            writeDonoffTrace();
        m_donoffRecording = false;
        Log::logger->log(Log::DEBUG, "[OPENXR] don/doff trace recording off");
        return std::string{"don/doff trace recording off"};
    }
    return std::unexpected<std::string>("donoff record: expected on|off");
}

void COpenXRManager::donoffTraceEdge(const char* edge) {
    if (!m_donoffRecording)
        return;
    if (m_donoffTraceEdgeNs)
        writeDonoffTrace(); // an edge inside the previous one's +5 s tail: flush that one first
    m_donoffTraceEdgeNs   = steadyNs();
    m_donoffTraceEdgeName = edge;
    const auto dur        = std::chrono::milliseconds(5000);
    if (!m_donoffTraceTimer) {
        m_donoffTraceTimer = makeShared<CEventLoopTimer>(dur, [this](SP<CEventLoopTimer> self, void*) { writeDonoffTrace(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_donoffTraceTimer);
    } else
        m_donoffTraceTimer->updateTimeout(dur);
}

void COpenXRManager::writeDonoffTrace() {
    // MAIN THREAD, off a timer. File IO never happens on the frame thread.
    if (!m_donoffTraceEdgeNs)
        return;
    const int64_t     edge = m_donoffTraceEdgeNs;
    const std::string name = m_donoffTraceEdgeName;
    m_donoffTraceEdgeNs    = 0;
    if (m_donoffTraceTimer)
        m_donoffTraceTimer->updateTimeout(std::nullopt);
    if (!m_historySnap)
        m_historySnap = makeUnique<OpenXR::CXRPoseHistory>();
    {
        std::scoped_lock lock(m_historyMu);
        *m_historySnap = m_history;
    }
    std::error_code ec;
    const auto      dir = traceDir();
    std::filesystem::create_directories(dir, ec);
    std::string sys = m_systemName.empty() ? std::string{"unknown"} : m_systemName;
    for (auto& c : sys) {
        if (!std::isalnum(sc<unsigned char>(c)))
            c = '_';
    }
    const auto        wall = std::chrono::system_clock::now();
    const std::string file = (dir / std::format("donoff-{:%Y%m%dT%H%M%S}-{}.jsonl", std::chrono::floor<std::chrono::seconds>(wall), sys)).string();
    std::ofstream     o(file);
    if (!o) {
        Log::logger->log(Log::WARN, "[OPENXR] don/doff trace: cannot write {}", file);
        return;
    }
    const auto& h   = *m_historySnap;
    const auto& t0  = h.t0();
    const bool  six = !t0.empty() && (t0.back(0).flags & OpenXR::XR_HF_POS_TRACKED);
    o << std::format(R"({{"v":1,"runtime":"{}","system":"{}","dof":{},"presence":{},"edge":"{}"}})", m_runtimeName, m_systemName, six ? 6 : 3,
                     m_userPresenceSupported ? "true" : "false", name)
      << "\n";
    // relative time only (ms from the edge), research/30 Q6
    for (size_t i = 0; i < t0.size(); ++i) {
        const auto& s = t0.at(i);
        if (s.tNs < edge - 15'000'000'000LL || s.tNs > edge + 5'000'000'000LL)
            continue;
        std::string mons;
        for (size_t m = 0; m < s.nMonitors; ++m) {
            const auto& mp = s.monitors[m];
            mons += std::format("{}[{},{},{},{:.4f},{:.4f},{:.4f},{:.5f},{:.5f},{:.5f},{:.5f}]", m ? "," : "", mp.id, mp.mode, mp.flags, mp.pos.x, mp.pos.y, mp.pos.z, mp.rot.x,
                                mp.rot.y, mp.rot.z, mp.rot.w);
        }
        o << std::format(R"({{"t":{:.3f},"g":{},"p":[{:.4f},{:.4f},{:.4f}],"q":[{:.5f},{:.5f},{:.5f},{:.5f}],"f":{},"w":{:.4f},"v":{:.4f},"gz":{},"e":{},"m":[{}]}})",
                         sc<double>(s.tNs - edge) * 1e-6, s.gen, s.headPos.x, s.headPos.y, s.headPos.z, s.headRot.x, s.headRot.y, s.headRot.z, s.headRot.w, s.flags, s.angSpeed,
                         s.linSpeed, s.gazeMonitorId, s.arrangeEpoch, mons)
          << "\n";
    }
    const auto& j = h.journalRing();
    for (size_t i = 0; i < j.size(); ++i) {
        const auto& e = j.at(i);
        if (e.tNs < edge - 15'000'000'000LL || e.tNs > edge + 5'000'000'000LL)
            continue;
        o << std::format(R"({{"t":{:.3f},"ev":"{}","a":{},"b":{},"x":{:.3f}}})", sc<double>(e.tNs - edge) * 1e-6, journalKindName(e.kind), e.a, e.b, e.x) << "\n";
    }
    Log::logger->log(Log::DEBUG, "[OPENXR] don/doff trace written: {}", file);
}

#endif
