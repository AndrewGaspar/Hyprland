#include "OpenXRManager.hpp"
#ifdef HAVE_OPENXR

// Include contract (doc 01): platform macros before EGL/GLES, those before the OpenXR
// platform header (which declares XrSwapchainImageOpenGLESKHR).
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <cstdint>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <cmath>
#include <format>
#include <optional>
#include <ranges>
#include <variant>

#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include "XRIpc.hpp"
#include "XRSession.hpp"
#include "XRGraphics.hpp"
#include "XRGpuProbe.hpp"
#include "XRMonitorLayer.hpp"
#include "XRDmabufImport.hpp" // OpenXR::xrContentPathName (status contentPath)
#include "XRInput.hpp"
#include "XRPointerDevice.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../event/EventBus.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../output/Monitor.hpp"
#include "../state/MonitorState.hpp"
#include "../helpers/Drm.hpp"    // DRM::sameGpu (physical-GPU compare for cross-GPU linear decision)
#include "../helpers/Format.hpp" // NFormatUtils::drmModifierName (post-reconfigure modifier log)
#include "../render/Renderer.hpp" // damageMonitor (force a fresh present into the re-allocated buffers)
#include "../desktop/state/FocusState.hpp"
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"
#include "../config/legacy/ConfigManager.hpp"
#include "../helpers/time/Time.hpp"

#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Headless.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <aquamarine/allocator/Allocator.hpp>
#include <drm_fourcc.h> // DRM_FORMAT_MOD_INVALID (negotiated-modifier log)

COpenXRManager::COpenXRManager() = default;

COpenXRManager::~COpenXRManager() {
    if (m_state != XR_STATE_DISABLED)
        stop(); // stop() abandons any in-flight handshake before tearing down
    // Belt-and-suspenders: if the dtor ran while DISABLED but a handshake was somehow still pending,
    // abandon it before the completion channel goes away (its worker signals m_handshakeFd).
    abandonHandshake();
    teardownHandshakeChannel();
    // Drop the grace timer from the event loop so its `this`-capturing callback can never fire on
    // freed memory (report-18 addendum). stop() only disarms it; the loop still holds an SP ref.
    if (m_unplugTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_unplugTimer);
    m_unplugTimer.reset();
    // Same for the first-plug settle timer (report-19).
    if (m_plugSettleTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_plugSettleTimer);
    m_plugSettleTimer.reset();
    // Same for the dormant re-probe timer (report-20 issue B1).
    if (m_reprobeTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_reprobeTimer);
    m_reprobeTimer.reset();
    // Event-driven re-probe (don-the-headset dead-air): close the inotify fd + drop its loop source,
    // then remove the debounce one-shot from the loop so its this-capturing callback can't fire later.
    teardownReprobeWatch();
    if (m_watchDebounceTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_watchDebounceTimer);
    m_watchDebounceTimer.reset();
}

void COpenXRManager::init() {
    // Register the hyprctl "openxr" command surface. Kept out of HyprCtl.cpp so the
    // XR footprint outside src/openxr/ stays at zero.
    m_ipc = makeUnique<CXRIpc>();
    m_ipc->registerCommands();

    // Task #89 phase 2 blocker B: the persistent handshake-completion channel. Created here (once) so an
    // async first-contact worker spawned by any start() below has a live eventfd to signal back on. Torn
    // down in the dtor. eventfd + wl_event_loop_add_fd essentially never fail this early; start() re-checks
    // and refuses to go async (UNAVAILABLE + reprobe) if it somehow could not be created, so a failure
    // here never wedges the state machine.
    setupHandshakeChannel();

    // React to reloads. `hyprctl keyword openxr:enabled 1` fires props_refreshed rather than
    // a full reload, so listen to both; onConfigReload() is idempotent.
    m_configReloadListener   = Event::bus()->m_events.config.reloaded.listen([this] { onConfigReload(); });
    m_propsRefreshedListener = Event::bus()->m_events.config.props_refreshed.listen([this] { onConfigReload(); });

    // Materialize any monitors declared in the config as plain headless outputs (doc 02 lazy
    // binding). Their quads bind when a session later starts. Done before start() so start()'s
    // bindExistingLayers() picks them up. With openxr:monitors_follow_session (default) they are
    // created UNPLUGGED (disabled) and only plug in when a session actually starts (research/18)
    // — no phantom monitors on a sessionless desktop login.
    reconcileDeclaredMonitors();

    // Honor openxr:enabled at startup.
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    if (*PENABLED)
        start();
}

const char* COpenXRManager::stateToString(eXRManagerState state) {
    switch (state) {
        case XR_STATE_DISABLED: return "disabled";
        case XR_STATE_UNAVAILABLE: return "unavailable";
        case XR_STATE_STARTING: return "starting";
        case XR_STATE_RUNNING_IDLE: return "idle";
        case XR_STATE_RUNNING_VISIBLE: return "visible";
        case XR_STATE_RUNNING_FOCUSED: return "focused";
        case XR_STATE_STOPPING: return "stopping";
    }
    return "disabled";
}

eXRManagerState COpenXRManager::state() const {
    return m_state;
}

const std::string& COpenXRManager::runtimeName() const {
    return m_runtimeName;
}

const std::string& COpenXRManager::systemName() const {
    return m_systemName;
}

const std::string& COpenXRManager::runtimeGpu() const {
    return m_runtimeGpu;
}

const std::string& COpenXRManager::runtimeJson() const {
    return m_runtimeJson;
}

std::string COpenXRManager::blendModeName() const {
    // Reflect the mode the frame loop actually submits while a session exists; the OPAQUE default
    // otherwise (nothing composited).
    if (m_session)
        return OpenXR::blendModeToString(xrBlendModeFromXr(m_session->m_blendMode));
    return OpenXR::blendModeToString(OpenXR::XR_BLEND_OPAQUE);
}

bool COpenXRManager::isOverlay() const {
    return m_session && m_session->m_isOverlay;
}

std::array<COpenXRManager::SXRHandInputInfo, 2> COpenXRManager::handInputInfos() const {
    // WP-G5: reflect each hand's active device for `hyprctl openxr status`. m_input's per-hand kind
    // is an atomic mirror (main-thread safe to read); hand_grab is read from config here.
    static auto                     PHANDGRAB   = CConfigValue<std::string>("openxr:hand_grab");
    static auto                     PGRABFILTER = CConfigValue<Hyprlang::INT>("openxr:grab_filter");
    const bool                      filterOn    = *PGRABFILTER != 0;
    std::array<SXRHandInputInfo, 2> out;
    for (int h = 0; h < 2; ++h) {
        const bool hands = m_input && m_input->handInputKind((OpenXR::eXRHand)h) == OpenXR::XR_INPUT_HANDS;
        out[h].hands     = hands;
        out[h].gesture   = hands ? OpenXR::xrHandGrabName(OpenXR::xrParseHandGrab(*PHANDGRAB)) : "";
        out[h].filtered  = hands && filterOn; // WP-G6: hand move-grabs will be 1€-filtered
    }
    return out;
}

OpenXR::eXRIdleInhibitMode COpenXRManager::idleInhibitMode() {
    // MAIN THREAD ONLY — CConfigValue<std::string> off the main thread is a session-killer (the
    // hyprlang value store is not thread-safe and a string read races config reloads). Every caller
    // of this and of shouldInhibitIdle() is main-thread: CInputManager::recheckIdleInhibitorStatus(),
    // the hyprctl status handler, and the config-reload/keyword hooks.
    static auto PINHIBIT = CConfigValue<std::string>("openxr:inhibit_idle");
    return OpenXR::parseIdleInhibitMode(*PINHIBIT);
}

bool COpenXRManager::shouldInhibitIdle() {
    // doc 05 §6.1 + research/20 phase 2. `off` never inhibits; `focused` is the historical predicate
    // (FOCUSED only — VISIBLE, e.g. a runtime dashboard in front, deliberately does not inhibit);
    // `present` (default) gates on the real worn signal when the runtime exposes XR_EXT_user_presence
    // and falls back to the `focused` predicate when it does not. Pure policy lives in
    // OpenXR::wantXRIdleInhibit (gtested); this only supplies the live facts. Main thread only.
    return OpenXR::wantXRIdleInhibit(idleInhibitMode(), sessionExists(), sessionVisible(), m_state == XR_STATE_RUNNING_FOCUSED, m_userPresenceSupported, m_presenceKnown,
                                     m_userPresent);
}

std::string COpenXRManager::idleInhibitModeName() {
    return OpenXR::idleInhibitModeToString(idleInhibitMode());
}

void COpenXRManager::setState(eXRManagerState newState) {
    if (m_state == newState)
        return;

    m_state = newState;

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"openxrsessionstate", stateToString(newState)});

    // openxractive is a derived boolean: active ⇔ state ∈ {visible, focused}. Post only on flip.
    const bool nowActive = newState == XR_STATE_RUNNING_VISIBLE || newState == XR_STATE_RUNNING_FOCUSED;
    if (nowActive != m_active) {
        m_active = nowActive;
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{"openxractive", nowActive ? "1" : "0"});
    }

    // Idle-inhibit is rechecked on every session-state transition (the hook itself lands in WP9).
    if (g_pInputManager)
        g_pInputManager->recheckIdleInhibitorStatus();

    // Dormant re-probe lifecycle (report-17 WP-L3 / report-20 issue B1). Entering UNAVAILABLE arms the
    // backoff timer (if enabled + openxr:reprobe); STARTING disarms it WITHOUT resetting the backoff
    // (so a failed attempt keeps growing the delay); reaching a running or disabled steady state
    // disarms AND resets the backoff (a clean success / user disable).
    switch (newState) {
        case XR_STATE_UNAVAILABLE: maybeArmReprobe(); break;
        case XR_STATE_STARTING: cancelReprobe(/*resetBackoff=*/false); break;
        case XR_STATE_RUNNING_IDLE:
        case XR_STATE_RUNNING_VISIBLE:
        case XR_STATE_RUNNING_FOCUSED:
        case XR_STATE_DISABLED: cancelReprobe(/*resetBackoff=*/true); break;
        default: break;
    }
}

void COpenXRManager::start() {
    // Idempotent: only DISABLED/UNAVAILABLE may (re)start. UNAVAILABLE retries from scratch.
    if (m_state != XR_STATE_DISABLED && m_state != XR_STATE_UNAVAILABLE)
        return;

    setState(XR_STATE_STARTING);

    // report-20 issue B1: assume "waiting for the runtime" until we learn otherwise. A failure past
    // createInstance flips this to HEADSET (getSystem FORM_FACTOR_UNAVAILABLE) or keeps it at RUNTIME.
    m_probeWait = XR_WAIT_RUNTIME;

    m_runtimeName.clear();
    m_systemName.clear();
    m_runtimeGpu.clear();
    m_frameRequestedTeardown = false;

    // Parse the adaptive STRING options to enums up front (main thread) so the frame thread never
    // reads a CConfigValue<const char*>. Must happen before the frame thread launches below.
    publishAdaptiveStringTuning();
    publishHandInputPolicy(); // research/16 Part A: seed the hand-input policy from openxr:hand_input
    publishGrabStringTuning(); // task #25: seed hand_grab / hand_grab_anywhere / grab_filter_scope enums

    // Concurrency guard for the off-main handshake below. A previously-in-flight OR abandoned handshake
    // worker may still be blocked in xrCreateInstance against a wedged runtime, or an abandoned bring-up
    // helper may still be self-cleaning a live-but-doomed XrInstance; starting a second attempt would race
    // the OpenXR loader's process-global init. Skip this attempt — the reprobe retries once the in-flight
    // worker resolves and clears its flag.
    if (m_handshakeInFlight.load(std::memory_order_acquire) || m_bringupInFlight.load(std::memory_order_acquire)) {
        Log::logger->log(Log::DEBUG, "[OPENXR] a prior runtime handshake/bring-up is still in flight — deferring this probe");
        abortStart();
        return;
    }

    // WP-XR1: apply openxr:runtime_json to the loader's XR_RUNTIME_JSON before the FIRST loader call.
    // The OpenXR loader resolves the runtime manifest exclusively from getenv("XR_RUNTIME_JSON") (no
    // programmatic override exists), so this is the only lever — and it must be set on THIS thread,
    // BEFORE beginHandshake() spawns the helper that calls xrCreateInstance. setenv in a threaded
    // process is hazardous: glibc's setenv can realloc `environ`, which is a use-after-free against any
    // concurrent getenv on another thread. We contain that three ways: (1) we are on the main thread and
    // have already passed the m_handshakeInFlight guard above — so NO XR loader thread (the only threads
    // that call getenv("XR_RUNTIME_JSON")) is live right now; the frame thread does not start until
    // createSession() far below; (2) resolveRuntimeJsonEnv returns NOOP whenever the environment already
    // holds the desired value, so steady-state reprobes never touch `environ`; (3) the value is captured
    // ONCE (s_originalRtJson) so clearing openxr:runtime_json back to empty restores exactly the runtime
    // the process launched with — the flat<->XR toggle is reversible with no residual state. This is a
    // rare, sequenced, main-thread-only mutation, which is the accepted way to steer the loader.
    {
        static auto PRTJSON = CConfigValue<std::string>("openxr:runtime_json");
        // Capture the login-time XR_RUNTIME_JSON exactly once, before we ever mutate it.
        static const std::optional<std::string> s_originalRtJson = [] {
            const char* v = std::getenv("XR_RUNTIME_JSON");
            return v ? std::optional<std::string>{std::string(v)} : std::nullopt;
        }();

        const std::string cfg     = *PRTJSON;
        const char*       curRaw  = std::getenv("XR_RUNTIME_JSON");
        const auto        action  = OpenXR::resolveRuntimeJsonEnv(cfg, s_originalRtJson.has_value(), s_originalRtJson.value_or(""), curRaw != nullptr, curRaw ? curRaw : "");
        switch (action.kind) {
            case OpenXR::XR_RTJSON_SET:
                setenv("XR_RUNTIME_JSON", action.value.c_str(), 1);
                Log::logger->log(Log::DEBUG, "[OPENXR] openxr:runtime_json -> XR_RUNTIME_JSON = {}", action.value);
                break;
            case OpenXR::XR_RTJSON_UNSET:
                unsetenv("XR_RUNTIME_JSON");
                Log::logger->log(Log::DEBUG, "[OPENXR] openxr:runtime_json cleared -> restored login XR_RUNTIME_JSON (unset)");
                break;
            case OpenXR::XR_RTJSON_NOOP: break;
        }
        // Publish the active override (empty = using the loader default / active_runtime.json) for status.
        m_runtimeJson = cfg;
    }

    // Build the session on the heap (raw ownership) so a timed-out handshake can hand it to the helper
    // thread cleanly — CUniquePointer has no release(). Adopted into m_session only on completion.
    auto* sess = new CXRSession();

    // Overlay session (doc 01). Read openxr:overlay / openxr:overlay_z once, BEFORE createInstance
    // (which enables XR_EXTX_overlay only if requested AND advertised). Same semantics as
    // blend_mode: read at session start — changing requires disable/enable. Requested-but-
    // unsupported downgrades to a normal session with a WARN (never fails startup).
    {
        static auto POVERLAY     = CConfigValue<Hyprlang::INT>("openxr:overlay");
        static auto POVERLAYZ    = CConfigValue<Hyprlang::INT>("openxr:overlay_z");
        sess->m_overlayRequested = (*POVERLAY != 0);
        sess->m_overlayZ         = (uint32_t)std::max<int64_t>(0, (int64_t)*POVERLAYZ);
    }

    // Task #89 phase 2 blocker B: kick the runtime FIRST-CONTACT handshake (xrCreateInstance +
    // xrGetSystem) FULLY off the main thread and return immediately. beginHandshake() spawns a detached
    // worker; its result marshals back through the handshake eventfd to onHandshakeComplete(). The main
    // thread never parks — the 30s-cadence reprobe against a cold WiVRn socket (which synchronously spawns
    // and fails a monado child over several seconds) no longer stalls the desktop. State stays STARTING
    // until the completion lands; a re-entrant start() during that window is refused by the guard at the
    // top (state != DISABLED/UNAVAILABLE) plus the m_handshakeInFlight check above.
    if (m_handshakeFd < 0 && !setupHandshakeChannel()) {
        // The completion channel could not be created (essentially impossible past init()). Refuse to go
        // async — an unsignalled worker would wedge STARTING forever. Drop to UNAVAILABLE so the reprobe
        // keeps retrying; the desktop is unaffected.
        Log::logger->log(Log::ERR, "[OPENXR] handshake-completion channel unavailable; deferring probe, state -> unavailable");
        delete sess;
        setState(XR_STATE_UNAVAILABLE);
        return;
    }
    beginHandshake(sess);
}

bool COpenXRManager::setupHandshakeChannel() {
    // Persistent eventfd + wl_event_loop source for async handshake completions (task #89 phase 2). Mirrors
    // the frame->main channel but lives for the manager's whole lifetime (init() -> dtor): a detached
    // handshake worker may signal it long after any single start() returns.
    if (m_handshakeFd >= 0)
        return true;
    m_handshakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_handshakeFd < 0) {
        Log::logger->log(Log::ERR, "[OPENXR] eventfd() failed for the handshake-completion channel");
        return false;
    }
    m_handshakeSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, m_handshakeFd, WL_EVENT_READABLE,
        [](int /*fd*/, uint32_t /*mask*/, void* data) -> int {
            static_cast<COpenXRManager*>(data)->onHandshakeChannelReadable();
            return 0;
        },
        this);
    if (!m_handshakeSource) {
        Log::logger->log(Log::ERR, "[OPENXR] wl_event_loop_add_fd failed for the handshake-completion channel");
        close(m_handshakeFd);
        m_handshakeFd = -1;
        return false;
    }
    return true;
}

void COpenXRManager::teardownHandshakeChannel() {
    if (m_handshakeSource) {
        wl_event_source_remove(m_handshakeSource);
        m_handshakeSource = nullptr;
    }
    if (m_handshakeFd >= 0) {
        close(m_handshakeFd);
        m_handshakeFd = -1;
    }
}

// Shared claim block for the async first-contact handshake (see the header). Full definition here so the
// worker lambda and the completion path share one type; the shared_ptr member's dtor is emitted in this TU.
struct COpenXRManager::SHandshakeClaim {
    std::mutex                            mu;
    CXRSession*                           sess       = nullptr; // owned by the winner (main on completion, worker on abandon)
    bool                                  done       = false;   // worker finished createInstance + getSystem
    bool                                  abandoned  = false;   // main abandoned; worker deletes sess + clears the guard
    bool                                  instanceOk = false;
    bool                                  systemOk   = false;
    std::chrono::steady_clock::time_point started;              // for the slow-handshake WARN threshold
};

void COpenXRManager::beginHandshake(CXRSession* sess) {
    // Main thread. start()'s guard confirmed m_handshakeInFlight clear.
    auto claim         = std::make_shared<SHandshakeClaim>();
    claim->sess        = sess;
    claim->started     = std::chrono::steady_clock::now();
    m_pendingHandshake = claim;
    m_handshakeInFlight.store(true, std::memory_order_release);

    std::atomic<bool>* inflight = &m_handshakeInFlight; // stable: the manager is a session-lifetime singleton
    const int          fd       = m_handshakeFd;
    std::thread([sess, claim, inflight, fd]() {
        // The two blocking FIRST-CONTACT calls. WiVRn's client lib may spawn+fail a monado child over
        // several seconds here (no-headset case); all of it is off the main thread now.
        const bool                  iok = sess->createInstance();
        const bool                  sok = iok && sess->getSystem();
        std::lock_guard<std::mutex> lg(claim->mu);
        claim->instanceOk = iok;
        claim->systemOk   = sok;
        if (claim->abandoned) {
            // Main handed us ownership (stop/dtor/abortStart while we were blocked). Destroy off-main
            // (xrDestroyInstance frees a possibly-late-created instance + the socket fd) and release the
            // guard. Touch NOTHING owned by main.
            delete sess;
            inflight->store(false, std::memory_order_release);
        } else {
            claim->done = true;
            if (fd >= 0) {
                const uint64_t        one = 1;
                [[maybe_unused]] auto _   = write(fd, &one, sizeof(one)); // wake the main loop
            }
        }
    }).detach();
}

void COpenXRManager::onHandshakeChannelReadable() {
    // Main thread (wl_event_loop). Drain the counter, then run the completion if the worker finished.
    uint64_t v;
    while (read(m_handshakeFd, &v, sizeof(v)) == sizeof(v)) {}

    if (!m_pendingHandshake)
        return; // abandoned/consumed already; the wake was for a handshake we no longer track
    {
        std::lock_guard<std::mutex> lg(m_pendingHandshake->mu);
        if (!m_pendingHandshake->done)
            return; // spurious wake (or abandoned between the wake and here)
    }
    auto claim = m_pendingHandshake;
    m_pendingHandshake.reset();
    onHandshakeComplete(claim);
}

void COpenXRManager::abandonHandshake() {
    // Main thread. Called from stop()/abortStart()/dtor to drop an in-flight handshake without ever
    // blocking on it. Idempotent.
    if (!m_pendingHandshake)
        return;
    auto                        claim = m_pendingHandshake;
    m_pendingHandshake.reset();
    std::lock_guard<std::mutex> lg(claim->mu);
    if (claim->done) {
        // Worker already finished (the runtime answered, so xrDestroyInstance is fast) — main owns sess.
        delete claim->sess;
        claim->sess = nullptr;
        m_handshakeInFlight.store(false, std::memory_order_release);
    } else {
        // Worker still blocked in first-contact IPC. Hand it ownership; it deletes sess + clears the guard
        // when it returns. Do NOT touch sess again.
        claim->abandoned = true;
    }
}

void COpenXRManager::onHandshakeComplete(std::shared_ptr<SHandshakeClaim> claim) {
    // Main thread (handshake eventfd). The worker has exited its critical section; we own `sess`.
    m_handshakeInFlight.store(false, std::memory_order_release);
    CXRSession* sess = claim->sess;
    claim->sess      = nullptr;

    const auto elapsedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - claim->started).count();
    if (elapsedMs >= OpenXR::XR_HANDSHAKE_TIMEOUT_MS)
        Log::logger->log(Log::WARN, "[OPENXR] runtime handshake took {}ms (runtime slow to answer); the desktop stayed responsive throughout", elapsedMs);

    // 1. Instance (extension checks; missing runtime / required extensions -> UNAVAILABLE). Adopt `sess`
    //    into m_session so abortStart() tears it down on the failure paths (no helper is in flight here).
    if (!claim->instanceOk) {
        m_session = UP<CXRSession>(sess);
        // Live-evidence bug 1: a REACHABLE runtime that lacks the required extensions is WiVRn's
        // degraded pre-don mode (its main server listens on comp_ipc from service start and only
        // advertises the real extension set once the headset connects and the compositor server
        // forks). Classify it as a HEADSET wait so the reprobe polls at the fixed base cadence —
        // growing the no-runtime backoff here is what produced the observed 30s don dead-air.
        // "Reachable" needs BOTH signals: the enumerate answered (m_runtimeReachable) AND a known
        // runtime IPC socket exists on disk — WiVRn's CLIENT lib answers enumerate with the same
        // degraded list even when NO server is running (verified in an isolated $XDG_RUNTIME_DIR),
        // so without the socket stat a stopped service would poll at base forever too.
        m_probeWait = m_session->m_runtimeReachable && runtimeSocketPresent() ? XR_WAIT_HEADSET : XR_WAIT_RUNTIME;
        Log::logger->log(Log::WARN, "[OPENXR] {}, state -> unavailable",
                         m_probeWait == XR_WAIT_HEADSET ? "runtime reachable but degraded (headset likely not connected)" : "no runtime / required extensions unavailable");
        abortStart();
        return;
    }

    // 2. System.
    if (!claim->systemOk) {
        m_session = UP<CXRSession>(sess);
        // report-20 issue B1: FORM_FACTOR_UNAVAILABLE = runtime up, headset not connected/donned — the
        // spec-intended "poll me later" result. The re-probe then waits gently for the headset rather
        // than growing the backoff as it would for an absent runtime.
        m_probeWait = m_session->m_formFactorUnavailable ? XR_WAIT_HEADSET : XR_WAIT_RUNTIME;
        Log::logger->log(Log::WARN, "[OPENXR] system lookup failed ({}), state -> unavailable", m_probeWait == XR_WAIT_HEADSET ? "headset not connected" : "runtime error");
        abortStart();
        return;
    }

    // Handshake OK: hand `sess` (still raw) to the bring-up continuation, which keeps it raw so a
    // timed-out bring-up can transfer it to the helper for self-cleanup.
    continueBringup(sess);
}

void COpenXRManager::continueBringup(CXRSession* sess) {
    // Main thread. `sess` is raw-owned (not yet in m_session). Graphics is likewise kept raw until bring-up
    // succeeds — both must be transferable to the bring-up helper on a timeout (CUniquePointer has no
    // release()). On any SYNCHRONOUS failure below (no helper in flight) we adopt the raws into the members
    // and reuse abortStart()'s tested teardown.
    auto* gfx = new CXRGraphics();

    // report-19: latch whether this runtime+device can drive the plug gate on user presence (else the
    // `visible` mode falls back to visibility + the first-plug blip guard). Fresh session -> clear the
    // per-session presence/first-plug bookkeeping before any plug decision runs.
    m_userPresenceSupported = sess->m_supportsUserPresence;
    resetPresenceState();

    // Publish runtime/system names for `hyprctl openxr status` as soon as we have them.
    m_runtimeName = sess->runtimeName();
    m_systemName  = sess->systemName();

    // Select the environment blend mode (doc 01) from openxr:blend_mode against the runtime's
    // enumerated list (getSystem filled m_blendModes in preference order). Read once at session
    // start — changing openxr:blend_mode takes effect on the next start. `auto` picks the runtime
    // preferred mode; an explicit mode the runtime doesn't advertise falls back with a WARN.
    {
        static auto      PBLEND = CConfigValue<std::string>("openxr:blend_mode");
        const auto       pick   = OpenXR::pickBlendMode(sess->m_blendModes, *PBLEND);
        sess->m_blendMode       = xrBlendModeToXr(pick.mode);
        if (pick.requestedUnsupported)
            Log::logger->log(Log::WARN, "[OPENXR] openxr:blend_mode '{}' is not supported by this runtime; falling back to '{}'", *PBLEND,
                             OpenXR::blendModeToString(pick.mode));
        else
            Log::logger->log(Log::DEBUG, "[OPENXR] environment blend mode: {} (openxr:blend_mode = {})", OpenXR::blendModeToString(pick.mode), *PBLEND);
    }

    // 3. EGL/GBM display + context on the right GPU.
    static auto       PGPU = CConfigValue<std::string>("openxr:gpu");
    const std::string gpu  = *PGPU;
    if (!gfx->initEGL(gpu)) {
        Log::logger->log(Log::ERR, "[OPENXR] EGL/GBM init failed, state -> unavailable");
        m_session  = UP<CXRSession>(sess); // adopt so abortStart() tears both down (synchronous failure)
        m_graphics = UP<CXRGraphics>(gfx);
        abortStart();
        return;
    }

    // 3b. Fail closed on a wrong openxr:gpu (coredumps 8986/39318). If the XR EGL context landed on
    // a DIFFERENT physical GPU than the one the runtime composites on, the runtime imports
    // cross-GPU buffers at xrCreateSwapchain (on the frame thread) and HARD-CRASHES inside the
    // graphics driver (radeonsi driUnbindContext) — an uncatchable SEGV that takes the whole
    // compositor, and with it the user's desktop session, down. WiVRn/Monado accept a mismatched
    // EGL binding at xrCreateSession without complaint, so this is the last point we can refuse
    // while the desktop is still intact. The runtime's GPU is probed via XR_KHR_vulkan_enable2
    // (best-effort: an undeterminable result proceeds with a warning rather than blocking a setup
    // that might be fine). Runs on the main thread, before the frame thread — no interop yet.
    {
        const auto&         node = gfx->selectedRenderNode();
        OpenXR::SRuntimeGpu rt;
        if (sess->m_hasVulkanEnable2) {
            // Run the probe on a THROWAWAY thread with a bounded wait. vkCreateInstance inside it
            // can deadlock indefinitely against the runtime's own in-process Vulkan usage (observed
            // hanging forever vs Monado's null compositor), and this must NEVER freeze the
            // compositor. On timeout we abandon the thread (it bails before any XrInstance call, so
            // a late unblock can't touch a torn-down instance) and proceed UNVERIFIED — strictly no
            // worse than before this guard existed. When the probe does answer (the common case on
            // a healthy runtime) we get a reliable cross-GPU verdict.
            auto             result  = std::make_shared<OpenXR::SRuntimeGpu>();
            auto             done    = std::make_shared<std::atomic<bool>>(false);
            auto             abandon = std::make_shared<std::atomic<bool>>(false);
            const XrInstance inst    = sess->m_instance;
            const XrSystemId sys     = sess->m_systemId;
            std::thread([result, done, abandon, inst, sys]() {
                auto r = OpenXR::probeRuntimeRenderNode(inst, sys, abandon.get());
                if (!abandon->load(std::memory_order_acquire))
                    *result = r;
                done->store(true, std::memory_order_release);
            }).detach();

            constexpr int kProbeTimeoutMs = 3000;
            for (int waited = 0; waited < kProbeTimeoutMs && !done->load(std::memory_order_acquire); waited += 25)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));

            if (done->load(std::memory_order_acquire))
                rt = *result;
            else {
                abandon->store(true, std::memory_order_release);
                rt.note = "GPU probe timed out";
                Log::logger->log(Log::WARN, "[OPENXR] runtime GPU probe did not respond within {}ms; proceeding without GPU verification", kProbeTimeoutMs);
            }
        } else
            rt.note = "runtime does not advertise XR_KHR_vulkan_enable2";

        // Publish the resolved runtime GPU for `hyprctl openxr status` (empty when undeterminable).
        m_runtimeGpu = rt.determined ? std::format("{} (drm {}:{})", rt.deviceName.empty() ? "GPU" : rt.deviceName, rt.drmMajor, rt.drmMinor) : "";

        if (rt.determined && node.valid && (rt.drmMajor != node.major || rt.drmMinor != node.minor)) {
            Log::logger->log(Log::ERR,
                             "[OPENXR] openxr:gpu selects {} (drm {}:{}) but the runtime '{}' composites on {} (drm {}:{}). Cross-GPU buffer import "
                             "crashes the graphics driver and would take the whole session down, so XR is refusing to start. Point openxr:gpu at the "
                             "runtime's GPU (or unset it). Desktop session unaffected.",
                             node.path, node.major, node.minor, m_runtimeName, rt.deviceName.empty() ? "another GPU" : rt.deviceName, rt.drmMajor, rt.drmMinor);
            m_session  = UP<CXRSession>(sess); // adopt so abortStart() tears both down (synchronous refusal)
            m_graphics = UP<CXRGraphics>(gfx);
            abortStart();
            return;
        }

        if (rt.determined && node.valid)
            Log::logger->log(Log::DEBUG, "[OPENXR] XR GPU verified against runtime: {} (drm {}:{})", rt.deviceName.empty() ? node.path : rt.deviceName, node.major, node.minor);
        else
            Log::logger->log(Log::WARN,
                             "[OPENXR] could not verify the XR GPU matches the runtime ({}); proceeding. If the session crashes at swapchain creation, "
                             "openxr:gpu is pointing at the wrong GPU (it must be the GPU the runtime renders on).",
                             !node.valid ? "XR render node unknown (shared-display fallback)" : rt.note);
    }

    // --- Direct-mode session BRING-UP (steps 4-8), task #89 phase 2 blocker A -------------------
    // Each of xrCreateSession / createSpaces / chooseSwapchainFormat / input->init makes a SYNCHRONOUS
    // OpenXR IPC round-trip; initBlitGL is pure GL on the XR context. In DRM-lease direct mode the IPC is
    // a latent cross-process deadlock: Monado is the DRM master of the leased connector and its render/IPC
    // servicing is coupled to that connector's vblank (see e80e03be #3 + #1). On 2026-07-15 a sick leased
    // DP link stalled Monado mid-bring-up and the WHOLE desktop (eDP included) froze until a power-cycle.
    // runBoundedBringup() now runs ALL of steps 4-8 on a HELPER thread while THIS (main) thread parks in a
    // bounded wait (XR_BRINGUP_TIMEOUT_MS); on timeout the main thread ABANDONS the helper (which
    // self-cleans sess/graphics off-main) and falls back to UNAVAILABLE — the desktop stays responsive.
    // The [XR-START] step breadcrumbs (stamped by the helper) and the [XR-START-WATCHDOG] stall marker
    // (stamped by main's park loop) still go to STDERR — the only sink that reached the journal and
    // survived the incident — so a future stall still self-documents the exact blocked IPC call.
    const eBringupResult br = runBoundedBringup(sess, gfx);
    if (br == BRINGUP_TIMEOUT) {
        // Ownership of sess + gfx (and any partially-created input) was transferred to the still-running
        // helper — do NOT touch them. m_bringupInFlight is latched; the helper clears it on late return.
        Log::logger->log(Log::WARN, "[OPENXR] session bring-up did not complete within {}ms — finishing it off-thread; state -> unavailable", OpenXR::XR_BRINGUP_TIMEOUT_MS);
        m_probeWait = XR_WAIT_RUNTIME; // the runtime is present-but-unresponsive; poll it again shortly
        markWatchActivity();           // treat as "runtime materializing": next probes at base cadence
        m_runtimeName.clear();
        m_systemName.clear();
        m_runtimeGpu.clear();
        setState(XR_STATE_UNAVAILABLE);
        return;
    }

    // Completed within the budget — the helper has exited and we own sess + gfx. Adopt them into the
    // members so the finish below (and abortStart() on BRINGUP_FAILED) operates on the manager's objects.
    m_session  = UP<CXRSession>(sess);
    m_graphics = UP<CXRGraphics>(gfx);

    if (br == BRINGUP_FAILED) {
        // A fatal step (createSession / createSpaces / initBlitGL) returned false. Any CXRInput the helper
        // created was already adopted into m_input by runBoundedBringup; abortStart() tears the lot down.
        abortStart();
        return;
    }

    // 9. Bind any layers whose monitor still exists (created while disabled — doc 02), then
    //    set up the frame->main channel and spawn the frame thread.
    bindExistingLayers();

    if (!setupFrameChannel()) {
        abortStart();
        return;
    }

    // The frame thread is the single producer for the frame->main queue; give CXRInput the
    // producer sink now that the eventfd exists and before the thread spawns.
    if (m_input)
        m_input->setEmitter([this](XRQueueItem item) { enqueue(std::move(item)); });

    // All bring-up OpenXR IPC is done (it ran on the bring-up helper; the main thread only parked). Stamp
    // the journal so the next hang's last [XR-START] line unambiguously means "blocked in the named step"
    // rather than "raced past it". The frame thread now owns all subsequent runtime IPC.
    fprintf(stderr, "[XR-START] session bring-up complete; frame thread taking over\n");
    fflush(stderr);

    m_running     = true;
    m_frameThread = std::thread([this] { frameThread(); });

    setState(XR_STATE_RUNNING_IDLE);
    Log::logger->log(Log::DEBUG, "[OPENXR] session up (runtime: {}, system: {}), frame thread started", m_runtimeName, m_systemName);

    // Re-assert the plugged state now that a session EXISTS (research/18 WP-M2). Under the
    // `session` mode this plugs the XR monitors immediately; under the default `visible` mode the
    // session is still IDLE here (not worn yet), so this is a no-op until the frame->main
    // dispatch reports VISIBLE/FOCUSED (report-18 addendum). allowGrace=false: settle the steady
    // state at once, no doff-grace on a fresh start. Done before the reconcile below so declared-set
    // diffs (mode changes) apply to enabled outputs.
    updateMonitorsPlugged(/*allowGrace=*/false);

    // Monitors now come only from the config keyword / dispatcher / hyprctl (WP4). Any monitors
    // declared/created while disabled were already materialized as headless outputs and bound
    // above (bindExistingLayers); create any declared-but-missing ones now that a session is up.
    reconcileDeclaredMonitors();
    recomputeQuadActive();

    // report-20 issue B2 (self-heal): re-assert the plugged state once more after the declared set is
    // reconciled + bound, so a monitor bound during reconcile above (or a presence/visibility event
    // that arrived on the queue DURING start(), before the frame->main channel could be drained)
    // cannot leave the plug edge stuck. Idempotent — a no-op when nothing changed.
    updateMonitorsPlugged(/*allowGrace=*/false);

    // Register the synthetic ray pointer (doc 04 §8), honoring openxr:pointer.
    ensurePointerDevice();
}

void COpenXRManager::abortStart() {
    // No frame thread has been started at any abortStart() call site, so there is nothing
    // to join. Tear down in the doc-01 order: GL -> XR handles -> EGL.
    teardownFrameChannel();
    if (m_graphics)
        m_graphics->destroyGL();
    if (m_input) {
        m_input->destroy(); // action spaces are session children — destroy before the session
        m_input.reset();
    }
    if (m_session)
        m_session->destroy();
    if (m_graphics)
        m_graphics->destroyEGL();
    m_graphics.reset();
    m_session.reset();

    m_runtimeName.clear();
    m_systemName.clear();
    m_runtimeGpu.clear();

    setState(XR_STATE_UNAVAILABLE);
}

COpenXRManager::eBringupResult COpenXRManager::runBoundedBringup(CXRSession* sess, CXRGraphics* gfx) {
    // Task #89 phase 2 blocker A. Runs steps 4-8 on a detached helper while the MAIN thread parks in a
    // bounded busy-wait (XR_BRINGUP_TIMEOUT_MS) — the runBoundedHandshake idiom. The shared claim decides
    // EXACTLY ONCE, under its mutex, who owns sess + gfx (+ any created input): the main thread (helper
    // finished in time -> BRINGUP_OK/FAILED, main adopts) or the helper (main timed out -> ownership
    // transferred; the helper tears everything down off-main in doc-01 order and clears m_bringupInFlight).
    //
    // Threading safety. (1) SP refcounts: sess/gfx are RAW (not CUniquePointer, which has no release()),
    // and no hyprutils SP/WP refcount op happens on the helper — ownership genuinely transfers on abandon.
    // (2) EGL: while main parks it does ZERO EGL/GL and never touches sess/gfx, so the helper owns the XR
    // EGL context uninterrupted (createSession/createSpaces bind then unbind it; initBlitGL uses
    // CScopedGLContext which restores the prior — here empty — binding). The context is unbound again
    // before the helper returns, so the frame thread can claim it, and the main thread's own renderer EGL
    // binding is never disturbed (the helper only ever touches its OWN thread's binding). (3) STRING
    // config: none is read on the helper — CXRInput::init/suggestBindings/createActionSpaces are pure XR
    // IPC; all string tuning was pre-parsed to atomics on the main thread in start().
    struct SBringupClaim {
        std::mutex       mu;
        CXRSession*      session   = nullptr;
        CXRGraphics*     graphics  = nullptr;
        CXRInput*        input     = nullptr; // created by the helper; adopted by main on success
        std::atomic<int> step{0};             // last-entered step index, for the watchdog breadcrumb
        bool             done      = false;
        bool             ok        = false;
        bool             abandoned = false;
    };
    auto               claim    = std::make_shared<SBringupClaim>();
    claim->session              = sess;
    claim->graphics             = gfx;
    std::atomic<bool>* inflight = &m_bringupInFlight; // stable: the manager is a session-lifetime singleton

    std::thread([claim, inflight]() {
        auto*      s     = claim->session;
        auto*      g     = claim->graphics;
        const auto crumb = [&](int idx, const char* name) {
            claim->step.store(idx, std::memory_order_release);
            fprintf(stderr, "[XR-START] entering %s\n", name); // survives a power-cycle in the journal
            fflush(stderr);
        };

        // Steps 4-7 (fatal on failure). 4 = xrCreateSession (IPC + binds the EGL context to the runtime),
        // 5 = reference/view spaces (IPC), 6 = blit GL resources (pure GL on the XR context), 7 = swapchain
        // format (IPC). initBlitGL runs here (not on main) so the whole leg is a SINGLE bounded park.
        crumb(1, "xrCreateSession");
        bool ok = s->createSession(*g);
        if (ok) {
            crumb(2, "createSpaces");
            ok = s->createSpaces(*g);
        }
        if (ok) {
            crumb(3, "initBlitGL");
            ok = g->initBlitGL();
        }
        if (ok) {
            crumb(4, "chooseSwapchainFormat");
            s->chooseSwapchainFormat();
        }

        // Step 8 (WP6 action system): NON-fatal — a session with no input still composites quads.
        CXRInput* in = nullptr;
        if (ok) {
            crumb(5, "input->init");
            in = new CXRInput();
            if (!in->init(*s, s->m_hasHandInteraction)) {
                Log::logger->log(Log::WARN, "[OPENXR] input action system unavailable; continuing without XR input");
                in->destroy();
                delete in;
                in = nullptr;
            }
        }

        std::lock_guard<std::mutex> lg(claim->mu);
        claim->input = in;
        claim->ok    = ok;
        if (claim->abandoned) {
            // Main timed out and handed us ownership of session + graphics (+ any input). Tear down in the
            // doc-01 order (GL -> input/XR handles -> EGL) off the main thread, then release the guard.
            // Touch NOTHING owned by main. These teardown IPC calls are bounded by monado's client-side
            // timeout (phase 1); we do NOT retry into the dead connection.
            if (in) {
                in->destroy();
                delete in;
            }
            g->destroyGL();
            s->destroy();
            g->destroyEGL();
            delete g;
            delete s;
            inflight->store(false, std::memory_order_release);
        } else
            claim->done = true;
    }).detach();

    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::milliseconds(OpenXR::XR_BRINGUP_TIMEOUT_MS);
    const auto started    = std::chrono::steady_clock::now();
    int        nextWarnMs = 6000;
    for (;;) {
        {
            std::lock_guard<std::mutex> lg(claim->mu);
            if (claim->done) {
                if (claim->input)
                    m_input = UP<CXRInput>(claim->input); // adopt the action system (may be null = input disabled)
                return claim->ok ? BRINGUP_OK : BRINGUP_FAILED;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::lock_guard<std::mutex> lg(claim->mu);
            if (claim->done) { // finished right at the boundary — reclaim ownership
                if (claim->input)
                    m_input = UP<CXRInput>(claim->input);
                return claim->ok ? BRINGUP_OK : BRINGUP_FAILED;
            }
            // Transfer ownership of session + graphics (+ any partial input) to the helper and latch the
            // guard so no reprobe starts a second attempt until the helper self-cleans and clears it.
            claim->abandoned = true;
            m_bringupInFlight.store(true, std::memory_order_release);
            return BRINGUP_TIMEOUT;
        }
        // Watchdog: stamp the journal if the (bounded) park runs unusually long, naming the step the helper
        // is stuck on. Main WILL abandon at the deadline, so — unlike before phase 2 — the desktop no longer
        // freezes here; this marker is now purely a diagnostic breadcrumb for a slow/wedged runtime.
        const int elapsedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (elapsedMs >= nextWarnMs) {
            static const char* const kStepName[] = {"(entered bring-up)", "xrCreateSession",       "createSpaces",
                                                     "initBlitGL",         "chooseSwapchainFormat", "input->init"};
            const int                st          = claim->step.load(std::memory_order_acquire);
            fprintf(stderr,
                    "[XR-START-WATCHDOG] direct-mode session bring-up BLOCKED %dms at step '%s' — probable cross-process "
                    "deadlock with the runtime on the leased connector (e80e03be #3 class, startup path). It runs on a helper "
                    "thread; the main thread's bounded wait abandons it at %dms and falls back to UNAVAILABLE, so the desktop "
                    "stays responsive.\n",
                    elapsedMs, kStepName[(st >= 0 && st <= 5) ? st : 0], OpenXR::XR_BRINGUP_TIMEOUT_MS);
            fflush(stderr);
            nextWarnMs += 3000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void COpenXRManager::stop() {
    if (m_state == XR_STATE_DISABLED)
        return;

    // A first-contact handshake may still be in flight if stop() lands during STARTING (e.g. a `hyprctl
    // openxr stop` or an openxr:enabled=0 reload while the async handshake is running). Abandon it here so
    // its late completion cannot resurrect a session we are tearing down. `sess` was never adopted into
    // m_session while in flight, so nothing below double-frees it. An abandoned bring-up helper (if any)
    // owns its own sess/graphics and self-cleans — m_session/m_graphics are already null in that case.
    abandonHandshake();

    // Whether stop was ultimately triggered by instance loss decides the final state.
    const bool lost = m_session && m_session->m_instanceLost;

    setState(XR_STATE_STOPPING);

    // Remove the ray pointer first (main thread) so no further input routes while tearing down.
    removePointerDevice();

    // Teardown ordering (doc 01): join the frame thread BEFORE touching any EGL/XR object.
    m_running = false;
    if (m_frameThread.joinable())
        m_frameThread.join();

    teardownFrameChannel();

    // Teardown ordering (doc 01). The frame thread is gone, so no removal barrier is needed
    // (path C): destroy per-layer frame resources directly.
    // 2. GL cleanup with the context current — per-layer EGLImage/staging tex, then shared.
    if (m_graphics && m_graphics->m_xrContext != EGL_NO_CONTEXT) {
        CXRGraphics::CScopedGLContext ctx(*m_graphics);
        for (auto& l : m_layers)
            l->destroyFrameResourcesGL(*m_graphics);
    }
    if (m_graphics)
        m_graphics->destroyGL();

    // 3. Per-layer swapchains (context NOT current), then the action system, then XR handles.
    //    On a lost runtime, skip the per-object xr destroy calls: each is a doomed IPC round-trip
    //    (the "Broken pipe" storm seen in the field) and a would-be main-thread block against a
    //    wedged runtime. xrDestroyInstance (in CXRSession::destroy) reaps all child handles.
    for (auto& l : m_layers)
        l->destroySwapchain(/*skipXrCall=*/lost);
    if (m_input) {
        m_input->destroy(/*runtimeLost=*/lost); // action spaces are session children — destroy before the session
        m_input.reset();
    }
    if (m_session)
        m_session->destroy(/*runtimeLost=*/lost);

    // 5. EGL/GBM.
    if (m_graphics)
        m_graphics->destroyEGL();

    // 6. Monitor disposition per openxr:destroy_monitors_on_stop.
    teardownLayers();

    // 7. Unplug the surviving XR monitors — the session no longer exists (research/18 WP-M2).
    //    Workspaces evacuate to the remaining monitors through the ordinary hotplug path; the
    //    layers (anchor state, adaptive phase, declared spec) persist for the next start().
    //    allowGrace=false: session END is immediate, never deferred by the doff-grace (which only
    //    covers a VISIBLE->hidden drop WHILE the session persists). m_state is STOPPING here, so
    //    the predicate sees sessionUp=false and unplugs (unless mode==off, which stays plugged).
    //    Also cancels any pending grace timer. No-op for layers teardownLayers just destroyed.
    updateMonitorsPlugged(/*allowGrace=*/false);
    m_userPresenceSupported = false; // recomputed at the next start(); status reads "unsupported" when stopped

    m_graphics.reset();
    m_session.reset();

    m_runtimeName.clear();
    m_systemName.clear();
    m_runtimeGpu.clear();

    setState(lost ? XR_STATE_UNAVAILABLE : XR_STATE_DISABLED);
}

eXRManagerState COpenXRManager::mapSessionState(int xrSessionState) {
    switch (xrSessionState) {
        case XR_SESSION_STATE_VISIBLE: return XR_STATE_RUNNING_VISIBLE;
        case XR_SESSION_STATE_FOCUSED: return XR_STATE_RUNNING_FOCUSED;
        // IDLE / READY / SYNCHRONIZED / STOPPING all map to RUNNING_IDLE (doc 01).
        default: return XR_STATE_RUNNING_IDLE;
    }
}

bool COpenXRManager::setupFrameChannel() {
    m_queue.reset(); // no producer/consumer yet — safe to clear any stale items from a prior run
    m_queueOverflowed.store(false);

    m_eventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_eventFd < 0) {
        Log::logger->log(Log::ERR, "[OPENXR] eventfd() failed for the frame->main channel");
        return false;
    }

    m_eventSource = wl_event_loop_add_fd(
        g_pCompositor->m_wlEventLoop, m_eventFd, WL_EVENT_READABLE,
        [](int /*fd*/, uint32_t /*mask*/, void* data) -> int {
            static_cast<COpenXRManager*>(data)->onFrameChannelReadable();
            return 0;
        },
        this);

    if (!m_eventSource) {
        Log::logger->log(Log::ERR, "[OPENXR] wl_event_loop_add_fd failed for the frame->main channel");
        close(m_eventFd);
        m_eventFd = -1;
        return false;
    }

    return true;
}

void COpenXRManager::teardownFrameChannel() {
    if (m_eventSource) {
        wl_event_source_remove(m_eventSource);
        m_eventSource = nullptr;
    }
    if (m_eventFd >= 0) {
        close(m_eventFd);
        m_eventFd = -1;
    }
    // The frame thread is joined before teardownFrameChannel runs, so this is single-threaded.
    m_queue.reset();
}

void COpenXRManager::wakeMain() {
    if (m_eventFd < 0)
        return;
    const uint64_t        one = 1;
    [[maybe_unused]] auto _   = write(m_eventFd, &one, sizeof(one));
}

void COpenXRManager::enqueue(XRQueueItem item) {
    // Frame thread (single producer). Full-queue policy (doc 04 §7.2): MOTION_ABS/AXIS may be
    // dropped (regenerated next frame); BUTTON/FRAME and every SXRStateEvent must not be — log
    // once if that ever happens (1024-deep with per-dispatch drain cannot realistically fill).
    bool droppable = false;
    if (auto* ie = std::get_if<SXRInputEvent>(&item))
        droppable = ie->type == eXRInputEventType::MOTION_ABS || ie->type == eXRInputEventType::AXIS;

    if (!m_queue.push(std::move(item))) {
        if (!droppable && !m_queueOverflowed.exchange(true))
            Log::logger->log(Log::ERR, "[OPENXR] frame->main event queue overflow; a non-droppable event was lost");
        return;
    }
    wakeMain();
}

void COpenXRManager::reportState(eXRManagerState s) {
    // Frame thread: SESSION_STATE event carrying the already-mapped manager state.
    SXRStateEvent ev;
    ev.type = eXRStateEventType::SESSION_STATE;
    ev.a    = (int32_t)s;
    enqueue(ev);
}

void COpenXRManager::dispatchStateEvent(const SXRStateEvent& e) {
    switch (e.type) {
        case eXRStateEventType::SESSION_STATE:
            setState((eXRManagerState)e.a);
            // report-18 addendum: the plugged-state visibility edge. Under the `visible` mode,
            // reaching VISIBLE/FOCUSED plugs the XR monitors immediately (fast don), and dropping
            // to IDLE/SYNCHRONIZED (doffed/standby) arms the anti-flap grace-unplug. allowGrace=true
            // — this is the live proximity-sensor edge the grace exists to smooth. No-op under the
            // `session`/`off` modes (their plugged state does not track visibility).
            updateMonitorsPlugged(/*allowGrace=*/true);
            break;
        case eXRStateEventType::USER_PRESENCE:
            // report-19: the donned/doffed edge. Under the `visible` mode with a presence-capable
            // runtime this is THE plug signal (don -> plug fast, doff -> grace-unplug). Record the
            // presence facts, then run the same funnel (allowGrace=true). No-op under session/off, or
            // if the runtime lacks presence support (we never subscribe/forward in that case).
            m_presenceKnown = true;
            m_userPresent   = e.a != 0;
            Log::logger->log(Log::DEBUG, "[OPENXR] user {} — re-evaluating monitor plug state", m_userPresent ? "present (donned)" : "absent (doffed)");
            updateMonitorsPlugged(/*allowGrace=*/true);
            // research/20 phase 2: presence is now an INPUT to shouldInhibitIdle() under
            // openxr:inhibit_idle = present, so the don/doff edge must re-fold the Wayland
            // idle-inhibit bit. Without this the bit would only move on session-state transitions,
            // and a doff that does not change the session state would leave the desktop pinned awake.
            // Unlike the plug gate there is NO grace here on purpose: releasing the inhibit does not
            // fire a pending idle event (CIdleNotifyProtocol::setInhibit(false) restarts the FULL
            // timeout — research/20 §3.4), so a doff already buys the listener's whole countdown and
            // a re-don inside it silently cancels.
            if (g_pInputManager)
                g_pInputManager->recheckIdleInhibitorStatus();
            break;
        case eXRStateEventType::LAYER_REMOVED:
            // Removal barrier step 3: the frame thread released a layer's resources and acked.
            finalizeLayerRemoval(e.str);
            break;
        case eXRStateEventType::GRAB:
            // The frame thread already resolved the monitor name at grab-begin/end time (it
            // carries a race with the layer-removed ack otherwise — doc 05 §5). Post verbatim.
            if (!e.str.empty() && g_pEventManager)
                g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorgrab", e.str + (e.a ? ",1" : ",0")});
            break;
        case eXRStateEventType::ADAPTIVE:
            // research/13 §6.4: the terminal dock/undock edge (a = 1 undocked / 0 docked).
            if (!e.str.empty() && g_pEventManager)
                g_pEventManager->postEvent(SHyprIPCEvent{e.a ? "xrmonitorundocked" : "xrmonitordocked", e.str});
            break;
        case eXRStateEventType::TRACKING: Log::logger->log(Log::DEBUG, "[OPENXR] device-lock tracking {} (monitor {})", e.a ? "gained" : "lost", e.str); break;
        case eXRStateEventType::SCHEDULE_FRAMES: {
            // Pacing on behalf of the frame thread (see the frame loop): scheduleFrame() is
            // main-thread-only (aquamarine idle-callback list has no lock).
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers) {
                if (l->m_pendingRemoval.load(std::memory_order_acquire))
                    continue;
                if (auto mon = l->m_monitor.lock())
                    mon->scheduleFrame();
            }
            break;
        }
    }
}

void COpenXRManager::dispatchInputEvent(const SXRInputEvent& e) {
    // Main thread: replay a frame-thread pointer event onto the synthetic device (doc 04 §8).
    // Hover bookkeeping (m_hovered / last-hovered) is kept up to date even when the pointer device
    // is absent (openxr:pointer = 0) so status JSON and selection still reflect the ray.
    switch (e.type) {
        case eXRInputEventType::MOTION_ABS: {
            if (e.monitorID < 0) {
                setHoveredMonitor(""); // ray left all quads
                break;
            }
            auto layer = layerByMonitorID(e.monitorID);
            if (!layer)
                break; // monitor died in flight
            setHoveredMonitor(layer->m_monitorName);
            if (m_pointerDevice) {
                // m_boundOutput routes the 0-1 absolute onto this monitor's box (doc 04 §8).
                m_pointerDevice->m_boundOutput = layer->m_monitorName;
                m_pointerDevice->m_pointerEvents.motionAbsolute.emit(IPointer::SMotionAbsoluteEvent{.timeMs = e.timeMs, .absolute = e.uv, .device = m_pointerDevice});
            }
            break;
        }
        case eXRInputEventType::BUTTON:
            if (m_pointerDevice)
                m_pointerDevice->m_pointerEvents.button.emit(IPointer::SButtonEvent{
                    .timeMs = e.timeMs, .button = e.button, .state = e.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED, .mouse = true});
            break;
        case eXRInputEventType::AXIS:
            if (m_pointerDevice)
                m_pointerDevice->m_pointerEvents.axis.emit(
                    IPointer::SAxisEvent{.timeMs = e.timeMs, .source = WL_POINTER_AXIS_SOURCE_CONTINUOUS, .axis = e.axis, .delta = e.axisDelta, .deltaDiscrete = 0, .mouse = true});
            break;
        case eXRInputEventType::FRAME:
            if (m_pointerDevice)
                m_pointerDevice->m_pointerEvents.frame.emit();
            break;
    }
}

void COpenXRManager::setHoveredMonitor(const std::string& name) {
    if (m_curHoveredMonitor == name)
        return;
    if (!m_curHoveredMonitor.empty())
        if (auto prev = layerByName(m_curHoveredMonitor))
            prev->m_hovered = false;
    m_curHoveredMonitor = name;
    if (!name.empty()) {
        if (auto l = layerByName(name))
            l->m_hovered = true;
        m_lastHoveredMonitor = name; // selection rule 2 (doc 05 §3.2) — persists as "last"
    }
}

PXRLAYER COpenXRManager::layerByMonitorID(MONITORID id) {
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers) {
        if (l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        if (auto mon = l->m_monitor.lock(); mon && mon->m_id == id)
            return l;
    }
    return nullptr;
}

void COpenXRManager::ensurePointerDevice() {
    static auto PPOINTER = CConfigValue<Hyprlang::INT>("openxr:pointer");
    if (!*PPOINTER || !m_running.load() || m_pointerDevice || !g_pInputManager)
        return;
    m_pointerDevice = CXRPointerDevice::create();
    g_pInputManager->newMouse(m_pointerDevice); // -> setupMouse -> attachPointer + destroy listener
    Log::logger->log(Log::DEBUG, "[OPENXR] ray pointer device registered");
}

void COpenXRManager::removePointerDevice() {
    if (!m_pointerDevice)
        return;
    m_pointerDevice->destroy(); // fires m_events.destroy -> InputManager + PointerManager detach
    m_pointerDevice.reset();
    m_curHoveredMonitor.clear();
    Log::logger->log(Log::DEBUG, "[OPENXR] ray pointer device removed");
}

void COpenXRManager::onFrameChannelReadable() {
    // Main thread (single consumer): consume the eventfd counter, then drain the ring to empty.
    uint64_t              v = 0;
    [[maybe_unused]] auto _ = read(m_eventFd, &v, sizeof(v));

    XRQueueItem           item;
    while (m_queue.pop(item)) {
        if (auto* se = std::get_if<SXRStateEvent>(&item))
            dispatchStateEvent(*se);
        else if (auto* ie = std::get_if<SXRInputEvent>(&item))
            dispatchInputEvent(*ie);
    }

    // EXITING / LOSS_PENDING: the frame loop has already exited. Defer stop() out of this
    // fd callback (doLater is main-thread-safe) so we don't remove the event source from
    // within its own dispatch. stop() decides DISABLED vs UNAVAILABLE from m_instanceLost.
    if (m_frameRequestedTeardown.exchange(false) && g_pEventLoopManager)
        g_pEventLoopManager->doLater([this] { stop(); });
}

void COpenXRManager::frameThread() {
    // The frame thread exclusively owns the EGL context while running. It snapshots the
    // layer set once per frame, blits each layer's latest presented buffer into its
    // swapchain, and submits one XrCompositionLayerQuad per layer (doc 01 loop / doc 02).
    eXRManagerState lastReported  = XR_STATE_RUNNING_IDLE;
    int64_t         lastPredicted = 0; // XrTime (ns) of the previous frame, for the solve dt
    int             frameFailStreak = 0; // consecutive xrWaitFrame/xrBeginFrame failures (loss backstop)

    while (m_running.load()) {
        m_session->pollEvents();

        // WP-G5: forward an interaction-profile change to the input system so it re-reads each
        // hand's active device (hands vs controllers) on the next sample().
        if (m_input && m_session->m_interactionProfileChanged) {
            m_session->m_interactionProfileChanged = false;
            m_input->notifyInteractionProfileChanged();
        }

        // report-19: forward a user-presence (don/doff) change to the main thread, where it gates the
        // `visible`-mode monitor plug/unplug edges. Cross via the frame->main queue (SXRStateEvent) —
        // the plug path re-enters onConnect/onDisconnect and touches hyprutils refcounts (forbidden
        // on the frame thread, see XRMonitorLayer.hpp).
        if (m_session->m_userPresenceChanged) {
            m_session->m_userPresenceChanged = false;
            SXRStateEvent ev;
            ev.type = eXRStateEventType::USER_PRESENCE;
            ev.a    = m_session->m_userPresent ? 1 : 0;
            enqueue(ev);
        }

        // Recenter (doc 03 §6): re-express every anchor across a reference-space change.
        if (m_session->m_recenterPending) {
            const OpenXR::SXRPose M      = m_session->m_recenterPose;
            const bool            valid  = m_session->m_recenterPoseValid;
            m_session->m_recenterPending = false;
            if (valid) {
                std::scoped_lock lock(m_layersMu);
                for (auto& l : m_layers)
                    l->m_anchor.onReferenceSpaceChanged(M);
            }
        }

        if (m_session->m_exitRequested) {
            // Signal the main thread to run teardown; the loop exits so the join is instant.
            m_frameRequestedTeardown.store(true);
            wakeMain();
            break;
        }

        const eXRManagerState mapped = mapSessionState((int)m_session->m_xrState);
        if (mapped != lastReported) {
            reportState(mapped);
            lastReported = mapped;
        }

        // Snapshot the layer set: bound + non-pending-removal layers become the active set;
        // pending-removal layers are collected for frame-side teardown (removal barrier
        // step 2). Both under m_layersMu (doc 00 handoff table).
        std::vector<PXRLAYER> active;
        std::vector<PXRLAYER> toRemove;
        {
            std::scoped_lock lock(m_layersMu);
            active.reserve(m_layers.size());
            for (auto& l : m_layers) {
                if (l->m_pendingRemoval.load(std::memory_order_acquire))
                    toRemove.push_back(l);
                else
                    active.push_back(l);
            }
        }

        // Removal barrier step 2: this layer is no longer submitted/blitted. Destroy its
        // frame-side resources (once), DROP our refs, then ack to main. Order matters: main
        // erases the layer on ack, and the last shared_ptr must die on the main thread —
        // ~CXRMonitorLayer releases hyprutils WPs/listeners, which must not be released
        // concurrently with main-thread refcount traffic (see XRMonitorLayer.hpp).
        std::vector<std::string> removedNames;
        for (auto& l : toRemove) {
            if (l->m_removalAcked.exchange(true))
                continue;
            {
                CXRGraphics::CScopedGLContext ctx(*m_graphics);
                l->destroyFrameResourcesGL(*m_graphics);
            }
            l->destroySwapchain(); // context NOT current
            removedNames.push_back(l->m_monitorName);
        }
        toRemove.clear(); // drop refs BEFORE the acks — main may finalize (and erase) immediately
        for (auto& name : removedNames)
            reportLayerRemoved(name);

        // DEVIATION from doc 01's frame-loop pseudocode (which gates the pump on the state
        // already being SYNCHRONIZED/VISIBLE/FOCUSED): the runtime only advances
        // READY -> SYNCHRONIZED -> VISIBLE once the app STARTS submitting frames, so gating
        // the pump on those states deadlocks the session at READY (confirmed against
        // Monado's null compositor). We therefore pump the frame lifecycle as soon as the
        // session is running (m_sessionBegan). xrWaitFrame blocks until the next frame time,
        // so this needs no throttle. Reported to WP13 for a doc-01 amendment.
        if (!m_session->m_sessionBegan) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // WIP idle throttle
            continue;
        }

        const bool visible = m_session->m_xrState == XR_SESSION_STATE_VISIBLE || m_session->m_xrState == XR_SESSION_STATE_FOCUSED;

        // Pacing (doc 02): drive compositor renders of each visible XR monitor at the XR
        // cadence, VISIBLE/FOCUSED only — no scheduleFrame churn while idle. Routed through
        // the frame->main queue: CMonitor::scheduleFrame() lands in aquamarine's idle-callback
        // vector, which the main thread concurrently drains in CBackend::dispatchIdle with no
        // lock — calling it from this thread corrupts the heap.
        if (visible && !active.empty())
            enqueue(SXRStateEvent{.type = eXRStateEventType::SCHEDULE_FRAMES});

        XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState    fs       = {XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(m_session->m_session, &waitInfo, &fs))) {
            // pollEvents at the top of the next iteration normally classifies a dead runtime (it maps
            // the loss to m_exitRequested and the loop breaks). This streak backstop latches loss for
            // any failure code it does NOT classify, so a dead runtime can never make the loop
            // busy-spin forever. The short sleep keeps a transient-failure streak off a CPU core.
            if (++frameFailStreak >= OpenXR::XR_MAX_FRAME_FAIL_STREAK)
                m_session->markRuntimeLost("xrWaitFrame failure streak", 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(m_session->m_session, &beginInfo))) {
            if (++frameFailStreak >= OpenXR::XR_MAX_FRAME_FAIL_STREAK)
                m_session->markRuntimeLost("xrBeginFrame failure streak", 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        frameFailStreak = 0; // a clean wait+begin clears the streak

        // Sample controller/hand actions once per frame (doc 04 §2): xrSyncActions + per-hand
        // aim/grip pose + analog reads at the predicted display time, located in the reference
        // space. Only delivers real input while the session is FOCUSED (a success code otherwise).
        if (m_input)
            m_input->sample(fs.predictedDisplayTime, m_session->m_refSpace);

        // Per-layer: ensure a swapchain, then blit the latest presented buffer into it.
        bool lostInFrame = false; // set if a per-layer xr call reveals a dead/wedged runtime
        for (auto& l : active) {
            // Mode change: recreate the swapchain at the new pixel size (doc 02).
            if (l->m_swapchainDirty.exchange(false, std::memory_order_acq_rel)) {
                Vector2D newSize;
                {
                    std::lock_guard<std::mutex> lk(l->m_bufMu);
                    newSize = l->m_pendingSize;
                }
                if (newSize.x >= 1 && newSize.y >= 1)
                    createLayerSwapchain(*l, newSize);
            }

            // First bind: create a swapchain sized to the monitor's pixel mode, as cached by
            // the main thread at bind/modeChanged (m_pendingSize under m_bufMu). The frame
            // thread must NOT lock() m_monitor — hyprutils refcounts are not atomic and racing
            // the main thread's copies corrupts them (see XRMonitorLayer.hpp).
            if (l->m_swapchain == XR_NULL_HANDLE) {
                Vector2D size;
                {
                    std::lock_guard<std::mutex> lk(l->m_bufMu);
                    size = l->m_pendingSize;
                }
                if (size.x >= 1 && size.y >= 1)
                    createLayerSwapchain(*l, size);
            }

            if (l->m_swapchain == XR_NULL_HANDLE)
                continue; // no swapchain yet (monitor has no mode) — skip this layer

            auto buf = l->takeLatestBuffer();

            // ---- WP-G2 chrome auto-hide fade (frame thread; predicted-display-time deltas, no
            // wall clock). hoverRegion/grabbedNow were written by LAST frame's processPointer
            // plumbing (end of this loop iteration) — a deliberate one-frame latency. activeNow
            // drives the envelope: fade in while the ray hovers/grabs this quad, out after the
            // hide delay. Advanced every frame regardless of whether we redraw. ----
            static auto   PFADEMS    = CConfigValue<Hyprlang::INT>("openxr:chrome_fade_ms");
            static auto   PHIDEMS    = CConfigValue<Hyprlang::INT>("openxr:chrome_hide_delay_ms");
            static auto   PCURSOREN  = CConfigValue<Hyprlang::INT>("openxr:cursor");
            static auto   PCUREPS    = CConfigValue<Hyprlang::FLOAT>("openxr:cursor_redraw_epsilon");
            const uint8_t hoverRegRaw   = l->m_hoverRegion.load(std::memory_order_acquire);
            const bool    grabbedRaw    = l->m_grabbedNow.load(std::memory_order_acquire);
            // research/16 §3.3: fold the gaze-selection state into the chrome so the whole monitor
            // glows as the gaze feedback (a gaze ray has no cursor for pre-grab selection). A gaze
            // CARRY reads like a grab (grab color, whole chrome); a dwell-stable CANDIDATE lights the
            // move-bar in hover color ("look here to grab") when the hand ray isn't already on a region.
            const bool    gazeSel       = l->m_gazeSelected.load(std::memory_order_acquire);
            const bool    gazeCar       = l->m_gazeCarried.load(std::memory_order_acquire);
            uint8_t       hoverReg      = hoverRegRaw;
            bool          grabbedNow    = grabbedRaw;
            if (gazeCar)
                grabbedNow = true;
            else if (gazeSel && hoverReg == OpenXR::XR_REGION_NONE)
                hoverReg = OpenXR::XR_REGION_BAR;
            const bool    activeNow  = grabbedNow || hoverReg != OpenXR::XR_REGION_NONE;
            const bool    chromeOn   = l->m_chrome.hasChrome();
            // report 14 Stage A1: per-hand endpoint cursor. Drawn (like chrome) into the swapchain
            // over content; its packed word was published last frame by processPointer's plumbing.
            static auto    PGAZECUR      = CConfigValue<Hyprlang::INT>("openxr:gaze_cursor");
            const bool     cursorEnabled = *PCURSOREN != 0;
            const bool     gazeCurEnabled = *PGAZECUR != 0;
            const uint32_t curL          = l->m_cursorPacked[0].load(std::memory_order_acquire);
            const uint32_t curR          = l->m_cursorPacked[1].load(std::memory_order_acquire);
            // research/16 §3.3: distinct gaze cursor on the carried monitor (packed by gazeSelectPass).
            const uint32_t curGaze       = gazeCurEnabled ? l->m_gazeCursorPacked.load(std::memory_order_acquire) : 0;
            // Damage dead-band: sub-pixel tremor must NOT force a full-swapchain restore + re-encode
            // every runtime frame while a ray hovers a static desktop (live: dropped-IDR / macroblock
            // storm that clusters in hover windows). A genuine cursor move past the band still redraws;
            // note m_cursorDrawn holds the LAST DRAWN word so slow drift accumulates across frames.
            const float    curEps       = (float)*PCUREPS;
            const bool     cursorChanged = OpenXR::xrCursorRedrawNeeded(l->m_cursorDrawn[0], curL, curEps) ||
                OpenXR::xrCursorRedrawNeeded(l->m_cursorDrawn[1], curR, curEps) ||
                OpenXR::xrCursorRedrawNeeded(l->m_gazeCursorDrawn, curGaze, curEps);
            // Snapshot the clean content whenever chrome OR any cursor may draw over it, so an
            // animation-only frame (chrome fade or a moving cursor with no new desktop buffer) can
            // restore the content before re-drawing the overlay.
            const bool     snapOn = chromeOn || cursorEnabled || gazeCurEnabled;

            const int64_t nowNs = fs.predictedDisplayTime;
            if (activeNow)
                l->m_chromeActiveNs = nowNs;
            float dtSec = 0.f;
            if (l->m_chromeUpdateNs != 0 && nowNs > l->m_chromeUpdateNs)
                dtSec = std::min(0.1f, (float)(nowNs - l->m_chromeUpdateNs) / 1e9f);
            l->m_chromeUpdateNs = nowNs;
            // m_chromeActiveNs == 0 means "never hovered/grabbed yet" -> treat as long-past so the
            // chrome starts (and stays) hidden until the first real interaction.
            const float sinceActiveSec = l->m_chromeActiveNs == 0 ? 1e9f : (float)(nowNs - l->m_chromeActiveNs) / 1e9f;
            const float newAlpha =
                chromeOn ? OpenXR::chromeFadeAdvance(l->m_chromeAlpha, activeNow, dtSec, sinceActiveSec, (float)*PFADEMS / 1000.f, (float)*PHIDEMS / 1000.f) : 0.f;
            l->m_chromeAlpha = newAlpha;

            // A chrome-only (no new desktop buffer) redraw is needed ONLY when the on-screen chrome
            // would actually differ (alpha/region/grab changed) and something is or was visible.
            // This is what keeps a static desktop with hidden chrome at zero GPU cost — the quad
            // re-presents the most recently released image every runtime frame (doc 01).
            const bool chromeVisualChanged = newAlpha != l->m_chromeDrawnAlpha || hoverReg != l->m_chromeDrawnRegion || grabbedNow != l->m_chromeDrawnGrab;
            const bool wantAnimTick        = l->m_hasContent && ((chromeOn && chromeVisualChanged && (newAlpha > 0.f || l->m_chromeDrawnAlpha > 0.f)) || ((cursorEnabled || gazeCurEnabled) && cursorChanged));

            if (!buf && !wantAnimTick && l->m_hasContent)
                continue;

            XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            uint32_t                    imgIdx  = 0;

            // The XR context must stay current from acquire through release (KHR_opengl_es_enable
            // contract): Monado's GL client inserts a native fence into OUR command stream inside
            // xrReleaseSwapchainImage — eglCreateSyncKHR requires a current context, and without
            // one the fence fails every frame and Monado falls back to an unsynchronized path
            // that corrupts the heap in-process.
            CXRGraphics::CScopedGLContext ctx(*m_graphics);

            if (XR_FAILED(xrAcquireSwapchainImage(l->m_swapchain, &acqInfo, &imgIdx))) {
                l->retireBuffer(std::move(buf)); // never let a valid buffer SP die on this thread
                continue;
            }
            // BOUNDED wait — never XR_INFINITE_DURATION. A wedged/dying runtime here would block the
            // frame thread indefinitely, and the main thread's join() in stop() blocks with it, so the
            // compositor stops painting the whole desktop (the 2026-07-14 live-restart freeze class).
            // On a healthy runtime the image is ready almost immediately; anything past the ceiling (or
            // an outright loss code) means the runtime is gone — latch loss, bail the frame, let the
            // main thread tear down to UNAVAILABLE + reprobe.
            XrSwapchainImageWaitInfo waitImg = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitImg.timeout                  = (XrDuration)OpenXR::XR_SWAPCHAIN_WAIT_TIMEOUT_NS;
            const XrResult waitRes           = xrWaitSwapchainImage(l->m_swapchain, &waitImg);
            if (waitRes != XR_SUCCESS) {
                l->retireBuffer(std::move(buf)); // never let a valid buffer SP die on this thread
                m_session->markRuntimeLost("xrWaitSwapchainImage", (int)waitRes);
                lostInFrame = true;
                break;
            }

            if (imgIdx < l->m_swapchainImages.size()) {
                const XR_GLuint dst = l->m_swapchainImages[imgIdx];
                if (buf) {
                    m_graphics->blitBuffer(buf, *l, dst);
                    if (!l->m_hasContent)
                        Log::logger->log(Log::DEBUG, "[OPENXR] first blit landed for XR monitor '{}' ({}x{})", l->m_monitorName, (int)l->m_swapchainSize.x,
                                         (int)l->m_swapchainSize.y);
                    l->m_hasContent = true;
                    // Snapshot the fresh content (WITHOUT chrome/cursor) so an animation-only frame
                    // can restore it into a different acquired image (WP-G2 / report 14). Only when
                    // chrome or the cursor may draw over it — pure overhead otherwise (zero-cost path).
                    if (snapOn)
                        m_graphics->snapshotSwapchain(*l, dst);
                } else if (l->m_hasContent) {
                    // Animation-only frame: no new desktop buffer, chrome fading or the cursor moved —
                    // restore the last content into this (possibly different) image, then re-overlay.
                    m_graphics->restoreSnapshot(*l, dst);
                } else
                    m_graphics->clearTex(dst, l->m_swapchainSize, 0.0f, 0.0f, 0.0f);

                // Chrome pass (WP-G2): draw the move-bar + corner handles into the transparent
                // margin over the content. No-op when disabled or fully faded out; drawChrome never
                // touches the content rect.
                if (chromeOn && l->m_hasContent) {
                    m_graphics->drawChrome(*l, dst, newAlpha, hoverReg, grabbedNow);
                    l->m_chromeDrawnAlpha  = newAlpha;
                    l->m_chromeDrawnRegion = hoverReg;
                    l->m_chromeDrawnGrab   = grabbedNow;
                }

                // Endpoint-cursor pass (report 14 Stage A1): draw each hand's cursor at its ray-hit
                // uv, over content + chrome. Plus the gaze cursor (research/16 §3.3) on the carried
                // monitor. No-op when disabled or no cursor present.
                if ((cursorEnabled || gazeCurEnabled) && l->m_hasContent) {
                    m_graphics->drawCursor(*l, dst, cursorEnabled ? curL : 0, cursorEnabled ? curR : 0, curGaze);
                    l->m_cursorDrawn[0]   = cursorEnabled ? curL : 0;
                    l->m_cursorDrawn[1]   = cursorEnabled ? curR : 0;
                    l->m_gazeCursorDrawn  = curGaze;
                }
            }

            XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(l->m_swapchain, &relInfo);

            // Hand the consumed buffer back for main-thread release — its final SP dec must
            // not happen on this thread (non-atomic refcounts, see XRMonitorLayer.hpp).
            l->retireBuffer(std::move(buf));
        }

        // Runtime revealed dead mid-layer (bounded swapchain wait failed): skip the anchor solve +
        // xrEndFrame (both would be more doomed IPC) and restart the loop — the m_exitRequested check
        // at the top breaks it and wakes the main thread to tear down (UNAVAILABLE + reprobe).
        if (lostInFrame)
            continue;

        // --- anchor solve (WP5) ---
        // Locate the head (VIEW) pose in our reference space at the predicted display time. In the
        // LOCAL fallback (no LOCAL_FLOOR) shift +floor_offset so the solver works in floor-relative
        // coordinates; the final quad pose is shifted back before submission.
        static auto     PFLOOR      = CConfigValue<Hyprlang::FLOAT>("openxr:floor_offset");
        const float     floorOffset = (float)*PFLOOR;
        // WP-G6: 1€ hand-grab carry filter parameters, read per-frame (hot-toggles). Only a hand
        // MOVE grab with grabFilter set is filtered (see CXRAnchor::solve grab override).
        static auto     PGRABFILTER   = CConfigValue<Hyprlang::INT>("openxr:grab_filter");
        static auto     PGRABFILTERMC = CConfigValue<Hyprlang::FLOAT>("openxr:grab_filter_min_cutoff");
        static auto     PGRABFILTERB  = CConfigValue<Hyprlang::FLOAT>("openxr:grab_filter_beta");
        const bool      grabFilter    = *PGRABFILTER != 0;
        // scope=all (default) filters controllers too; anything else (e.g. "hands") = hands only.
        // Read the parsed atomic — NEVER deref openxr:grab_filter_scope (a std::string) on this thread:
        // a concurrent reload rebuilds/frees its backing store -> heap corruption (task #25). Published
        // by publishGrabStringTuning() on the main thread.
        const bool      grabFilterAll = m_grabFilterScopeAll.load(std::memory_order_relaxed);
        const float     grabFilterMc  = (float)*PGRABFILTERMC;
        const float     grabFilterB   = (float)*PGRABFILTERB;
        OpenXR::SXRPose viewPose;
        bool            viewValid = false;
        XrSpaceLocation loc       = {XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(m_session->m_viewSpace, m_session->m_refSpace, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            viewPose  = xrToPose(loc.pose);
            viewValid = true;
            if (!m_session->m_usingLocalFloor)
                viewPose.pos.y += floorOffset;
        }

        // dt from predicted display time deltas (monotone, no wall clock); clamped in solve().
        float dt = 0.f;
        if (lastPredicted != 0 && fs.predictedDisplayTime > lastPredicted)
            dt = (float)(fs.predictedDisplayTime - lastPredicted) / 1e9f;
        lastPredicted = fs.predictedDisplayTime;

        const OpenXR::SXRAnchorTuning tune = readAnchorTuning();

        // Feed the sampled grip poses (in the reference space) into the anchor solve. In the LOCAL
        // fallback the solver works in floor-relative coordinates, so shift grips +floor_offset the
        // same way as the view; the device-lock late-latch path (below) composes that shift back
        // out algebraically, so the grip ActionSpace is still submitted unshifted.
        std::optional<OpenXR::SXRPose> gripLeft, gripRight;
        // WP-G5: hand pinch poses, floor-shifted identically to the grips (same LOCAL-fallback
        // handling) so a pinch-anchored hand grab carries in the same reference frame as everything
        // else. Empty for controller/remote-driver sessions (no pinch pose bound).
        std::optional<OpenXR::SXRPose> pinchLeft, pinchRight;
        if (m_input) {
            if (auto g = m_input->grip(OpenXR::XR_HAND_LEFT)) {
                if (!m_session->m_usingLocalFloor)
                    g->pos.y += floorOffset;
                gripLeft = g;
            }
            if (auto g = m_input->grip(OpenXR::XR_HAND_RIGHT)) {
                if (!m_session->m_usingLocalFloor)
                    g->pos.y += floorOffset;
                gripRight = g;
            }
            if (auto p = m_input->pinch(OpenXR::XR_HAND_LEFT)) {
                if (!m_session->m_usingLocalFloor)
                    p->pos.y += floorOffset;
                pinchLeft = p;
            }
            if (auto p = m_input->pinch(OpenXR::XR_HAND_RIGHT)) {
                if (!m_session->m_usingLocalFloor)
                    p->pos.y += floorOffset;
                pinchRight = p;
            }
        }

        std::vector<OpenXR::SXRSolveResult> results(active.size());
        std::vector<bool>                   solved(active.size(), false);
        bool                                anyRoaming = false; // research/16 Part A: AUTO gate OR-term
        {
            std::scoped_lock lock(m_layersMu);
            m_lastVerbCtx = OpenXR::SXRVerbContext{viewPose, viewValid, gripLeft, gripRight};

            // report-20 issue C: consume a pending recenter-on-plug now that a valid head pose exists.
            // Armed by the main thread on the first plug of a session; the frame thread owns the head
            // pose, so it re-seats every anchor:local monitor to the CURRENT head (yaw-only, floor XZ),
            // reinterpreting each monitor's DECLARED offset as head-relative. Passing the same viewPose
            // to every layer transforms the group rigidly (relative arrangement preserved). Held armed
            // while the view is invalid so a plug during momentary tracking loss still recenters on the
            // next good frame. onReferenceSpaceChanged already ran above (this overrides it for LOCAL).
            if (viewValid && m_recenterArmed.load(std::memory_order_acquire)) {
                m_recenterArmed.store(false, std::memory_order_release);
                for (auto& l : m_layers) {
                    if (l->m_pendingRemoval.load(std::memory_order_acquire))
                        continue;
                    l->m_anchor.recenterLocalToHead(viewPose, l->m_declaredAnchor);
                }
            }

            for (size_t i = 0; i < active.size(); ++i) {
                auto&      l         = active[i];
                // Adaptive anchoring needs the head pose for the geofence even though its persistent
                // mode is LOCAL. When the view is invalid (tracking loss) it falls to the hold-at-
                // lastWorld branch, which freezes the phase machine + envelope (research/13 §4.2).
                const bool needsView = l->m_anchor.state().mode != OpenXR::XR_ANCHOR_LOCAL || l->m_anchor.adaptiveEnabled() || l->m_anchor.gazeGrabbed();
                if (viewValid || !needsView) {
                    OpenXR::SXRSolveInput in;
                    in.view       = viewPose;
                    in.dt         = dt;
                    in.gripLeft   = gripLeft;
                    in.gripRight  = gripRight;
                    in.pinchLeft  = pinchLeft;  // WP-G5: pinch-anchored hand MOVE grabs
                    in.pinchRight = pinchRight;
                    in.grabFilter          = grabFilter; // WP-G6: 1€ carry filter (on by default)
                    in.grabFilterScopeAll  = grabFilterAll; // filter controllers too (scope=all)
                    in.grabFilterMinCutoff = grabFilterMc;
                    in.grabFilterBeta      = grabFilterB;
                    // Aspect from the CONTENT pixel mode (not the chrome-expanded swapchain) so
                    // widthMeters/heightMeters stay CONTENT geometry — `size:` and layout
                    // serialization keep meaning content meters (WP-G1).
                    in.pxW       = (uint32_t)std::max(1.0, l->m_contentSize.x);
                    in.pxH       = (uint32_t)std::max(1.0, l->m_contentSize.y);
                    results[i]   = l->m_anchor.solve(in, tune);
                    solved[i]    = true;

                    // Adaptive anchoring (research/13 §5): publish the phase for status and emit the
                    // terminal dock/undock event exactly once on the ROAMING/DOCKED edge (never the
                    // begin edge, so a reversed/aborted transition emits nothing). Frame thread →
                    // main via the SPSC queue, mirroring the GRAB event path.
                    const auto newPhase = (uint8_t)l->m_anchor.adaptivePhase();
                    if (l->m_anchor.adaptiveEnabled() && newPhase != (uint8_t)OpenXR::XRAD_DOCKED)
                        anyRoaming = true;
                    const auto oldPhase = l->m_adPhase.exchange(newPhase, std::memory_order_acq_rel);
                    if (newPhase != oldPhase && (newPhase == (uint8_t)OpenXR::XRAD_ROAMING || newPhase == (uint8_t)OpenXR::XRAD_DOCKED)) {
                        SXRStateEvent ev;
                        ev.type = eXRStateEventType::ADAPTIVE;
                        ev.a    = newPhase == (uint8_t)OpenXR::XRAD_ROAMING ? 1 : 0;
                        ev.str  = l->m_monitorName;
                        enqueue(ev);
                    }
                } else if (l->m_anchor.hasLastWorld()) {
                    // No head pose this frame: hold the quad at its last composed world pose.
                    results[i].space        = OpenXR::XR_SPACE_LOCAL_FLOOR;
                    results[i].pose         = l->m_anchor.lastWorld();
                    results[i].worldPose    = results[i].pose;
                    results[i].widthMeters  = l->m_anchor.state().widthMeters;
                    results[i].heightMeters = results[i].widthMeters * (float)l->m_contentSize.y / (float)std::max(1.0, l->m_contentSize.x);
                    solved[i]               = true;
                }
            }
        }
        // research/16 Part A: publish "any monitor roaming" for the AUTO hand-input gate (main-thread
        // status reads it too). Plain atomic — never a refcount op.
        m_anyRoaming.store(anyRoaming, std::memory_order_release);

        std::vector<XrCompositionLayerQuad>              quads;
        std::vector<const XrCompositionLayerBaseHeader*> layerPtrs;
        quads.reserve(active.size());
        layerPtrs.reserve(active.size());

        // Composition order: OpenXR composites the layer array in SUBMISSION order (later
        // entries draw on top) — a quad's 3D position plays no part, so ordering must be
        // computed per frame from the solved poses: farthest-from-viewer first, so nearer
        // quads occlude farther ones. m_zOrder remains an explicit override tier; creation
        // seq breaks remaining ties.
        std::vector<size_t> order(active.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (active[a]->m_zOrder != active[b]->m_zOrder)
                return active[a]->m_zOrder < active[b]->m_zOrder;
            if (viewValid && solved[a] && solved[b]) {
                const float da = (results[a].worldPose.pos - viewPose.pos).length();
                const float db = (results[b].worldPose.pos - viewPose.pos).length();
                if (std::fabs(da - db) > 1e-4F)
                    return da > db; // farther composites first (ends up behind)
            }
            return active[a]->m_seq < active[b]->m_seq;
        });

        // Ray-pointer targets: the same visible quads, with their solved world pose expressed in
        // the reference frame the aim poses were sampled in (doc 04 §3). Built alongside the quad
        // array below so the two sets stay in lockstep.
        std::vector<SXRPointerTarget> pointerTargets;
        pointerTargets.reserve(active.size());

        for (size_t i : order) {
            auto& l = active[i];
            if (quads.size() >= m_session->m_maxLayerCount)
                break;
            if (!l->m_quadActive || !l->m_hasContent || !solved[i])
                continue;

            const int w = (int)l->m_swapchainSize.x;
            const int h = (int)l->m_swapchainSize.y;
            if (w <= 0 || h <= 0)
                continue;

            const OpenXR::SXRSolveResult& res = results[i];

            // Map the solver's space selector to a real XrSpace. A device-locked / grabbed quad
            // whose selector is a grip space is submitted against CXRInput's grip ActionSpace with
            // the stored grip-space offset verbatim — the runtime late-latches the controller pose
            // at display time (doc 03 §3.4), so the quad tracks 1:1 with zero added latency. Any
            // other selector (incl. a grip selector with no valid grip space) submits the world
            // pose in the reference space, with the LOCAL-fallback floor shift removed.
            XrSpace deviceSpace = XR_NULL_HANDLE;
            if (m_input) {
                if (res.space == OpenXR::XR_SPACE_GRIP_LEFT || res.space == OpenXR::XR_SPACE_GRIP_RIGHT)
                    deviceSpace = m_input->gripSpace(res.space == OpenXR::XR_SPACE_GRIP_LEFT ? OpenXR::XR_HAND_LEFT : OpenXR::XR_HAND_RIGHT);
                else if (res.space == OpenXR::XR_SPACE_PINCH_LEFT || res.space == OpenXR::XR_SPACE_PINCH_RIGHT)
                    // WP-G5: a pinch-anchored hand MOVE grab late-latches the pinch pose action space.
                    deviceSpace = m_input->pinchSpace(res.space == OpenXR::XR_SPACE_PINCH_LEFT ? OpenXR::XR_HAND_LEFT : OpenXR::XR_HAND_RIGHT);
            }

            XrSpace         quadSpace = m_session->m_refSpace;
            OpenXR::SXRPose quadPose;
            if (deviceSpace != XR_NULL_HANDLE) {
                quadSpace = deviceSpace;
                quadPose  = res.pose; // device-space offset; the grip/pinch ActionSpace is unshifted
            } else {
                quadPose = res.space == OpenXR::XR_SPACE_LOCAL_FLOOR ? res.pose : res.worldPose;
                if (!m_session->m_usingLocalFloor)
                    quadPose.pos.y -= floorOffset; // back to the LOCAL reference frame
            }

            // Chrome margins (WP-G1): the SOLVE poses the CONTENT center (`res` = content meters),
            // but the submitted quad is content + transparent margins. Grow to the full quad meters
            // (contentMeters / contentFrac) and shift the submit pose from the content center to the
            // quad geometric center so the content stays exactly where the anchor placed it — the
            // asymmetric bottom margin (which holds the move-bar) would otherwise drift layouts.
            const OpenXR::SXRChromeGeometry& chrome = l->m_chrome;
            const float                      quadW  = res.widthMeters / (chrome.contentFracW() > 0.f ? chrome.contentFracW() : 1.f);
            const float                      quadH  = res.heightMeters / (chrome.contentFracH() > 0.f ? chrome.contentFracH() : 1.f);
            const OpenXR::SXRPose            quadCenterPose = OpenXR::contentPoseToQuadCenter(quadPose, chrome, quadW, quadH);
            // Cache the full quad meters for next frame's drawCursor (metric cursor sizing, report 14).
            l->m_quadWMeters = quadW;
            l->m_quadHMeters = quadH;

            XrCompositionLayerQuad quad   = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space                    = quadSpace;
            quad.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain       = l->m_swapchain;
            quad.subImage.imageRect       = {{0, 0}, {w, h}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose                     = xrFromPose(quadCenterPose);
            quad.size                     = {quadW, quadH};
            quads.push_back(quad);

            // Ray-pointer target: hit-test against the FULL quad (content + margins) so chrome hits
            // (bar / corners) are classifiable — worldPose is the quad-center world pose in the aim
            // poses' reference frame (undo the LOCAL fallback floor shift, as the ref-frame submit
            // path above does; the chrome offset then goes from content to quad center). `chrome`
            // travels along so processPointer classifies each hit and remaps BODY hits to content
            // uv. `anchor` hands the grab machine (WP8) the live CXRAnchor, valid only for the rest
            // of this frame (backed by `active`, a local PXRLAYER vector). The monitor id comes from
            // the layer's main-thread-written cache — no m_monitor.lock() here (non-atomic
            // refcounts, see XRMonitorLayer.hpp).
            if (const auto MONID = l->m_monitorId.load(std::memory_order_acquire); MONID >= 0) {
                OpenXR::SXRPose worldRef = res.worldPose;
                if (!m_session->m_usingLocalFloor)
                    worldRef.pos.y -= floorOffset;
                SXRPointerTarget pt;
                pt.id        = MONID;
                pt.worldPose = OpenXR::contentPoseToQuadCenter(worldRef, chrome, quadW, quadH);
                pt.w         = quadW;
                pt.h         = quadH;
                pt.name      = l->m_monitorName;
                pt.anchor    = &l->m_anchor;
                pt.chrome    = chrome;
                pointerTargets.push_back(std::move(pt));
            }
        }
        for (auto& q : quads)
            layerPtrs.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));

        // Ray pointer (doc 04 §3-§5) + grab machine (doc 04 §6, WP8): cast every hand's aim ray
        // against the visible quads and emit motion/button/axis/frame/grab across the frame->main
        // queue. Runs after the solve so hit tests use this frame's poses; only produces events
        // while FOCUSED (aim poses are valid then). The grab machine mutates layer anchor state
        // (beginGrab/grabPushPull/grabResize/endGrab), so it runs under m_layersMu — the same
        // discipline as the solve loop above, guarding against a concurrent main-thread verb
        // (move/anchor/scale/...) touching the same CXRAnchor.
        if (m_input) {
            OpenXR::SXRSolveInput pointerSolveIn;
            pointerSolveIn.view       = viewPose;
            pointerSolveIn.dt         = dt;
            pointerSolveIn.gripLeft   = gripLeft;
            pointerSolveIn.gripRight  = gripRight;
            pointerSolveIn.pinchLeft  = pinchLeft; // WP-G5: pinch grab begin/carry/latch device pose
            pointerSolveIn.pinchRight = pinchRight;
            // research/16 Part A: resolve the conditional hand-input gate for this frame (keyboard
            // recency / roam / manual). Reads atomics + the static keyboard signal + numeric config —
            // all frame-thread-safe. Controllers are never affected (handActive is false for them).
            const bool handsEnabled = handInputEnabled();
            std::scoped_lock lock(m_layersMu);
            m_input->processPointer(pointerTargets, (uint32_t)Time::millis(Time::steadyNow()), pointerSolveIn, tune,
                                    (OpenXR::eXRHandGrab)m_handGrabMode.load(std::memory_order_relaxed), (OpenXR::eXRHandGrabAnywhere)m_handGrabAnyMode.load(std::memory_order_relaxed),
                                    handsEnabled);

            // research/16 Part B: gaze-hover selection + gaze-cursor pass. Runs after processPointer so
            // the chrome publish below can OR the gaze-selected highlight, and it reuses the same quad
            // targets. Under m_layersMu (mutates the carried anchor's runtime state is NOT done here —
            // it only reads gazeGrabbed() + publishes atomics; the carry itself is driven by the solve).
            gazeSelectPass(pointerTargets, active, viewPose, viewValid, dt);

            // WP-G2 chrome visual-state plumbing: publish each active quad's current ray-hover
            // region + grab flag onto the layer for the NEXT frame's chrome draw pass (frame
            // thread → frame thread, plain atomics — no processPointer/grab-machine change; the
            // input path only EXPOSES its per-hand region/grab state via read-only accessors).
            for (auto& l : active) {
                const MONITORID mid = l->m_monitorId.load(std::memory_order_acquire);
                const auto reg = mid >= 0 ? m_input->chromeHoverRegion(mid) : OpenXR::XR_REGION_NONE;
                l->m_hoverRegion.store((uint8_t)reg, std::memory_order_release);
                l->m_grabbedNow.store(mid >= 0 && m_input->isMonitorGrabbed(mid), std::memory_order_release);
                // report 14 Stage A1: publish each hand's endpoint-cursor sample onto this layer for
                // next frame's drawCursor (present iff that hand's ray hit THIS monitor). Same plain-
                // atomic frame->frame contract as m_hoverRegion (no hyprutils refcount op).
                for (int hand = 0; hand < 2; ++hand) {
                    const auto h = (OpenXR::eXRHand)hand;
                    uint32_t   packed = 0; // present=false
                    if (mid >= 0 && m_input->cursorMon(h) == mid) {
                        const Vector2D uv = m_input->cursorUV(h);
                        packed            = OpenXR::xrPackCursor(true, m_input->cursorState(h), (float)uv.x, (float)uv.y);
                    }
                    l->m_cursorPacked[hand].store(packed, std::memory_order_release);
                }
            }
        }

        // hypxrvoice WP-V1: append this frame's head pose + gaze candidate to the rolling ring so a
        // voice daemon can resolve deixis ("drop this monitor HERE") against where the head was
        // pointed at speech-onset, not at parse time (VOICE-CONTROL.md). Unconditional (even with
        // no input configured we still log the head pose); reads m_gazeHoveredId + the frame-thread
        // gaze selector state, no config/refcount. Pushes under its own small mutex.
        recordPoseSample(viewPose, viewValid);

        // xrEndFrame with zero layers is valid (nothing composited yet) — doc 01.
        XrFrameEndInfo endInfo       = {XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime          = fs.predictedDisplayTime;
        // Blend mode selected once at session start from openxr:blend_mode (doc 01). On
        // passthrough-capable runtimes ALPHA_BLEND composites quads over the on-device
        // passthrough underlay instead of a black void; the blit forces dst alpha to 1.0 so
        // monitors stay fully opaque against it (XRGraphics.cpp).
        endInfo.environmentBlendMode = m_session->m_blendMode;
        endInfo.layerCount           = (uint32_t)layerPtrs.size();
        endInfo.layers               = layerPtrs.empty() ? nullptr : layerPtrs.data();
        {
            // Monado's layer_commit inserts a fence via context_begin(SYNCHRONIZE), which
            // assumes the app's GL context is current (it never binds for that reason) — same
            // contract as xrReleaseSwapchainImage above.
            CXRGraphics::CScopedGLContext ctx(*m_graphics);
            xrEndFrame(m_session->m_session, &endInfo);
        }
    }
}

bool COpenXRManager::createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size) {
    // Frame thread. Destroy any existing swapchain first (context NOT current — interop rule).
    if (layer.m_swapchain != XR_NULL_HANDLE)
        layer.destroySwapchain();

    // Chrome margins (WP-G1): expand the swapchain by a transparent margin around the content so
    // chrome (bottom move-bar + corner handles) has a place that never covers a desktop pixel.
    // `size` is the monitor's pixel mode = the inner content rect. Derive the normalized layout
    // from CONTENT meters + config, then the full px size from the same fractions — both stored on
    // the layer so the blit (px insets) and the quad-submit/classifier (fractions) share one
    // source. The anchor widthMeters read here is a benign unlocked frame-thread read (cosmetic
    // margin sizing only; content geometry is unaffected), same tolerance as the config reads.
    static auto PENABLED                   = CConfigValue<Hyprlang::INT>("openxr:chrome_enabled");
    static auto PMARGIN                     = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_margin");
    static auto PBARH                       = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_height");
    static auto PBARWF                      = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_width_frac");
    static auto PCORNER                     = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_corner_size");
    // Master toggle: chrome_enabled=0 collapses ALL margins to 0 -> makeChromeGeometry yields a
    // full-quad content rect (hasChrome()==false), so the swapchain is content-only and the draw
    // pass no-ops — zero visual change vs. pre-WP-G1 (the documented disable mechanism, doc §8).
    const bool                      chromeOn = *PENABLED;
    const float                     margin   = chromeOn ? (float)*PMARGIN : 0.f;
    const float                     barH     = chromeOn ? (float)*PBARH : 0.f;
    const float                     cW     = std::max(0.001f, layer.m_anchor.state().widthMeters);
    const float                     cH     = cW * (float)std::max(1.0, size.y) / (float)std::max(1.0, size.x);
    const OpenXR::SXRChromeGeometry chrome = OpenXR::makeChromeGeometry(cW, cH, margin, barH, (float)*PBARWF, (float)*PCORNER);

    // Full swapchain px = content px expanded by the same fractions (px/fraction stay consistent).
    const double   fw = chrome.contentFracW() > 0.0 ? (double)chrome.contentFracW() : 1.0;
    const double   fh = chrome.contentFracH() > 0.0 ? (double)chrome.contentFracH() : 1.0;
    const Vector2D fullSize{std::max(size.x, std::round(size.x / fw)), std::max(size.y, std::round(size.y / fh))};
    const Vector2D contentOffset{std::round(chrome.contentU0 * fullSize.x), std::round(chrome.contentV0 * fullSize.y)};

    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags            = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format                = m_session->m_swapchainFormat;
    info.sampleCount           = 1;
    info.width                 = (uint32_t)fullSize.x;
    info.height                = (uint32_t)fullSize.y;
    info.faceCount             = 1;
    info.arraySize             = 1;
    info.mipCount              = 1;

    // Do NOT bind the context ourselves — Monado's context_begin calls eglMakeCurrent
    // internally; a context already current when it does so crashes AMD gallium (doc 01).
    eglMakeCurrent(m_graphics->m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    XrResult r = xrCreateSwapchain(m_session->m_session, &info, &layer.m_swapchain);
    if (XR_FAILED(r)) {
        Log::logger->log(Log::ERR, "[OPENXR] xrCreateSwapchain for '{}' ({}x{}) failed: {}", layer.m_monitorName, (int)fullSize.x, (int)fullSize.y, (int)r);
        layer.m_swapchain = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(layer.m_swapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageOpenGLESKHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(layer.m_swapchain, imgCount, &imgCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    layer.m_swapchainImages.clear();
    layer.m_swapchainImages.reserve(imgCount);
    for (auto& img : imgs)
        layer.m_swapchainImages.push_back(img.image);

    layer.m_swapchainSize   = fullSize;
    layer.m_contentSize     = size;
    layer.m_contentOffsetPx = contentOffset;
    layer.m_chrome          = chrome;
    layer.m_hasContent      = false;

    // Reset the per-layer CPU staging tex + chrome snapshot so they realloc to the new mode.
    {
        CXRGraphics::CScopedGLContext ctx(*m_graphics);
        m_graphics->destroyLayerGL(layer.m_lastEGLImg, layer.m_cpuTex, layer.m_contentTex);
    }
    layer.m_lastEGLImg     = nullptr;
    layer.m_cpuTex         = 0;
    layer.m_cpuTexSize     = Vector2D{};
    layer.m_contentTex     = 0;
    layer.m_contentTexSize = Vector2D{};
    // Chrome fade must start hidden after a (re)create so a resized/rebound quad does not flash
    // its chrome; the draw-diff trackers reset too so the first real frame draws cleanly.
    layer.m_chromeAlpha       = 0.f;
    layer.m_chromeDrawnAlpha  = 0.f;
    layer.m_chromeDrawnRegion = 0;
    layer.m_chromeDrawnGrab   = false;
    layer.m_chromeUpdateNs    = 0;
    layer.m_chromeActiveNs    = 0;

    Log::logger->log(Log::DEBUG, "[OPENXR] swapchain created for XR monitor '{}': {}x{} (content {}x{} @ +{},+{}), {} images, format 0x{:x}", layer.m_monitorName,
                     (int)fullSize.x, (int)fullSize.y, (int)size.x, (int)size.y, (int)contentOffset.x, (int)contentOffset.y, imgCount,
                     (unsigned long long)m_session->m_swapchainFormat);
    return true;
}

std::expected<PXRLAYER, std::string> COpenXRManager::createXRMonitor(const SXRMonitorParams& params) {
    // Runs on the main thread; works in EVERY manager state (doc 02 — that is what makes lazy
    // binding possible).

    // 1. Validate uniqueness (same checks as dispatchOutput, HyprCtl.cpp).
    if (params.m_name.empty())
        return std::unexpected<std::string>("monitor name must not be empty");
    for (auto const& m : State::monitorState()->allMonitors())
        if (m->m_name == params.m_name) {
            // Name reuse right after `openxr destroy`: the removal barrier is asynchronous (the
            // frame thread must ack before the output is finalized), and status hides
            // pendingRemoval layers early — so a scripted destroy+create of the same name can land
            // in this window. Tell the caller the truth instead of a misleading generic collision.
            {
                std::scoped_lock lock(m_layersMu);
                for (auto& l : m_layers)
                    if (l->m_monitorName == params.m_name && l->m_pendingRemoval.load(std::memory_order_acquire))
                        return std::unexpected<std::string>("that XR monitor is still being destroyed — retry shortly");
            }
            return std::unexpected<std::string>("a monitor already uses that name");
        }
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (l->m_monitorName == params.m_name)
                return std::unexpected<std::string>("an XR monitor with that name already exists");
    }

    // 2. Construct the layer (still unbound) and register it.
    static auto PSIZE      = CConfigValue<Hyprlang::FLOAT>("openxr:default_size");
    const float sizeMeters = params.m_sizeMeters.value_or((float)*PSIZE);
    auto        layer      = std::make_shared<CXRMonitorLayer>(params.m_name, ++m_seqCounter, sizeMeters);

    // Seed the anchoring engine (WP5). widthMeters is the live quad width; height derives from
    // the pixel mode each frame.
    OpenXR::SXRAnchorState st = params.m_anchor;
    st.widthMeters            = sizeMeters;
    // Create verb with no explicit anchor: place along the current gaze at default distance
    // (doc 05 §3.1). Falls back to a sensible default pose when there is no tracking yet.
    if (!params.m_anchorProvided) {
        static auto       PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
        const float       dist     = (float)*PDEFDIST;
        OpenXR::CXRAnchor tmp;
        st.mode = OpenXR::XR_ANCHOR_LOCAL;
        tmp.initFromState(st);
        if (tmp.applyCenter(currentVerbContext(), dist))
            st = tmp.m_state;
        else
            st.anchorPose = OpenXR::SXRPose{OpenXR::Vec3{0.f, 1.4f, -dist}, OpenXR::Quat{}}; // no tracking yet
    }
    layer->m_anchor.initFromState(st);
    layer->m_declaredAnchor = st;
    layer->m_reqResolution  = params.m_resolution;
    layer->m_reqRefresh     = params.m_refreshRate;
    {
        std::scoped_lock lock(m_layersMu);
        m_layers.push_back(layer);
    }

    // 3. Create the headless output (same recipe as dispatchOutput / the WIP).
    bool created = false;
    for (auto const& impl : g_pCompositor->m_aqBackend->getImplementations()) {
        if (impl->type() == Aquamarine::AQ_BACKEND_HEADLESS) {
            impl->createOutput(params.m_name);
            created = true;
            break;
        }
    }
    if (!created) {
        std::scoped_lock lock(m_layersMu);
        std::erase(m_layers, layer);
        return std::unexpected<std::string>("no headless backend available");
    }

    // newOutput -> monitorState()->add() -> CMonitor ctor + onConnect all run synchronously,
    // so the monitor is queryable now.
    auto mon = State::monitorState()->query().name(params.m_name).run();
    if (!mon) {
        std::scoped_lock lock(m_layersMu);
        std::erase(m_layers, layer);
        return std::unexpected<std::string>("headless output did not materialize");
    }

    // report-20 issue A: mark this headless output as XR-plug-managed so the monitor-rule manager's
    // ensureMonitorStatus never re-enables it while we hold it unplugged (the phantom-plug leak: its
    // config rule says "enabled", so an ordinary rule refresh would onConnect() it back). Only
    // XR-created outputs get the flag — an adopted pre-existing monitor keeps its normal lifecycle.
    if (layer->m_createdByXR)
        mon->m_xrManagedPlug = true;

    // 4. Bind: cache the monitor + wire listeners. The onGone callback runs the removal
    //    barrier when the monitor is externally destroyed (path B).
    layer->bindToMonitor(mon, [this, name = params.m_name]() {
        PXRLAYER l;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& cand : m_layers)
                if (cand->m_monitorName == name) {
                    l = cand;
                    break;
                }
        }
        if (!l)
            return;
        if (m_running.load()) {
            // Frame thread is alive: hand off through the barrier (it acks -> finalize).
            l->stopMainListeners();
            l->m_pendingRemoval.store(true, std::memory_order_release);
        } else
            finalizeLayerRemoval(name); // no frame thread — clean up directly
    });

    // 4b. Cross-GPU linear policy — decide it NOW, BEFORE the mode-apply below triggers the output's
    //     first composite. Setting m_forceLinearSwapchain up front means that very first composite (and
    //     every one after) allocates LINEAR buffers, so the presented listener never stashes a
    //     foreign-tiled buffer that the XR GPU can't import. Running it AFTER the mode apply (the old
    //     order) left a window where a tiled buffer was composited + stashed before the flag flipped —
    //     the frame thread would import that stale tiled buffer and the quad went black (live
    //     2026-07-12 "monitor created after a destroy goes blank", intermittent by timing). Needs the
    //     XR EGL node, known only while running; monitors created while stopped get this at
    //     bindExistingLayers() (before the frame thread starts, so no stale stash there either).
    if (m_running.load()) {
        applyCrossGpuLinear(mon);
        layer->m_swapchainDirty.store(true, std::memory_order_release);
    }

    // 5. Apply the requested pixel mode, if any. An explicit user `monitor=` rule matching this name
    //    wins (doc 02 step 5): capture whether the user already set a resolution BEFORE we register our
    //    own declared-mode rule (report-20 issue E), so we never clobber theirs. Then register the
    //    persistent rule (durable across plug/unplug/reload) and apply the effective rule now so the
    //    initial swapchain is the right size (registration only schedules an ensureMonitorStatus pass).
    if (params.m_resolution && Config::monitorRuleMgr()) {
        layer->m_userProvidedMode = Config::monitorRuleMgr()->get(mon).m_resolution != Vector2D{};
        if (layer->m_userProvidedMode)
            Log::logger->log(Log::DEBUG, "[OPENXR] XR monitor '{}' keeps its explicit monitor= resolution", params.m_name);
        registerDeclaredMonitorRule(mon, layer);
        Config::CMonitorRule rule = Config::monitorRuleMgr()->get(mon);
        mon->applyMonitorRule(std::move(rule));
    }

    // 5b. Plugged-state gate (research/18 WP-M1/M3 + report-18 addendum): with
    //     monitors_follow_session (default `visible`), a monitor created while the session is not
    //     usable (no session, or — under `visible` — a doffed/standby session) starts life
    //     UNPLUGGED: created (stable id, mode applied above) then immediately disabled through the
    //     ordinary hotplug path, so a sessionless/doffed desktop never places workspaces on a
    //     display that isn't really there. It plugs in on the next visibility/session edge
    //     (updateMonitorsPlugged()). Consults the same instantaneous predicate as that funnel.
    {
        if (!monitorsShouldBePluggedNow() && mon->m_enabled) {
            Log::logger->log(Log::DEBUG, "[OPENXR] XR monitor '{}' created unplugged (headset not worn / session not usable)", params.m_name);
            mon->onDisconnect();
        }
    }

    // 6. Notify (doc 05 event surface).
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoradded", params.m_name});

    // 7. The cross-GPU linear policy + swapchain-dirty flag were set in step 4b (before the mode
    //    apply). If a session is running the frame thread creates the swapchain on its next pass — it
    //    already snapshots m_layers per frame, so no extra wakeup is needed.

    // 8. Layer cap: a new quad may push the oldest past maxLayerCount (doc 02 recency policy).
    recomputeQuadActive();

    Log::logger->log(Log::DEBUG, "[OPENXR] created XR monitor '{}' (seq {}, size {}m)", params.m_name, layer->m_seq, sizeMeters);
    return layer;
}

void COpenXRManager::destroyXRMonitor(const std::string& name) {
    PXRLAYER layer;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (l->m_monitorName == name) {
                layer = l;
                break;
            }
    }
    if (!layer)
        return;

    if (m_running.load()) {
        // Path A: session running — removal barrier. Stop queueing buffers, flag removal; the
        // frame thread destroys frame-side resources and acks, then the main thread erases the
        // layer + destroys the output (finalizeLayerRemoval).
        layer->stopMainListeners();
        layer->m_pendingRemoval.store(true, std::memory_order_release);
    } else
        finalizeLayerRemoval(name); // no frame thread — direct teardown
}

void COpenXRManager::finalizeLayerRemoval(const std::string& name) {
    // Main thread. Erase the layer, then destroy its output if it still exists.
    PXRLAYER layer;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto it = m_layers.begin(); it != m_layers.end(); ++it) {
            if ((*it)->m_monitorName == name) {
                layer = *it;
                m_layers.erase(it);
                break;
            }
        }
    }
    if (!layer)
        return;

    // Release any queued/retired presented buffers here, on the main thread — their final SP
    // dec must not happen in ~CXRMonitorLayer if that ever ran off-main (belt-and-braces; the
    // frame thread has already dropped its ref by the time it acks, see frameThread()).
    layer->releaseBuffers();

    // No frame thread (direct-teardown path): destroy any lingering frame resources. While
    // DISABLED there should be none, but be defensive.
    if (!m_running.load() && m_graphics) {
        if (m_graphics->m_xrContext != EGL_NO_CONTEXT) {
            CXRGraphics::CScopedGLContext ctx(*m_graphics);
            layer->destroyFrameResourcesGL(*m_graphics);
        }
        layer->destroySwapchain();
    }

    if (auto mon = layer->m_monitor.lock(); mon && mon->m_output)
        destroyOutputDeferred(mon->m_output); // path B (external destroy) already gone -> mon expired, skipped

    // Clear the explicit selection if it pointed at this monitor (doc 05 §3.2).
    if (m_selectedMonitor == name)
        m_selectedMonitor.clear();
    if (m_lastHoveredMonitor == name)
        m_lastHoveredMonitor.clear();
    // research/16 Part B: drop a gaze carry that pointed at this monitor (the anchor is gone with it).
    if (m_gazeCarryMonitor == name)
        m_gazeCarryMonitor.clear();

    // Freeing capacity may re-activate a suspended quad (doc 02 recency policy).
    recomputeQuadActive();

    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorremoved", name});

    Log::logger->log(Log::DEBUG, "[OPENXR] destroyed XR monitor '{}'", name);
}

void COpenXRManager::destroyOutputDeferred(SP<Aquamarine::IOutput> output) {
    // Main thread. Works around an aquamarine headless-output lifetime bug that produced the
    // dispatchIdle -> CSignalBase::emitInternal use-after-free crash during XR monitor teardown.
    //
    // CHeadlessOutput::scheduleFrame() enqueues the output's own `framecb` (an
    // SP<std::function<void()>> that captures the output by raw `this` and holds no liveness guard)
    // into CBackend::idle.pending via addIdleEvent(). Neither CHeadlessOutput::destroy() nor
    // ~CHeadlessOutput removes that pending idle event. If the output is freed while a framecb is
    // still queued, the next CBackend::dispatchIdle() invokes it on freed memory —
    // events.frame.emit() on a dangling CSignal — which is exactly the observed crash
    // (aquamarineFDWrite -> dispatchIdle -> emitInternal). Our XR frame pacing calls
    // CMonitor::scheduleFrame() every runtime frame, so a headless XR output is almost always
    // "due" (the immediate addIdleEvent path in scheduleFrame) when it is torn down during the
    // heavy create/destroy churn of a config reload — hence the crash on most suite runs.
    //
    // We cannot remove the pending idle event (framecb is private to CHeadlessOutput) nor patch the
    // system aquamarine library, so instead we guarantee the output outlives any stale callback.
    // destroy() detaches it from the backend + monitor and (via CMonitor's destroy listener) clears
    // its frame listener, so the pending framecb becomes a harmless listener-less emit. We then keep
    // a reference alive inside a sentinel idle event, enqueued AFTER any pending framecb, which is
    // processed in the same dispatchIdle pass right after that framecb fires — dropping the last
    // reference only once the queue has drained past it.
    if (!output)
        return;

    output->destroy();

    if (!g_pCompositor || !g_pCompositor->m_aqBackend)
        return; // no backend to dispatch idle again (shutdown) — release the reference now.

    auto sentinel = makeShared<std::function<void(void)>>([output]() {}); // capture keeps `output` alive
    g_pCompositor->m_aqBackend->addIdleEvent(sentinel);
}

void COpenXRManager::registerDeclaredMonitorRule(const PHLMONITOR& mon, const PXRLAYER& layer) {
    // report-20 issue E. Make the xrmonitor-declared pixel mode DURABLE by giving the rule manager a
    // persistent named rule for the XR output — otherwise every plug edge's onConnect (and every
    // ensureMonitorStatus refresh) re-derives the mode from a rule manager that has no XR entry and
    // falls back to the headless default (1920x1080@60), silently dropping the declared 2560x1440@90.
    // Precedence: an explicit user `monitor=NAME,<mode>,...` wins — captured once at create as
    // layer->m_userProvidedMode, so we never clobber it. Building the rule from get(mon) preserves any
    // other user-set fields (scale/transform) while we override only the mode. add() replaces our own
    // prior rule by name (idempotent) and schedules an ensureMonitorStatus pass to apply it.
    if (!mon || !layer || !layer->m_reqResolution || layer->m_userProvidedMode || !Config::monitorRuleMgr())
        return;
    Config::CMonitorRule rule = Config::monitorRuleMgr()->get(mon);
    rule.m_name               = mon->m_name;
    rule.m_resolution         = *layer->m_reqResolution;
    if (layer->m_reqRefresh)
        rule.m_refreshRate = *layer->m_reqRefresh;
    // Keep the output ENABLED in the rule — the unplug lifecycle is driven separately through
    // onConnect/onDisconnect + the m_xrManagedPlug guard (issue A), not the rule's disabled bit.
    rule.m_disabled = false;
    Config::monitorRuleMgr()->add(std::move(rule));
}

void COpenXRManager::bindExistingLayers() {
    // Main thread, on start(): layers created while disabled bind to their still-live monitor
    // and get marked dirty; layers whose named monitor disappeared are dropped (doc 02).
    std::vector<std::string> gone;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_monitor.lock())
                continue; // already bound
            auto mon = State::monitorState()->query().name(l->m_monitorName).run();
            if (!mon) {
                gone.push_back(l->m_monitorName);
                continue;
            }
            l->bindToMonitor(mon, [this, name = l->m_monitorName]() {
                PXRLAYER layer;
                {
                    std::scoped_lock lk(m_layersMu);
                    for (auto& cand : m_layers)
                        if (cand->m_monitorName == name) {
                            layer = cand;
                            break;
                        }
                }
                if (!layer)
                    return;
                if (m_running.load()) {
                    layer->stopMainListeners();
                    layer->m_pendingRemoval.store(true, std::memory_order_release);
                } else
                    finalizeLayerRemoval(name);
            });
            // report-20 issue A: keep the XR-plug-managed flag set on the (re)bound output so the rule
            // manager never re-enables it while we hold it unplugged (see createXRMonitor).
            if (l->m_createdByXR)
                mon->m_xrManagedPlug = true;
            l->m_swapchainDirty.store(true, std::memory_order_release);
        }
    }
    for (auto& name : gone)
        finalizeLayerRemoval(name);

    // Cross-GPU linear pass: now that the layers are bound (and EGL is up), decide per output
    // whether its buffers must be linear for the XR GPU to import them.
    {
        std::vector<PHLMONITOR> mons;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers)
                if (auto mon = l->m_monitor.lock())
                    mons.push_back(mon);
        }
        for (auto& mon : mons)
            applyCrossGpuLinear(mon);
    }
}

void COpenXRManager::applyCrossGpuLinear(const PHLMONITOR& mon) {
    // Main thread. Only meaningful once the XR EGL context exists (its DRM node is the thing we
    // compare against). Before a session is up we don't know the runtime GPU, so leave the output
    // native — bindExistingLayers() re-runs this for every layer at start().
    if (!mon || !mon->m_output || !m_graphics)
        return;

    static auto             PFORCE = CConfigValue<std::string>("openxr:force_linear");
    const auto              mode   = OpenXR::parseForceLinearMode(*PFORCE); // main-thread string read (never near the frame thread)
    const auto&             xrNode = m_graphics->selectedRenderNode();

    // Resolve the output's buffer-allocator DRM fd (same source isMultiGPU() uses). Keep its device
    // numbers for the diagnostic log, but decide cross-GPU with DRM::sameGpu below — NOT by comparing
    // device numbers. The allocator fd is a *card* node while the XR EGL fd is a *render* node, and a
    // render node vs a card node of ONE GPU never share a minor, so a numeric compare always
    // mis-reported "cross-GPU" on a single-GPU box (the NVIDIA all-black bug: force-linear engaged,
    // NVIDIA returned BLOCK_LINEAR anyway, and every non-linear frame was dropped → black panel).
    int     allocFD    = -1;
    int64_t allocMajor = -1, allocMinor = -1;
    if (auto backend = mon->m_output->getBackend()) {
        if (auto alloc = backend->preferredAllocator(); alloc && alloc->drmFD() >= 0) {
            allocFD = alloc->drmFD();
            struct stat st;
            if (fstat(allocFD, &st) == 0) {
                allocMajor = (int64_t)major(st.st_rdev);
                allocMinor = (int64_t)minor(st.st_rdev);
            }
        }
    }

    // Compare the PHYSICAL GPUs (drmDevicesEqual, node-type agnostic) rather than the node numbers.
    // Open the XR render node read-only just for the comparison — a render node needs no DRM master,
    // and this runs only on (re)bind, so the transient fd is cheap. Both devices known ⇒ trust the
    // result; either unresolved ⇒ leave native (shared-display fallback, unstat-able fd).
    bool gpusKnown = false, sameGpu = true;
    if (xrNode.valid && !xrNode.path.empty() && allocFD >= 0) {
        Hyprutils::OS::CFileDescriptor xrFD{open(xrNode.path.c_str(), O_RDONLY | O_CLOEXEC)};
        if (xrFD.isValid()) {
            gpusKnown = true;
            sameGpu   = DRM::sameGpu(xrFD.get(), allocFD);
        }
    }

    const bool force = OpenXR::shouldForceLinear(mode, gpusKnown, sameGpu);

    if (mon->m_forceLinearSwapchain == force)
        return; // already in the desired state — no reconfigure churn

    mon->m_forceLinearSwapchain = force;
    // Rebuild the swapchain with the new modifier policy. updateSwapchain() now forces aquamarine's
    // fullReconfigure path on a multigpu flip (a bare flag flip is otherwise swallowed as a no-op /
    // resize that keeps the old tiling — the live 2026-07-12 black-quad root cause), so the buffers
    // are actually re-allocated LINEAR here.
    mon->m_state.updateSwapchain();

    // Report the modifier the (re)allocated buffers ACTUALLY carry — the one thing that decides
    // whether the cross-GPU EGL import can succeed — so a live run confirms LINEAR in one line
    // instead of inferring it from the absence of import-failure spam. Peek + rollback so the
    // render loop's buffer rotation is undisturbed.
    uint64_t negModifier = DRM_FORMAT_MOD_INVALID;
    if (auto& sc = mon->m_output->swapchain) {
        if (auto buf = sc->next(nullptr)) {
            negModifier = buf->dmabuf().modifier;
            sc->rollback();
        }
    }

    Log::logger->log(Log::DEBUG,
                     "[OPENXR] XR monitor '{}' buffers: {} — negotiated modifier 0x{:x} ({}) (force_linear={}, XR drm {}:{}, allocator drm {}:{}, gpus {}, {})", mon->m_name,
                     force ? "LINEAR (cross-GPU import)" : "native tiling", negModifier, NFormatUtils::drmModifierName(negModifier), *PFORCE, xrNode.major, xrNode.minor, allocMajor,
                     allocMinor, gpusKnown ? "known" : "unresolved", gpusKnown ? (sameGpu ? "same GPU" : "cross-GPU") : "assumed same");

    // Force a fresh composite so the newly-allocated buffer is presented promptly (and the XR import
    // flips content: black → dmabuf without waiting for incidental desktop damage).
    if (g_pHyprRenderer)
        g_pHyprRenderer->damageMonitor(mon);
}

void COpenXRManager::teardownLayers() {
    // Main thread, during stop() after the join (frame resources already freed there).
    // Monitor disposition per openxr:destroy_monitors_on_stop.
    static auto                      PDESTROY = CConfigValue<Hyprlang::INT>("openxr:destroy_monitors_on_stop");
    const bool                       destroy  = *PDESTROY;

    std::vector<PXRLAYER> layers;
    {
        std::scoped_lock lock(m_layersMu);
        layers = m_layers;
    }

    for (auto& l : layers) {
        // Release presented-buffer refs on the main thread (no frame thread here — path C).
        l->releaseBuffers();
        if (destroy && l->m_createdByXR) {
            l->stopMainListeners();
            l->m_destroyListener.reset(); // avoid re-entering the removal path from our own destroy
            if (auto mon = l->m_monitor.lock(); mon && mon->m_output)
                destroyOutputDeferred(mon->m_output);
        } else {
            // Keep the output as a plain headless monitor; unbind the layer record so the next
            // start() re-binds it (lazy binding).
            l->m_swapchain = XR_NULL_HANDLE;
            l->m_swapchainImages.clear();
            l->m_hasContent = false;
            l->m_removalAcked.store(false);
            l->m_pendingRemoval.store(false);
        }
    }

    if (destroy) {
        std::scoped_lock lock(m_layersMu);
        std::erase_if(m_layers, [](const PXRLAYER& l) { return l->m_createdByXR; });
    }
}

bool COpenXRManager::sessionExists() const {
    return m_state == XR_STATE_RUNNING_IDLE || m_state == XR_STATE_RUNNING_VISIBLE || m_state == XR_STATE_RUNNING_FOCUSED;
}

bool COpenXRManager::sessionVisible() const {
    return m_state == XR_STATE_RUNNING_VISIBLE || m_state == XR_STATE_RUNNING_FOCUSED;
}

std::string COpenXRManager::monitorFollowModeName() const {
    static auto PFOLLOW = CConfigValue<std::string>("openxr:monitors_follow_session");
    switch (OpenXR::parseMonitorFollowMode(*PFOLLOW)) {
        case OpenXR::XR_FOLLOW_OFF: return "off";
        case OpenXR::XR_FOLLOW_SESSION: return "session";
        case OpenXR::XR_FOLLOW_VISIBLE: return "visible";
    }
    return "visible";
}

int COpenXRManager::monitorUnplugPendingMs() const {
    if (!m_unplugTimer || !m_unplugTimer->armed())
        return -1;
    const float leftUs = m_unplugTimer->leftUs();
    return leftUs <= 0.f ? 0 : (int)(leftUs / 1000.f);
}

void COpenXRManager::armUnplugTimer(int ms) {
    const auto dur = std::chrono::milliseconds(std::max(0, ms));
    if (!m_unplugTimer) {
        m_unplugTimer = makeShared<CEventLoopTimer>(
            dur, [this](SP<CEventLoopTimer> self, void*) { onUnplugGraceExpired(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_unplugTimer);
    } else
        m_unplugTimer->updateTimeout(dur);
}

void COpenXRManager::cancelUnplugTimer() {
    if (m_unplugTimer)
        m_unplugTimer->updateTimeout(std::nullopt); // disarm; keep the object (removed in stop()/dtor)
}

void COpenXRManager::onUnplugGraceExpired() {
    // The grace window elapsed with the headset still doffed/standby. Re-evaluate against the
    // CURRENT state — if presence/visibility returned in the meantime, updateMonitorsPlugged already
    // cancelled us and re-plugged, but re-check defensively so a stale fire can never unplug a
    // still-worn session.
    if (!monitorsShouldBePluggedNow() && m_monitorsPlugged) {
        Log::logger->log(Log::DEBUG, "[OPENXR] monitor unplug grace elapsed (headset doffed/standby) — unplugging XR monitors");
        setMonitorsPlugged(false);
    }
}

int64_t COpenXRManager::visibleSustainedMs() const {
    if (!m_visibleSince)
        return 0;
    const auto elapsed = Time::steadyNow() - *m_visibleSince;
    return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

int COpenXRManager::plugSettleMs() const {
    static auto PSETTLE = CConfigValue<Hyprlang::INT>("openxr:monitor_plug_settle_ms");
    return (int)std::max<int64_t>(0, (int64_t)*PSETTLE);
}

bool COpenXRManager::monitorsShouldBePluggedNow() const {
    // Single source of truth for the plugged target: the pure predicate (mode + session + presence)
    // AND the no-presence first-plug blip guard. Shared by updateMonitorsPlugged(), the timer
    // callbacks, and createXRMonitor()'s per-monitor gate.
    static auto PFOLLOW = CConfigValue<std::string>("openxr:monitors_follow_session");
    const auto  mode    = OpenXR::parseMonitorFollowMode(*PFOLLOW);
    const bool  want    = OpenXR::wantXRMonitorsPlugged(mode, sessionExists(), sessionVisible(), m_userPresenceSupported, m_presenceKnown, m_userPresent);
    if (!want)
        return false;
    // The first-plug settle guard is a VISIBLE-mode concern only — OFF (always) and SESSION
    // (existence) must never be deferred by it. In VISIBLE mode it now applies regardless of presence
    // support (report-20 issue D): the FIRST plug waits for sustained visibility even with presence,
    // since a presence-capable runtime can report 'present' on the session-create blip.
    if (mode != OpenXR::XR_FOLLOW_VISIBLE)
        return true;
    return !OpenXR::xrDeferFirstPlug(m_everPlugged, visibleSustainedMs(), plugSettleMs());
}

void COpenXRManager::armPlugSettleTimer(int ms) {
    const auto dur = std::chrono::milliseconds(std::max(0, ms));
    if (!m_plugSettleTimer) {
        m_plugSettleTimer = makeShared<CEventLoopTimer>(
            dur, [this](SP<CEventLoopTimer> self, void*) { onPlugSettleExpired(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_plugSettleTimer);
    } else
        m_plugSettleTimer->updateTimeout(dur);
}

void COpenXRManager::cancelPlugSettleTimer() {
    if (m_plugSettleTimer)
        m_plugSettleTimer->updateTimeout(std::nullopt); // disarm; keep the object (removed in stop()/dtor)
}

void COpenXRManager::onPlugSettleExpired() {
    // The blip window elapsed with the session still VISIBLE on a no-presence runtime — re-run the
    // funnel so the now-permitted first plug lands (there is no external state edge to trigger it).
    updateMonitorsPlugged(/*allowGrace=*/true);
}

void COpenXRManager::resetPresenceState() {
    // Per-session reset (start()/session end). Presence knowledge and the first-plug bookkeeping do
    // not survive a session — a fresh session must re-earn its plug through presence or the blip gate.
    m_presenceKnown = false;
    m_userPresent   = false;
    m_everPlugged   = false;
    m_visibleSince.reset();
    cancelPlugSettleTimer();
    // report-20 issue C: a fresh session re-earns its recenter-on-plug. The frame thread's armed flag
    // is cleared too so a stale arm from a prior session cannot re-seat the next one.
    m_recenteredThisSession = false;
    m_recenterArmed.store(false, std::memory_order_release);
}

// ---- dormant re-probe (report-17 WP-L3 / report-20 issue B1). Main thread only. ----

void COpenXRManager::maybeArmReprobe() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    static auto PREPROBE = CConfigValue<Hyprlang::INT>("openxr:reprobe");
    // Only re-probe a dormant session the user actually wants enabled. Session loss / a failed start
    // both land here (UNAVAILABLE); a user disable lands in DISABLED and never arms.
    if (m_state != XR_STATE_UNAVAILABLE || !*PENABLED || !*PREPROBE) {
        cancelReprobe(/*resetBackoff=*/true);
        return;
    }

    static auto   PBASE = CConfigValue<Hyprlang::INT>("openxr:reprobe_interval_ms");
    const int64_t base  = std::max<int64_t>(250, (int64_t)*PBASE);
    // Delay policy (pure, gtested — live-evidence bug 1). HEADSET wait (runtime up/reachable, headset
    // undonned — includes WiVRn's degraded pre-don mode) polls at the gentle fixed cadence; growing a
    // backoff there is what produced the observed 30s don dead-air. Recent relevant filesystem
    // activity in the watched dirs also caps the delay at base: the runtime is materializing, and
    // that is precisely when polling hard is right. Only a truly absent, quiet runtime backs off.
    const bool    activity = watchActivityRecent();
    const int64_t ms       = OpenXR::xrReprobeDelayMs(m_probeWait == XR_WAIT_HEADSET, activity, m_reprobeAttempt, base, 30000);
    Log::logger->log(Log::DEBUG, "[OPENXR] dormant — re-probing in {}ms (waiting for {}, attempt {}{})", ms, m_probeWait == XR_WAIT_HEADSET ? "headset" : "runtime", m_reprobeAttempt,
                     activity ? ", fs activity recent" : "");
    armReprobeTimer((int)ms);

    // Event-driven leg (don-the-headset dead-air fix): also inotify-watch the runtime socket dir so a
    // probe fires the instant the socket appears, not after the grown backoff. Idempotent + gated on
    // openxr:reprobe_watch inside. The timer above stays as the fallback.
    setupReprobeWatch();
}

void COpenXRManager::armReprobeTimer(int ms) {
    const auto dur = std::chrono::milliseconds(std::max(0, ms));
    if (!m_reprobeTimer) {
        m_reprobeTimer = makeShared<CEventLoopTimer>(
            dur, [this](SP<CEventLoopTimer> self, void*) { onReprobeExpired(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_reprobeTimer);
    } else
        m_reprobeTimer->updateTimeout(dur);
}

void COpenXRManager::cancelReprobe(bool resetBackoff) {
    if (m_reprobeTimer)
        m_reprobeTimer->updateTimeout(std::nullopt); // disarm; keep the object (removed in dtor)
    // Drop the inotify watch too — it only makes sense while dormant in UNAVAILABLE (event-driven leg).
    teardownReprobeWatch();
    if (resetBackoff) {
        m_reprobeAttempt = 0;
        m_probeWait      = XR_WAIT_NONE;
        // End of the dormant period (real success / user disable): clear the per-period watch
        // bookkeeping. A mere STARTING transition keeps it — the watch fd is torn down and re-armed
        // around every probe, and "did the watch ever fire / did we already log the miss" must span
        // the whole dormant period, not one probe cycle.
        m_watchEverFired    = false;
        m_watchMissLogged   = false;
        m_lastWatchActivity.reset();
    }
}

void COpenXRManager::onReprobeExpired() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    static auto PREPROBE = CConfigValue<Hyprlang::INT>("openxr:reprobe");
    if (m_state != XR_STATE_UNAVAILABLE || !*PENABLED || !*PREPROBE)
        return; // state moved on / disabled since the timer was armed

    // Paranoid silent-miss guard (live-evidence bug 2 hardening): while the watch is armed, every
    // timer-fired probe also stats the known trigger paths. A trigger file existing while the watch
    // never fired this dormant cycle means either the expected pre-don steady state (WiVRn keeps
    // comp_ipc alive in degraded mode — HEADSET wait, log once at DEBUG) or a genuine watch miss
    // (RUNTIME wait believes there is no runtime, yet its socket exists — WARN once so future
    // silent-miss classes are diagnosable from a normal log).
    if (m_watchInotifyFd >= 0 && !m_watchEverFired && !m_watchMissLogged) {
        struct stat st;
        for (const auto& spec : m_watchSpecs) {
            for (const auto& trig : spec.triggerNames) {
                const std::string path = spec.dir + "/" + trig;
                if (stat(path.c_str(), &st) == 0) {
                    Log::logger->log(m_probeWait == XR_WAIT_HEADSET ? Log::DEBUG : Log::WARN,
                                     "[OPENXR] reprobe-watch: {} exists but the watch has not fired this dormant cycle ({})", path,
                                     m_probeWait == XR_WAIT_HEADSET ? "expected: pre-created socket, waiting for the headset" : "possible watch miss — timer probe proceeding");
                    m_watchMissLogged = true;
                    break;
                }
            }
            if (m_watchMissLogged)
                break;
        }
    }

    m_reprobeAttempt++;
    // start() re-attempts from scratch. On success setState() cancels+resets us; on failure it lands
    // back in UNAVAILABLE and setState() re-arms with the grown backoff (or the headset cadence).
    start();
}

std::string COpenXRManager::reprobeWaitString() const {
    if (m_state != XR_STATE_UNAVAILABLE || !m_reprobeTimer || !m_reprobeTimer->armed())
        return "";
    return m_probeWait == XR_WAIT_HEADSET ? "headset" : "runtime";
}

int COpenXRManager::reprobePendingMs() const {
    if (m_state != XR_STATE_UNAVAILABLE || !m_reprobeTimer || !m_reprobeTimer->armed())
        return -1;
    const float leftUs = m_reprobeTimer->leftUs();
    return leftUs <= 0.f ? 0 : (int)(leftUs / 1000.f);
}

bool COpenXRManager::reprobeWatchArmed() const {
    return m_watchInotifyFd >= 0;
}

// ---- event-driven re-probe: inotify watch on the runtime socket dir (don-the-headset dead-air) ----

void COpenXRManager::setupReprobeWatch() {
    static auto PWATCH = CConfigValue<Hyprlang::INT>("openxr:reprobe_watch");
    if (!*PWATCH)
        return;
    if (m_watchInotifyFd >= 0)
        return; // already armed (maybeArmReprobe is idempotent per dormant cycle)
    // Armed in BOTH wait modes (live-evidence bug 2 revision): the HEADSET wait was originally
    // excluded on the theory that nothing new is created on don — false for WiVRn, whose forked
    // compositor server creates/rewrites $XDG_RUNTIME_DIR/wivrn.pid exactly at headset-connect
    // (monado u_process.c pidfile machinery). That pid event IS the don signal; the socket itself
    // never changes (pre-created by the main server and inherited across the fork).

    const char* xdg = getenv("XDG_RUNTIME_DIR");
    m_watchSpecs    = OpenXR::xrReprobeWatchDirs(xdg ? std::string(xdg) : std::string());
    if (m_watchSpecs.empty()) {
        Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: no $XDG_RUNTIME_DIR — timer fallback only");
        return;
    }

    m_watchInotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_watchInotifyFd < 0) {
        Log::logger->log(Log::WARN, "[OPENXR] reprobe-watch: inotify_init1 failed — timer fallback only");
        return;
    }

    if (g_pCompositor && g_pCompositor->m_wlEventLoop) {
        m_watchSource = wl_event_loop_add_fd(
            g_pCompositor->m_wlEventLoop, m_watchInotifyFd, WL_EVENT_READABLE,
            [](int /*fd*/, uint32_t /*mask*/, void* data) -> int {
                static_cast<COpenXRManager*>(data)->onReprobeWatchReadable();
                return 0;
            },
            this);
    }
    if (!m_watchSource) {
        Log::logger->log(Log::WARN, "[OPENXR] reprobe-watch: wl_event_loop_add_fd failed — timer fallback only");
        close(m_watchInotifyFd);
        m_watchInotifyFd = -1;
        return;
    }

    // Add a watch for every spec dir that currently exists. The primary ($XDG_RUNTIME_DIR) always does;
    // the wivrn/ subdir is added here if it already exists, else dynamically when its create event fires.
    // No checkTriggerNow at arm time: a trigger file already present means the just-failed probe saw it
    // too, so an immediate re-probe would only busy-loop; the timer covers that. checkTriggerNow is for
    // the live watch-(re)added-while-things-move races in onReprobeWatchReadable().
    for (const auto& spec : m_watchSpecs)
        addReprobeWatchDir(spec, /*checkTriggerNow=*/false);

    Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch armed on {} ({} dir watch(es))", m_watchSpecs.front().dir, m_watchByWd.size());
}

void COpenXRManager::addReprobeWatchDir(const OpenXR::SXRReprobeWatch& spec, bool checkTriggerNow) {
    if (m_watchInotifyFd < 0)
        return;
    // Skip if this dir is already watched (avoid duplicate wds on re-entry / dynamic subdir add).
    // NOTE: a stale wd for a deleted+recreated dir must be ERASED before re-adding (the IN_IGNORED
    // handler in onReprobeWatchReadable does that) or this dedup would pin the watch to the old inode.
    for (const auto& [wd, s] : m_watchByWd)
        if (s.dir == spec.dir)
            return;

    struct stat st;
    if (stat(spec.dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return; // dir doesn't exist yet — a parent watch will catch it appearing

    // Mask (live-evidence bug 2): IN_CREATE|IN_MOVED_TO for sockets/dirs appearing; IN_MODIFY|
    // IN_CLOSE_WRITE for the pid file being truncated+rewritten on a re-don (pidfile_close leaves the
    // file behind, so the second and later dons of a boot only ever MODIFY it); IN_DELETE_SELF so a
    // deleted/recreated watched dir surfaces as an event (with IN_IGNORED) and the watch is re-added
    // by path instead of following the dead inode.
    const int wd = inotify_add_watch(m_watchInotifyFd, spec.dir.c_str(), IN_CREATE | IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF);
    if (wd < 0) {
        Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: inotify_add_watch({}) failed", spec.dir);
        return;
    }
    m_watchByWd[wd] = spec;

    // Race close (used when a watch is (re)added while the runtime is materializing — a nested dir
    // just appeared, or a watched dir was recreated): a trigger file may have been created between
    // the event that got us here and the add above — stat now and probe if it's already there.
    if (checkTriggerNow) {
        for (const auto& trig : spec.triggerNames) {
            const std::string path = spec.dir + "/" + trig;
            if (stat(path.c_str(), &st) == 0) {
                Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: trigger {} already present on add — probing", path);
                triggerWatchedProbe();
                break;
            }
        }
    }
}

void COpenXRManager::onReprobeWatchReadable() {
    if (m_watchInotifyFd < 0)
        return;
    // Drain all pending inotify events. alignas for the flexible-array struct; NAME_MAX+1 for the name.
    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        const ssize_t n = read(m_watchInotifyFd, buf, sizeof(buf));
        if (n <= 0)
            break; // EAGAIN (drained) or error — done for this wake
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            p += sizeof(struct inotify_event) + ev->len;
            const auto it = m_watchByWd.find(ev->wd);
            if (it == m_watchByWd.end())
                continue;

            // Watch invalidation (live-evidence bug 2 hardening): the watched dir was deleted,
            // unmounted, or the kernel dropped the watch — the wd is dead and, left in the map, its
            // path-dedup entry would block a re-add, silently pinning us to the old inode. Drop it
            // and re-add BY PATH right away (stat-guarded: if the dir is gone the parent watch
            // catches its recreation), probing if a trigger file already exists in the new dir.
            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_UNMOUNT)) {
                const OpenXR::SXRReprobeWatch spec = it->second; // copy — erase invalidates the ref
                m_watchByWd.erase(it);
                if (ev->mask & (IN_DELETE_SELF | IN_UNMOUNT)) // IN_IGNORED accompanies these; skip doubles
                    continue;
                Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: watch on {} invalidated — re-adding by path", spec.dir);
                addReprobeWatchDir(spec, /*checkTriggerNow=*/true);
                continue;
            }

            if (ev->len == 0)
                continue;
            const std::string name(ev->name);
            const auto&       spec = it->second;

            // A nested socket dir (e.g. "wivrn") just appeared: start watching it too, and close the
            // race where its socket landed before we could add the watch (checkTriggerNow=true).
            if (OpenXR::xrReprobeSubdirMatch(spec, name)) {
                markWatchActivity(); // runtime dir materializing — poll at the fast cadence for a while
                for (const auto& sub : m_watchSpecs)
                    if (sub.dir == spec.dir + "/" + name) {
                        Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: nested dir {} appeared — watching it", sub.dir);
                        addReprobeWatchDir(sub, /*checkTriggerNow=*/true);
                        break;
                    }
            }

            // Any named event inside a NESTED watch dir (e.g. wivrn/) is runtime activity by
            // definition — that dir only ever holds runtime artifacts. Root-dir churn ($XDG_RUNTIME_DIR
            // sees wayland/dbus/pipewire traffic constantly) only counts when the name matches, or the
            // activity window would be permanently open and the backoff never grow.
            if (!m_watchSpecs.empty() && spec.dir != m_watchSpecs.front().dir)
                markWatchActivity();

            // A trigger (runtime socket created, or pid file created/rewritten by the forked
            // compositor server at headset-connect) -> probe (debounced).
            if (OpenXR::xrReprobeTriggerMatch(spec, name)) {
                markWatchActivity();
                Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: trigger {}/{} (mask 0x{:x}) — probing soon", spec.dir, name, ev->mask);
                triggerWatchedProbe();
            }
        }
    }
}

void COpenXRManager::triggerWatchedProbe() {
    m_watchEverFired = true; // silent-miss guard bookkeeping (spans the dormant period)
    markWatchActivity();
    // Debounce (XR_REPROBE_WATCH_DEBOUNCE_MS): coalesce a burst of create events AND give the server a
    // beat to start accept()ing before we probe. The one-shot re-arm just pushes the deadline out.
    const auto dur = std::chrono::milliseconds(OpenXR::XR_REPROBE_WATCH_DEBOUNCE_MS);
    if (!m_watchDebounceTimer) {
        m_watchDebounceTimer = makeShared<CEventLoopTimer>(
            dur, [this](SP<CEventLoopTimer> self, void*) { onWatchDebounceExpired(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_watchDebounceTimer);
    } else
        m_watchDebounceTimer->updateTimeout(dur);
}

void COpenXRManager::onWatchDebounceExpired() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    static auto PREPROBE = CConfigValue<Hyprlang::INT>("openxr:reprobe");
    if (m_state != XR_STATE_UNAVAILABLE || !*PENABLED || !*PREPROBE)
        return; // state moved on since the event fired
    // A prior bounded handshake may still be in flight on its helper thread (2026-07-14 freeze audit:
    // the OpenXR loader must never be entered twice concurrently). Don't fire another probe into it —
    // reset the backoff and re-arm the fallback timer at the base cadence instead; by then the helper
    // has usually resolved (and if not, start()'s own guard defers again, with the activity window
    // keeping the cadence fast).
    if (m_handshakeInFlight.load(std::memory_order_acquire)) {
        static auto PBASE = CConfigValue<Hyprlang::INT>("openxr:reprobe_interval_ms");
        m_reprobeAttempt  = 0;
        Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: trigger fired but a handshake is in flight — retrying at base cadence");
        armReprobeTimer((int)std::max<int64_t>(250, (int64_t)*PBASE));
        return;
    }
    // Reset the backoff to the fast end: a trigger just fired, so this is a fresh, promising attempt.
    // If it still fails (server not answering usefully yet) setState(UNAVAILABLE) re-arms the timer at
    // attempt 0 (base ms) — and the recent-activity window in maybeArmReprobe keeps subsequent delays
    // capped at base while the runtime keeps materializing. No 30s dead-air either way.
    m_reprobeAttempt = 0;
    Log::logger->log(Log::DEBUG, "[OPENXR] reprobe-watch: trigger fired — probing now (backoff reset)");
    start();
}

bool COpenXRManager::runtimeSocketPresent() const {
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    struct stat st;
    for (const auto& path : OpenXR::xrRuntimeSocketPaths(xdg ? std::string(xdg) : std::string()))
        if (stat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode))
            return true;
    return false;
}

void COpenXRManager::markWatchActivity() {
    m_lastWatchActivity = Time::steadyNow();
}

bool COpenXRManager::watchActivityRecent() const {
    if (!m_lastWatchActivity)
        return false;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - *m_lastWatchActivity).count();
    return elapsed >= 0 && elapsed < OpenXR::XR_REPROBE_ACTIVITY_WINDOW_MS;
}

void COpenXRManager::teardownReprobeWatch() {
    if (m_watchDebounceTimer)
        m_watchDebounceTimer->updateTimeout(std::nullopt); // disarm; object removed from the loop in dtor
    if (m_watchSource) {
        wl_event_source_remove(m_watchSource);
        m_watchSource = nullptr;
    }
    if (m_watchInotifyFd >= 0) {
        // inotify watch descriptors are freed with the fd; no per-wd inotify_rm_watch needed.
        close(m_watchInotifyFd);
        m_watchInotifyFd = -1;
    }
    m_watchByWd.clear();
    m_watchSpecs.clear();
}

std::string COpenXRManager::visibleStatusString() const {
    if (!sessionExists())
        return "n/a";
    return sessionVisible() ? "yes" : "no";
}

void COpenXRManager::updateMonitorsPlugged(bool allowGrace) {
    // Decision funnel (report-18/19 addenda). Compute the desired plugged state from the mode +
    // session + presence facts, then apply — deferring the plugged->unplugged edge under the anti-flap
    // grace, and the FIRST plug (no-presence fallback) behind the settle window. Main thread only.
    const bool up  = sessionExists();
    const bool vis = sessionVisible();

    // Maintain the visible-since stamp used by the fallback blip guard (edge-driven; this funnel runs
    // on every session-state / presence edge). Cleared the moment visibility (or the session) drops.
    if (up && vis) {
        if (!m_visibleSince)
            m_visibleSince = Time::steadyNow();
    } else
        m_visibleSince.reset();

    if (!up)
        resetPresenceState(); // session gone — forget presence + first-plug bookkeeping

    const bool want      = monitorsShouldBePluggedNow();
    const bool firstPlug = !m_everPlugged;

    if (want) {
        // Donned / session came up (and past any settle): plug immediately, cancel pending timers.
        cancelUnplugTimer();
        cancelPlugSettleTimer();
        // report-20 issue C: on the FIRST (presence/visibility-confirmed) plug of a session, re-seat
        // anchor:local monitors relative to the current head pose. Arm the frame thread (which owns the
        // head pose) BEFORE the plug so it re-seats on its next valid-view frame. A re-plug after a
        // brief doff (firstPlug == false) never re-arms — the head-relative pose from the first don is
        // kept. Gated on openxr:recenter_on_plug.
        if (firstPlug && !m_recenteredThisSession) {
            static auto PRECENTER = CConfigValue<Hyprlang::INT>("openxr:recenter_on_plug");
            if (*PRECENTER) {
                m_recenteredThisSession = true;
                m_recenterArmed.store(true, std::memory_order_release);
                Log::logger->log(Log::DEBUG, "[OPENXR] first plug of session — arming recenter-on-plug (anchor:local monitors re-seat to the current head)");
            }
        }
        setMonitorsPlugged(true);
        m_everPlugged = true;
        return;
    }

    // want == false. In VISIBLE mode this can be *only* because the first-plug settle window has not
    // yet elapsed while the pure predicate (both signals agreeing) is already plug-worthy — arm the
    // settle timer to re-check (there is no other edge coming). Applies with OR without presence
    // support now (report-20 issue D): the settle guards the visibility side of the create-time blip.
    {
        static auto PFOLLOW = CConfigValue<std::string>("openxr:monitors_follow_session");
        const auto  mode    = OpenXR::parseMonitorFollowMode(*PFOLLOW);
        const bool  signalsAgree =
            OpenXR::wantXRMonitorsPlugged(mode, up, vis, m_userPresenceSupported, m_presenceKnown, m_userPresent);
        if (mode == OpenXR::XR_FOLLOW_VISIBLE && firstPlug && !m_monitorsPlugged && signalsAgree &&
            OpenXR::xrDeferFirstPlug(m_everPlugged, visibleSustainedMs(), plugSettleMs())) {
            const int64_t remaining = std::max<int64_t>(0, (int64_t)plugSettleMs() - visibleSustainedMs());
            Log::logger->log(Log::DEBUG, "[OPENXR] plug signals agree — deferring first plug {}ms (session-start visibility blip guard)", remaining);
            armPlugSettleTimer((int)remaining);
            return;
        }
    }

    // If the monitors are already unplugged there is nothing to grace — settle.
    if (!m_monitorsPlugged) {
        cancelUnplugTimer();
        return;
    }

    // Currently plugged, now want unplugged. `want==false && up` is the doffed/standby (or presence-
    // absent) anti-flap case. A vanished session (up==false) or a deliberate reload unplugs now.
    if (allowGrace && up) {
        static auto PGRACE = CConfigValue<Hyprlang::INT>("openxr:monitor_unplug_grace_ms");
        const int   ms     = (int)std::max<int64_t>(0, (int64_t)*PGRACE);
        Log::logger->log(Log::DEBUG, "[OPENXR] headset doffed / no longer present — arming {}ms monitor unplug grace", ms);
        armUnplugTimer(ms);
        return;
    }

    cancelUnplugTimer();
    setMonitorsPlugged(false);
}

std::string COpenXRManager::presenceStatusString() const {
    if (!m_userPresenceSupported)
        return "unsupported";
    if (!m_presenceKnown)
        return "unknown";
    return m_userPresent ? "yes" : "no";
}

void COpenXRManager::setMonitorsPlugged(bool plugged) {
    // research/18 WP-M1/M2 (option b — create-but-disabled): XR-created headless outputs behave
    // like UNPLUGGED external monitors while their session is unusable. Pure applicator: drive
    // every session-following XR monitor to `plugged`. Main thread only — onConnect/onDisconnect
    // are hotplug entry points and CMonitor refs are hyprutils SPs (see the thread rule in
    // XRMonitorLayer.hpp). The plug/unplug DECISION (mode + grace) lives in updateMonitorsPlugged().
    const bool want   = plugged;
    m_monitorsPlugged = plugged;

    // Snapshot under the lock; drive the transitions without it (onConnect/onDisconnect re-enter
    // large parts of the compositor: workspace moves, focus, events).
    std::vector<PXRLAYER> layers;
    {
        std::scoped_lock lock(m_layersMu);
        layers = m_layers;
    }

    for (auto& l : layers) {
        // Adopted pre-existing outputs (m_createdByXR == false) are the user's real monitors —
        // never unplug those; pending-removal layers are mid-barrier and owned by that path.
        if (!l->m_createdByXR || l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        auto mon = l->m_monitor.lock();
        if (!mon || !mon->m_output)
            continue;
        if (mon->m_enabled == want)
            continue;

        Log::logger->log(Log::DEBUG, "[OPENXR] {} XR monitor '{}'", want ? "plugging" : "unplugging", l->m_monitorName);

        // The same two functions the rule manager dispatches for a runtime `monitor=...,disable`
        // flip (MonitorRuleManager::ensureMonitorStatus): onDisconnect() evacuates workspaces to a
        // backup monitor and remembers them by THIS monitor's name; onConnect(true) runs the full
        // name-keyed return (m_lastMonitor tags + remembered-workspace map + workspace-binding
        // rules). onConnect consults the monitor rule itself, so an explicit `monitor=NAME,disable`
        // in the user config still wins over a plug attempt. NEVER destroy the output here — that
        // is the aquamarine headless framecb UAF hot path (destroyOutputDeferred).
        if (want)
            mon->onConnect(true);
        else
            mon->onDisconnect();
    }
}

void COpenXRManager::reportLayerRemoved(const std::string& name) {
    // Frame thread: enqueue the ack and wake the main thread (removal barrier step 2->3).
    SXRStateEvent ev;
    ev.type = eXRStateEventType::LAYER_REMOVED;
    ev.str  = name;
    enqueue(ev);
}

void COpenXRManager::markSwapchainsDirtyIfChromeChanged() {
    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:chrome_enabled");
    static auto PMARGIN  = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_margin");
    static auto PBARH    = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_height");
    static auto PBARWF   = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_bar_width_frac");
    static auto PCORNER  = CConfigValue<Hyprlang::FLOAT>("openxr:chrome_corner_size");

    const std::array<double, 5> cur{(double)*PENABLED, (double)*PMARGIN, (double)*PBARH, (double)*PBARWF, (double)*PCORNER};
    const bool                  changed = !m_lastChromeGeom.has_value() || *m_lastChromeGeom != cur;
    m_lastChromeGeom                     = cur;
    if (!changed)
        return;

    // Force the frame thread to recreate every layer's swapchain, which recomputes m_contentSize/
    // m_contentOffsetPx/m_chrome from the new values (createLayerSwapchain). Without this the
    // margin px + chrome fractions stay frozen at their creation-time values.
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers)
        l->m_swapchainDirty.store(true, std::memory_order_release);
}

void COpenXRManager::onConfigReload() {
    // Re-parse the adaptive string options to enums for the frame thread (main-thread parse). Cheap
    // + unconditional so hot re-tuning (openxr:adaptive_roam_mode / adaptive_transition_ease) applies
    // live on both /reload and the keyword special-cases below.
    publishAdaptiveStringTuning();
    // research/16 Part A: re-apply openxr:hand_input with change detection (a runtime `handinput`
    // dispatcher change survives an unrelated reload; an actual config change wins + clears the latch).
    publishHandInputPolicy();
    // task #25: re-parse hand_grab / hand_grab_anywhere / grab_filter_scope so a hot re-tune applies
    // live AND the frame thread never derefs the backing strings (the corrupted-heap-at-teardown crash).
    publishGrabStringTuning();

    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    const bool  enabled  = *PENABLED;

    // report-17 WP-L7 / report-20 issue B1: also start from UNAVAILABLE, not just DISABLED. Previously
    // `hyprctl keyword openxr:enabled 1` while dormant in UNAVAILABLE (value already 1) was a silent
    // no-op — start()'s own guard accepts UNAVAILABLE, the reload path just never asked. Now a keyword
    // re-assert kicks a fresh attempt immediately (the dormant re-probe timer makes it unnecessary, but
    // this keeps the explicit control working).
    if (enabled && (m_state == XR_STATE_DISABLED || m_state == XR_STATE_UNAVAILABLE))
        start(); // start() reconciles declared monitors itself
    else if (!enabled && m_state != XR_STATE_DISABLED)
        stop();
    else
        reconcileDeclaredMonitors(); // no state edge: still reconcile the declared set

    // Hot-toggle the ray pointer device (doc 04 §8: openxr:pointer = 0 removes it live).
    if (m_running.load()) {
        static auto PPOINTER = CConfigValue<Hyprlang::INT>("openxr:pointer");
        if (*PPOINTER)
            ensurePointerDevice();
        else
            removePointerDevice();

        // WP-G2: pick up hot-edited chrome geometry (margin/bar/corner/enabled) by recreating
        // swapchains when they changed. Colors + fade/hide timings are read per-frame (no recreate).
        markSwapchainsDirtyIfChromeChanged();
    }

    // Re-assert the plugged state (research/18 + report-18 addendum): idempotent, so the normal
    // reload path is a no-op, but this (a) applies a re-tuned openxr:monitors_follow_session mode
    // live, and (b) heals the rare reload where a changed `monitor=` rule made the rule manager
    // re-enable a monitor we hold unplugged (ensureMonitorStatus runs before config.reloaded is
    // emitted, so this listener always has the last word). allowGrace=false: a deliberate reload
    // settles the steady state at once rather than arming a doff-grace.
    updateMonitorsPlugged(/*allowGrace=*/false);

    // Re-run so that toggling openxr:inhibit_idle takes effect immediately (WP9 hook).
    if (g_pInputManager)
        g_pInputManager->recheckIdleInhibitorStatus();
}

// ---------------------------------------------------------------------------------------------
// WP4: reconciliation, verb funnel, selection, layer cap, status/layout serialization.
// ---------------------------------------------------------------------------------------------

namespace {
    // Note: yaw/pitch-from-quat serialization (doc 03 §7) now lives in the pure, unconditional
    // OpenXR::serializeXRMonitorLine (XRMonitorConfig.cpp) so layoutDump() and the gtest
    // round-trip test (tests/xr/parser.cpp) share one implementation.

    OpenXR::eXRAnchorMode parseAnchorModeSpec(const std::string& tok, OpenXR::eXRHand& hand, bool& ok) {
        ok = true;
        if (tok == "local")
            return OpenXR::XR_ANCHOR_LOCAL;
        if (tok == "head")
            return OpenXR::XR_ANCHOR_HEAD;
        if (tok == "body")
            return OpenXR::XR_ANCHOR_BODY;
        if (tok == "device:left") {
            hand = OpenXR::XR_HAND_LEFT;
            return OpenXR::XR_ANCHOR_DEVICE;
        }
        if (tok == "device:right") {
            hand = OpenXR::XR_HAND_RIGHT;
            return OpenXR::XR_ANCHOR_DEVICE;
        }
        ok = false;
        return OpenXR::XR_ANCHOR_LOCAL;
    }
}

void COpenXRManager::publishAdaptiveStringTuning() {
    // MAIN-THREAD ONLY. Parse the two adaptive STRING options to plain enums and publish them as
    // atomics for the frame thread (readAnchorTuning) to read. Never let the frame thread read a
    // string config value: a reload frees/rebuilds the backing value and dangles the cached pointer.
    // Called from start() (before the frame thread launches) and onConfigReload() (hot re-tune).
    // NOTE: must be CConfigValue<std::string> — NOT CConfigValue<Hyprlang::STRING> (const char*).
    // Under the legacy hyprlang manager, string values populate m_hlangp and leave m_p null (the
    // "cursed case" in local__configValuePopulate); only the CConfigValue<std::string> operator*
    // specialization handles that — the generic const char* ptr() derefs the null m_p -> SIGSEGV.
    static auto PADEASE = CConfigValue<std::string>("openxr:adaptive_transition_ease");
    static auto PADROAM = CConfigValue<std::string>("openxr:adaptive_roam_mode");
    m_adEase.store(OpenXR::xrParseEase(*PADEASE), std::memory_order_relaxed);
    m_adRoamMode.store(*PADROAM == "head" ? OpenXR::XR_ANCHOR_HEAD : OpenXR::XR_ANCHOR_BODY, std::memory_order_relaxed);
}

void COpenXRManager::publishGrabStringTuning() {
    // MAIN-THREAD ONLY. Parse the grab STRING options to enums the frame thread reads as atomics.
    // Same rationale + legacy-manager null-m_p caveat as publishAdaptiveStringTuning (must be
    // CConfigValue<std::string>, never <Hyprlang::STRING>). See the atomic decls in the header:
    // the frame thread MUST NOT deref these strings (a reload rebuilds/frees the backing store
    // under it -> heap corruption, task #25). Called from start() + onConfigReload().
    static auto PHANDGRAB    = CConfigValue<std::string>("openxr:hand_grab");
    static auto PHANDGRABANY = CConfigValue<std::string>("openxr:hand_grab_anywhere");
    static auto PGRABFILTSC  = CConfigValue<std::string>("openxr:grab_filter_scope");
    m_handGrabMode.store((uint8_t)OpenXR::xrParseHandGrab(*PHANDGRAB), std::memory_order_relaxed);
    m_handGrabAnyMode.store((uint8_t)OpenXR::xrParseHandGrabAnywhere(*PHANDGRABANY), std::memory_order_relaxed);
    // scope=all (default) filters controllers too; anything else (e.g. "hands") = hands only.
    m_grabFilterScopeAll.store(*PGRABFILTSC != "hands", std::memory_order_relaxed);
}

// ---- conditional hand input (research/16 Part A) ---------------------------------------------

void COpenXRManager::publishHandInputPolicy() {
    // MAIN-THREAD. Re-read openxr:hand_input into m_handPolicy with CHANGE DETECTION: a reload that
    // did NOT alter the config value preserves any runtime `xrmonitor handinput` change; an actual
    // change (or first apply at start()) resets the manual force latch. (CConfigValue<std::string>,
    // never <Hyprlang::STRING> — same legacy-manager null-m_p caveat as publishAdaptiveStringTuning.)
    static auto PHANDIN = CConfigValue<std::string>("openxr:hand_input");
    const std::string v = *PHANDIN;
    if (v == m_handPolicyConfigStr)
        return; // unchanged since last apply -> keep the runtime policy/force as the dispatcher left it
    m_handPolicyConfigStr = v;
    const uint8_t pol = v == "on" ? HANDPOL_ON : (v == "off" ? HANDPOL_OFF : HANDPOL_AUTO);
    m_handPolicy.store(pol, std::memory_order_relaxed);
    m_handForce.store(HANDFORCE_NONE, std::memory_order_relaxed);
}

COpenXRManager::eXRHandInputState COpenXRManager::handInputState() const {
    // Away-from-keyboard: openxr:hand_input_idle_s of physical-keyboard silence.
    static auto    PIDLE     = CConfigValue<Hyprlang::FLOAT>("openxr:hand_input_idle_s");
    static auto    PROAM     = CConfigValue<Hyprlang::INT>("openxr:hand_input_roam_enables");
    const uint64_t lastKeyMs = CInputManager::lastPhysicalKeyEventMs();
    const uint64_t nowMs     = Time::millis(Time::steadyNow());
    const uint64_t idleMs    = (uint64_t)std::max(0.0, (double)*PIDLE * 1000.0);
    const bool     awayFromKbd = lastKeyMs == 0 || (nowMs >= lastKeyMs && (nowMs - lastKeyMs) >= idleMs);
    const bool     roaming     = m_anyRoaming.load(std::memory_order_acquire);
    // The gate DECISION is the pure OpenXR::handInputGate (gtested); map its 4-state to ours 1:1.
    switch (OpenXR::handInputGate(m_handPolicy.load(std::memory_order_relaxed), m_handForce.load(std::memory_order_relaxed), awayFromKbd, roaming, *PROAM != 0)) {
        case OpenXR::XR_HANDGATE_ACTIVE: return HANDIN_ACTIVE;
        case OpenXR::XR_HANDGATE_KBD: return HANDIN_GATED_KBD;
        case OpenXR::XR_HANDGATE_MANUAL: return HANDIN_GATED_MANUAL;
        default: return HANDIN_OFF;
    }
}

COpenXRManager::SXRHandInputStatus COpenXRManager::handInputStatus() const {
    SXRHandInputStatus s;
    switch (m_handPolicy.load(std::memory_order_relaxed)) {
        case HANDPOL_ON: s.mode = "on"; break;
        case HANDPOL_OFF: s.mode = "off"; break;
        default: s.mode = "auto"; break;
    }
    switch (handInputState()) {
        case HANDIN_ACTIVE: s.state = "active"; break;
        case HANDIN_GATED_KBD: s.state = "gated (keyboard)"; break;
        case HANDIN_GATED_MANUAL: s.state = "gated (manual)"; break;
        default: s.state = "off"; break;
    }
    return s;
}

// ---- gaze grab (research/16 Part B) ----------------------------------------------------------

void COpenXRManager::gazeSelectPass(const std::vector<SXRPointerTarget>& targets, const std::vector<PXRLAYER>& active, const OpenXR::SXRPose& view, bool viewValid, float dt) {
    // FRAME THREAD, under m_layersMu (caller holds it). NUMERIC config reads only (frame-thread safe;
    // never a STRING config here — openxr:gaze_source is main-thread-only and v1 is always "view").
    static auto PGFILT  = CConfigValue<Hyprlang::INT>("openxr:gaze_filter");
    static auto PGFMC   = CConfigValue<Hyprlang::FLOAT>("openxr:gaze_filter_min_cutoff");
    static auto PGFB    = CConfigValue<Hyprlang::FLOAT>("openxr:gaze_filter_beta");
    static auto PGDWELL = CConfigValue<Hyprlang::INT>("openxr:gaze_dwell_ms");
    static auto PGHYST  = CConfigValue<Hyprlang::FLOAT>("openxr:gaze_hysteresis_deg");

    // A gaze carry may be active even while the head pose is momentarily invalid; find the carried
    // layer regardless so its m_gazeCarried highlight holds.
    auto publishClear = [&](bool keepCarry) {
        for (auto& l : active) {
            const bool carried = keepCarry && l->m_anchor.gazeGrabbed();
            l->m_gazeSelected.store(false, std::memory_order_release);
            l->m_gazeCarried.store(carried, std::memory_order_release);
            l->m_gazeCursorPacked.store(0, std::memory_order_release);
        }
    };

    if (!viewValid) {
        m_gazeSel.reset();
        m_gazeFilter.reset();
        m_gazeHitId = -1;
        m_gazeHoveredId.store(-1, std::memory_order_release);
        publishClear(/*keepCarry=*/true);
        return;
    }

    // 1€-filter the gaze pose before hit-testing (research/16 §3.1 stage B).
    OpenXR::SXRPose g = view;
    if (*PGFILT != 0 && dt > 0.f)
        g = OpenXR::oneEuroStepPose(m_gazeFilter, view, dt, (float)*PGFMC, (float)*PGFB);
    else
        m_gazeFilter.reset();
    const OpenXR::Vec3 origin = g.pos;
    const OpenXR::Vec3 dir    = OpenXR::poseForward(g.rot);
    const float        hystTan = std::tan(std::clamp((float)*PGHYST, 0.f, 15.f) * (float)M_PI / 180.f);

    // Nearest-hit monitor; the currently-stable target gets extra angular slack (sticky selection).
    int64_t  rawHit = -1;
    float    bestT  = std::numeric_limits<float>::max();
    Vector2D hitUV;
    for (const auto& t : targets) {
        float slack = 0.f;
        if (t.id == m_gazeSel.stable && hystTan > 0.f) {
            const float d = (t.worldPose.pos - origin).length();
            slack         = hystTan * d;
        }
        const OpenXR::SXRQuadHit hit = OpenXR::rayQuadIntersect(t.worldPose, origin, dir, t.w, t.h, slack);
        if (hit.hit && hit.t < bestT) {
            bestT  = hit.t;
            rawHit = t.id;
            hitUV  = Vector2D{std::clamp(hit.u, 0.f, 1.f), std::clamp(hit.v, 0.f, 1.f)};
        }
    }

    const float   dwellSec = (float)std::max(0, (int)*PGDWELL) / 1000.f;
    const int64_t stable   = OpenXR::stepGazeSelect(m_gazeSel, rawHit, dt, dwellSec);
    m_gazeHitId            = rawHit;
    m_gazeHitUV           = hitUV;
    m_gazeHoveredId.store(stable, std::memory_order_release);

    // Publish per-layer highlight + gaze cursor.
    static auto PGCUR = CConfigValue<Hyprlang::INT>("openxr:gaze_cursor");
    const bool  gazeCursorOn = *PGCUR != 0;
    for (auto& l : active) {
        const MONITORID mid     = l->m_monitorId.load(std::memory_order_acquire);
        const bool      carried = l->m_anchor.gazeGrabbed();
        l->m_gazeSelected.store(mid >= 0 && mid == stable, std::memory_order_release);
        l->m_gazeCarried.store(carried, std::memory_order_release);
        // Gaze cursor: only on the carried monitor, at the gaze hit uv (center if the ray isn't on it).
        uint32_t packed = 0;
        if (gazeCursorOn && carried) {
            const Vector2D uv = (mid >= 0 && mid == rawHit) ? hitUV : Vector2D{0.5, 0.5};
            packed            = OpenXR::xrPackCursor(true, OpenXR::XR_CURSOR_GRAB, (float)uv.x, (float)uv.y);
        }
        l->m_gazeCursorPacked.store(packed, std::memory_order_release);
    }
}

OpenXR::SXRAnchorTuning COpenXRManager::readAnchorTuning() const {
    static auto             PRESP    = CConfigValue<Hyprlang::FLOAT>("openxr:leash_response");
    static auto             PANG     = CConfigValue<Hyprlang::FLOAT>("openxr:leash_deadzone_angle");
    static auto             PDIST    = CConfigValue<Hyprlang::FLOAT>("openxr:leash_deadzone_distance");
    static auto             PFOLLOW  = CConfigValue<Hyprlang::INT>("openxr:body_leash_follow_height");
    static auto             PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
    // Adaptive anchoring (research/13 §6.1) — hot-read per frame, same as the leash vars above.
    // THREAD-SAFETY RULE: this runs on the FRAME THREAD (OpenXRManager.cpp frameThread). It may
    // read NUMERIC CConfigValues directly — a config reload can swap the cached pointer under us so
    // we may read a torn number from live memory, but that is a tolerated benign race (a well-formed
    // number, never a crash). It must NEVER read STRING CConfigValues here: a reload can rebuild and
    // free the backing value, so the cached pointer DANGLES and dereferencing it is a use-after-free
    // -> SIGSEGV (crash report 54542). (The original code additionally used CConfigValue<Hyprlang::
    // STRING>, whose generic ptr() derefs the null m_p for legacy-manager strings — see the note in
    // publishAdaptiveStringTuning.) The two string options (openxr:adaptive_transition_ease /
    // adaptive_roam_mode) are therefore parsed to enums on the MAIN thread in
    // publishAdaptiveStringTuning() and read here as atomics — see m_adEase/m_adRoamMode.
    static auto             PADLEAVE   = CConfigValue<Hyprlang::FLOAT>("openxr:adaptive_leave_radius");
    static auto             PADRETURN  = CConfigValue<Hyprlang::FLOAT>("openxr:adaptive_return_radius");
    static auto             PADLDWELL  = CConfigValue<Hyprlang::INT>("openxr:adaptive_leave_dwell_ms");
    static auto             PADRDWELL  = CConfigValue<Hyprlang::INT>("openxr:adaptive_return_dwell_ms");
    static auto             PADTRANS   = CConfigValue<Hyprlang::INT>("openxr:adaptive_transition_ms");
    static auto             PADHEIGHT  = CConfigValue<Hyprlang::INT>("openxr:adaptive_use_height");
    static auto             PADCARRY   = CConfigValue<Hyprlang::INT>("openxr:adaptive_carry_offset");

    OpenXR::SXRAnchorTuning t;
    t.leashResponse    = (float)*PRESP;
    t.deadzoneAngleRad = (float)*PANG * (float)M_PI / 180.f;
    t.deadzoneDistance = (float)*PDIST;
    t.bodyFollowHeight = *PFOLLOW;
    t.defaultDistance  = (float)*PDEFDIST;
    t.adLeaveRadius    = (float)*PADLEAVE;
    t.adReturnRadius   = (float)*PADRETURN;
    t.adLeaveDwell     = (float)*PADLDWELL / 1000.f;
    t.adReturnDwell    = (float)*PADRDWELL / 1000.f;
    t.adTransition     = (float)*PADTRANS / 1000.f;
    // Strings NOT read here (frame thread) — published as enums on the main thread. See rule above.
    t.adEase           = (OpenXR::eXREase)m_adEase.load(std::memory_order_relaxed);
    t.adRoamMode       = (OpenXR::eXRAnchorMode)m_adRoamMode.load(std::memory_order_relaxed);
    t.adUseHeight      = *PADHEIGHT;
    t.adCarryOffset    = *PADCARRY;
    return t;
}

OpenXR::SXRVerbContext COpenXRManager::currentVerbContext() {
    std::scoped_lock lock(m_layersMu);
    return m_lastVerbCtx;
}

PXRLAYER COpenXRManager::layerByName(const std::string& name) {
    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers)
        if (l->m_monitorName == name && !l->m_pendingRemoval.load(std::memory_order_acquire))
            return l;
    return nullptr;
}

PXRLAYER COpenXRManager::resolveSelected() {
    // doc 05 §3.2 resolution order.
    if (!m_selectedMonitor.empty())
        if (auto l = layerByName(m_selectedMonitor))
            return l;
    // 2. Last ray-hovered (WP7 sets m_lastHoveredMonitor).
    if (!m_lastHoveredMonitor.empty())
        if (auto l = layerByName(m_lastHoveredMonitor))
            return l;
    // 3. Focused-if-XR.
    if (Desktop::focusState()) {
        if (auto mon = Desktop::focusState()->monitor())
            if (auto l = layerByName(mon->m_name))
                return l;
    }
    return nullptr;
}

std::string COpenXRManager::selectedName() {
    // GAP 3: the concrete monitor `active` resolves to right now ("" if none), so a status consumer
    // (hypxrvoice) can name the target without replicating the resolution order.
    auto l = resolveSelected();
    return l ? l->m_monitorName : std::string{};
}

void COpenXRManager::reconcileDeclaredMonitors() {
    // doc 05 §2.5: D = declared set (this parse), L = live layers created BY config
    // (m_declaredByConfig). Runtime-created layers are never touched.
    auto cm = Config::Legacy::mgr();
    if (!cm)
        return; // Lua config or no legacy manager: no declared xrmonitor set (v1 = classic only)

    const auto& declared = cm->declaredXRMonitors();

    // Snapshot current declared layer names.
    std::vector<std::string> liveDeclared;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (l->m_declaredByConfig && !l->m_pendingRemoval.load(std::memory_order_acquire))
                liveDeclared.push_back(l->m_monitorName);
    }

    // D \ L -> create (declared);  D ∩ L -> diff (mode change / anchor change).
    for (const auto& d : declared) {
        auto existing = layerByName(d.m_name);
        if (existing && existing->m_declaredByConfig) {
            // D ∩ L: diff. Mode change -> apply new rule (emits modeChanged -> swapchain recreate).
            const bool modeChanged = existing->m_reqResolution != d.m_resolution || existing->m_reqRefresh != d.m_refreshRate;
            if (modeChanged) {
                existing->m_reqResolution = d.m_resolution;
                existing->m_reqRefresh    = d.m_refreshRate;
            }
            if (auto mon = existing->m_monitor.lock()) {
                // report-20 issue E: (re-)register the persistent declared-mode rule every reconcile —
                // a config reparse (reload) clears our rule from the manager, so this re-installs it
                // (idempotent otherwise, and a no-op when the user set their own mode). Then, on an
                // actual declared-mode change, apply the effective rule now so the swapchain recreates.
                registerDeclaredMonitorRule(mon, existing);
                if (modeChanged && d.m_resolution && Config::monitorRuleMgr()) {
                    Config::CMonitorRule rule = Config::monitorRuleMgr()->get(mon);
                    mon->applyMonitorRule(std::move(rule));
                }
            }
            // Anchor / size change -> re-anchor to the new declared state (doc 05 §2.5 / doc 03).
            static auto PSIZE             = CConfigValue<Hyprlang::FLOAT>("openxr:default_size");
            const float wantSize          = d.m_sizeMeters.value_or((float)*PSIZE);
            const bool  anchorChanged     = !(existing->m_declaredAnchor == d.m_anchor);
            const bool  sizeChanged       = existing->m_anchor.state().widthMeters != wantSize;
            const bool  anchorModeChanged = existing->m_declaredAnchor.mode != d.m_anchor.mode || existing->m_declaredAnchor.device != d.m_anchor.device;
            if (anchorChanged || sizeChanged) {
                std::scoped_lock       lock(m_layersMu);
                OpenXR::SXRAnchorState st = d.m_anchor;
                st.widthMeters            = wantSize;
                existing->m_anchor.initFromState(st);
                existing->m_declaredAnchor = st;
                existing->m_sizeMeters     = wantSize;
                if (anchorChanged && g_pEventManager && anchorModeChanged)
                    g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoranchor", d.m_name + "," + OpenXR::anchorModeToString(d.m_anchor)});
            }
            continue;
        }
        if (existing && !existing->m_declaredByConfig) {
            // Name collision with a runtime-created monitor: leave the runtime one alone.
            Log::logger->log(Log::WARN, "[OPENXR] xrmonitor '{}' declared in config but a runtime monitor already owns the name; ignoring the declaration", d.m_name);
            continue;
        }
        // D \ L: create.
        auto res = createXRMonitor(d);
        if (!res) {
            Log::logger->log(Log::WARN, "[OPENXR] failed to create declared xrmonitor '{}': {}", d.m_name, res.error());
            continue;
        }
        res.value()->m_declaredByConfig = true;
    }

    // L \ D: destroy declared layers no longer present in the declared set.
    for (const auto& name : liveDeclared) {
        const bool stillDeclared = std::ranges::any_of(declared, [&](const SXRMonitorParams& p) { return p.m_name == name; });
        if (!stillDeclared)
            destroyXRMonitor(name);
    }
}

void COpenXRManager::recomputeQuadActive() {
    // doc 02 recency policy: the newest maxLayerCount quads stay active; older ones suspend but
    // keep rendering as plain headless outputs. Compute under the lock; post events after.
    const uint32_t                            maxCount = m_session ? m_session->m_maxLayerCount : 16u;

    std::vector<std::pair<std::string, bool>> flips; // name, nowActive
    {
        std::scoped_lock                 lock(m_layersMu);

        std::vector<PXRLAYER> active;
        for (auto& l : m_layers)
            if (!l->m_pendingRemoval.load(std::memory_order_acquire))
                active.push_back(l);

        // Oldest first; suspend everything past the cap counting from the newest.
        std::sort(active.begin(), active.end(), [](const PXRLAYER& a, const PXRLAYER& b) { return a->m_seq < b->m_seq; });

        const size_t n = active.size();
        for (size_t i = 0; i < n; ++i) {
            const bool shouldBeActive = (n <= maxCount) || (i >= n - maxCount);
            if (active[i]->m_quadActive != shouldBeActive) {
                active[i]->m_quadActive = shouldBeActive;
                flips.emplace_back(active[i]->m_monitorName, shouldBeActive);
            }
        }
    }

    for (auto& [name, nowActive] : flips) {
        if (!nowActive)
            Log::logger->log(Log::WARN, "[OPENXR] XR monitor '{}' quad suspended: over the runtime layer cap ({})", name, maxCount);
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorquad", name + (nowActive ? ",1" : ",0")});
    }
}

std::expected<void, std::string> COpenXRManager::cmdCreate(const std::string& args) {
    auto parsed = OpenXR::parseXRMonitorCreateArgs(args);
    if (!parsed.has_value())
        return std::unexpected(parsed.error());

    // Defaults (doc 05 §3.1): mode 1920x1080@60; anchor:local placed along gaze at default
    // distance/size (WP4 keeps WP3's static pose — full placement solve is WP5).
    if (!parsed->m_resolution) {
        parsed->m_resolution = Vector2D{1920, 1080};
        if (!parsed->m_refreshRate)
            parsed->m_refreshRate = 60.f;
    }

    auto res = createXRMonitor(*parsed);
    if (!res)
        return std::unexpected(res.error());
    // Runtime-created: NOT declared, so reload reconciliation never touches it (doc 05 §2.5).
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdDestroy(const std::string& target) {
    std::string name = target;
    if (target == "active" || target.empty()) {
        auto sel = resolveSelected();
        if (!sel)
            return std::unexpected<std::string>("no XR monitor selected");
        name = sel->m_monitorName;
    } else if (!layerByName(name))
        return std::unexpected<std::string>("no XR monitor named '" + name + "'");

    destroyXRMonitor(name);
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdSelect(const std::string& arg) {
    if (arg.empty())
        return std::unexpected<std::string>("select: expected <name|next|prev>");

    if (arg == "next" || arg == "prev") {
        // Cycle in creation order (m_seq).
        std::vector<PXRLAYER> ordered;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers)
                if (!l->m_pendingRemoval.load(std::memory_order_acquire))
                    ordered.push_back(l);
        }
        if (ordered.empty())
            return std::unexpected<std::string>("no XR monitors exist");
        std::sort(ordered.begin(), ordered.end(), [](const PXRLAYER& a, const PXRLAYER& b) { return a->m_seq < b->m_seq; });

        size_t cur = 0;
        for (size_t i = 0; i < ordered.size(); ++i)
            if (ordered[i]->m_monitorName == m_selectedMonitor) {
                cur = i;
                break;
            }
        const size_t next = arg == "next" ? (cur + 1) % ordered.size() : (cur + ordered.size() - 1) % ordered.size();
        m_selectedMonitor = ordered[next]->m_monitorName;
        // GAP 3: fire a socket2 event so a voice daemon's dialogue state is push-driven (no poll).
        if (g_pEventManager)
            g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorselect", m_selectedMonitor});
        return {};
    }

    if (!layerByName(arg))
        return std::unexpected<std::string>("no XR monitor named '" + arg + "'");
    m_selectedMonitor = arg;
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitorselect", m_selectedMonitor});
    return {};
}

std::vector<COpenXRManager::SXRMonitorInfo> COpenXRManager::monitorInfos() {
    std::vector<SXRMonitorInfo> out;
    std::scoped_lock            lock(m_layersMu);
    for (auto& l : m_layers) {
        if (l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        const auto&    st = l->m_anchor.state();
        SXRMonitorInfo info;
        info.name       = l->m_monitorName;
        info.sizeMeters = st.widthMeters;
        info.anchorMode = OpenXR::anchorModeToString(st);
        // doc 05 §4.3: for local the pose is in LOCAL_FLOOR; for leashed/device modes it is the
        // configured offset (pos) + relative rotation (quat) in the leash/device frame. WP8
        // DEVIATION: while grabbed, `m_state.anchorPose` is a frozen snapshot of the
        // pre-grab pose (the grab override lives in a separate m_grabOffset, doc 03 §4.2, and
        // is only folded back into anchorPose on release) — report the live world-composed pose
        // instead so `hyprctl openxr status` tracks the controller during a grab, not a stale
        // value. Not explicitly specified by doc 05 (written before grab existed); this is the
        // more useful behavior for any status-polling consumer, noted for WP13 to fold into the
        // doc.
        // Adaptive: while roaming/transitioning the live world pose (lastWorld) is the follow pose,
        // not the desk pose — report it so a status consumer sees where the monitor actually is
        // (research/13 §6.4); a docked adaptive monitor reports its desk pose like any local one.
        // research/16: a gaze carry also submits a live world pose (grab override), so report it live
        // like a hand grab — the status/layout tracks where the monitor actually is mid-carry.
        const bool            reportLive = l->m_anchor.hasLastWorld() && (l->m_anchor.grabbed() || l->m_anchor.gazeGrabbed() || (l->m_anchor.adaptiveEnabled() && l->m_anchor.adaptivePhase() != OpenXR::XRAD_DOCKED));
        const OpenXR::SXRPose reportPose = reportLive ? l->m_anchor.lastWorld() : st.anchorPose;
        info.posX                        = reportPose.pos.x;
        info.posY                        = reportPose.pos.y;
        info.posZ                        = reportPose.pos.z;
        info.quatX                       = reportPose.rot.x;
        info.quatY                       = reportPose.rot.y;
        info.quatZ                       = reportPose.rot.z;
        info.quatW                       = reportPose.rot.w;
        info.grabbed                     = l->m_anchor.grabbed(); // WP8 drives the grab machine
        switch (l->m_anchor.grabKind()) {                         // WP-G3: move vs resize
            case OpenXR::XR_GRABKIND_MOVE: info.grabKind = "move"; break;
            case OpenXR::XR_GRABKIND_RESIZE: info.grabKind = "resize"; break;
            default: info.grabKind = "none"; break;
        }
        info.hovered                     = l->m_hovered;          // WP7
        info.region                      = OpenXR::xrRegionName((OpenXR::eXRQuadRegion)l->m_hoverRegion.load(std::memory_order_relaxed)); // report 14
        info.contentPath                 = OpenXR::xrContentPathName(l->m_contentPath.load(std::memory_order_relaxed)); // WP-L2
        // Adaptive anchoring (research/13 §6.4).
        info.adaptiveEnabled  = l->m_anchor.adaptiveEnabled();
        info.adaptivePhase    = OpenXR::xrAdaptivePhaseName(l->m_anchor.adaptivePhase());
        info.adaptiveRoamMode = l->m_anchor.adaptiveRoamMode() == OpenXR::XR_ANCHOR_HEAD ? "head" : "body";
        info.adaptiveSeatDist = l->m_anchor.adaptiveSeatDist();
        info.adaptiveT        = l->m_anchor.adaptiveTransitionT();
        if (l->m_reqResolution) {
            info.w = (int)l->m_reqResolution->x;
            info.h = (int)l->m_reqResolution->y;
        }
        if (l->m_reqRefresh)
            info.refresh = *l->m_reqRefresh;
        if (auto mon = l->m_monitor.lock()) {
            info.id      = mon->m_id;
            info.plugged = mon->m_enabled; // research/18: unplugged (disabled) while sessionless
            info.linear  = mon->m_forceLinearSwapchain; // cross-GPU linear-buffer state
            if (info.w == 0) {
                info.w = (int)mon->m_pixelSize.x;
                info.h = (int)mon->m_pixelSize.y;
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::string COpenXRManager::layoutDump() {
    // Paste-ready `xrmonitor = ...` lines (doc 05 §4.2 / doc 03 §7). Reflects the CURRENT live
    // layout, not just the parsed/declared spec:
    //  - anchor:local serializes the anchor's *live solved world pose* (CXRAnchor::lastWorld()),
    //    not the persistent m_state.anchorPose. The two are identical for LOCAL whenever the
    //    monitor isn't mid-grab (solve() sets m_lastWorld = anchorPose verbatim, doc 03 §3.1),
    //    but diverge while grabbed (the solve override composes grip * offset, doc 03 §4.2) —
    //    using lastWorld makes `hyprctl openxr layout` reproduce what the user actually sees
    //    right now even mid-grab, rather than the stale pre-grab pose. Falls back to the stored
    //    state if no solve has run yet (no session ever started for this layer).
    //  - head/body/device serialize the persistent offset (m_state.anchorPose) unchanged: that
    //    offset *is* the config representation for those modes (the live world position it
    //    produces continuously tracks the head/body/grip by design, doc 03 §3.2-3.4, so it is
    //    not itself a meaningful thing to freeze into a config line — only the offset is).
    //    applyMove/rotate/distance/center verbs already mutate this offset directly, and
    //    endGrab() re-derives it on grab release (doc 03 §4.4), so it stays live for those modes
    //    without any extra plumbing here.
    std::string              out;
    std::vector<std::string> lines;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;

            // Mode: prefer the requested mode; fall back to the monitor's current pixel size.
            int   w = 0, h = 0;
            float hz = 0.f;
            if (l->m_reqResolution) {
                w = (int)l->m_reqResolution->x;
                h = (int)l->m_reqResolution->y;
            } else if (auto mon = l->m_monitor.lock()) {
                w = (int)mon->m_pixelSize.x;
                h = (int)mon->m_pixelSize.y;
            }
            if (l->m_reqRefresh)
                hz = *l->m_reqRefresh;

            const auto& st = l->m_anchor.state();
            // Live substitution for LOCAL only (see comment above). EXCEPTION: an adaptive monitor
            // serializes its SAVED dock pose (m_state.anchorPose), never the live roam pose, so a
            // save-while-roaming round-trips to the desk pose — the persistent identity (research/13
            // §6.4). lastWorld while roaming is the follow pose, which must NOT be persisted.
            const bool            useLive  = st.mode == OpenXR::XR_ANCHOR_LOCAL && !st.adaptive.enabled && l->m_anchor.hasLastWorld();
            const OpenXR::SXRPose livePose = useLive ? l->m_anchor.lastWorld() : st.anchorPose;

            lines.push_back(OpenXR::serializeXRMonitorLine(l->m_monitorName, Vector2D{(double)w, (double)h}, hz > 0.f ? std::optional<float>(hz) : std::nullopt, st, livePose,
                                                             st.widthMeters));
        }
    }
    for (auto& ln : lines)
        out += ln + "\n";
    return out;
}

// ---------------------------------------------------------------------------------------------
// WP5: pose-mutation verbs. One implementation, two transports (dispatcher + hyprctl). Each runs
// on the main thread, captures the frame-thread solve context, then mutates the layer's anchor
// under m_layersMu (doc 03 §5 / doc 05 §3.1).
// ---------------------------------------------------------------------------------------------

namespace {
    std::vector<std::string> splitWs(const std::string& s) {
        std::vector<std::string> out;
        size_t                   i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace((unsigned char)s[i]))
                ++i;
            size_t b = i;
            while (i < s.size() && !std::isspace((unsigned char)s[i]))
                ++i;
            if (i > b)
                out.push_back(s.substr(b, i - b));
        }
        return out;
    }

    std::optional<float> parseFloatArg(const std::string& s) {
        try {
            size_t idx = 0;
            float  v   = std::stof(s, &idx);
            if (idx != s.size())
                return std::nullopt;
            return v;
        } catch (...) { return std::nullopt; }
    }

    std::optional<OpenXR::Vec3> parseVec3Arg(const std::string& s) {
        std::vector<std::string> parts;
        size_t                   start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == ',') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.size() != 3)
            return std::nullopt;
        auto x = parseFloatArg(parts[0]);
        auto y = parseFloatArg(parts[1]);
        auto z = parseFloatArg(parts[2]);
        if (!x || !y || !z)
            return std::nullopt;
        return OpenXR::Vec3{*x, *y, *z};
    }
}

std::expected<void, std::string> COpenXRManager::cmdAnchor(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() < 2)
        return std::unexpected<std::string>("anchor: expected <name|active> <local|head|body|device:left|right> [offset:x,y,z]");

    const std::string   target = tokens[0];
    PXRLAYER layer  = target == "active" ? resolveSelected() : layerByName(target);
    if (!layer)
        return std::unexpected<std::string>(target == "active" ? "no XR monitor selected" : "no XR monitor named '" + target + "'");

    bool                  modeOk  = false;
    OpenXR::eXRHand       hand    = OpenXR::XR_HAND_LEFT;
    OpenXR::eXRAnchorMode newMode = parseAnchorModeSpec(tokens[1], hand, modeOk);
    if (!modeOk)
        return std::unexpected<std::string>("anchor: unknown mode '" + tokens[1] + "' (expected local|head|body|device:left|right)");

    bool         hasOffset = false;
    OpenXR::Vec3 offset;
    for (size_t i = 2; i < tokens.size(); ++i) {
        if (tokens[i].starts_with("offset:")) {
            auto v = parseVec3Arg(tokens[i].substr(7));
            if (!v)
                return std::unexpected<std::string>("anchor: bad offset '" + tokens[i] + "' (expected offset:x,y,z)");
            offset    = *v;
            hasOffset = true;
        } else
            return std::unexpected<std::string>("anchor: unexpected token '" + tokens[i] + "'");
    }

    const auto ctx  = currentVerbContext();
    const auto tune = readAnchorTuning();

    bool       modeChanged = false;
    {
        std::scoped_lock lock(m_layersMu);
        const auto&      before = layer->m_anchor.state();
        modeChanged             = before.mode != newMode || (newMode == OpenXR::XR_ANCHOR_DEVICE && before.device != hand);

        if (hasOffset) {
            OpenXR::SXRAnchorState st = layer->m_anchor.state(); // keep width
            st.mode                   = newMode;
            if (newMode == OpenXR::XR_ANCHOR_DEVICE)
                st.device = hand;
            st.anchorPose.pos = offset;
            st.anchorPose.rot = OpenXR::Quat{};
            layer->m_anchor.initFromState(st);
        } else if (!layer->m_anchor.setMode(newMode, hand, ctx, tune))
            return std::unexpected<std::string>("anchor: cannot re-anchor to that mode (head/body need head tracking, device needs a tracked controller)");
    }

    if (modeChanged && g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoranchor", layer->m_monitorName + "," + OpenXR::anchorModeToString(newMode, hand)});
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdMove(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 3)
        return std::unexpected<std::string>("move: expected <dx> <dy> <dz> (meters)");
    auto dx = parseFloatArg(tokens[0]);
    auto dy = parseFloatArg(tokens[1]);
    auto dz = parseFloatArg(tokens[2]);
    if (!dx || !dy || !dz)
        return std::unexpected<std::string>("move: arguments must be numbers");

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    const auto       ctx = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyMove(OpenXR::Vec3{*dx, *dy, *dz}, ctx))
        return std::unexpected<std::string>("move: head tracking (or controller for device anchors) unavailable");
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdRotate(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.empty() || tokens.size() > 2)
        return std::unexpected<std::string>("rotate: expected <dyaw> [dpitch] (degrees)");
    auto yaw = parseFloatArg(tokens[0]);
    if (!yaw)
        return std::unexpected<std::string>("rotate: dyaw must be a number");
    float pitch = 0.f;
    if (tokens.size() == 2) {
        auto p = parseFloatArg(tokens[1]);
        if (!p)
            return std::unexpected<std::string>("rotate: dpitch must be a number");
        pitch = *p;
    }
    // Pitch clamped to ±85° (doc 05 §3.1).
    pitch = std::clamp(pitch, -85.f, 85.f);

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    const auto       ctx     = currentVerbContext();
    constexpr float  DEG2RAD = (float)M_PI / 180.f;
    std::scoped_lock lock(m_layersMu);
    layer->m_anchor.applyRotate(*yaw * DEG2RAD, pitch * DEG2RAD, ctx);
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdScale(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("scale: expected <f|+d|-d>");
    const std::string& a = tokens[0];
    // Explicitly signed => additive delta in meters; bare number => multiplicative factor.
    const bool isDelta = a.front() == '+' || a.front() == '-';
    auto       f       = parseFloatArg(a);
    if (!f)
        return std::unexpected<std::string>("scale: argument must be a number");

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    std::scoped_lock lock(m_layersMu);
    layer->m_anchor.applyScale(isDelta, *f);
    layer->m_sizeMeters = layer->m_anchor.state().widthMeters;
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdDistance(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("distance: expected <±m>");
    auto dm = parseFloatArg(tokens[0]);
    if (!dm)
        return std::unexpected<std::string>("distance: argument must be a number");

    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    const auto       ctx = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyDistance(*dm, ctx))
        return std::unexpected<std::string>("distance: head tracking unavailable or no current pose");
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdCenter() {
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    static auto      PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
    const auto       ctx      = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyCenter(ctx, (float)*PDEFDIST))
        return std::unexpected<std::string>("center: head tracking unavailable");
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdPlace(const std::string& args) {
    // hypxrvoice GAP 2. Syntax: <name|active> at <x>,<y>,<z>  (the "at" keyword is optional/lenient).
    const auto tokens = splitWs(args);
    if (tokens.size() < 2)
        return std::unexpected<std::string>("place: expected <name|active> at <x>,<y>,<z>");

    const std::string target = tokens[0];
    PXRLAYER          layer  = target == "active" ? resolveSelected() : layerByName(target);
    if (!layer)
        return std::unexpected<std::string>(target == "active" ? "no XR monitor selected" : "no XR monitor named '" + target + "'");

    const size_t ci = tokens[1] == "at" ? 2 : 1; // skip the optional "at"
    if (ci >= tokens.size())
        return std::unexpected<std::string>("place: expected <x>,<y>,<z> after 'at'");
    if (ci + 1 != tokens.size())
        return std::unexpected<std::string>("place: unexpected token '" + tokens[ci + 1] + "' (expected a single x,y,z point)");
    auto pt = parseVec3Arg(tokens[ci]);
    if (!pt)
        return std::unexpected<std::string>("place: bad point '" + tokens[ci] + "' (expected x,y,z meters in LOCAL_FLOOR)");

    const auto ctx = currentVerbContext();
    {
        std::scoped_lock lock(m_layersMu);
        // Face the headset by default (like `center`); with no head tracking, keep the quad's current
        // facing (its live world rot if placed, else its stored anchor rot). placeAtFacing is pure.
        const OpenXR::Quat curRot = layer->m_anchor.hasLastWorld() ? layer->m_anchor.lastWorld().rot : layer->m_anchor.state().anchorPose.rot;
        const OpenXR::SXRPose W   = OpenXR::placeAtFacing(*pt, ctx.view.pos, ctx.viewValid, curRot);
        layer->m_anchor.placeLocalAt(W);
    }

    // Placing re-anchors to local: fire the same event as an anchor-mode change so a status consumer
    // (and any bar) sees the mode transition.
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrmonitoranchor", layer->m_monitorName + ",local"});
    return {};
}

// ---- adaptive anchoring verbs (research/13 §6.3) --------------------------------------------

std::expected<void, std::string> COpenXRManager::cmdAdaptive(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("adaptive: expected on|off|toggle");
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    std::scoped_lock lock(m_layersMu);
    // Adaptive decorates an anchor:local desk pose (the persistent identity); it is nonsensical on a
    // leash/device-anchored monitor.
    if (layer->m_anchor.state().mode != OpenXR::XR_ANCHOR_LOCAL)
        return std::unexpected<std::string>("adaptive: only valid on an anchor:local monitor");

    const std::string& a = tokens[0];
    bool               en;
    if (a == "on")
        en = true;
    else if (a == "off")
        en = false;
    else if (a == "toggle")
        en = !layer->m_anchor.adaptiveEnabled();
    else
        return std::unexpected<std::string>("adaptive: expected on|off|toggle");

    layer->m_anchor.adaptiveSetEnabled(en);
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdDock(const std::string& args) {
    const auto tokens = splitWs(args);
    const bool here   = tokens.size() == 1 && (tokens[0] == "here" || tokens[0] == "-here");
    if (!tokens.empty() && !here)
        return std::unexpected<std::string>("dock: expected no arg or 'here'");
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.adaptiveEnabled())
        return std::unexpected<std::string>("dock: adaptive anchoring is not enabled on this monitor");
    if (here)
        layer->m_anchor.adaptiveDockHere();
    else
        layer->m_anchor.adaptiveForceDock();
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdUndock() {
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.adaptiveEnabled())
        return std::unexpected<std::string>("undock: adaptive anchoring is not enabled on this monitor");
    layer->m_anchor.adaptiveForceUndock();
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdRoam(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1 || (tokens[0] != "head" && tokens[0] != "body"))
        return std::unexpected<std::string>("roam: expected head|body");
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    std::scoped_lock lock(m_layersMu);
    layer->m_anchor.adaptiveSetRoamMode(tokens[0] == "head" ? OpenXR::XR_ANCHOR_HEAD : OpenXR::XR_ANCHOR_BODY);
    return {};
}

// ---- gaze grab + conditional hand input verbs (research/16) ----------------------------------

// Shared begin-a-gaze-carry tail (research/16 §3.2), used by both the argument-less dwell grab and
// the targeted `gazegrab <name>` (hypxrvoice GAP 1). The layer is resolved by the caller (from the
// live dwell id, or by name on the MAIN thread — only the resolved layer crosses no thread here).
std::expected<void, std::string> COpenXRManager::beginGazeCarry(PXRLAYER layer) {
    if (!layer)
        return std::unexpected<std::string>("gazegrab: monitor is gone");
    // Idempotent: a begin on the monitor already being carried is a no-op success (deterministic for
    // a voice executor that may re-issue). A begin on a DIFFERENT monitor errors — only one gaze
    // carry exists at a time (m_gazeCarryMonitor); the daemon must gazerelease first.
    if (m_gazeCarryMonitor == layer->m_monitorName)
        return {};
    if (!m_gazeCarryMonitor.empty())
        return std::unexpected<std::string>("gazegrab: already carrying '" + m_gazeCarryMonitor + "' (release it first)");

    const auto ctx = currentVerbContext();
    if (!ctx.viewValid)
        return std::unexpected<std::string>("gazegrab: head tracking unavailable");

    static auto      PFOLLOW = CConfigValue<Hyprlang::INT>("openxr:gaze_follow");
    std::scoped_lock lock(m_layersMu);
    // Mutual exclusion (§4.6): a hand/controller grab owns it -> the gaze grab no-ops with a message.
    if (layer->m_anchor.grabbed())
        return std::unexpected<std::string>("gazegrab: that monitor is already grabbed by a hand/controller");
    if (!layer->m_anchor.hasLastWorld())
        return std::unexpected<std::string>("gazegrab: monitor not placed yet");
    layer->m_anchor.beginGazeGrab(ctx.view, *PFOLLOW != 0);
    m_gazeCarryMonitor = layer->m_monitorName;
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdGazeGrab(const std::string& arg) {
    const auto tokens = splitWs(arg);
    if (tokens.size() > 1)
        return std::unexpected<std::string>("gazegrab: expected an optional monitor name (<name>|active), or no arg to toggle");

    // TARGETED (hypxrvoice GAP 1): begin a carry on the NAMED monitor regardless of the live dwell
    // candidate. Deterministic — no dependence on where the head happens to point at execution time.
    if (!tokens.empty()) {
        const std::string& name  = tokens[0];
        PXRLAYER           layer = name == "active" ? resolveSelected() : layerByName(name);
        if (!layer)
            return std::unexpected<std::string>(name == "active" ? "gazegrab: no XR monitor selected" : "gazegrab: no XR monitor named '" + name + "'");
        return beginGazeCarry(layer);
    }

    // TOGGLE: a gaze carry is active -> release it (reanchor via endGazeGrab), like a keyboard-up.
    if (!m_gazeCarryMonitor.empty()) {
        const auto ctx   = currentVerbContext();
        const auto tune  = readAnchorTuning();
        auto       layer = layerByName(m_gazeCarryMonitor);
        m_gazeCarryMonitor.clear();
        if (layer) {
            OpenXR::SXRSolveInput in;
            in.view      = ctx.view;
            in.gripLeft  = ctx.gripLeft;
            in.gripRight = ctx.gripRight;
            std::scoped_lock lock(m_layersMu);
            if (layer->m_anchor.gazeGrabbed())
                layer->m_anchor.endGazeGrab(in, tune);
        }
        return {};
    }

    // Grab the dwell-stable gazed-at monitor. Fail cleanly if not looking at one — NEVER fall back to
    // the sticky selection (that would grab a monitor behind you).
    const int64_t id = m_gazeHoveredId.load(std::memory_order_acquire);
    if (id < 0)
        return std::unexpected<std::string>("gazegrab: not looking at a monitor");
    return beginGazeCarry(layerByMonitorID((MONITORID)id));
}

std::expected<void, std::string> COpenXRManager::cmdGazeRelease() {
    // Explicit release (for a bindr): a no-op when nothing is gaze-carried, so it never error-spams.
    if (m_gazeCarryMonitor.empty())
        return {};
    const auto ctx  = currentVerbContext();
    const auto tune = readAnchorTuning();
    auto       layer = layerByName(m_gazeCarryMonitor);
    m_gazeCarryMonitor.clear();
    if (layer) {
        OpenXR::SXRSolveInput in;
        in.view      = ctx.view;
        in.gripLeft  = ctx.gripLeft;
        in.gripRight = ctx.gripRight;
        std::scoped_lock lock(m_layersMu);
        if (layer->m_anchor.gazeGrabbed())
            layer->m_anchor.endGazeGrab(in, tune);
    }
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdGazePush(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() > 1)
        return std::unexpected<std::string>("gazepush: expected <±m> (or no arg for the default step)");
    float delta;
    if (tokens.empty()) {
        static auto PSTEP = CConfigValue<Hyprlang::FLOAT>("openxr:gaze_dist_step");
        delta             = (float)*PSTEP; // default: push farther by one step
    } else {
        auto d = parseFloatArg(tokens[0]);
        if (!d)
            return std::unexpected<std::string>("gazepush: argument must be a number");
        delta = *d;
    }

    // While carrying -> move the carry along the gaze ray. Otherwise -> nudge the gaze-selected
    // monitor along the view->quad ray (research/16 §4.3), reusing the shipped distance math.
    if (!m_gazeCarryMonitor.empty()) {
        auto layer = layerByName(m_gazeCarryMonitor);
        if (!layer)
            return std::unexpected<std::string>("gazepush: carried monitor is gone");
        std::scoped_lock lock(m_layersMu);
        if (!layer->m_anchor.gazeGrabbed()) {
            m_gazeCarryMonitor.clear();
            return std::unexpected<std::string>("gazepush: no active gaze carry");
        }
        layer->m_anchor.gazePushPull(delta);
        return {};
    }

    const int64_t id = m_gazeHoveredId.load(std::memory_order_acquire);
    if (id < 0)
        return std::unexpected<std::string>("gazepush: not carrying and not looking at a monitor");
    auto layer = layerByMonitorID((MONITORID)id);
    if (!layer)
        return std::unexpected<std::string>("gazepush: gazed-at monitor is gone");
    const auto       ctx = currentVerbContext();
    std::scoped_lock lock(m_layersMu);
    if (!layer->m_anchor.applyDistance(delta, ctx))
        return std::unexpected<std::string>("gazepush: head tracking unavailable or no current pose");
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdHandInput(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("handinput: expected on|off|auto|toggle");
    const std::string& a = tokens[0];
    if (a == "on") {
        m_handPolicy.store(HANDPOL_ON, std::memory_order_relaxed);
        m_handForce.store(HANDFORCE_NONE, std::memory_order_relaxed);
    } else if (a == "off") {
        m_handPolicy.store(HANDPOL_OFF, std::memory_order_relaxed);
        m_handForce.store(HANDFORCE_NONE, std::memory_order_relaxed);
    } else if (a == "auto") {
        m_handPolicy.store(HANDPOL_AUTO, std::memory_order_relaxed);
        m_handForce.store(HANDFORCE_NONE, std::memory_order_relaxed);
    } else if (a == "toggle") {
        // The key-chord: flip the effective gate regardless of the prior policy, landing in AUTO +
        // a manual force latch (so a later `handinput auto` returns to the pure keyboard/roam gate).
        const bool wasOn = handInputEnabled();
        m_handPolicy.store(HANDPOL_AUTO, std::memory_order_relaxed);
        m_handForce.store(wasOn ? HANDFORCE_OFF : HANDFORCE_ON, std::memory_order_relaxed);
    } else
        return std::unexpected<std::string>("handinput: expected on|off|auto|toggle");
    return {};
}

COpenXRManager::SXRGazeStatus COpenXRManager::gazeStatus() {
    SXRGazeStatus s;
    static auto   PGSRC = CConfigValue<std::string>("openxr:gaze_source");
    s.source            = *PGSRC == "eye" ? "view (eye requested; unsupported HW -> view)" : "view";
    const int64_t id    = m_gazeHoveredId.load(std::memory_order_acquire);
    s.hoveredMonitor    = id;
    if (id >= 0)
        if (auto l = layerByMonitorID((MONITORID)id))
            s.hoveredName = l->m_monitorName;
    if (!m_gazeCarryMonitor.empty()) {
        if (auto l = layerByName(m_gazeCarryMonitor)) {
            std::scoped_lock lock(m_layersMu);
            if (l->m_anchor.gazeGrabbed()) {
                s.carrying     = true;
                s.carryMonitor = m_gazeCarryMonitor;
                s.dist         = l->m_anchor.gazeDist();
            }
        }
    }
    return s;
}

// hypxrvoice WP-V1 — FRAME THREAD. Append the current head pose + gaze candidate to the ring.
void COpenXRManager::recordPoseSample(const OpenXR::SXRPose& view, bool viewValid) {
    OpenXR::SXRPoseSample s;
    // Time::steadyNow() == std::chrono::steady_clock == CLOCK_MONOTONIC on Linux (Time.cpp) — the
    // exact clock the daemon queries with clock_gettime(CLOCK_MONOTONIC). See SXRPoseRing's CLOCK
    // CONTRACT. processPointer stamps its input events off the same source (frameThread above).
    s.timestampMs   = (int64_t)Time::millis(Time::steadyNow());
    s.viewValid     = viewValid;
    s.headPos       = view.pos;
    s.headRot       = view.rot;
    s.gazeMonitorId = m_gazeHoveredId.load(std::memory_order_acquire); // dwell-stable id (frame->main atomic)
    s.gazeRawId     = m_gazeHitId;                                     // frame-thread-only, same thread
    s.gazeDwell     = m_gazeSel.dwell;                                 // frame-thread-only, same thread

    std::scoped_lock lock(m_poseRingMu);
    m_poseRing.push(s);
}

// Shared body for the `gaze` / `gaze at <ms>` verbs: turn a ring sample into an SXRGazeSample,
// resolving the stored MONITORID -> name on THIS (main) thread. `sample` must be a copy taken
// under m_poseRingMu by the caller.
COpenXRManager::SXRGazeSample COpenXRManager::gazeSampleNow() {
    SXRGazeSample out;
    OpenXR::SXRPoseSample s;
    {
        std::scoped_lock lock(m_poseRingMu);
        if (m_poseRing.empty())
            return out; // ok = false
        s = m_poseRing.newest();
    }
    out.ok               = true;
    out.timestampMs      = s.timestampMs;
    out.viewValid        = s.viewValid;
    out.headPos          = s.headPos;
    out.headRot          = s.headRot;
    out.headForward      = OpenXR::poseForward(s.headRot);
    out.gazeMonitorId    = s.gazeMonitorId;
    out.selected         = s.gazeMonitorId >= 0;
    out.dwell            = s.gazeDwell;
    if (s.gazeMonitorId >= 0)
        if (auto l = layerByMonitorID((MONITORID)s.gazeMonitorId))
            out.gazeName = l->m_monitorName;
    return out;
}

COpenXRManager::SXRGazeSample COpenXRManager::gazeSampleAt(int64_t requestedTimestampMs) {
    SXRGazeSample out;
    OpenXR::SXRPoseSample s;
    {
        std::scoped_lock lock(m_poseRingMu);
        if (!OpenXR::poseRingNearest(m_poseRing, requestedTimestampMs, s))
            return out; // ok = false (ring empty)
    }
    out.ok                   = true;
    out.matched              = true;
    out.requestedTimestampMs = requestedTimestampMs;
    out.matchedTimestampMs   = s.timestampMs;
    out.ageMs                = requestedTimestampMs - s.timestampMs; // >0: matched sample is older than asked
    out.timestampMs          = s.timestampMs;
    out.viewValid            = s.viewValid;
    out.headPos              = s.headPos;
    out.headRot              = s.headRot;
    out.headForward          = OpenXR::poseForward(s.headRot);
    out.gazeMonitorId        = s.gazeMonitorId;
    out.selected             = s.gazeMonitorId >= 0;
    out.dwell                = s.gazeDwell;
    if (s.gazeMonitorId >= 0)
        if (auto l = layerByMonitorID((MONITORID)s.gazeMonitorId))
            out.gazeName = l->m_monitorName;
    return out;
}

#endif
