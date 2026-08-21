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
#include "XRStereoPair.hpp" // WP X1: the stereo quad-pair policy + pane rect math (pure)
#include "XRBoundedCall.hpp" // OpenXR::runBoundedProbe — the throwaway-thread pattern for driver/runtime calls
#include "XRInput.hpp"
#include "XRPointerDevice.hpp"
#include "XRViewpointEligibility.hpp"
#include "../protocols/XRViewpointProtocol.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../config/ConfigValue.hpp"
#include "../event/EventBus.hpp"
#include "../managers/EventManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../output/Monitor.hpp"
#include "../state/MonitorState.hpp"
#include "../state/MonitorLayoutController.hpp" // 2D-plane sync: scheduleRecheck() after moving XR monitors
#include "../helpers/Drm.hpp"    // DRM::sameGpu (physical-GPU compare for cross-GPU linear decision)
#include "../helpers/Format.hpp" // NFormatUtils::drmModifierName (post-reconfigure modifier log)
#include "../render/Renderer.hpp" // damageMonitor (force a fresh present into the re-allocated buffers)
#include "../desktop/state/FocusState.hpp"
#include "../desktop/Workspace.hpp"                     // xrrule: the monitor's active workspace + last-focused window
#include "../desktop/view/Window.hpp"                   // xrrule: m_class / m_title of the focused window
#include "../managers/fullscreen/FullscreenController.hpp" // xrrule: the fullscreen window of a monitor
#include "../managers/eventLoop/EventLoopTimer.hpp"     // xrrule: the transition-envelope tick
#include "../config/shared/monitor/MonitorRuleManager.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"
#include "../config/shared/xr/XRDeclarationManager.hpp"
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
    // Same for the transparency transition-envelope tick (doc 05 §xrrule).
    if (m_fxTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_fxTimer);
    m_fxTimer.reset();
    // ...and for the 2D-plane-sync debounce (report 12 WP-S2).
    if (m_l2dTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_l2dTimer);
    m_l2dTimer.reset();
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

    // Situational per-monitor transparency (doc 05 §xrrule): the re-evaluation triggers. Each one
    // only REQUESTS an evaluation — requestEffectEval() coalesces a burst into a single pass at the
    // end of the loop iteration, and the pass is a few RE2 matches per XR monitor. The remaining
    // triggers need no listener: anchor-state changes ride the existing frame->main GRAB/ADAPTIVE
    // state events (dispatchStateEvent), and config reloads go through onConfigReload().
    m_fxWindowActiveListener     = Event::bus()->m_events.window.active.listen([this](PHLWINDOW, Desktop::eFocusReason) { requestEffectEval(); });
    m_fxWindowFullscreenListener = Event::bus()->m_events.window.fullscreen.listen([this](PHLWINDOW) { requestEffectEval(); });
    m_fxWindowCloseListener      = Event::bus()->m_events.window.close.listen([this](PHLWINDOW) { requestEffectEval(); });
    // Viewpoint eligibility triggers. These four are safe to raise the eval rate with because the
    // pass they schedule cannot re-emit any of them, so there is no doLater that feeds itself:
    // evaluateMonitorEffects only reads state (rule context, config) and writes layer atomics +
    // monitor damage, and reevaluateViewpoints only reads window/workspace/fullscreen state and
    // sends protocol events. The four emitters are the window-rule applicator, the float toggle,
    // CWindow::moveToWorkspace, and the three monitor-layout sites (ensureMonitorStatus, setMirror,
    // MonitorLayoutController::arrange) — none of which is on either path. Anything added to that
    // pass which can move a window, a workspace, a rule or the monitor layout needs a re-entrancy
    // guard here, because requestEffectEval clears its queued flag BEFORE running the pass.
    m_viewpointWindowRulesListener     = Event::bus()->m_events.window.updateRules.listen([this](PHLWINDOW) { requestEffectEval(); });
    m_viewpointWindowFloatingListener  = Event::bus()->m_events.window.floating.listen([this](PHLWINDOW) { requestEffectEval(); });
    m_viewpointWindowWorkspaceListener = Event::bus()->m_events.window.moveToWorkspace.listen([this](PHLWINDOW, PHLWORKSPACE) { requestEffectEval(); });
    m_viewpointMonitorLayoutListener   = Event::bus()->m_events.monitor.layoutChanged.listen([this] { requestEffectEval(); });
    m_fxWorkspaceActiveListener  = Event::bus()->m_events.workspace.active.listen([this](PHLWORKSPACE) { requestEffectEval(); });
    m_fxMonitorAddedListener     = Event::bus()->m_events.monitor.added.listen([this](PHLMONITOR) { requestEffectEval(); });
    m_fxMonitorRemovedListener   = Event::bus()->m_events.monitor.removed.listen([this](PHLMONITOR) { requestEffectEval(); });
    // Titles fire on every browser tab switch and every terminal `cd`, so this one is gated: unless
    // some rule actually carries a focustitle: condition (m_rulesUseTitle, computed at rule load),
    // the callback is a single bool test and nothing else happens.
    m_fxWindowTitleListener = Event::bus()->m_events.window.title.listen([this](PHLWINDOW) {
        if (m_rulesUseTitle)
            requestEffectEval();
    });

    // Materialize any monitors declared in the config as plain headless outputs (doc 02 lazy
    // binding). Their quads bind when a session later starts. Done before start() so start()'s
    // bindExistingLayers() picks them up. With openxr:monitors_follow_session (default) they are
    // created UNPLUGGED (disabled) and only plug in when a session actually starts (research/18)
    // — no phantom monitors on a sessionless desktop login.
    reconcileDeclaredMonitors();

    // Seed the rule snapshot + resolve the freshly-created monitors' effects (init() runs after the
    // first config parse, so the declared list is already populated).
    reloadXRRules();

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

const std::string& COpenXRManager::blockedReason() const {
    return m_blockedReason;
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
    requestEffectEval();

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

    // Reload-storm containment: stamp every attempt that gets past the guard, so onConfigReload()'s
    // floor can tell "the config genuinely changed again" from "the same second, a hundred reloads".
    m_lastProbeAt = Time::steadyNow();

    // lastWorld and layers survive a session restart, but last session's submitted counts must not:
    // LOCAL_FLOOR has not been established yet and no quad is eligible until this session submits it.
    clearCrossingSubmissionState();
    setState(XR_STATE_STARTING);

    // `xrmonitor view` is deliberately a session-scoped convenience latch. Always recover to a
    // visible desktop after a user restart, runtime loss/re-probe, or compositor relog.
    m_monitorViewVisible.store(true, std::memory_order_release);

    // report-20 issue B1: assume "waiting for the runtime" until we learn otherwise. A failure past
    // createInstance flips this to HEADSET (getSystem FORM_FACTOR_UNAVAILABLE) or keeps it at RUNTIME.
    m_probeWait = XR_WAIT_RUNTIME;

    m_runtimeName.clear();
    m_systemName.clear();
    m_runtimeGpu.clear();
    m_frameRequestedTeardown = false;

    // ---- KERNEL-TAINT TRIPWIRE (doc 01, "Sick-driver refusal") --------------------------------
    // FIRST thing in bring-up, before the runtime handshake and before ANY GPU enumeration —
    // because once enumeration starts there is no way back. libglvnd's first eglGetProcAddress
    // loads every installed vendor library and the count-only eglQueryDevicesEXT contacts every
    // vendor's kernel driver, before a single device handle exists for openxr:gpu to filter on
    // (measured; see the comment block above scanEglDevices in XRGraphics.cpp). So "don't touch the
    // sick driver" is not implementable as a pin or an ordering — the only version of it that
    // works is not starting.
    //
    // Forensics (hard reboot #6): an NVIDIA driver use-after-free cascaded through the kernel,
    // which printed "Fixing recursive fault but reboot is needed!" 29 minutes before the machine
    // died. The compositor was a bystander that boot. This check would have refused the session
    // outright rather than walking into the corrupt driver seconds later.
    //
    // Re-checked on EVERY attempt (this is the retry entry point too), so a user who ignores it
    // keeps being told why instead of silently getting nothing.
    {
        static auto PIGNORETAINT = CConfigValue<Hyprlang::INT>("openxr:ignore_kernel_taint");

        // An unreadable/unparsable file yields nullopt, which evaluateKernelTaint deliberately
        // treats as "proceed" — see its header comment on failing open.
        const auto verdict = OpenXR::evaluateKernelTaint(OpenXR::readKernelTaint(), *PIGNORETAINT != 0);

        // Published for `hyprctl openxr status` — the same sentence the log carries, so the user
        // who never reads the log still finds out why XR will not come up.
        m_blockedReason = verdict.blocked ? verdict.reason : "";

        if (verdict.blocked) {
            // Loud ONCE per block, then quiet: the re-probe retries this every few seconds and an
            // ERR per retry would bury the rest of the log. A disable/enable cycle clears the latch
            // (see stop()), so asking again explicitly always gets a fresh, loud answer.
            Log::logger->log(m_taintBlockLogged ? Log::DEBUG : Log::ERR, "[OPENXR] refusing XR bring-up: {}. The desktop session is unaffected.", verdict.reason);
            m_taintBlockLogged = true;
            setState(XR_STATE_UNAVAILABLE);
            return;
        }

        if (verdict.oopsed)
            Log::logger->log(Log::WARN, "[OPENXR] {}", verdict.reason);
        m_taintBlockLogged = false;
    }

    // Parse the adaptive STRING options to enums up front (main thread) so the frame thread never
    // reads a CConfigValue<const char*>. Must happen before the frame thread launches below.
    publishAdaptiveStringTuning();
    publishHandInputPolicy(); // research/16 Part A: seed the hand-input policy from openxr:hand_input
    publishGrabStringTuning(); // task #25: seed hand_grab / hand_grab_anywhere / grab_filter_scope enums
    publishRecenterPolicy(); // doc 03 §8.4: seed openxr:recenter (hold|follow) before the frame thread launches
    publishBlackAlphaTuning(); // report 09: seed the luma key (re-published below once the blend mode is picked)
    publishCursorCrossingMode(); // task #139: seed openxr:cursor_crossing (raycast | layout)
    publishStereoPairTuning(); // WP X1: seed the stereo kill switch before any layer publishes a declaration
    publishDepthDesktopTuning(); // WP X3: seed the depth desktop (packs each XR monitor's mode)

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

    // The luma key (openxr:black_alpha) is gated on the blend mode we just picked — re-publish now
    // that it is known, so the first frame already carries the right value (and an ignored setting
    // warns at session start rather than at the next reload). m_session is not adopted yet, so pass
    // the picked mode explicitly.
    publishBlackAlphaTuning(xrBlendModeFromXr(sess->m_blendMode));
    // Same for the per-monitor resolution (doc 05 §xrrule): an xrrule's `blackalpha` rides the same
    // blend-mode gate. Deferred, so by the time it runs m_session is adopted and the gate is live.
    requestEffectEval();

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
    // while the desktop is still intact.
    //
    // WHICH QUESTION WE ASK MATTERS (research 26 §8.4). The guard needs the COMPOSITOR's GPU, and
    // that is an EGL question: XR_MND_query_egl_device's xrGetSystemEGLDeviceMND names the device
    // an EGL client must build its context on, which is the compositor's by construction (the GL
    // client compositor imports its swapchain images by an OPAQUE_FD, valid only on the exporting
    // device). xrGetVulkanGraphicsDevice2KHR answers a DIFFERENT question — "which GPU should a
    // Vulkan application render on" — and a WiVRn configured for cross-GPU rendering answers it
    // with the dGPU the game renders on, not the iGPU it composites and encodes on. So: EGL query
    // first; the Vulkan probe only as a fallback for runtimes that lack it, where the two questions
    // have the same answer anyway. Best-effort throughout — an undeterminable result proceeds with
    // a warning rather than blocking a setup that might be fine. Runs on the main thread, before
    // the frame thread — no interop yet.
    {
        const auto&         node = gfx->selectedRenderNode();
        OpenXR::SRuntimeGpu rt;

        // The EGL device query is answered in-process (no IPC, no Vulkan) — but the runtime answers
        // it by calling back through the getProcAddress we hand it, i.e. by doing a full glvnd EGL
        // device enumeration inside our process. That touches every installed vendor driver, so a
        // wedged one hangs this call exactly like the Vulkan probe. It therefore gets the SAME
        // bounded throwaway thread rather than running on the main thread as it used to: a hang
        // here would freeze the desktop, and "could not verify the GPU" is a survivable outcome
        // while a frozen desktop is not.
        if (sess->m_hasEglDeviceQuery) {
            const XrInstance inst = sess->m_instance;
            const XrSystemId sys  = sess->m_systemId;
            int              eglBlockedMs = 0;
            const auto       eglProbe     = OpenXR::runBoundedProbe([inst, sys](const std::atomic<bool>& abandon) { return OpenXR::probeRuntimeEglDevice(inst, sys, &abandon); },
                                                                    OpenXR::XR_PROBE_TIMEOUT_MS, &eglBlockedMs);
            // The bound abandons the WORKER; it does not stop this thread parking. Say so when the
            // park was long enough for the user to feel it (reload-storm investigation 2026-08-21).
            if (eglBlockedMs >= OpenXR::XR_MAIN_THREAD_STALL_WARN_MS)
                Log::logger->log(Log::WARN, "[OPENXR] the runtime's EGL device query blocked the compositor main thread for {}ms", eglBlockedMs);
            if (eglProbe.has_value())
                rt = *eglProbe;
            else {
                rt.probe = "EGL device query (XR_MND_query_egl_device)";
                rt.note  = "EGL device query timed out";
                Log::logger->log(Log::WARN, "[OPENXR] the runtime's EGL device query did not respond within {}ms; falling back", OpenXR::XR_PROBE_TIMEOUT_MS);
            }
        } else
            rt.note = "runtime does not advertise XR_MND_query_egl_device";

        if (!rt.determined && sess->m_hasVulkanEnable2) {
            const std::string eglNote = rt.note;
            // Run the probe on a THROWAWAY thread with a bounded wait. vkCreateInstance inside it
            // can deadlock indefinitely against the runtime's own in-process Vulkan usage (observed
            // hanging forever vs Monado's null compositor), and this must NEVER freeze the
            // compositor. On timeout we abandon the thread (it bails before any XrInstance call, so
            // a late unblock can't touch a torn-down instance) and proceed UNVERIFIED — strictly no
            // worse than before this guard existed. When the probe does answer (the common case on
            // a healthy runtime) we get a reliable cross-GPU verdict.
            const XrInstance inst = sess->m_instance;
            const XrSystemId sys  = sess->m_systemId;
            int              vkBlockedMs = 0;
            const auto       vkProbe     = OpenXR::runBoundedProbe([inst, sys](const std::atomic<bool>& abandon) { return OpenXR::probeRuntimeRenderNode(inst, sys, &abandon); },
                                                                   OpenXR::XR_PROBE_TIMEOUT_MS, &vkBlockedMs);
            if (vkBlockedMs >= OpenXR::XR_MAIN_THREAD_STALL_WARN_MS)
                Log::logger->log(Log::WARN, "[OPENXR] the runtime GPU (Vulkan) probe blocked the compositor main thread for {}ms", vkBlockedMs);

            if (vkProbe.has_value())
                rt = *vkProbe;
            else {
                rt.probe = "Vulkan device query (XR_KHR_vulkan_enable2)";
                rt.note  = "GPU probe timed out";
                Log::logger->log(Log::WARN, "[OPENXR] runtime GPU probe did not respond within {}ms; proceeding without GPU verification", OpenXR::XR_PROBE_TIMEOUT_MS);
            }

            // Both questions went unanswered — carry both reasons into the "could not verify" WARN.
            if (!rt.determined)
                rt.note = std::format("{}; {}", eglNote, rt.note);
            else
                Log::logger->log(Log::DEBUG, "[OPENXR] EGL device query unavailable ({}), fell back to the Vulkan device query — correct unless this runtime renders clients on a separate GPU", eglNote);
        } else if (!rt.determined)
            rt.note = std::format("{}; no XR_KHR_vulkan_enable2 to fall back on", rt.note);

        // Publish the resolved runtime GPU for `hyprctl openxr status` (empty when undeterminable).
        m_runtimeGpu = rt.determined ? std::format("{} (drm {}:{}, via {})", rt.deviceName.empty() ? "GPU" : rt.deviceName, rt.drmMajor, rt.drmMinor, rt.probe) : "";

        if (rt.determined && node.valid && (rt.drmMajor != node.major || rt.drmMinor != node.minor)) {
            Log::logger->log(Log::ERR,
                             "[OPENXR] openxr:gpu selects {} (drm {}:{}) but the runtime '{}' composites on {} (drm {}:{}), per its {}. Cross-GPU buffer "
                             "import crashes the graphics driver and would take the whole session down, so XR is refusing to start. Point openxr:gpu at "
                             "the runtime's GPU (or unset it). Desktop session unaffected.",
                             node.path, node.major, node.minor, m_runtimeName, rt.deviceName.empty() ? "another GPU" : rt.deviceName, rt.drmMajor, rt.drmMinor,
                             rt.probe);
            m_session  = UP<CXRSession>(sess); // adopt so abortStart() tears both down (synchronous refusal)
            m_graphics = UP<CXRGraphics>(gfx);
            abortStart();
            return;
        }

        if (rt.determined && node.valid)
            Log::logger->log(Log::DEBUG, "[OPENXR] XR GPU verified against runtime: {} (drm {}:{}), per its {}", rt.deviceName.empty() ? node.path : rt.deviceName, node.major,
                             node.minor, rt.probe);
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

    // report 12 §3a: the latched desk orientation belongs to the SESSION that captured it — a
    // LOCAL_FLOOR origin is not stable across a runtime restart (report-20 issue C is the same fact
    // from the anchor side). Drop it so the next session latches its own from a fresh head pose;
    // until it does, syncLayout2D no-ops and the historic append-right stands.
    m_l2dRef       = OpenXR::SXRLayout2DRef{};
    m_l2dPrev.clear();

    setState(XR_STATE_STOPPING);

    // Remove the ray pointer first (main thread) so no further input routes while tearing down.
    removePointerDevice();

    // Teardown ordering (doc 01): join the frame thread BEFORE touching any EGL/XR object.
    m_running = false;
    if (m_frameThread.joinable())
        m_frameThread.join();

    // The frame thread can publish a final submitted count while it exits, so clear only after the
    // join. Surviving layers keep their anchors for restart, but none remains raycast-visible.
    clearCrossingSubmissionState();

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
    // Nothing is blocking a stopped session — and dropping the "already logged" latch means an
    // explicit re-enable gets the taint refusal shouted at it again rather than only whispered.
    m_blockedReason.clear();
    m_taintBlockLogged = false;

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
            requestEffectEval();
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
            // doc 05 §xrrule: a grab begin/end IS an anchor-state transition (-> carried / back).
            requestEffectEval();
            // report 12 §4: re-derive the 2D plane on the RELEASE edge only. Never on grab-begin —
            // the layout must not churn mid-drag, and syncLayout2D() refuses to run while anything
            // is carried anyway. Debounced, so a flurry of releases costs one relayout.
            if (!e.a)
                requestLayout2DSync();
            break;
        case eXRStateEventType::ADAPTIVE:
            // research/13 §6.4: the terminal dock/undock edge (a = 1 undocked / 0 docked).
            if (!e.str.empty() && g_pEventManager)
                g_pEventManager->postEvent(SHyprIPCEvent{e.a ? "xrmonitorundocked" : "xrmonitordocked", e.str});
            // doc 05 §xrrule: the docked <-> follow transition — the trigger the walking rule
            // (`xrrule = alpha 0.55, anchorstate:follow`) rides.
            requestEffectEval();
            // report 12 §4 / LAYOUT-AND-NAMING "seams": adaptive dock/undock is a spatial change, so
            // the plane re-derives on the terminal edge. An adaptive monitor is projected from its
            // SAVED desk pose either way (see syncLayout2D), so this settles the plane back onto the
            // desk arrangement after a roam rather than tracking you around the room.
            requestLayout2DSync();
            break;
        case eXRStateEventType::VIEWPOINT_STATE: {
            const auto layer        = layerByName(e.str);
            const auto subscription = layer ? layer->viewpointSubscription() : std::nullopt;
            if (!subscription || !PROTO::xrViewpoint)
                break;

            bool hasActiveOwner = false;
            PROTO::xrViewpoint->forEachViewpoint([&](CXRViewpointResource& viewpoint) {
                if (viewpoint.token() != subscription->token)
                    return;
                hasActiveOwner = true;
                if (!e.a)
                    viewpoint.invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_TRACKING_LOST);
            });
            if (!e.a)
                layer->revokeViewpointEpoch(subscription->token);
            if (e.a && !hasActiveOwner)
                requestEffectEval();
            break;
        }
        case eXRStateEventType::TRACKING: Log::logger->log(Log::DEBUG, "[OPENXR] device-lock tracking {} (monitor {})", e.a ? "gained" : "lost", e.str); break;
        case eXRStateEventType::RECENTERED: {
            // doc 03 §8.4: the runtime moved LOCAL_FLOOR and openxr:recenter = follow, so the user's
            // recenter button means "bring my monitors to me". Re-check the policy HERE (the frame
            // thread's atomic could have been published a moment before a reload flipped it back)
            // and run the identical operation the `reseat` verb runs — there is one re-seat.
            if (recenterPolicy() != OpenXR::XR_RECENTER_FOLLOW)
                break;
            const auto r = requestReseatToHead("headset recenter (openxr:recenter = follow)");
            // A recenter that cannot be followed is not an error the user asked a question about —
            // typically there is simply nothing anchor:local to move. Log it and move on.
            if (!r)
                Log::logger->log(Log::DEBUG, "[OPENXR] recenter: follow policy did not re-seat — {}", r.error());
            break;
        }
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

    drainViewpointSamples();

    // report 12 §3a: a runtime recenter (the user pressed the recenter button) moved LOCAL_FLOOR
    // under us. Every anchor was re-expressed into the new space by the frame loop, but the latched
    // desk orientation is still in the old one — drop it and re-derive. Consumed with an exchange so
    // exactly one sync is requested no matter how often this fd fires; the re-latch then happens in
    // syncLayout2D against a fresh head pose, which is also the semantic the user expects from
    // recentering ("re-sync to how I'm sitting now").
    if (m_l2dRefStale.exchange(false, std::memory_order_acq_rel)) {
        m_l2dRef.valid = false;
        m_l2dPrev.clear();
        requestLayout2DSync();
    }

    // EXITING / LOSS_PENDING: the frame loop has already exited. Defer stop() out of this
    // fd callback (doLater is main-thread-safe) so we don't remove the event source from
    // within its own dispatch. stop() decides DISABLED vs UNAVAILABLE from m_instanceLost.
    if (m_frameRequestedTeardown.exchange(false) && g_pEventLoopManager)
        g_pEventLoopManager->doLater([this] { stop(); });
}

void COpenXRManager::applyReferenceSpaceChange(const OpenXR::SXRPose& m) {
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            l->m_anchor.onReferenceSpaceChanged(m);
    }
    // report 12 §3a: the whole reference space moved under us, so the latched desk orientation the
    // 2D projection measures world monitors against is now expressed in a space that no longer
    // exists. A plain atomic store plus a wake is all the frame thread may do here
    // (XRMonitorLayer.hpp: no refcounts, no strings, no config); onFrameChannelReadable consumes it
    // on the main thread and re-latches.
    m_l2dRefStale.store(true, std::memory_order_release);
    wakeMain();
}

void COpenXRManager::frameThread() {
    // The frame thread exclusively owns the EGL context while running. It snapshots the
    // layer set once per frame, blits each layer's latest presented buffer into its
    // swapchain, and submits one XrCompositionLayerQuad per layer (doc 01 loop / doc 02).
    eXRManagerState lastReported  = XR_STATE_RUNNING_IDLE;
    int64_t         lastPredicted = 0; // XrTime (ns) of the previous frame, for the solve dt
    int             frameFailStreak = 0; // consecutive xrWaitFrame/xrBeginFrame failures (loss backstop)

    // Reference-space change reconstruction (doc 03 §8.1). Deliberately FUNCTION-LOCAL, not members:
    // only this thread ever reads or writes them, and a session restart re-enters frameThread() with
    // them already reset — no atomics, no lock, nothing for the main thread to get wrong.
    OpenXR::SXRPose headLast;                    // newest valid head pose, in the CURRENT reference space
    bool            headLastValid        = false;
    int64_t         headLastTime         = 0;    // XrTime (ns) at which `headLast` was located
    bool            recenterSolvePending = false; // a poseValid=false change is waiting for its head pair
    OpenXR::SXRPose recenterHeadOld;             // `headLast` as it stood in the space that just died
    bool            recenterHeadOldValid = false;
    int64_t         recenterHeadOldTime  = 0;

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

        // Recenter (doc 03 §8.1): re-express every anchor across a reference-space change.
        if (m_session->m_recenterPending) {
            const OpenXR::SXRPose M      = m_session->m_recenterPose;
            const bool            valid  = m_session->m_recenterPoseValid;
            m_session->m_recenterPending = false;
            if (valid) {
                applyReferenceSpaceChange(M);
                // doc 03 §8.4: the change is fully handled — hand the EDGE to the main thread, which
                // owns the openxr:recenter policy. Under `hold` (the default) notifyRecentered() does
                // nothing at all, so this path is byte-identical to what shipped.
                notifyRecentered();
            } else {
                // monado — so WiVRn, so every session on this machine — pushes this event with
                // pose_valid = false and an identity pose even though recenter_local_spaces just
                // computed the exact delta (research/22 §4.3). That used to end the story here, and
                // the consequence was the whole bug: the origin moved, every anchor kept coordinates
                // in the space it had just left, and the monitors were flung across the room by the
                // frame shift (one live session: 8.25 m and ~155 deg of yaw across a single
                // recenter). Remember where the head was in the space that just died — the locate
                // below catches the same head in the new one, and the pair reconstructs the delta.
                recenterSolvePending = true;
                recenterHeadOld      = headLast;
                recenterHeadOldValid = headLastValid;
                recenterHeadOldTime  = headLastTime;
                Log::logger->log(Log::DEBUG, "[OPENXR] recenter carried no pose — reconstructing the frame change from the head across it");
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

        // A hidden view is still a fully live OpenXR session: keep pumping wait/begin/end frames and
        // processing removal barriers, but make the presentation set empty. This avoids swapchain
        // churn, output/workspace movement, and session recreation; xrEndFrame below legally submits
        // zero layers. CXRInput still samples actions and processPointer receives no targets, which
        // clears hover/releases without letting an invisible quad intercept the pointer.
        if (!m_monitorViewVisible.load(std::memory_order_acquire)) {
            for (auto& l : active)
                l->m_quadsSubmitted.store(0, std::memory_order_relaxed);
            active.clear();
        }

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

        // One PRIMARY_STEREO locate per runtime frame, shared by every subscribed surface. These
        // are real per-eye view positions at predictedDisplayTime; the XrTime itself is not exposed
        // because no XrTime↔CLOCK_MONOTONIC conversion has been established.
        std::array<XrView, 2> viewpointViews{};
        for (auto& view : viewpointViews)
            view.type = XR_TYPE_VIEW;
        bool anyViewpointSubscription = std::ranges::any_of(active, [](const auto& layer) { return layer->viewpointSubscription().has_value(); });
        bool viewpointViewsValid      = false;
        if (visible && anyViewpointSubscription) {
            XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime           = fs.predictedDisplayTime;
            locateInfo.space                 = m_session->m_refSpace;
            XrViewState viewState{XR_TYPE_VIEW_STATE};
            uint32_t    viewCount = 0;
            viewpointViewsValid   = XR_SUCCEEDED(xrLocateViews(m_session->m_session, &locateInfo, &viewState, viewpointViews.size(), &viewCount, viewpointViews.data())) &&
                viewCount == viewpointViews.size() && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
        }

        // Per-layer: ensure a swapchain, then blit the latest presented buffer into it.
        bool lostInFrame = false; // set if a per-layer xr call reveals a dead/wedged runtime
        for (auto& l : active) {
            // The main thread's declaration for this monitor (research/24 §5.1 + §6, WP X1/X3): the
            // producer, the split, whether to submit a pair, and the pixel mode all of that describes.
            // Read ONCE here and used for the whole iteration — a re-read mid-frame could see a
            // different declaration for the same image.
            const auto DECL = layerDecl(*l);
            // WP X4: a depth-packed monitor's swapchain is TWO independently-margined panes, so the
            // pane count is part of the swapchain's shape, not just of the submission. It only ever
            // changes together with the mode (the pack IS the doubled mode), but deriving it here and
            // comparing keeps the swapchain self-healing if the two ever arrive out of order — the
            // cost of getting it wrong is a frame showing each eye half a mono desktop.
            const int WANTPANES = DECL.producer == OpenXR::Stereo::PRODUCER_DEPTH ? 2 : 1;

            // Mode change (doc 02), pane-count change (WP X4), or first bind: (re)create the
            // swapchain at the monitor's pixel mode, as cached by the main thread at bind/modeChanged
            // (m_pendingSize under m_bufMu). The frame thread must NOT lock() m_monitor — hyprutils
            // refcounts are not atomic and racing the main thread's copies corrupts them (see
            // XRMonitorLayer.hpp).
            const bool DIRTY = l->m_swapchainDirty.exchange(false, std::memory_order_acq_rel);
            if (DIRTY || l->m_swapchain == XR_NULL_HANDLE || l->m_paneGeom.panes != WANTPANES) {
                Vector2D newSize;
                {
                    std::lock_guard<std::mutex> lk(l->m_bufMu);
                    newSize = l->m_pendingSize;
                }
                if (newSize.x >= 1 && newSize.y >= 1)
                    createLayerSwapchain(*l, newSize, WANTPANES);
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
            // WP X1 (research/24 §5.1 option 2): while a CONTENT pair is submitted, the eye quads
            // cover the content rect only — the chrome margins are outside both, and the ray cursor
            // would be drawn at a full-quad uv into a swapchain whose full quad is no longer what
            // anyone sees. Drawing either would put pixels where no eye looks (chrome) or in one eye
            // at the wrong place (cursor), so both stay suppressed for the pure-content case: a
            // monitor showing a fullscreen 3D film is not one you reposition mid-frame.
            //
            // WP X4 removes that suppression for the DEPTH producer, and it had to: chrome is the
            // primary grab affordance, so a depth desktop that hides it on EVERY monitor is not a
            // trade, it is a regression. The depth pack gives each eye its own margined pane
            // (createLayerSwapchain above), so chrome and cursor are drawn once PER PANE and land
            // inside both eyes' quads.
            const bool    contentPaired = DECL.producer == OpenXR::Stereo::PRODUCER_CONTENT && DECL.submit;
            const int     drawPanes   = std::max(1, l->m_paneGeom.panes);
            const bool    chromeOn   = l->m_chrome.hasChrome() && !contentPaired;
            l->m_chromeLive.store(chromeOn, std::memory_order_relaxed);
            // report 14 Stage A1: per-hand endpoint cursor. Drawn (like chrome) into the swapchain
            // over content; its packed word was published last frame by processPointer's plumbing.
            static auto    PGAZECUR      = CConfigValue<Hyprlang::INT>("openxr:gaze_cursor");
            const bool     cursorEnabled = *PCURSOREN != 0 && !contentPaired;
            const bool     gazeCurEnabled = *PGAZECUR != 0 && !contentPaired;
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
            // Situational transparency (doc 05 §xrrule): the per-monitor effect values the MAIN
            // thread resolved (defaults -> rules -> manual), eased by its envelope tick and published
            // as plain atomics. Read once here. blackAlpha/knee go into the blit (the luma key is
            // per-pixel, so it must be baked while we have the source buffer); fxAlpha is the uniform
            // monitor alpha, applied LAST as a multiply over the finished image so chrome ghosts with
            // the content and an animation-only frame can re-apply it from the snapshot alone.
            const float    blackAlpha = l->m_fxBlackAlpha.load(std::memory_order_relaxed);
            const float    blackKnee  = l->m_fxKnee.load(std::memory_order_relaxed);
            const float    fxAlpha    = l->m_fxAlpha.load(std::memory_order_relaxed);
            // Snapshot the clean content whenever chrome OR any cursor may draw over it, so an
            // animation-only frame (chrome fade or a moving cursor with no new desktop buffer) can
            // restore the content before re-drawing the overlay. A monitor with a uniform fade needs
            // it too: that is what lets the fade animate over a completely static desktop.
            const bool     snapOn = chromeOn || cursorEnabled || gazeCurEnabled || OpenXR::xrUniformAlphaActive(fxAlpha);

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

            // WP X3 (§5.4): ease the ray cursor's depth toward whatever it is over. The main thread
            // publishes the target (it needs the hovered view); the ease runs here because this is
            // where a real frame delta exists. A window edge is a STEP in this quantity and a cursor
            // that teleports between depths reads as a glitch — 3τ ≈ 80 ms, inside §5.4's window.
            const float dispTarget = l->m_cursorDisparityTarget.load(std::memory_order_acquire);
            l->m_cursorDisparity   = OpenXR::Stereo::easeCursorDisparity(l->m_cursorDisparity, dispTarget, dtSec, OpenXR::Stereo::CURSOR_DISPARITY_EASE_TAU_SEC);
            // While the ease is running the cursor moves with no new desktop buffer and no pointer
            // motion, so it is its own animation source — exactly like the chrome fade above. The
            // epsilon is a pane-width fraction; below it the shift rounds to the same pixel anyway.
            const bool  dispChanged = std::fabs(l->m_cursorDisparity - l->m_cursorDisparityDrawn) > 1e-4f;

            // A chrome-only (no new desktop buffer) redraw is needed ONLY when the on-screen chrome
            // would actually differ (alpha/region/grab changed) and something is or was visible.
            // This is what keeps a static desktop with hidden chrome at zero GPU cost — the quad
            // re-presents the most recently released image every runtime frame (doc 01).
            const bool chromeVisualChanged = newAlpha != l->m_chromeDrawnAlpha || hoverReg != l->m_chromeDrawnRegion || grabbedNow != l->m_chromeDrawnGrab;
            // The uniform fade is its own animation source: while it eases, the composed image must
            // be rebuilt each frame even with no new desktop buffer and no chrome. Once settled the
            // comparison is exact and this costs nothing.
            const bool fadeVisualChanged   = fxAlpha != l->m_fxAlphaDrawn;
            const bool wantAnimTick        = l->m_hasContent &&
                ((chromeOn && chromeVisualChanged && (newAlpha > 0.f || l->m_chromeDrawnAlpha > 0.f)) || ((cursorEnabled || gazeCurEnabled) && (cursorChanged || dispChanged)) ||
                 fadeVisualChanged);

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
                    m_graphics->blitBuffer(buf, *l, dst, blackAlpha, blackKnee);
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
                    m_graphics->clearTex(dst, l->m_swapchainSize, 0.0f, 0.0f, 0.0f, blackAlpha, blackKnee);

                // Chrome pass (WP-G2): draw the move-bar + corner handles into the transparent
                // margin over the content. No-op when disabled or fully faded out; drawChrome never
                // touches the content rect.
                if (chromeOn && l->m_hasContent) {
                    // WP X4: once per pane. On a mono monitor drawPanes == 1 and this is the shipped
                    // single call; on a depth-packed one each eye gets its own bar and handles, at
                    // the same place in its own picture, so the quad still reads as grabbable.
                    for (int pane = 0; pane < drawPanes; ++pane)
                        m_graphics->drawChrome(*l, dst, newAlpha, hoverReg, grabbedNow, pane);
                    l->m_chromeDrawnAlpha  = newAlpha;
                    l->m_chromeDrawnRegion = hoverReg;
                    l->m_chromeDrawnGrab   = grabbedNow;
                }

                // Endpoint-cursor pass (report 14 Stage A1): draw each hand's cursor at its ray-hit
                // uv, over content + chrome. Plus the gaze cursor (research/16 §3.3) on the carried
                // monitor. No-op when disabled or no cursor present.
                if ((cursorEnabled || gazeCurEnabled) && l->m_hasContent) {
                    // WP X3: once per pane, each with that pane's share of the disparity, so the dot
                    // floats at the depth of what it is pointing at instead of behind it (§5.4's
                    // "subtitle behind the object"). A mono monitor draws one dot at disparity 0 —
                    // the shipped call, reached through a loop of length one.
                    for (int pane = 0; pane < drawPanes; ++pane)
                        m_graphics->drawCursor(*l, dst, cursorEnabled ? curL : 0, cursorEnabled ? curR : 0, curGaze, pane,
                                               drawPanes > 1 ? OpenXR::Stereo::cursorDisparityForPane(l->m_cursorDisparity, pane) : 0.f);
                    l->m_cursorDrawn[0]   = cursorEnabled ? curL : 0;
                    l->m_cursorDrawn[1]   = cursorEnabled ? curR : 0;
                    l->m_gazeCursorDrawn  = curGaze;
                    l->m_cursorDisparityDrawn = l->m_cursorDisparity;
                } else if (contentPaired) {
                    // WP X1: while paired we drew neither chrome nor cursor, and the content blit
                    // above erased whatever was there. Clear the redraw-diff trackers to match, or
                    // leaving the pair would compare against pixels that no longer exist and skip the
                    // redraw that puts the chrome and the cursor back until the next time they move.
                    l->m_chromeDrawnAlpha = 0.f;
                    l->m_cursorDrawn[0]   = 0;
                    l->m_cursorDrawn[1]   = 0;
                    l->m_gazeCursorDrawn  = 0;
                    l->m_cursorDisparityDrawn = l->m_cursorDisparity;
                }

                // Uniform monitor alpha (doc 05 §xrrule) — LAST, over content + chrome + cursors, so
                // a ghosted monitor ghosts ENTIRELY (report 09 §3.3). No-op at 1.0.
                m_graphics->fadeTex(dst, l->m_swapchainSize, fxAlpha);
                l->m_fxAlphaDrawn = fxAlpha;
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

        // Reference-space change with no runtime delta (doc 03 §8.1). The head located just above is
        // the same skull the pre-change sample caught one frame earlier, so the pair pins the
        // transform the runtime withheld. Deliberately BEFORE the solve and OUTSIDE m_layersMu (which
        // applyReferenceSpaceChange takes itself), so this very frame already renders corrected and
        // the user never sees the wrong placement. Held pending while the view is invalid, exactly
        // like the recenter-on-plug arming below, so a change during a tracking dropout still lands.
        //
        // Do NOT be tempted to gate this on the event's `changeTime` against predictedDisplayTime,
        // however spec-shaped that reads. monado's origin offset is not time-indexed: a locate
        // returns whatever offset is installed at CALL time, regardless of the XrTime asked about.
        // The previous frame's locate ran before pollEvents saw the event and is therefore old-space
        // no matter what its predicted display time says — while a changeTime comparison, on a
        // display time that always sits a frame or two in the future, would routinely misjudge that
        // sample as post-change and no-op the correction away. Call order is the ground truth here.
        if (recenterSolvePending && viewValid) {
            const int64_t AGE_NS = fs.predictedDisplayTime - recenterHeadOldTime;
            switch (OpenXR::xrRecenterFix(false, recenterHeadOldValid, AGE_NS)) {
                case OpenXR::eXRRecenterFix::XR_RECENTER_SOLVE_FROM_HEAD: {
                    const auto M = OpenXR::solveReferenceSpaceChangeFromHead(recenterHeadOld, viewPose);
                    if (OpenXR::xrRecenterIsNoOp(M))
                        Log::logger->log(Log::DEBUG, "[OPENXR] recenter: the reconstructed frame change is the identity — nothing to re-express");
                    else {
                        applyReferenceSpaceChange(M);
                        Log::logger->log(Log::DEBUG, "[OPENXR] recenter: reconstructed a {:.2f}m / {:.1f} deg frame change from the head — monitors held where they are in the room",
                                         M.pos.length(), OpenXR::qYawOf(M.rot, 0.F) * 180.F / 3.14159265F);
                    }
                    break;
                }
                case OpenXR::eXRRecenterFix::XR_RECENTER_RESEAT_TO_HEAD: {
                    // Nothing observed the old frame (the usual cause: the headset was off across the
                    // change), so no correction is derivable — and the wearer may not even be standing
                    // where they were. Leaving the anchors alone is what produced the reported
                    // teleports, so re-seat the group to the head instead: the same rigid,
                    // arrangement-preserving operation the first plug of a session performs, under the
                    // same user permission — or under openxr:recenter = follow, which asks for that
                    // very re-seat on EVERY reference-space change and so plainly covers this one.
                    static auto PRECENTER = CConfigValue<Hyprlang::INT>("openxr:recenter_on_plug");
                    const bool  follow    = m_recenterPolicy.load(std::memory_order_relaxed) == OpenXR::XR_RECENTER_FOLLOW;
                    if (*PRECENTER || follow) {
                        // RESTORE, not GROUP: nothing observed the old frame, so the LIVE arrangement
                        // is expressed in coordinates that no longer mean anything and deriving a
                        // seat frame from it would be deriving one from noise. The stored offsets are
                        // the only description of the arrangement that survived, and the §8.3 capture
                        // was gated off across the tracking gap, so they still describe it.
                        armReseat(OpenXR::XR_RESEAT_ARM_RESTORE);
                        m_l2dRefStale.store(true, std::memory_order_release);
                        wakeMain();
                        Log::logger->log(Log::WARN, "[OPENXR] recenter: no head sample straddles the change (tracking gap {}ms) — re-seating the monitor group to the current head",
                                         AGE_NS / 1'000'000);
                    } else
                        Log::logger->log(Log::WARN,
                                         "[OPENXR] recenter: no head sample straddles the change (tracking gap {}ms) and recenter_on_plug is off — monitors keep coordinates from a "
                                         "reference frame that no longer exists",
                                         AGE_NS / 1'000'000);
                    break;
                }
                case OpenXR::eXRRecenterFix::XR_RECENTER_APPLY_RUNTIME_POSE: break; // unreachable: poseValid was false
            }
            recenterSolvePending = false;
            // doc 03 §8.4: whichever rung the ladder took, the change is now handled — hand the edge
            // to the main thread so the openxr:recenter policy can act on it. A no-op under `hold`.
            // The RESEAT rung above may have armed already; arming twice is idempotent (the frame
            // thread clears the flag when it consumes it, and a re-seat from the same head plants the
            // same pose), so no coordination between the two is needed.
            notifyRecentered();
        }

        if (viewValid) {
            headLast      = viewPose;
            headLastValid = true;
            headLastTime  = fs.predictedDisplayTime;
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
        // Re-seat tally, logged after the lock is dropped (doc 03 §8.3/§8.4). The RESTORE kind splits
        // its count by offset source; the GROUP kind moves the whole arrangement at once, or refuses.
        int  reseatRestored = 0, reseatDeclared = 0, reseatGrouped = 0;
        bool reseatRan = false, reseatUnseatable = false;
        {
            std::scoped_lock lock(m_layersMu);
            m_lastVerbCtx = OpenXR::SXRVerbContext{viewPose, viewValid, gripLeft, gripRight};

            // report-20 issue C: consume a pending recenter-on-plug now that a valid head pose exists.
            // Armed by the main thread on the first plug of a session; the frame thread owns the head
            // pose, so it re-seats every anchor:local monitor to the CURRENT head (yaw-only, floor XZ),
            // reinterpreting each monitor's offset as head-relative. Passing the same viewPose to every
            // layer transforms the group rigidly (relative arrangement preserved). Held armed while the
            // view is invalid so a plug during momentary tracking loss still recenters on the next good
            // frame. onReferenceSpaceChanged already ran above (this overrides it for LOCAL). WHICH
            // offset it plants is doc 03 §8.3 (xrReseatSource) — see the RESTORE branch below.
            //
            // The DELIBERATE re-seat (doc 03 §8.4, XR_RESEAT_ARM_GROUP) takes the other branch, and
            // must: the capture below re-derives every stored offset EVERY FRAME while the headset is
            // worn, so replanting one around the current head gives the monitor its own pose back.
            // It re-seats the LIVE arrangement instead, rigidly, onto the current head.
            const auto reseatKind = (OpenXR::eXRReseatKind)m_reseatArmed.load(std::memory_order_acquire);
            if (viewValid && reseatKind != OpenXR::XR_RESEAT_ARM_NONE) {
                m_reseatArmed.store(OpenXR::XR_RESEAT_ARM_NONE, std::memory_order_release);
                reseatRan = true;

                // The eligible set, gathered once. xrReseatEligible is the ONE definition of it (doc
                // 03 §8.4) — the same predicate the main-thread `reseat` verb counts through, so the
                // number it reports and the number that actually move cannot drift apart.
                // recenterLocalToHead self-guards on mode too; filtering here keeps the tally honest.
                // Raw pointers deliberately: a PXRLAYER copy is a shared_ptr refcount op, and this is
                // the frame thread (XRMonitorLayer.hpp). Both vectors die inside this locked scope.
                std::vector<CXRMonitorLayer*> targets;
                std::vector<OpenXR::SXRPose>  worlds;
                for (auto& l : m_layers) {
                    if (!OpenXR::xrReseatEligible(l->m_anchor.state().mode, l->m_pendingRemoval.load(std::memory_order_acquire)))
                        continue;
                    targets.push_back(l.get());
                    worlds.push_back(l->m_anchor.state().anchorPose);
                }

                if (reseatKind == OpenXR::XR_RESEAT_ARM_GROUP) {
                    // Derive the frame this arrangement was arranged FOR and move it onto the head.
                    // Every monitor is re-expressed in the SAME derived frame, so the transform is
                    // rigid: the relative layout is preserved exactly and only the group as a whole
                    // travels. A group with no common facing (normals cancelling) yields no frame —
                    // leave it alone rather than invent one.
                    const auto seatFrame = OpenXR::xrGroupSeatFrame(worlds.data(), worlds.size(), viewPose);
                    if (!seatFrame.valid)
                        reseatUnseatable = true;
                    else {
                        for (size_t i = 0; i < targets.size(); ++i) {
                            OpenXR::SXRAnchorState seat;
                            seat.mode       = OpenXR::XR_ANCHOR_LOCAL;
                            seat.anchorPose = OpenXR::xrPoseInHeadFrame(seatFrame.frame, worlds[i]);
                            targets[i]->m_anchor.recenterLocalToHead(viewPose, seat);
                        }
                        reseatGrouped = (int)targets.size();
                    }
                } else {
                    // WHICH offset gets planted is doc 03 §8.3 (xrReseatSource). A config-declared rig
                    // is head-relative by construction and re-seats to itself; anything the user
                    // actually placed — an `openxr create` monitor, or a declared one they grab-moved
                    // — re-seats to the offset captured while they were wearing the headset, which is
                    // the only form of its placement that outlives the reference space it was measured
                    // in. Planting the raw LOCAL pose (what an ad-hoc monitor's "declared" anchor
                    // holds) throws it as far as the old and new origins differ: 7.13 m in the session
                    // that produced this fix.
                    for (auto* l : targets) {
                        OpenXR::SXRAnchorState seat = l->m_declaredAnchor;
                        if (OpenXR::xrReseatSource(l->m_anchor.state().mode, l->m_restoreValid) == OpenXR::XR_RESEAT_RESTORED) {
                            seat.anchorPose = l->m_restoreOffset;
                            ++reseatRestored;
                        } else
                            ++reseatDeclared;
                        l->m_anchor.recenterLocalToHead(viewPose, seat);
                    }
                }
            }

            // doc 03 §8.3: re-capture every anchor:local monitor's DURABLE placement — its desk pose
            // expressed in the wearer's yaw-only floor frame. Deliberately AFTER the re-seat above, so
            // the first plug's capture reads the freshly seated pose rather than clobbering a good
            // stored offset with the dead-frame coordinates it is in the middle of replacing.
            //
            // Gated on m_restoreCapture (plugged AND wearing, publishRestoreCapture) so a headset on a
            // desk never gets to define "the room". Skipped while an adaptive monitor is anything but
            // DOCKED: its anchorPose is the saved desk pose while the user has walked away from it, so
            // measuring that against their current head would remember the walk, not the desk.
            if (viewValid && m_restoreCapture.load(std::memory_order_relaxed)) {
                const OpenXR::SXRPose headFrameInv = OpenXR::poseInverse(OpenXR::xrHeadFrame(viewPose));
                for (auto& l : m_layers) {
                    if (l->m_pendingRemoval.load(std::memory_order_acquire))
                        continue;
                    if (l->m_anchor.state().mode != OpenXR::XR_ANCHOR_LOCAL) {
                        // A non-LOCAL anchorPose is an offset in view/body/grip space, not a world
                        // pose; keeping a stale LOCAL capture around would be a lie if the monitor is
                        // ever switched back. The re-seat is a no-op for these modes either way.
                        l->m_restoreValid = false;
                        continue;
                    }
                    if (l->m_anchor.adaptiveEnabled() && l->m_anchor.adaptivePhase() != OpenXR::XRAD_DOCKED)
                        continue;
                    // A pose still bit-equal to the declaration is a RIG, not a placement: nothing has
                    // re-seated or moved this monitor since its `xrmonitor` line was applied, so it is
                    // literally sitting at "pos: relative to the runtime's origin" — the arbitrary spot
                    // §8.2 exists to rescue it from. Capturing that would remember the arbitrary spot
                    // and defeat the rescue next session. Skipping leaves whatever is already stored:
                    // an ad-hoc monitor's create-time seed (which IS head-derived), or nothing, in
                    // which case the re-seat falls back to the declared rig exactly as it should.
                    if (OpenXR::xrPoseIdentical(l->m_anchor.state().anchorPose, l->m_declaredAnchor.anchorPose))
                        continue;
                    l->m_restoreOffset = OpenXR::poseCompose(headFrameInv, l->m_anchor.state().anchorPose);
                    l->m_restoreValid  = true;
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
                    //
                    // WP X1 (research/24 §5.2): when this monitor is submitting a quad PAIR, the
                    // pixels each eye actually sees are one un-squeezed pane, not the whole mode, and
                    // the quad's height is derived from that aspect. This is the entire reason `sbs`
                    // and `hsbs` must be DECLARED rather than measured: they are the same pixels and
                    // ask for different shapes. `hsbs`/`htab` come back as the mode itself, so the
                    // common case (a half-packed 3D video on a 16:9 monitor) leaves geometry alone.
                    const auto PANEPX = Render::Stereo::presentedPaneSize(l->m_contentSize, layerDecl(*l).layout);
                    in.pxW       = (uint32_t)std::max(1.0, PANEPX.x);
                    in.pxH       = (uint32_t)std::max(1.0, PANEPX.y);
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
                    // Same pane-aspect rule as the solve path above (WP X1) — a tracking dropout
                    // must not silently un-pair the geometry and pop the quad's height. Through the
                    // shared derivation and the same truncated pane pixels solve() feeds itself, so
                    // a dropout does not move the published height by a rounding step either.
                    const auto HOLDPANEPX   = Render::Stereo::presentedPaneSize(l->m_contentSize, layerDecl(*l).layout);
                    results[i].heightMeters = OpenXR::quadHeightMeters(results[i].widthMeters, (uint32_t)std::max(1.0, HOLDPANEPX.x), (uint32_t)std::max(1.0, HOLDPANEPX.y));
                    solved[i]               = true;
                }
            }
        }
        // research/16 Part A: publish "any monitor roaming" for the AUTO hand-input gate (main-thread
        // status reads it too). Plain atomic — never a refcount op.
        m_anyRoaming.store(anyRoaming, std::memory_order_release);

        // doc 03 §8.3: say which way each anchor:local monitor was re-seated. `restored` is the
        // placement the user left, replayed around their current head; `declared` is the config rig
        // (or the fallback for a monitor that has never been placed under tracking). Outside the
        // layer lock, POD counters only.
        if (reseatUnseatable)
            Log::logger->log(Log::WARN,
                             "[OPENXR] re-seat: the monitor group has no common facing (their normals cancel) — there is no 'in front of' to bring it around to, so nothing moved");
        else if (reseatGrouped)
            Log::logger->log(Log::DEBUG, "[OPENXR] re-seated the arrangement of {} anchor:local monitor(s) onto the current head (rigid; relative layout preserved)", reseatGrouped);
        else if (reseatRan)
            Log::logger->log(Log::DEBUG, "[OPENXR] re-seated {} anchor:local monitor(s) to the head: {} restored from the last wearing, {} from their declared rig", reseatRestored + reseatDeclared,
                             reseatRestored, reseatDeclared);

        std::vector<XrCompositionLayerQuad>              quads;
        std::vector<const XrCompositionLayerBaseHeader*> layerPtrs;
        // WP X1: a stereo monitor submits a PAIR (one quad per eye), so the worst case is two per
        // layer. Over-reserving is free; under-reserving would only cost a realloc, but layerPtrs is
        // filled in a second pass precisely because these vectors may move, so keep both honest.
        quads.reserve(active.size() * 2);
        layerPtrs.reserve(active.size() * 2);

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

        // WP X1: "how many quads did this monitor cost" is reported in status, so it must mean THIS
        // frame. Zero every layer first and let the successful submissions below write 1 or 2 — a
        // layer that is skipped (no content yet, suspended by the layer cap, or a pair the budget
        // refused) then reads 0, "submitted nothing", instead of whatever it cost last time. The
        // budget-refusal case is the one that matters: it is invisible in the headset, and a stale
        // count would make it invisible in status too.
        for (auto& l : active)
            l->m_quadsSubmitted.store(0, std::memory_order_relaxed);

        for (size_t i : order) {
            auto& l = active[i];
            // WP X1: what the MAIN thread declared for this monitor (research/24 §5.1). CONTENT_OFF
            // is the ordinary one-quad path and is what every monitor in a session that has never
            // configured stereo reads, every frame, forever.
            const auto DECL         = layerDecl(*l);
            const auto STEREOLAYOUT = DECL.layout;
            const bool STEREOPAIR   = DECL.submit && OpenXR::Stereo::pairActive(STEREOLAYOUT);
            const bool DEPTHPACKED  = DECL.producer == OpenXR::Stereo::PRODUCER_DEPTH && l->m_paneGeom.panes >= 2;
            // The budget is checked for the WHOLE pair, never a quad at a time (§F2's cost note).
            // A pair that only half-fits is submitted left-eye-only — one eye sees the desktop and
            // the other sees nothing, which is not a degraded picture but a nauseating one. `continue`
            // rather than `break` so a cheaper monitor behind it can still take the last slot.
            if (!OpenXR::Stereo::submissionFits(quads.size(), m_session->m_maxLayerCount, DECL)) {
                if (quads.size() >= m_session->m_maxLayerCount)
                    break;
                continue;
            }
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
            // WP X1: a paired monitor shows ONLY its content rect — each eye's imageRect selects a
            // half of it, and the chrome margins are outside both. So the pair is submitted at
            // CONTENT meters on the CONTENT pose, which is simply what the anchor already solved;
            // the margin grow/shift below exists to keep content where the anchor put it, and with
            // no margins on screen there is nothing to correct for. Chrome is suppressed to match
            // (§5.1 option 2) — drawing a move-bar into pixels no eye can see would leave a monitor
            // that looks grabbable and is not.
            //
            // WP X4: a DEPTH-packed monitor keeps its chrome. Its swapchain holds two independently
            // margined panes, each eye's quad IS one of them, and the margin grow/shift below is the
            // same correction it always was — computed from per-pane meters, which is what the anchor
            // solved. Only the pure CONTENT pair (a client's packed frame, no margins inside the eye
            // rects) still flattens to an empty chrome.
            const OpenXR::SXRChromeGeometry  EMPTYCHROME{};
            const OpenXR::SXRChromeGeometry& chrome = (STEREOPAIR && !DEPTHPACKED) ? EMPTYCHROME : l->m_chrome;
            const float                      quadW  = res.widthMeters / (chrome.contentFracW() > 0.f ? chrome.contentFracW() : 1.f);
            const float                      quadH  = res.heightMeters / (chrome.contentFracH() > 0.f ? chrome.contentFracH() : 1.f);
            const OpenXR::SXRPose            quadCenterPose = OpenXR::contentPoseToQuadCenter(quadPose, chrome, quadW, quadH);
            // Cache the full quad meters for next frame's drawCursor (metric cursor sizing, report 14).
            l->m_quadWMeters = quadW;
            l->m_quadHMeters = quadH;

            // The content rect inside the swapchain, in imageRect's coordinate space: bottom-left,
            // the same flip blitBuffer computes as `contentGL`. For a mono layer this is the whole
            // image, exactly as before — paneImageRect is the identity on CONTENT_OFF.
            const int32_t                 contentW = (int32_t)std::max(0.0, l->m_contentSize.x);
            const int32_t                 contentH = (int32_t)std::max(0.0, l->m_contentSize.y);
            const OpenXR::Stereo::SImageRect CONTENTRECT = STEREOPAIR && !DEPTHPACKED && contentW > 0 && contentH > 0 ?
                OpenXR::Stereo::SImageRect{(int32_t)l->m_contentOffsetPx.x, h - (int32_t)l->m_contentOffsetPx.y - contentH, contentW, contentH} :
                OpenXR::Stereo::SImageRect{0, 0, w, h};

            XrCompositionLayerQuad quad   = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space                    = quadSpace;
            quad.subImage.swapchain       = l->m_swapchain;
            quad.subImage.imageArrayIndex = 0;
            quad.pose                     = xrFromPose(quadCenterPose);
            quad.size                     = {quadW, quadH};

            // THE PAIR (research/24 §5.1a, F2). Two quads at one pose, one image, no GL work at all:
            // the runtime's own sampler crops each eye to its half, which it was going to sample
            // anyway. Both members are pushed here, back to back, so the depth sort above — which
            // orders LAYERS, not quads — cannot separate a pair that shares a pose.
            //
            // Monado converts eyeVisibility for quads in its state tracker and consults it in BOTH
            // render backends; WiVRn's own squasher carries the same helper. If a runtime ever gets
            // this wrong, openxr:stereo_quad_pair flattens back to the single quad below.
            //
            // WP X3/X4: the DEPTH producer's rects are the swapchain's two HALVES (each a whole
            // margined pane), not two halves of a content rect — that is the only geometric
            // difference between the producers at submission time. And when the kill switch is off on
            // a packed monitor, ONE quad is submitted showing PANE 0 only: a mono desktop at the
            // right shape. Submitting the whole image instead would show a doubled side-by-side
            // picture, which is not a degradation anyone can work in.
            for (int eye = 0; eye < (STEREOPAIR ? 2 : 1); ++eye) {
                const auto RECT = DEPTHPACKED ? OpenXR::Stereo::paneFullRect(l->m_paneGeom, eye) : OpenXR::Stereo::paneImageRect(CONTENTRECT, STEREOLAYOUT, eye);
                quad.eyeVisibility = !STEREOPAIR ? XR_EYE_VISIBILITY_BOTH : (eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT);
                quad.subImage.imageRect = {{RECT.x, RECT.y}, {RECT.w, RECT.h}};
                quads.push_back(quad);
            }
            l->m_quadsSubmitted.store((uint8_t)(STEREOPAIR ? 2 : 1), std::memory_order_relaxed);

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
                // WP X1 §5.6: tell the hit tester this quad is showing one PANE, so the content uv it
                // hands the absolute-pointer path is un-mapped back into the whole packed image.
                // Without it, pointing at the middle of the quad drives the cursor to the seam
                // between the two eyes — half a screen away from where the user is pointing.
                pt.stereo    = DECL;
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

        // Publish surface-relative eye geometry only after input/gaze processing. A grab or carry
        // can begin in that pass; checking afterward prevents one last stable-looking sample from
        // escaping after the layer has entered any hand/device/gaze late-latched path.
        {
            std::scoped_lock lock(m_layersMu);
            for (size_t i = 0; i < active.size(); ++i) {
                auto&      layer        = active[i];
                const auto subscription = layer->viewpointSubscription();
                if (!subscription)
                    continue;

                if (layer->m_viewpointFrameToken != subscription->token) {
                    layer->m_viewpointFrameToken  = subscription->token;
                    layer->m_viewpointFrameSerial = 0;
                }

                bool valid = visible && viewpointViewsValid && solved[i] && layer->m_quadsSubmitted.load(std::memory_order_relaxed) == 2 &&
                    layer->m_anchor.state().mode == OpenXR::XR_ANCHOR_LOCAL && !layer->m_anchor.grabbed() && !layer->m_anchor.gazeGrabbed() &&
                    (!layer->m_anchor.adaptiveEnabled() || layer->m_anchor.adaptivePhase() == OpenXR::XRAD_DOCKED) && results[i].space == OpenXR::XR_SPACE_LOCAL_FLOOR &&
                    layer->m_viewpointFrameSerial != std::numeric_limits<uint64_t>::max();

                OpenXR::SXRViewpointEncodedSample sample;
                if (valid) {
                    OpenXR::SXRPose contentPose = results[i].worldPose;
                    if (!m_session->m_usingLocalFloor)
                        contentPose.pos.y -= floorOffset;
                    const std::array<OpenXR::Vec3, 2> viewPositions{
                        OpenXR::Vec3{viewpointViews[0].pose.position.x, viewpointViews[0].pose.position.y, viewpointViews[0].pose.position.z},
                        OpenXR::Vec3{viewpointViews[1].pose.position.x, viewpointViews[1].pose.position.y, viewpointViews[1].pose.position.z},
                    };
                    const auto geometry = OpenXR::surfaceRelativeViewpoint(contentPose, results[i].widthMeters, results[i].heightMeters, viewPositions, 2);
                    // The subscription's micrometres are what the client was told to render for, so
                    // they are what the sample carries — this thread does not round the rectangle a
                    // second time and hand the client a value one micrometre off the contract it is
                    // matching against. The interlock survives as an AGREEMENT test: a monitor whose
                    // mode or stereo declaration has genuinely moved under the subscription stops
                    // publishing (main will re-subscribe with the new shape), while two honest
                    // roundings of the SAME rectangle can no longer disagree their way into a
                    // permanently inactive viewpoint.
                    valid = OpenXR::encodeViewpointSample(geometry, ++layer->m_viewpointFrameSerial, subscription->geometryId, subscription->widthUM, subscription->heightUM,
                                                          sample) &&
                        OpenXR::viewpointDimensionAgrees(geometry.widthMeters, subscription->widthUM) && OpenXR::viewpointDimensionAgrees(geometry.heightMeters, subscription->heightUM);
                }

                const auto PREVIOUS   = sc<OpenXR::eXRViewpointRuntimeState>(layer->m_viewpointRuntimeState.load(std::memory_order_acquire));
                const auto TRANSITION = OpenXR::viewpointRuntimeTransition(PREVIOUS, valid);
                if (TRANSITION.changed) {
                    layer->m_viewpointRuntimeState.store(TRANSITION.state, std::memory_order_release);
                    enqueue(SXRStateEvent{.type = eXRStateEventType::VIEWPOINT_STATE, .a = valid ? 1 : 0, .str = layer->m_monitorName});
                }
                if (!valid)
                    continue;

                const auto published = layer->publishViewpointSample(*subscription, sample);
                if (published.shouldWake)
                    wakeMain();
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

// FRAME THREAD (research/24 WP X1/X3). The published declaration, checked against the image the
// layer is actually holding. Everything the frame thread derives from the declaration — the split,
// the pane rects, and the ASPECT the anchor solve turns into quad metres — has to come through
// here, or a monitor mid-mode-change gets its geometry from the new declaration and its pixels from
// the old image and pops a frame. A declaration that does not describe the image degrades to one
// quad: pane 0 when the image is itself two panes (submitting the whole thing would put a
// side-by-side picture in both eyes), the whole image otherwise.
OpenXR::Stereo::SPairDecl COpenXRManager::layerDecl(const CXRMonitorLayer& layer) {
    const auto DECL = OpenXR::Stereo::unpackDecl(layer.m_stereoPairDecl.load(std::memory_order_acquire));
    if (OpenXR::Stereo::describes(DECL, (int32_t)layer.m_contentSize.x, (int32_t)layer.m_contentSize.y))
        return DECL;

    return layer.m_paneGeom.panes >= 2 ? OpenXR::Stereo::SPairDecl{.producer = OpenXR::Stereo::PRODUCER_DEPTH, .layout = Render::Stereo::CONTENT_SBS, .submit = false} :
                                         OpenXR::Stereo::SPairDecl{};
}

bool COpenXRManager::createLayerSwapchain(CXRMonitorLayer& layer, const Vector2D& size, int panes) {
    // Frame thread. Destroy any existing swapchain first (context NOT current — interop rule).
    if (layer.m_swapchain != XR_NULL_HANDLE)
        layer.destroySwapchain();

    // WP X4: `size` is the monitor's whole pixel mode. On a depth-packed monitor that mode holds TWO
    // panes side by side, and each of them needs its OWN chrome ring — one ring around the pair would
    // put the left eye's right-hand margin inside the right eye's picture. So the chrome geometry
    // below is computed for ONE pane and the swapchain is two of those. panes == 1 for every ordinary
    // monitor, and then every expression here is exactly what shipped.
    const int      PANES = std::clamp(panes, 1, 2);
    const Vector2D PANECONTENT{std::max(1.0, size.x / PANES), std::max(1.0, size.y)};

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
    const float                     cH     = cW * (float)PANECONTENT.y / (float)PANECONTENT.x;
    const OpenXR::SXRChromeGeometry chrome = OpenXR::makeChromeGeometry(cW, cH, margin, barH, (float)*PBARWF, (float)*PCORNER);

    // One pane's full px = its content px expanded by the same fractions (px/fraction stay
    // consistent); the swapchain is `panes` of those laid out horizontally.
    const double            fw = chrome.contentFracW() > 0.0 ? (double)chrome.contentFracW() : 1.0;
    const double            fh = chrome.contentFracH() > 0.0 ? (double)chrome.contentFracH() : 1.0;
    OpenXR::Stereo::SPaneGeom paneGeom;
    paneGeom.panes       = PANES;
    paneGeom.paneContent = PANECONTENT;
    paneGeom.paneFull    = {std::max(PANECONTENT.x, std::round(PANECONTENT.x / fw)), std::max(PANECONTENT.y, std::round(PANECONTENT.y / fh))};
    paneGeom.contentOffsetPx = {std::round(chrome.contentU0 * paneGeom.paneFull.x), std::round(chrome.contentV0 * paneGeom.paneFull.y)};

    const Vector2D fullSize      = OpenXR::Stereo::swapchainSizeFor(paneGeom);
    const Vector2D contentOffset = paneGeom.contentOffsetPx;

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
    layer.m_paneGeom        = paneGeom;
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
        st.mode        = OpenXR::XR_ANCHOR_LOCAL;
        const auto ctx = currentVerbContext();
        tmp.initFromState(st);
        if (tmp.applyCenter(ctx, dist)) {
            st = tmp.m_state;
            // doc 03 §8.3: this pose was DERIVED from the head, so its durable head-relative form is
            // known right now — seed it, and an ad-hoc monitor is restorable from its very first
            // frame, even if the session dies before the monitors are ever plugged (the frame-thread
            // capture would never have run). An EXPLICIT anchor is deliberately left unseeded: a
            // caller-supplied `pos:` is a declared rig, and the declared path already reinterprets it
            // as head-relative — measuring where it currently sits would instead make a monitor the
            // user cannot see (declared into an arbitrary origin) permanently unreachable.
            layer->m_restoreOffset = OpenXR::xrPoseInHeadFrame(ctx.view, st.anchorPose);
            layer->m_restoreValid  = true;
        } else
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

    // report 12 §4 per-monitor opt-out: capture whether an explicit user
    // `monitor=NAME,...,<x>x<y>,...` rule owns POSITION (and likewise MODE) before registering our
    // durable rule. Such a monitor is excluded from the projection. Field provenance, rather than
    // the numeric value alone, keeps a same-name output recreation from promoting the sync engine's
    // old offset to a user pin. refreshMonitorRuleOwnership repeats this on config reload.
    if (Config::monitorRuleMgr()) {
        const auto USERRULE       = Config::monitorRuleMgr()->get(mon);
        layer->m_l2dUserPinned    = USERRULE.m_offset != Vector2D{-INT32_MAX, -INT32_MAX} && !USERRULE.m_offsetOwnedByXR;
        layer->m_userProvidedMode = USERRULE.m_resolution != Vector2D{} && !USERRULE.m_resolutionOwnedByXR;
        if (layer->m_l2dUserPinned)
            Log::logger->log(Log::DEBUG, "[OPENXR] XR monitor '{}' is pinned by an explicit monitor= offset — excluded from 2D-plane sync", params.m_name);
        if (params.m_resolution && layer->m_userProvidedMode)
            Log::logger->log(Log::DEBUG, "[OPENXR] XR monitor '{}' keeps its explicit monitor= resolution", params.m_name);
    }

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
    //    The registration is NOT gated on a requested mode: it also carries this monitor's default
    //    scale (task #129), which a mode-less create needs just as much.
    if (Config::monitorRuleMgr()) {
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

    // 9. doc 05 §xrrule: resolve the new monitor's transparency effects — its name and anchor state
    //    may match rules the moment it exists. Coalesced, so a config declaring five monitors still
    //    costs a single evaluation pass.
    requestEffectEval();

    // 10. report 12 §4: the monitor SET changed, so the 2D plane must be re-derived. Debounced, so a
    //     config declaring five monitors still costs a single relayout — and it lands after all five
    //     exist, which is what makes the projection see the whole arrangement instead of a prefix.
    requestLayout2DSync();

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

    // Path B (external output destruction) has already expired m_monitor, so rule cleanup must be
    // name-based and unconditional. The generated rule itself uses this exact output name.
    releaseLayout2DRuleOffset(name);
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

    // report 12 §4: the set changed. Without this the survivors keep the offsets that were computed
    // when the departed monitor still occupied a column, leaving a hole in the middle of the plane.
    requestLayout2DSync();

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
    // Precedence: an explicit user `monitor=NAME,<mode>,...` wins — tracked as
    // layer->m_userProvidedMode, so we never clobber it. Building the rule from get(mon) preserves
    // every user-set field we do not deliberately own (mode, offset, and — task #129 — scale).
    // add() replaces our own prior rule by name (idempotent) and schedules an ensureMonitorStatus
    // pass to apply it.
    if (!mon || !layer || !Config::monitorRuleMgr())
        return;
    const bool wantMode = layer->m_reqResolution && !layer->m_userProvidedMode;
    // report 12 WP-S2 durability: once the 2D-plane sync owns this monitor's POSITION, the offset
    // rides the persistent rule too. syncLayout2D writes m_activeMonitorRule.m_offset directly (the
    // light path — no rule-manager traffic, no mode re-apply), but a rule refresh from any other
    // source re-derives m_activeMonitorRule from the rule MANAGER, which would otherwise hand back
    // the {-INT32_MAX,-INT32_MAX} "auto" sentinel and drop us back to append-right.
    const bool           wantOffset  = layer->m_l2dPlaced;
    Config::CMonitorRule rule        = Config::monitorRuleMgr()->get(mon);
    const bool           clearMode   = !layer->m_reqResolution && rule.m_resolutionOwnedByXR;
    const bool           clearOffset = !wantOffset && rule.m_offsetOwnedByXR;

    // A same-name output with no requested mode belongs on the headless preferred/default rung,
    // not on the previous incarnation's XR-owned request. Clear before deriving depthPane and
    // effectiveMode below so stereo/depth policy cannot accidentally adopt that stale pane.
    if (clearMode) {
        rule.m_resolution          = Vector2D{};
        rule.m_resolutionOwnedByXR = false;
        rule.m_refreshRate         = 60.F;
    }

    // task #129: the same durability, for SCALE. A headless output has no EDID, so getDefaultScale()'s
    // PPI heuristic reads an XR quad as a tiny dense panel and picks 2.0 — cramped through a headset,
    // and a monitor a voice command or keybind minted seconds ago has no `monitor =` line to correct
    // it. Fill in openxr:default_monitor_scale when nothing else owns the field.
    //
    // Who owns it is asked of the rules that NAME this output (xrPinnedRuleScale), not of
    // rule.m_scale: get() falls back to the nameless catch-all almost every config carries, and
    // reading that back would disable the default for everyone who has one. A wlr-output-management
    // override is not consulted either, and needs not be — get() re-applies it on top of whatever we
    // store, every time, so a display GUI still wins.
    //
    // Re-deciding from the live rules on every call keeps precedence live in both directions: a
    // reload clears the rule manager, so a scale the user ADDS later — or a retuned default — lands
    // on the next reassert instead of being shadowed by a stale capture. XR-owned scales carry
    // provenance and are skipped, so same-name output recreation also re-derives the default.
    static auto PDEFAULTSCALE = CConfigValue<Hyprlang::FLOAT>("openxr:default_monitor_scale");
    static auto PNOSCALECHECK = CConfigValue<Hyprlang::INT>("debug:disable_scale_checks");
    const float PINNEDSCALE   = OpenXR::xrPinnedRuleScale(Config::monitorRuleMgr()->all(), [&mon](const std::string& sel) { return mon->matchesStaticSelector(sel); });

    // THE DEPTH DESKTOP (research/24 §6, WP X3). An XR monitor that composites once per eye scans out
    // a PAIR of panes, so its pixel mode is its declared size doubled — and the declared size keeps
    // meaning exactly what it meant, which is the per-eye desktop the user asked for. That inversion
    // is `m_stereoVirtualMode`: the rule's resolution is ONE PANE and Monitor::Stereo::requestedMode
    // derives the mode from it. (The `monitor = …, stereo:` token means the opposite — a panel names
    // its mode and the logical size is halved out of it — which is why this deliberately does not go
    // through that token. Halving a declared XR size would shrink every XR desktop the moment depth
    // engaged.)
    //
    // Only monitors WE created. An `xrmonitor`-adopted real output has a panel, a mode list and a
    // user who chose them; doubling its mode is not ours to do — the same line xrDefaultMonitorScale
    // draws, for the same reason.
    // A virtual pack DERIVES its mode from a per-pane resolution, so it needs one. `preferred` and
    // the highrr/highres/maxwidth sentinels name no resolution, and packing on top of whatever the
    // mode search then picks would halve that desktop — the regression the inversion exists to
    // prevent. When nothing names one, adopt the pane the monitor is already running (for a
    // headless output that is simply its current mode); if even that is unknown, decline.
    static auto PDEPTHDESKTOP = CConfigValue<Hyprlang::INT>("openxr:depth_desktop");
    Vector2D    depthPane     = wantMode && layer->m_reqResolution ? *layer->m_reqResolution : rule.m_resolution;
    if (depthPane.x <= 0 || depthPane.y <= 0) {
        // createOutput() applies the named rule synchronously. On same-name reuse that means the
        // monitor's current pane can still describe the previous XR-owned request even though we
        // just handed that request back above. Prefer the backend's actual default in that one
        // handback case so depth packing cannot immediately claim the stale pane again.
        const auto preferred = clearMode && mon->m_output ? mon->m_output->preferredMode() : nullptr;
        depthPane            = preferred ? preferred->pixelSize : mon->paneSize();
    }
    const bool WANTDEPTH  = *PDEPTHDESKTOP != 0 && layer->m_createdByXR && depthPane.x > 0 && depthPane.y > 0;
    const auto WANTSTEREO = WANTDEPTH ? Config::STEREO_SBS : Config::STEREO_OFF;
    // A user who wrote an explicit `stereo:` token on this output keeps it when depth is off; depth
    // on takes over, because a virtual pack and a physical one are not composable.
    const bool STEREOCHANGED = WANTDEPTH ? (rule.m_stereo != WANTSTEREO || !rule.m_stereoVirtualMode) : rule.m_stereoVirtualMode;
    // The mode the output will run once this rule lands — the divisor gate needs it. What we are
    // about to ask for, else what the rule asks for, else what it is scanning out now: `preferred` /
    // `highres` / `maxwidth` leave m_resolution zeroed or at a negative sentinel, and the mode those
    // resolve to is precisely the one the monitor already has. Zero everywhere = not knowable, and
    // xrDefaultMonitorScale declines rather than guessing.
    Vector2D effectiveMode = wantMode ? *layer->m_reqResolution : rule.m_resolution;
    // paneSize(), not m_pixelSize: applyMonitorRule validates pane/scale, so proving the divisor on
    // a packed mode would prove the wrong quantity. Identical off a pack.
    if (effectiveMode.x <= 0 || effectiveMode.y <= 0) {
        const auto preferred = clearMode && mon->m_output ? mon->m_output->preferredMode() : nullptr;
        effectiveMode        = preferred ? preferred->pixelSize : mon->paneSize();
    }
    // `stereoOutput` asks "is this a PHYSICAL per-eye panel", because that is the only case whose
    // scale must be pinned to 1.0. A virtual pack has no physical pixel grid, its pane IS the
    // declared size, and openxr:default_monitor_scale still divides that pane cleanly — so the XR
    // scale default keeps applying and turning depth on does not reflow the session.
    const auto WANTSCALE  = OpenXR::xrDefaultMonitorScale(layer->m_createdByXR, OpenXR::xrRuleScaleIsExplicit(PINNEDSCALE), WANTSTEREO != Config::STEREO_OFF && !WANTDEPTH,
                                                          effectiveMode, *PNOSCALECHECK, (float)*PDEFAULTSCALE);
    const bool clearScale = !WANTSCALE && rule.m_scaleOwnedByXR;

    if (!wantMode && !clearMode && !wantOffset && !clearOffset && !WANTSCALE && !clearScale && !STEREOCHANGED)
        return;
    rule.m_name = mon->m_name;
    if (WANTDEPTH) {
        rule.m_stereo            = WANTSTEREO;
        rule.m_stereoVirtualMode = true;
        // ...and NAME the pane, so the derivation has something to derive from even when the rule
        // only asked for `preferred`. Writing it is what makes the pack expressible at all.
        rule.m_resolution = depthPane;
        if (!layer->m_userProvidedMode)
            rule.m_resolutionOwnedByXR = true;
    } else if (rule.m_stereoVirtualMode) {
        // Ours to clear, and only ours: a physical `stereo:` token never sets the virtual flag.
        rule.m_stereo            = Config::STEREO_OFF;
        rule.m_stereoVirtualMode = false;
    }
    if (wantMode) {
        rule.m_resolution          = *layer->m_reqResolution;
        rule.m_resolutionOwnedByXR = true;
        rule.m_refreshRate         = layer->m_reqRefresh.value_or(60.F);
    }
    if (wantOffset) {
        rule.m_offset          = layer->m_l2dOffset;
        rule.m_offsetOwnedByXR = true;
    } else if (clearOffset) {
        rule.m_offset          = Vector2D{-INT32_MAX, -INT32_MAX};
        rule.m_offsetOwnedByXR = false;
    }
    if (WANTSCALE) {
        rule.m_scale          = *WANTSCALE;
        rule.m_scaleOwnedByXR = true;
    } else if (clearScale) {
        rule.m_scale          = -1.F;
        rule.m_scaleOwnedByXR = false;
    }
    // Keep the output ENABLED in the rule — the unplug lifecycle is driven separately through
    // onConnect/onDisconnect + the m_xrManagedPlug guard (issue A), not the rule's disabled bit.
    rule.m_disabled = false;
    Config::monitorRuleMgr()->add(std::move(rule));
}

void COpenXRManager::reassertMonitorModeRules() {
    // Main thread, on the config-reload edge. CConfigManager::reload() clears the rule manager and
    // re-parses the file, which drops the mode rules we installed; reconcileDeclaredMonitors()
    // re-installs them only for `xrmonitor =` DECLARED layers (by design — runtime-created monitors
    // are never touched by declaration reconciliation, doc 05 §2.5). That left the mode of a
    // `hyprctl openxr create NAME 2560x1440@60` monitor un-owned after any reload: the user's
    // `monitor = NAME, preferred` line (or the headless preferred mode) took it back to 1920x1080,
    // which is what the live 2026-08-01 report saw. Re-install for every live layer;
    // registerDeclaredMonitorRule() itself skips layers whose mode the user pinned.
    //
    // The reload also drops the default SCALE we own (task #129), and re-deriving it here is what
    // lets a retuned openxr:default_monitor_scale — or a scale the user has just added to their
    // config — land on monitors that already exist. So this walks every live layer now, not only the
    // ones that requested a mode.
    //
    // add() schedules an ensureMonitorStatus() pass, and that pass compares before applying, so this
    // costs no modeset when the effective mode is already right.
    if (!Config::monitorRuleMgr())
        return;

    std::vector<std::pair<PHLMONITOR, PXRLAYER>> pending;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (!l || l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;
            if (auto mon = l->m_monitor.lock())
                pending.emplace_back(mon, l);
        }
    }
    // Outside m_layersMu: the rule manager runs listeners of its own.
    for (auto& [mon, layer] : pending)
        registerDeclaredMonitorRule(mon, layer);
}

void COpenXRManager::refreshMonitorRuleOwnership() {
    // MAIN THREAD, immediately after a config/keyword reparse. Snapshot the monitor handles without
    // holding m_layersMu across rule-manager lookups; get() consults output-management state and may
    // run independently-owned code.
    if (!Config::monitorRuleMgr())
        return;

    struct SRuleOwnership {
        PXRLAYER   layer;
        PHLMONITOR mon;
        bool       userPosition = false;
        bool       userMode     = false;
    };
    std::vector<SRuleOwnership> ownership;
    {
        std::scoped_lock lock(m_layersMu);
        ownership.reserve(m_layers.size());
        for (auto& l : m_layers) {
            if (!l || l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;
            if (auto mon = l->m_monitor.lock())
                ownership.push_back({l, mon});
        }
    }

    for (auto& o : ownership) {
        const auto rule = Config::monitorRuleMgr()->get(o.mon);
        o.userPosition  = rule.m_offset != Vector2D{-INT32_MAX, -INT32_MAX} && !rule.m_offsetOwnedByXR;
        o.userMode      = rule.m_resolution != Vector2D{} && !rule.m_resolutionOwnedByXR;
    }

    bool layoutOwnershipChanged = false;
    {
        std::scoped_lock lock(m_layersMu);
        for (const auto& o : ownership) {
            if (o.layer->m_l2dUserPinned != o.userPosition) {
                Log::logger->log(Log::DEBUG, "[OPENXR] XR monitor '{}' {} explicit monitor= position ownership", o.layer->m_monitorName, o.userPosition ? "gained" : "released");
                layoutOwnershipChanged = true;
            }
            o.layer->m_l2dUserPinned    = o.userPosition;
            o.layer->m_userProvidedMode = o.userMode;
            if (o.userPosition)
                o.layer->m_l2dPlaced = false;
        }
    }

    if (layoutOwnershipChanged)
        requestLayout2DSync();
}

void COpenXRManager::onMonitorRulesChanged() {
    // MAIN THREAD. The legacy dynamic-monitor keyword updates the rule manager synchronously but
    // emits neither config.reloaded nor config.props_refreshed. Keep this deliberately narrower
    // than onConfigReload(): only ownership and the durable monitor-rule fields depend on it.
    refreshMonitorRuleOwnership();
    reassertMonitorModeRules();
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

OpenXR::eXRRecenterPolicy COpenXRManager::recenterPolicy() const {
    // MAIN THREAD (status + the RECENTERED dispatch). Reads the STRING config directly, which is
    // legal here and forbidden on the frame thread — that side gets m_recenterPolicy instead.
    static auto PRECENTER = CConfigValue<std::string>("openxr:recenter");
    return OpenXR::parseRecenterPolicy(*PRECENTER);
}

std::string COpenXRManager::recenterPolicyName() const {
    return OpenXR::recenterPolicyName(recenterPolicy());
}

void COpenXRManager::publishRecenterPolicy() {
    // MAIN-THREAD ONLY. Same publish pattern (and the same reason) as publishGrabStringTuning:
    // openxr:recenter is a STRING and the frame thread must never deref it. Called from start() +
    // onConfigReload() (+ the legacy-keyword special-case in ConfigManager::parseKeyword, since a
    // bare `hyprctl keyword openxr:recenter follow` fires neither reloaded nor props_refreshed —
    // and being able to flip this from inside the headset is most of the point of having it).
    static auto      PRECENTER = CConfigValue<std::string>("openxr:recenter");
    const auto       policy    = OpenXR::parseRecenterPolicy(*PRECENTER);
    const auto       prev      = (OpenXR::eXRRecenterPolicy)m_recenterPolicy.exchange((uint8_t)policy, std::memory_order_relaxed);
    if (prev != policy)
        Log::logger->log(Log::DEBUG, "[OPENXR] recenter policy: {} -> {}", OpenXR::recenterPolicyName(prev), OpenXR::recenterPolicyName(policy));
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
    m_reseatArmed.store(OpenXR::XR_RESEAT_ARM_NONE, std::memory_order_release);
    // doc 03 §8.3: no session, nothing to capture from. The per-layer offsets themselves deliberately
    // survive — they are what the NEXT session restores the room from.
    publishRestoreCapture();
}

// ---- reload-storm containment (live evidence 2026-08-21). Main thread only. ----

bool COpenXRManager::probeInputsChanged() {
    // MAIN THREAD ONLY — reads STRING config values (the frame thread must never; see task #25).
    //
    // Every config value a bring-up ATTEMPT depends on, and nothing else. The dozens of hot-tunable
    // knobs onConfigReload() re-publishes above (chrome, grab, adaptive, black_alpha, ...) are
    // deliberately absent: changing one of those has no bearing on whether an OpenXR runtime is
    // there to talk to, so it must not buy a probe.
    static auto PENABLED  = CConfigValue<Hyprlang::INT>("openxr:enabled");
    static auto PGPU      = CConfigValue<std::string>("openxr:gpu");
    static auto PRTJSON   = CConfigValue<std::string>("openxr:runtime_json");
    static auto PBLEND    = CConfigValue<std::string>("openxr:blend_mode");
    static auto POVERLAY  = CConfigValue<Hyprlang::INT>("openxr:overlay");
    static auto POVERLAYZ = CConfigValue<Hyprlang::INT>("openxr:overlay_z");
    static auto PTAINT    = CConfigValue<Hyprlang::INT>("openxr:ignore_kernel_taint");
    static auto PREPROBE  = CConfigValue<Hyprlang::INT>("openxr:reprobe");
    static auto PBASE     = CConfigValue<Hyprlang::INT>("openxr:reprobe_interval_ms");
    static auto PWATCH    = CConfigValue<Hyprlang::INT>("openxr:reprobe_watch");

    // Unit-separator joined so no value can forge a boundary out of its own content.
    const std::string sig = std::format("{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}", (int)*PENABLED, *PGPU, *PRTJSON, *PBLEND, (int)*POVERLAY, (int64_t)*POVERLAYZ,
                                        (int)*PTAINT, (int)*PREPROBE, (int64_t)*PBASE, (int)*PWATCH);

    if (sig == m_lastProbeInputs)
        return false;

    // First call of the session establishes the baseline. Report it as "changed" so the very first
    // reload after startup still behaves exactly as it always did.
    const bool first  = m_lastProbeInputs.empty();
    m_lastProbeInputs = sig;
    if (!first)
        Log::logger->log(Log::DEBUG, "[OPENXR] probe-relevant config changed — this reload may start a fresh attempt");
    return true;
}

void COpenXRManager::userDisable() {
    // STICKY (live evidence 2026-08-21): `hyprctl openxr disable` used to call stop() and nothing
    // else, so the next config reload read `openxr:enabled = 1`, saw DISABLED, and started right
    // back up. Under the observed 1 Hz reload storm the disable was undone within a second and the
    // user could not turn the thing off at all. The latch outranks openxr:enabled until an explicit
    // `enable` (or a change to openxr:enabled itself) clears it.
    m_manualDisable = true;
    Log::logger->log(Log::DEBUG, "[OPENXR] user disable — sticky across reloads until `hyprctl openxr enable`");
    stop();
}

void COpenXRManager::userEnable() {
    m_manualDisable = false;
    // Explicit user intent: bypass the reload gates and the probe floor entirely.
    m_lastProbeAt.reset();
    start();
}

bool COpenXRManager::manuallyDisabled() const {
    return m_manualDisable;
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
    // Reload-storm containment: an arm request may only push out an ALREADY-PENDING probe if it
    // would fire sooner. Belt-and-braces behind the onConfigReload() gate — whatever else learns to
    // call this, a stream of re-arms can no longer walk the deadline forward forever.
    if (m_reprobeTimer && m_reprobeTimer->armed()) {
        const float   leftUs = m_reprobeTimer->leftUs();
        const int64_t leftMs = leftUs <= 0.f ? 0 : (int64_t)(leftUs / 1000.f);
        if (!OpenXR::xrShouldRearmReprobe(true, leftMs, (int64_t)std::max(0, ms))) {
            Log::logger->log(Log::DEBUG, "[OPENXR] re-probe already pending in {}ms — not pushing it out to {}ms", leftMs, ms);
            return;
        }
    }
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

    // doc 03 §8.3: re-derive the cross-session placement capture gate here, because this funnel is
    // where every input to it lands — the session-state edge (visibility), the don/doff edge, and
    // start/stop/reload. Deliberately BEFORE the plug decision below and independent of it: whether
    // the headless outputs follow the session is a policy the user can turn off, while whether they
    // are wearing the headset is not.
    publishRestoreCapture();

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
                // RESTORE (doc 03 §8.4): this is the discontinuity case — a brand-new session whose
                // LOCAL_FLOOR is a frame nothing in the room has ever been measured against, so the
                // stored head-relative offsets are the only usable description of the arrangement.
                armReseat(OpenXR::XR_RESEAT_ARM_RESTORE);
                // report 12 §3a: a recenter re-seats every anchor:local monitor around the current
                // head, so the desk orientation the 2D projection measures against must be re-taken
                // too — otherwise the plane would describe the arrangement that existed before the
                // re-seat. Dropping it (rather than latching now) makes the next sync capture the
                // head pose AFTER the frame thread has actually applied the re-seat.
                m_l2dRef = OpenXR::SXRLayout2DRef{};
                m_l2dPrev.clear();
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

        // report 12 §4: a plug edge changes WHICH monitors are in the layout, so the plane has to be
        // re-derived — an unplugged monitor must not keep a reserved column (a hole the cursor falls
        // into), and a re-plugged one must get its spatial slot back rather than land append-right
        // via the ordinary hotplug path. Debounced, so donning the headset with six monitors costs
        // one relayout.
        requestLayout2DSync();
    }

}

void COpenXRManager::publishRestoreCapture() {
    // doc 03 §8.3. Capture only from frames the user is really WEARING the headset — the frames after
    // a doff come from a headset lying on a desk, and preserving THAT arrangement into the next
    // session would be worse than the stale-coordinate bug this fixes.
    //
    // VISIBILITY is the signal, not the monitor plug state. Plugging is a policy
    // (openxr:monitors_follow_session) that the user may switch off entirely, and m_monitorsPlugged
    // then stays false forever while the headset is worn and the quads are perfectly visible — gating
    // on it disabled this whole feature under `off`, silently, which is how the container caught it.
    // VISIBLE/FOCUSED means the runtime is compositing our frames, which is exactly the condition
    // `visible` mode itself keys the plug on, and it drops on doff before the unplug grace does.
    //
    // Presence refines it when the runtime has XR_EXT_user_presence: a doff is reported the moment the
    // proximity sensor opens, ahead of the visibility drop. Presence SUPPORTED but not yet KNOWN
    // counts as wearing — refusing to capture until the first don event would leave a freshly visible
    // session with no durable placement at all.
    const bool wearing = !m_userPresenceSupported || !m_presenceKnown || m_userPresent;
    const bool want    = sessionVisible() && wearing;
    if (m_restoreCapture.exchange(want, std::memory_order_relaxed) == want)
        return;
    // Worth a line: this gate is the difference between "your layout will come back" and "it will be
    // re-seated from the config", and `hyprctl openxr status` only shows the resulting per-monitor
    // state. Say which of the two inputs closed it.
    Log::logger->log(Log::DEBUG, "[OPENXR] cross-session placement capture {} (session {}, user {})", want ? "ON" : "OFF", sessionVisible() ? "visible" : "not visible",
                     !m_userPresenceSupported ? "presence unsupported" : (!m_presenceKnown ? "presence unknown" : (m_userPresent ? "present" : "absent")));
}

bool COpenXRManager::restoreCaptureActive() const {
    return m_restoreCapture.load(std::memory_order_relaxed);
}

void COpenXRManager::armReseat(OpenXR::eXRReseatKind kind) {
    // Either thread. The enum is ordered by precedence (NONE < GROUP < RESTORE) so "the stronger
    // pending kind wins" is a max, done with a CAS loop because std::atomic::fetch_max is C++26.
    // Both producers are rare events (a plug, a recenter, a keypress), so the loop cannot spin.
    if (kind == OpenXR::XR_RESEAT_ARM_NONE)
        return;
    uint8_t cur = m_reseatArmed.load(std::memory_order_acquire);
    while (cur < (uint8_t)kind && !m_reseatArmed.compare_exchange_weak(cur, (uint8_t)kind, std::memory_order_acq_rel, std::memory_order_acquire))
        ; // cur is reloaded by the failed exchange
}

std::expected<std::string, std::string> COpenXRManager::requestReseatToHead(const char* why) {
    // MAIN THREAD. The deliberate re-seat (doc 03 §8.4). Everything here is bookkeeping + arming:
    // the re-seat ITSELF is the frame thread's, because only it locates the head, and asking main to
    // do the geometry would mean either duplicating recenterLocalToHead against a stale ring sample
    // or reaching for a head pose from the wrong thread. So this arms exactly the flag the first
    // plug arms, and the frame loop runs exactly the loop it already runs.
    //
    // The head sample the DECISION is made from is the ring's newest (newestPoseSample) — published
    // by the frame thread once per frame, POD, no refcounts. It is not the pose that gets planted
    // (the frame thread re-locates for that, a frame fresher); it is only the evidence that there
    // IS a live head to re-seat to, which is what turns "nothing happened" into a sentence.
    OpenXR::SXRPoseSample head{};
    const bool            haveHead = newestPoseSample(head);
    const int64_t         ageMs    = haveHead ? (int64_t)Time::millis(Time::steadyNow()) - head.timestampMs : -1;

    // The eligible set, under the same lock every other main-thread anchor read uses, and through
    // the same predicate the frame loop applies — one definition of "which monitors move". The
    // world poses come along so the answer below can be exact rather than optimistic.
    std::vector<OpenXR::SXRPose> worlds;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (OpenXR::xrReseatEligible(l->m_anchor.state().mode, l->m_pendingRemoval.load(std::memory_order_acquire)))
                worlds.push_back(l->m_anchor.state().anchorPose);
    }
    const int eligible = (int)worlds.size();

    switch (OpenXR::xrReseatBlock(sessionExists(), haveHead, head.viewValid, ageMs, eligible)) {
        case OpenXR::XR_RESEAT_NO_SESSION: return std::unexpected<std::string>("no OpenXR session — nothing to re-seat");
        case OpenXR::XR_RESEAT_NO_HEAD:
            return std::unexpected<std::string>(!haveHead ? "no head pose yet — the session has not rendered a frame" :
                                                            std::format("no live head pose ({}, last sample {}ms old) — put the headset on and try again",
                                                                        head.viewValid ? "tracking stale" : "tracking lost", ageMs));
        case OpenXR::XR_RESEAT_NO_MONITORS: return std::unexpected<std::string>("no anchor:local monitors — head/body/device-anchored monitors already follow you");
        case OpenXR::XR_RESEAT_READY: break;
    }

    // Answer the degenerate arrangement HERE rather than letting the user press a key, see nothing
    // happen, and go looking in the log for the frame thread's WARN. Validity depends only on the
    // monitors' normals, not on the head, so this verdict is exactly the one the frame thread will
    // reach with its own (fresher) head pose a frame from now.
    if (!OpenXR::xrGroupSeatFrame(worlds.data(), worlds.size(), OpenXR::SXRPose{head.headPos, head.headRot}).valid)
        return std::unexpected<std::string>("your monitors have no common facing (they surround you) — there is no 'in front of' to bring them round to");

    // Arm the GROUP re-seat (doc 03 §8.4) — NOT the RESTORE one the first plug arms. Everything here
    // is live and continuous: the §8.3 capture has been re-deriving every stored offset against the
    // current head all along, so replanting one around that same head would hand each monitor its own
    // pose straight back. The live arrangement is the description that means something, and moving it
    // rigidly onto the head is the operation the user is asking for.
    //
    // Not gated on openxr:recenter_on_plug: that option is permission for the compositor to move
    // monitors on its OWN initiative, and this is the user asking. It also does not touch
    // m_recenteredThisSession — that gate exists so a doff-and-don does not re-seat, and an explicit
    // request is not a don.
    armReseat(OpenXR::XR_RESEAT_ARM_GROUP);

    // report 12 §3a / doc 03 §8.2: the group is about to be replanted around a new head, so the
    // latched desk orientation the 2D projection measures against describes an arrangement that is
    // about to stop existing. Drop it rather than re-latch now — the next sync then captures the
    // head pose AFTER the frame thread has actually applied the re-seat.
    m_l2dRef = OpenXR::SXRLayout2DRef{};
    m_l2dPrev.clear();
    requestLayout2DSync();

    Log::logger->log(Log::DEBUG, "[OPENXR] re-seat requested ({}) — {} anchor:local monitor(s) will be replanted around the current head", why, eligible);
    return std::format("re-seated {} monitor{} to the current head", eligible, eligible == 1 ? "" : "s");
}

std::expected<std::string, std::string> COpenXRManager::cmdReseat() {
    return requestReseatToHead("xrmonitor reseat");
}

void COpenXRManager::notifyRecentered() {
    // FRAME THREAD (doc 03 §8.4). One atomic read and, under `follow`, one queue push — no refcount
    // traffic, no string config, no head math. The policy DECISION is re-read on the main thread in
    // dispatchStateEvent; this atomic only decides whether to bother it at all, so that the default
    // `hold` costs a relaxed load per reference-space change (an event that fires seconds apart at
    // worst) and nothing else.
    if (m_recenterPolicy.load(std::memory_order_relaxed) != OpenXR::XR_RECENTER_FOLLOW)
        return;
    SXRStateEvent ev;
    ev.type = eXRStateEventType::RECENTERED;
    enqueue(ev);
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

void COpenXRManager::onConfigReload(bool forceProbe) {
    // The rule manager now contains either freshly parsed config or a runtime keyword update. Take
    // field ownership before any reassertion can copy our durable mode/position back over it.
    refreshMonitorRuleOwnership();

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
    // doc 03 §8.4: re-parse openxr:recenter so the hold<->follow choice applies live (the frame
    // thread reads the atomic; dereferencing the string there is the task #25 hazard).
    publishRecenterPolicy();
    // task #139: re-parse openxr:cursor_crossing so a live raycast<->layout A/B applies immediately.
    publishCursorCrossingMode();
    // report 09: re-resolve the luma key (openxr:black_alpha / :black_alpha_knee) — clamped, gated on
    // the session blend mode, and damaging the XR monitors so a live re-tune shows up immediately.
    publishBlackAlphaTuning();
    // WP X1: re-resolve each monitor's stereo declaration (a windowrule change moves it) and apply
    // openxr:stereo_quad_pair immediately rather than at the next repaint.
    publishStereoPairTuning();

    // research/24 WP X3: openxr:depth_desktop decides whether each XR monitor scans out a per-eye
    // PAIR of panes. Unlike its neighbours this changes the output's MODE, so it re-registers the
    // monitor rules rather than poking an atomic — and only on the edge (see m_lastDepthDesktop).
    publishDepthDesktopTuning();
    // doc 05 §xrrule: re-snapshot the declared transparency rules and re-resolve every monitor. Must
    // run AFTER publishBlackAlphaTuning — the black_alpha globals are the DEFAULT layer it folds on.
    reloadXRRules();

    static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
    const bool  enabled  = *PENABLED;

    // report-17 WP-L7 / report-20 issue B1: also start from UNAVAILABLE, not just DISABLED. Previously
    // `hyprctl keyword openxr:enabled 1` while dormant in UNAVAILABLE (value already 1) was a silent
    // no-op — start()'s own guard accepts UNAVAILABLE, the reload path just never asked. A keyword
    // re-assert must still kick a fresh attempt immediately, which is what `forceProbe` is for.
    //
    // Everything else now goes through the gate (reload-storm containment, live evidence 2026-08-21 —
    // see the block above OpenXR::xrReloadAction). A reload that changes nothing probe-relevant must
    // not start(): start() would setState(STARTING), which cancels the re-probe timer and tears down
    // the inotify watch, and the failing attempt then re-arms both from scratch. At the observed 1 Hz
    // that meant the 16s timer was pushed forward before it could ever fire — the manager probed
    // every second while `hyprctl openxr status` truthfully reported the timer's own "retrying in
    // 15916ms". The backoff cannot back off if every reload resets it.
    static auto   PBASE         = CConfigValue<Hyprlang::INT>("openxr:reprobe_interval_ms");
    static auto   PREPROBE_GATE = CConfigValue<Hyprlang::INT>("openxr:reprobe");
    const int64_t base          = std::max<int64_t>(250, (int64_t)*PBASE);

    // An explicit flip of openxr:enabled back to 1 outranks the sticky disable — the user has said
    // "on" through the other control, and leaving the latch set would make that config edit look
    // broken. A reload that merely RE-APPLIES an unchanged `= 1` (what every reload does) must not.
    const int enabledNow = enabled ? 1 : 0;
    if (m_lastEnabledSeen >= 0 && m_lastEnabledSeen != enabledNow && enabledNow == 1 && m_manualDisable) {
        Log::logger->log(Log::DEBUG, "[OPENXR] openxr:enabled flipped back to 1 — clearing the sticky manual disable");
        m_manualDisable = false;
    }
    m_lastEnabledSeen = enabledNow;

    OpenXR::SXRReloadInputs gate;
    gate.enabled            = enabled;
    gate.manualDisable      = m_manualDisable;
    gate.stateDisabled      = m_state == XR_STATE_DISABLED;
    gate.stateUnavailable   = m_state == XR_STATE_UNAVAILABLE;
    gate.probeInputsChanged = probeInputsChanged(); // ALWAYS evaluated: it also refreshes the cache
    gate.forceProbe         = forceProbe;
    // With openxr:reprobe off there is no timer to retry instead, so the storm gate must not strand
    // a dormant manager: a reload stays the user's retry (bounded by the floor below).
    gate.reprobeEnabled     = *PREPROBE_GATE != 0;
    gate.msSinceLastProbe   = m_lastProbeAt ? std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - *m_lastProbeAt).count() : -1;
    gate.minProbeIntervalMs = base;

    switch (OpenXR::xrReloadAction(gate)) {
        case OpenXR::XR_RELOAD_START: start(); break; // start() reconciles declared monitors itself
        case OpenXR::XR_RELOAD_STOP: stop(); break;
        case OpenXR::XR_RELOAD_RECONCILE_ONLY: reconcileDeclaredMonitors(); break;
    }

    // A reparse wiped the rule manager, taking our requested-mode rules with it. reconcile above
    // re-installed them for DECLARED layers only; do the same for runtime-created ones (which
    // reconciliation deliberately never touches) so `openxr create NAME 2560x1440@60` keeps its mode
    // across a reload instead of snapping back to the headless preferred mode. Idempotent, so the
    // no-reparse callers of onConfigReload() (the openxr:enabled keyword special-case) are unharmed.
    reassertMonitorModeRules();

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

    // report 12 §1.2 + §4: a reload RE-PARSES the monitor rules, so any runtime offset we injected is
    // gone by the time this listener runs (CConfigManager::reloadRules clears and re-adds, then
    // ensureMonitorStatus re-applies). Re-inject. This also picks up a hot-edited openxr:layout2d:*
    // knob, since the projection reads its config fresh on every pass. Debounced like every other
    // trigger, so a reload that changes nothing costs one no-op pass.
    requestLayout2DSync();
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

void COpenXRManager::publishBlackAlphaTuning(std::optional<OpenXR::eXRBlendMode> modeOverride) {
    // MAIN-THREAD ONLY. Resolve openxr:black_alpha / :black_alpha_knee (clamp + blend-mode gate) and
    // publish them as atomics for the frame loop. See the header for why numeric values still go
    // through a publish. Called from start() (twice: before the frame thread launches, and again once
    // the blend mode is known — that call passes modeOverride because m_session is not adopted yet),
    // onConfigReload(), and the keyword special-case in ConfigManager.
    static auto PBLACK = CConfigValue<Hyprlang::FLOAT>("openxr:black_alpha");
    static auto PKNEE  = CConfigValue<Hyprlang::FLOAT>("openxr:black_alpha_knee");
    const float want   = std::clamp((float)*PBLACK, 0.F, 1.F);
    const float knee   = std::clamp((float)*PKNEE, OpenXR::XR_BLACK_ALPHA_KNEE_MIN, 1.F);

    // Gate: keying only REVEALS something when the runtime composites us over passthrough (alpha) or
    // an additive display. Under opaque it would just make monitors look dim and dirty (report 09
    // §3.1), so force it off there and say so once per config set. No session yet = nothing to key.
    const std::optional<OpenXR::eXRBlendMode> mode =
        modeOverride ? modeOverride : (m_session ? std::optional<OpenXR::eXRBlendMode>(xrBlendModeFromXr(m_session->m_blendMode)) : std::nullopt);
    const bool  showsThrough = mode && OpenXR::blendModeShowsThrough(*mode);
    const float eff          = showsThrough ? want : 1.F;

    if (OpenXR::xrBlackKeyActive(want) && mode && !showsThrough) {
        if (m_blackAlphaWarnedFor != want) {
            m_blackAlphaWarnedFor = want;
            Log::logger->log(Log::WARN,
                             "[OPENXR] openxr:black_alpha = {:.2f} has no effect under blend mode '{}': luma-keyed transparency needs a see-through composite. Set "
                             "openxr:blend_mode = alpha (passthrough) or additive and restart the session (hyprctl openxr disable && hyprctl openxr enable)",
                             want, OpenXR::blendModeToString(*mode));
        }
    } else
        m_blackAlphaWarnedFor = -1.F; // re-arm

    const float prevA = m_blackAlpha.exchange(eff, std::memory_order_relaxed);
    const float prevK = m_blackAlphaKnee.exchange(knee, std::memory_order_relaxed);
    m_blackAlphaConfigured.store(want, std::memory_order_relaxed);

    // A live re-tune must be visible immediately, even on a completely static desktop: the frame loop
    // only re-blits a layer when a NEW buffer arrives (an animation-only frame just restores the
    // already-keyed snapshot). Force a fresh composite per XR monitor so the new key lands now.
    if ((prevA != eff || prevK != knee) && m_running.load(std::memory_order_acquire) && g_pHyprRenderer) {
        std::vector<PHLMONITOR> mons;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers)
                if (auto mon = l->m_monitor.lock())
                    mons.push_back(mon);
        }
        for (auto& mon : mons)
            g_pHyprRenderer->damageMonitor(mon);
    }
}

// MAIN THREAD (WP X1). Each layer's stereo declaration is normally re-published from its `presented`
// listener, which means it only moves when the monitor draws. Two things must not wait for that: a
// windowrule change (the fold that produces the declaration just changed under us) and, above all,
// `hyprctl keyword openxr:stereo_quad_pair 0` — the kill switch exists for the case where a runtime
// is showing something wrong, and "it applies once the desktop next repaints" is not good enough
// when the whole point is to get out of a broken picture. Re-resolve every monitor unconditionally
// (cheap: one fullscreen lookup each), and force a composite only when the switch itself moved.
//
// It re-publishes the frame DECLARATION only, never publishStereoMode, and that asymmetry is
// correct rather than an oversight: the mode declaration describes how the OUTPUT BUFFER is
// physically laid out, and nothing this function reacts to (a windowrule fold, a config keyword) can
// move it. The pane count only ever moves through CMonitorRule::compare, which treats m_stereo and
// m_stereoVirtualMode as HARD props — so a stereo change is COMPARISON_NO_MATCH and takes the full
// applyMonitorRule path, which emits modeChanged unconditionally and re-runs publishStereoMode. The
// soft path cannot smuggle one past: applyMonitorRuleSoft re-derives m_stereoMode from the same rule
// and narrows it only through sanitizeStereoMode, whose non-rule inputs (m_pixelSize,
// m_modeSearchFellBack) a soft apply cannot change — backComputeMode is the exact inverse of
// deriveGeometry, so even the m_createdByUser re-derivation is a fixed point under a soft transform
// change. The stereo WATCH, the other way a live pack moves, deliberately re-applies the whole rule.
void COpenXRManager::publishStereoPairTuning() {
    static auto PPAIR = CConfigValue<Hyprlang::INT>("openxr:stereo_quad_pair");

    const bool  want    = *PPAIR != 0;
    const bool  changed = !m_lastStereoQuadPair.has_value() || *m_lastStereoQuadPair != want;
    m_lastStereoQuadPair = want;

    std::vector<PHLMONITOR> mons;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (auto mon = l->m_monitor.lock()) {
                l->publishStereoPairLayout(mon);
                mons.push_back(mon);
            }
        }
    }

    if (!changed || !m_running.load(std::memory_order_acquire) || !g_pHyprRenderer)
        return;

    for (auto& mon : mons)
        g_pHyprRenderer->damageMonitor(mon);
}

// MAIN THREAD (research/24 §6, WP X3). openxr:depth_desktop is the depth desktop's master switch,
// and it is the only openxr:* value that changes an output's MODE: a depth-producing monitor scans
// out two panes, so its pixel mode is its declared (per-eye) size doubled. That cannot be published
// as an atomic — it has to travel through the monitor rule, the mode search and applyMonitorRule,
// which is exactly what registerDeclaredMonitorRule already owns.
//
// So this is an EDGE trigger, not a refresh: a `hyprctl reload` with the value unchanged must not
// re-modeset every XR output. On the edge, re-registering the rules is the whole action — the rule
// manager's ensureMonitorStatus compares before applying, applyMonitorRule re-derives the pane
// geometry and the render resources, modeChanged reaches the layer, and the frame thread rebuilds
// the swapchain at the new pane count on its next pass.
void COpenXRManager::publishDepthDesktopTuning() {
    static auto PDEPTH = CConfigValue<Hyprlang::INT>("openxr:depth_desktop");

    const bool  want    = *PDEPTH != 0;
    const bool  changed = !m_lastDepthDesktop.has_value() || *m_lastDepthDesktop != want;
    m_lastDepthDesktop  = want;

    if (!changed)
        return;

    Log::logger->log(Log::DEBUG, "[OPENXR] depth desktop {} — re-deriving XR monitor modes", want ? "ON (per-eye composite, packed mode)" : "OFF (single composite)");
    reassertMonitorModeRules();

    // The declaration the frame thread reads is derived from the monitor's live packing state, and
    // the reassert above has only SCHEDULED the rule pass that changes it — so this republish sees
    // the OLD state and cannot, by itself, make the toggle land sooner. It is here for the monitors
    // the pass does not touch (nothing to re-apply, so nothing would ever republish them) and to
    // keep every layer's declaration on one code path. The pack itself lands on the first `presented`
    // after the modeset, which is where the frame thread picks it up.
    publishStereoPairTuning();
}

COpenXRManager::SXRBlackAlpha COpenXRManager::blackAlphaStatus() const {
    SXRBlackAlpha s;
    s.configured = m_blackAlphaConfigured.load(std::memory_order_relaxed);
    s.effective  = m_blackAlpha.load(std::memory_order_relaxed);
    s.knee       = m_blackAlphaKnee.load(std::memory_order_relaxed);
    s.active     = OpenXR::xrBlackKeyActive(s.effective);
    s.gatedOff   = OpenXR::xrBlackKeyActive(s.configured) && !s.active;
    return s;
}

// ---- situational per-monitor transparency: the `xrrule` engine (doc 05 §xrrule, report 09) ----

void COpenXRManager::reloadXRRules() {
    // MAIN THREAD. Snapshot the declared list (config order is load-bearing) and re-evaluate. The
    // snapshot is a copy so a later reload rebuilding CConfigManager's vector can never pull the
    // ground out from under an in-flight evaluation.
    m_xrRules = Config::xrDeclarationMgr()->rules();

    // Titles change on every browser tab switch; only pay for that trigger if a rule can act on it.
    m_rulesUseTitle = std::ranges::any_of(m_xrRules, [](const OpenXR::SXRRule& r) { return (bool)r.conds.focusTitleRe; });

    if (!m_xrRules.empty())
        Log::logger->log(Log::DEBUG, "[OPENXR] {} xrrule(s) loaded (title trigger: {})", m_xrRules.size(), m_rulesUseTitle ? "on" : "off");

    requestEffectEval();
}

void COpenXRManager::requestEffectEval() {
    // Coalesce: every trigger funnels here, and a burst (focus change -> workspace change -> title
    // change, all in one keypress) collapses into ONE evaluation at the end of the loop iteration.
    if (m_effectEvalQueued || !g_pEventLoopManager)
        return;
    m_effectEvalQueued = true;
    // Capture NOTHING and re-check the global: a deferred callback must never run against a
    // destroyed manager (the same discipline as the xrmonitor reconcile doLater in ConfigManager).
    g_pEventLoopManager->doLater([] {
        if (!g_pOpenXRManager)
            return;
        g_pOpenXRManager->onEffectEvalDue();
    });
}

void COpenXRManager::onEffectEvalDue() {
    m_effectEvalQueued = false;
    evaluateMonitorEffects();
    reevaluateViewpoints();
}

void COpenXRManager::invalidateViewpoints(uint32_t reason) {
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& layer : m_layers)
            layer->setViewpointSubscription(std::nullopt);
    }

    if (!PROTO::xrViewpoint)
        return;
    PROTO::xrViewpoint->forEachViewpoint([reason](CXRViewpointResource& viewpoint) { viewpoint.invalidate(sc<hypxrViewpointV1InactiveReason>(reason)); });
}

void COpenXRManager::reevaluateViewpoints() {
    if (!PROTO::xrViewpoint)
        return;

    std::vector<CXRViewpointResource*> viewpoints;
    PROTO::xrViewpoint->forEachViewpoint([&](CXRViewpointResource& viewpoint) { viewpoints.push_back(&viewpoint); });
    // Total order (OpenXR::viewpointOrderBefore): a viewpoint whose surface is already destroyed
    // stays in the list, and a comparator that switches between id-order and pointer-order across
    // that mix is not a strict weak ordering — see the key's header comment for the cycle.
    const auto ORDERKEY = [](const CXRViewpointResource* viewpoint) {
        const auto SURFACE = viewpoint->surface();
        return OpenXR::SXRViewpointOrderKey{
            .hasSurface = sc<bool>(SURFACE),
            .surfaceId  = SURFACE ? SURFACE->id() : 0,
            .identity   = rc<uintptr_t>(viewpoint),
        };
    };
    std::ranges::sort(viewpoints, [&](const auto* a, const auto* b) { return OpenXR::viewpointOrderBefore(ORDERKEY(a), ORDERKEY(b)); });

    // m_layersMu is the frame thread's mutex: it takes it every frame, twice, around the solve and
    // the pointer pass. This walk used to hold it across the whole eligibility test — window,
    // workspace, fullscreen, rule and surface queries for every viewpoint — and across every
    // protocol send, and the four new listeners made it run far more often than the old
    // effects-only triggers did. That is contention the frame loop pays for in the headset.
    //
    // Only two things here actually need the mutex: the m_layers container itself, and the anchor
    // state (which the frame thread mutates under it). Both are snapshotted here, and nothing below
    // touches either again. The subscription accessors carry their OWN mutex (m_viewpointMu, which
    // is what makes subscription replacement atomic against frame publication), so they never
    // needed this one.
    struct SLayerEligibility {
        PXRLAYER               layer;
        PHLMONITOR             monitor;
        OpenXR::SXRAnchorState anchorState;
        bool                   pendingRemoval = false;
        // LOCAL, not carried by hand or gaze, and not mid-adaptive-transition: the anchor is sitting
        // still enough for a client to render view-dependent content against it.
        bool                   anchorSettled = false;
    };

    std::vector<SLayerEligibility> layers;
    {
        std::scoped_lock lock(m_layersMu);
        layers.reserve(m_layers.size());
        for (auto& l : m_layers) {
            const auto& ANCHOR = l->m_anchor;
            layers.push_back(SLayerEligibility{
                .layer          = l,
                .monitor        = l->m_monitor.lock(),
                .anchorState    = ANCHOR.state(),
                .pendingRemoval = l->m_pendingRemoval.load(std::memory_order_acquire),
                .anchorSettled  = ANCHOR.state().mode == OpenXR::XR_ANCHOR_LOCAL && !ANCHOR.grabbed() && !ANCHOR.gazeGrabbed() &&
                    (!ANCHOR.adaptiveEnabled() || ANCHOR.adaptivePhase() == OpenXR::XRAD_DOCKED),
            });
        }
    }

    std::vector<CXRMonitorLayer*> claimed;
    for (auto* viewpoint : viewpoints) {
        if (!viewpoint->enabled())
            continue;
        if (!viewpoint->requested()) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_SUPPORTED);
            continue;
        }

        const auto SURFACE = viewpoint->surface();
        if (!SURFACE || !SURFACE->m_hlSurface) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }

        const auto WINDOW = Desktop::View::CWindow::fromView(SURFACE->m_hlSurface->view());
        if (!WINDOW) {
            // A surface that is not a window (yet) is not a policy decision — reporting it as
            // NOT_AUTHORIZED sent a client hunting for a permission it already had. `not_eligible`
            // is what "this surface cannot present right now" means.
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }
        if (!WINDOW->m_ruleApplicator->viewpoint().valueOrDefault()) {
            // The `viewpoint` window rule is the entire authorization gate. Two upstream facts about
            // it cost a live session an afternoon of thinking the compositor was broken, so they are
            // written down here: a rule added by `hyprctl keyword windowrule` is dropped by the next
            // config reload (CConfigManager::reload clears m_keywordRules, and reloads fire on any
            // sourced file changing), and re-adding it registers the rule WITHOUT re-running it over
            // already-mapped windows — so the client has to be restarted to pick it up. The inactive
            // reason now reaches the log (CXRViewpointResource::invalidate), which is how a future
            // session tells this apart from a compositor fault.
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_AUTHORIZED);
            continue;
        }

        if (!sessionVisible() || !m_monitorViewVisible.load(std::memory_order_acquire) || (m_userPresenceSupported && (!m_presenceKnown || !m_userPresent))) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_XR_INACTIVE);
            continue;
        }

        const auto MONITOR = WINDOW->m_monitor.lock();
        if (!MONITOR || !WINDOW->m_isMapped || WINDOW->isHidden() || !WINDOW->m_workspace || !WINDOW->m_workspace->isVisible() || !WINDOW->wlSurface()->exists() ||
            WINDOW->wlSurface()->resource() != SURFACE || !SURFACE->m_mapped || !viewpoint->subsurfaceTreeObservable() || SURFACE->hasVisibleSubsurface() ||
            SURFACE->m_current.offset != Vector2D{} ||
            !OpenXR::viewpointSBSBufferMapping(SURFACE->m_current.bufferSize, SURFACE->m_current.size, MONITOR->m_size, SURFACE->m_current.viewport.hasSource,
                                               SURFACE->m_current.viewport.hasDestination, SURFACE->m_current.viewport.destination,
                                               SURFACE->m_current.transform == WL_OUTPUT_TRANSFORM_NORMAL, SURFACE->m_current.scale) ||
            WINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) != MONITOR->m_position ||
            WINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) != MONITOR->m_size || Fullscreen::controller()->getFullscreenWindow(MONITOR) != WINDOW ||
            !Fullscreen::controller()->isFullscreen(WINDOW, Fullscreen::FSMODE_FULLSCREEN, true) || WINDOW->stereoLayout() != Render::Stereo::CONTENT_SBS) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }

        const auto LAYERIT = std::ranges::find_if(layers, [&](const auto& snapshot) { return !snapshot.pendingRemoval && snapshot.monitor == MONITOR; });
        if (LAYERIT == layers.end()) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }

        const auto& LAYER = LAYERIT->layer;
        if (std::ranges::find(claimed, LAYER.get()) != claimed.end()) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_SUPERSEDED);
            continue;
        }
        if (!LAYERIT->anchorSettled) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }

        // The rectangle the client is about to be told to render for. Derived through the SAME
        // expression, from the SAME pane pixels, that the frame thread's solve will use — the two
        // results are compared as micrometres at every publish, and a rounding step between them
        // would make the viewpoint inactive forever with nothing logged. The size SOURCE is the
        // monitor's pixel mode on both sides: the frame thread reads it from the layer's
        // m_contentSize, which the layer latched from this same m_pixelSize at swapchain creation
        // (the main thread must not read m_contentSize — it is frame-thread-owned and non-atomic).
        // While a mode change is in flight the two genuinely disagree, and that is exactly when the
        // publish interlock should hold the samples back.
        const float WIDTH   = LAYERIT->anchorState.widthMeters;
        const auto  PANEPX  = Render::Stereo::presentedPaneSize(MONITOR->m_pixelSize, Render::Stereo::CONTENT_SBS);
        const float HEIGHT  = OpenXR::quadHeightMeters(WIDTH, (uint32_t)std::max(1.0, PANEPX.x), (uint32_t)std::max(1.0, PANEPX.y));
        uint32_t    widthUM = 0, heightUM = 0;
        if (!OpenXR::encodeViewpointDimensionUM(WIDTH, widthUM) || !OpenXR::encodeViewpointDimensionUM(HEIGHT, heightUM)) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }

        const auto RUNTIME_STATE = sc<OpenXR::eXRViewpointRuntimeState>(LAYER->m_viewpointRuntimeState.load(std::memory_order_acquire));
        const auto EXISTING      = LAYER->viewpointSubscription();
        if (RUNTIME_STATE != OpenXR::XR_VIEWPOINT_RUNTIME_INVALID && EXISTING &&
            OpenXR::viewpointActivationUnchanged(EXISTING->surfaceId, SURFACE->id(), EXISTING->epoch, EXISTING->token, viewpoint->token(), EXISTING->widthUM, widthUM,
                                                 EXISTING->heightUM, heightUM, EXISTING->anchorState == LAYERIT->anchorState)) {
            claimed.push_back(LAYER.get());
            continue;
        }
        if (RUNTIME_STATE == OpenXR::XR_VIEWPOINT_RUNTIME_INVALID) {
            if ((!EXISTING || EXISTING->surfaceId != SURFACE->id()) &&
                (m_viewpointTokenCounter == std::numeric_limits<uint64_t>::max() || m_viewpointGeometryCounter == std::numeric_limits<uint64_t>::max())) {
                viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
                continue;
            }
            const uint64_t TOKEN      = EXISTING && EXISTING->surfaceId == SURFACE->id() ? EXISTING->token : ++m_viewpointTokenCounter;
            const uint64_t GEOMETRYID = EXISTING && EXISTING->surfaceId == SURFACE->id() ? EXISTING->geometryId : ++m_viewpointGeometryCounter;
            LAYER->setViewpointSubscription(CXRMonitorLayer::SViewpointSubscription{
                .token       = TOKEN,
                .epoch       = 0,
                .geometryId  = GEOMETRYID,
                .surfaceId   = SURFACE->id(),
                .widthUM     = widthUM,
                .heightUM    = heightUM,
                .anchorState = LAYERIT->anchorState,
            });
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_TRACKING_LOST);
            claimed.push_back(LAYER.get());
            continue;
        }

        if (m_viewpointTokenCounter == std::numeric_limits<uint64_t>::max() || m_viewpointGeometryCounter == std::numeric_limits<uint64_t>::max()) {
            viewpoint->invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
            continue;
        }

        const uint64_t TOKEN      = ++m_viewpointTokenCounter;
        const uint64_t GEOMETRYID = ++m_viewpointGeometryCounter;
        const uint64_t EPOCH      = viewpoint->activate(TOKEN, GEOMETRYID, widthUM, heightUM);
        if (EPOCH == 0)
            continue;
        LAYER->setViewpointSubscription(CXRMonitorLayer::SViewpointSubscription{
            .token       = TOKEN,
            .epoch       = EPOCH,
            .geometryId  = GEOMETRYID,
            .surfaceId   = SURFACE->id(),
            .widthUM     = widthUM,
            .heightUM    = heightUM,
            .anchorState = LAYERIT->anchorState,
        });
        claimed.push_back(LAYER.get());
    }

    for (auto& snapshot : layers)
        if (std::ranges::find(claimed, snapshot.layer.get()) == claimed.end())
            snapshot.layer->setViewpointSubscription(std::nullopt);
}

void COpenXRManager::drainViewpointSamples() {
    if (!PROTO::xrViewpoint)
        return;

    struct SDelivery {
        uint64_t                          token = 0;
        OpenXR::SXRViewpointEncodedSample sample;
    };
    std::vector<SDelivery> deliveries;
    {
        std::scoped_lock lock(m_layersMu);
        deliveries.reserve(m_layers.size());
        for (auto& layer : m_layers) {
            CXRMonitorLayer::SViewpointSubscription subscription;
            OpenXR::SXRViewpointMailboxRead         sample;
            if (layer->consumeViewpointSample(subscription, sample))
                deliveries.push_back({.token = subscription.token, .sample = sample.sample});
        }
    }

    for (const auto& delivery : deliveries)
        PROTO::xrViewpoint->deliverSample(delivery.token, delivery.sample);
}

OpenXR::eXRAnchorState COpenXRManager::layerAnchorState(const PXRLAYER& layer) {
    // MAIN THREAD, caller holds m_layersMu. The three states a user reasons about, in priority
    // order. A hand/gaze carry outranks everything (it is what the monitor is doing RIGHT NOW);
    // then "leashed to me" — an adaptive monitor that has left its desk pose, or a persistently
    // head/body/device-anchored one; else world-fixed.
    if (layer->m_anchor.grabbed() || layer->m_anchor.gazeGrabbed())
        return OpenXR::XR_ANCHORSTATE_CARRIED;
    if (layer->m_anchor.adaptiveEnabled() && layer->m_anchor.adaptivePhase() != OpenXR::XRAD_DOCKED)
        return OpenXR::XR_ANCHORSTATE_FOLLOW;
    switch (layer->m_anchor.state().mode) {
        case OpenXR::XR_ANCHOR_HEAD:
        case OpenXR::XR_ANCHOR_BODY:
        case OpenXR::XR_ANCHOR_DEVICE: return OpenXR::XR_ANCHORSTATE_FOLLOW;
        default: return OpenXR::XR_ANCHORSTATE_DOCKED;
    }
}

OpenXR::SXRRuleContext COpenXRManager::buildRuleContext(const PXRLAYER& layer) {
    // MAIN THREAD, caller holds m_layersMu. Each monitor is evaluated with its OWN tuple — that is
    // what makes `monitor:` a filter rather than a selector.
    OpenXR::SXRRuleContext ctx;
    ctx.monitorName = layer->m_monitorName;
    ctx.anchorState = layerAnchorState(layer);

    const auto mon = layer->m_monitor.lock();
    if (!mon)
        return ctx;

    // "The focused window OF a monitor" (doc 05 §xrrule): the fullscreen window on the monitor's
    // active workspace if there is one — a fullscreen window IS what you are looking at, even if
    // focus technically sits on a floating overlay — else that workspace's last-focused window.
    // Hyprland tracks focus per workspace (CWorkspace::m_lastFocusedWindow), so this stays correct
    // for an XR monitor that is not the compositor's globally-focused one, which is the whole point:
    // every XR monitor must resolve its own situation, not the global focus.
    PHLWINDOW win = Fullscreen::controller() ? Fullscreen::controller()->getFullscreenWindow(mon) : nullptr;
    if (!win && mon->m_activeWorkspace)
        win = mon->m_activeWorkspace->getLastFocusedWindow();
    if (!win)
        return ctx;

    ctx.hasFocus   = true;
    ctx.focusClass = win->m_class;
    ctx.focusTitle = win->m_title;
    ctx.fullscreen = Fullscreen::controller() && Fullscreen::controller()->isFullscreen(win);
    return ctx;
}

void COpenXRManager::publishLayerEffects(const PXRLAYER& layer) {
    // MAIN THREAD, caller holds m_layersMu. Plain floats into plain atomics — the frame loop reads
    // exactly these three numbers and nothing else (no strings, no refcounts).
    layer->m_fxAlpha.store(layer->m_fxAlphaEnv.value(), std::memory_order_relaxed);
    layer->m_fxBlackAlpha.store(layer->m_fxBlackAlphaEnv.value(), std::memory_order_relaxed);
    layer->m_fxKnee.store(layer->m_fxKneeEnv.value(), std::memory_order_relaxed);
}

void COpenXRManager::evaluateMonitorEffects() {
    // MAIN THREAD. Resolve defaults -> rules -> manual for EVERY layer, retarget the envelopes, and
    // damage any monitor whose targets actually moved (see below). Cheap: a handful of RE2 matches
    // per monitor, and only on real events.
    static auto        PBLACK = CConfigValue<Hyprlang::FLOAT>("openxr:black_alpha");
    static auto        PKNEE  = CConfigValue<Hyprlang::FLOAT>("openxr:black_alpha_knee");

    // Layer 1 — defaults. The uniform alpha default is 1.0 (fully opaque, the historic behavior);
    // the luma-key defaults are the EXISTING globals, which is what keeps every pre-xrrule config
    // behaving bit-identically.
    OpenXR::SXREffects defaults;
    defaults.alpha      = 1.F;
    defaults.blackAlpha = std::clamp((float)*PBLACK, 0.F, 1.F);
    defaults.blackKnee  = std::clamp((float)*PKNEE, OpenXR::XR_BLACK_ALPHA_KNEE_MIN, 1.F);

    // The luma key only REVEALS anything when the runtime composites us over passthrough/additive
    // (report 09 §3.1) — under opaque it just looks dim and dirty, so it is forced off there for
    // rules exactly as it is for the global. The UNIFORM alpha is NOT gated: dimming toward the
    // background is a legitimate de-emphasis cue under any blend mode.
    const bool showsThrough = m_session && OpenXR::blendModeShowsThrough(xrBlendModeFromXr(m_session->m_blendMode));
    bool       gatedAnyKey  = false;

    std::vector<PHLMONITOR> damage;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;

            const auto ctx      = buildRuleContext(l);
            auto       resolved = OpenXR::xrResolveEffects(defaults, m_xrRules, ctx, l->m_manualFx);

            if (!showsThrough && OpenXR::xrBlackKeyActive(resolved.blackAlpha)) {
                gatedAnyKey        = true;
                resolved.blackAlpha = 1.F;
            }

            const bool moved = resolved.alpha != l->m_fxAlphaEnv.to || resolved.blackAlpha != l->m_fxBlackAlphaEnv.to || resolved.blackKnee != l->m_fxKneeEnv.to;
            l->m_fxResolved  = resolved;
            l->m_fxAlphaEnv.retarget(resolved.alpha);
            l->m_fxBlackAlphaEnv.retarget(resolved.blackAlpha);
            l->m_fxKneeEnv.retarget(resolved.blackKnee);
            publishLayerEffects(l);

            // A change needs ONE fresh composite to start from: the frame loop only re-blits a layer
            // when a new desktop buffer arrives, and an animation-only frame needs a content snapshot
            // that may not exist yet on a monitor that had no effects at all. Damaging the monitor
            // once produces both. The ongoing luma-key transition keeps damaging from the envelope
            // tick (the key is baked into the blit); the uniform alpha needs nothing further.
            if (moved) {
                if (auto mon = l->m_monitor.lock())
                    damage.push_back(mon);
            }
        }
    }

    if (showsThrough)
        m_fxKeyGateWarned = false; // re-arm: a later session on an opaque blend mode should warn again
    // Only complain once a session EXISTS — before start() there is no blend mode to be wrong about,
    // and the rules are (correctly) resolved with keying off until one is picked.
    else if (gatedAnyKey && m_session && !m_fxKeyGateWarned) {
        m_fxKeyGateWarned = true;
        Log::logger->log(Log::WARN,
                         "[OPENXR] an xrrule (or manual override) asks for luma-keyed transparency, but the session blend mode is '{}': keying needs a see-through composite and "
                         "is ignored. Set openxr:blend_mode = alpha (passthrough) or additive and restart the session. Uniform `alpha` still applies (as a dim)",
                         blendModeName());
    }

    if (g_pHyprRenderer)
        for (auto& mon : damage)
            g_pHyprRenderer->damageMonitor(mon);

    armEffectEnvelopeTimer();
}

void COpenXRManager::armEffectEnvelopeTimer() {
    // MAIN THREAD. One-shot 8ms tick, re-armed from the callback while anything is still moving
    // (the codebase timer idiom). Nothing in flight = the timer is disarmed and costs nothing, so a
    // settled desktop pays zero for this feature.
    bool moving = false;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            if (!l->m_fxAlphaEnv.settled() || !l->m_fxBlackAlphaEnv.settled() || !l->m_fxKneeEnv.settled()) {
                moving = true;
                break;
            }
    }

    if (!moving) {
        m_fxTickRunning = false;
        if (m_fxTimer)
            m_fxTimer->updateTimeout(std::nullopt);
        return;
    }

    const auto dur = std::chrono::milliseconds(OpenXR::XR_FX_TICK_MS);
    if (!m_fxTimer) {
        m_fxTimer = makeShared<CEventLoopTimer>(
            dur, [this](SP<CEventLoopTimer> self, void*) { onEffectEnvelopeTick(); }, nullptr);
        if (g_pEventLoopManager)
            g_pEventLoopManager->addTimer(m_fxTimer);
    } else
        m_fxTimer->updateTimeout(dur);
}

void COpenXRManager::onEffectEnvelopeTick() {
    // MAIN THREAD. Advance every envelope by the wall-clock delta since the last tick and republish.
    static auto PBLENDMS = CConfigValue<Hyprlang::INT>("openxr:transparency_blend_ms");
    const float durSec   = std::max(0, (int)*PBLENDMS) / 1000.F;

    const auto  now = Time::steadyNow();
    float       dt  = 0.F;
    if (m_fxTickRunning)
        dt = std::min(0.25F, (float)std::chrono::duration_cast<std::chrono::microseconds>(now - m_fxLastTick).count() / 1e6F);
    m_fxLastTick    = now;
    m_fxTickRunning = true;

    std::vector<PHLMONITOR> damage;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            // The LUMA KEY is baked into the blit, so a static desktop must be re-rendered for each
            // step of a key transition to actually re-key. Sample the "was moving" bit BEFORE the
            // advance so the final settling step damages too. The uniform alpha needs no damage: it
            // is a post-multiply over the already-composed image, so the frame loop can re-apply it
            // from the content snapshot on an animation-only frame.
            const bool keyMoving = !l->m_fxBlackAlphaEnv.settled() || !l->m_fxKneeEnv.settled();

            l->m_fxAlphaEnv.advance(dt, durSec);
            l->m_fxBlackAlphaEnv.advance(dt, durSec);
            l->m_fxKneeEnv.advance(dt, durSec);
            publishLayerEffects(l);

            if (keyMoving) {
                if (auto mon = l->m_monitor.lock())
                    damage.push_back(mon);
            }
        }
    }

    if (g_pHyprRenderer)
        for (auto& mon : damage)
            g_pHyprRenderer->damageMonitor(mon);

    armEffectEnvelopeTimer(); // re-arm or disarm
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

void COpenXRManager::publishCursorCrossingMode() {
    // MAIN-THREAD. openxr:cursor_crossing -> m_cursorCrossMode. Unlike its neighbours this is not a
    // frame-thread safety measure (the only reader is redirectCursorCrossing, main thread) — see the
    // header. Called from start() + onConfigReload() + the parseKeyword special-case.
    static auto PCROSS = CConfigValue<std::string>("openxr:cursor_crossing");
    m_cursorCrossMode.store((uint8_t)OpenXR::xrParseCursorCrossing(*PCROSS), std::memory_order_relaxed);
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
        m_gazeHitId    = -1;
        m_gazeHitValid = false;
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
    // hypxrvoice GAP 4: also keep the intersection against the PRE-STEP stable target, whatever depth
    // it is at — stepGazeSelect() below can only leave the selection at that id or at rawHit, so
    // those two are the complete candidate set for the hit point of the REPORTED candidate.
    const int64_t prevStable = m_gazeSel.stable;
    int64_t       rawHit     = -1;
    float         bestT      = std::numeric_limits<float>::max();
    Vector2D      hitUV;
    bool          prevStableHit = false;
    float         prevStableT   = 0.f;
    for (const auto& t : targets) {
        float slack = 0.f;
        if (t.id == m_gazeSel.stable && hystTan > 0.f) {
            const float d = (t.worldPose.pos - origin).length();
            slack         = hystTan * d;
        }
        const OpenXR::SXRQuadHit hit = OpenXR::rayQuadIntersect(t.worldPose, origin, dir, t.w, t.h, slack);
        if (!hit.hit)
            continue;
        if (t.id == prevStable) {
            prevStableHit = true;
            prevStableT   = hit.t;
        }
        if (hit.t < bestT) {
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

    // The hit point that belongs to `stable`, along the SAME (1€-filtered) ray the selection used —
    // so the reported point is exactly the surface point that chose gaze.monitorId. Plain floats;
    // recordPoseSample copies them into the ring for the main-thread IPC to serialize.
    const OpenXR::SXRGazeHitPick pick = OpenXR::pickGazeHitT(stable, rawHit, rawHit >= 0, bestT, prevStable, prevStableHit, prevStableT);
    m_gazeHitValid                    = pick.valid;
    m_gazeHitDist                     = pick.valid ? pick.t : 0.f;
    m_gazeHitPoint                    = pick.valid ? origin + dir * pick.t : OpenXR::Vec3{};

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
    // Front-end independent: both `xrmonitor =` (classic) and hl.xr_monitor (Lua) deposit here.
    const auto& declared = Config::xrDeclarationMgr()->monitors();

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
            // Declared-vs-declared, like anchorChanged above: comparing the LIVE width here meant a
            // grab-resized monitor re-entered this branch on every unrelated config reload, resetting
            // its pose along with its size — a live-placement eraser on desktops that reload often.
            const bool  sizeChanged       = existing->m_declaredAnchor.widthMeters != wantSize;
            const bool  anchorModeChanged = existing->m_declaredAnchor.mode != d.m_anchor.mode || existing->m_declaredAnchor.device != d.m_anchor.device;
            if (anchorChanged || sizeChanged) {
                std::scoped_lock       lock(m_layersMu);
                OpenXR::SXRAnchorState st = d.m_anchor;
                st.widthMeters            = wantSize;
                existing->m_anchor.initFromState(st);
                existing->m_declaredAnchor = st;
                existing->m_sizeMeters     = wantSize;
                // doc 03 §8.3: the user just re-declared where this monitor goes, so any placement
                // captured before that is superseded — the next session must re-seat from the NEW rig,
                // not replay the old arrangement. The frame-thread capture will not re-validate while
                // the live pose is still the declaration it was just seeded from.
                existing->m_restoreValid = false;
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
    // The refresh default is applied whether or not a resolution was given, so `create NAME
    // 2560x1440` pins @60 into the rule we register instead of inheriting whatever refresh the
    // matched rule happened to carry.
    if (!parsed->m_resolution)
        parsed->m_resolution = Vector2D{1920, 1080};
    if (!parsed->m_refreshRate)
        parsed->m_refreshRate = 60.f;

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
        // Cross-session restore (doc 03 §8.3): answers "will this monitor come back where I left it".
        info.restorable                  = l->m_restoreValid;
        info.restoreX                    = l->m_restoreOffset.pos.x;
        info.restoreY                    = l->m_restoreOffset.pos.y;
        info.restoreZ                    = l->m_restoreOffset.pos.z;
        // Adaptive anchoring (research/13 §6.4).
        info.adaptiveEnabled  = l->m_anchor.adaptiveEnabled();
        info.adaptivePhase    = OpenXR::xrAdaptivePhaseName(l->m_anchor.adaptivePhase());
        info.adaptiveRoamMode = l->m_anchor.adaptiveRoamMode() == OpenXR::XR_ANCHOR_HEAD ? "head" : "body";
        info.adaptiveSeatDist = l->m_anchor.adaptiveSeatDist();
        info.adaptiveT        = l->m_anchor.adaptiveTransitionT();
        // Situational transparency (doc 05 §xrrule): the live (eased) values, their resolved targets
        // and the provenance of each. m_fxResolved is main-thread state under this same lock.
        info.fxAlpha            = l->m_fxAlpha.load(std::memory_order_relaxed);
        info.fxAlphaTarget      = l->m_fxResolved.alpha;
        info.fxAlphaSrc         = OpenXR::xrEffectSourceName(l->m_fxResolved.alphaSrc);
        info.fxBlackAlpha       = l->m_fxBlackAlpha.load(std::memory_order_relaxed);
        info.fxBlackAlphaTarget = l->m_fxResolved.blackAlpha;
        info.fxBlackAlphaSrc    = OpenXR::xrEffectSourceName(l->m_fxResolved.blackAlphaSrc);
        info.fxKnee             = l->m_fxKnee.load(std::memory_order_relaxed);
        info.fxKneeSrc          = OpenXR::xrEffectSourceName(l->m_fxResolved.blackKneeSrc);
        info.fxTransitioning    = !l->m_fxAlphaEnv.settled() || !l->m_fxBlackAlphaEnv.settled() || !l->m_fxKneeEnv.settled();
        info.anchorState        = OpenXR::xrAnchorStateName(layerAnchorState(l));
        // 2D-plane sync (report 12): where the projection put this monitor and the angles it used, so
        // "why does my cursor cross there" is answerable in one command.
        info.l2dSource          = l->m_l2dUserPinned ? "pinned" : (l->m_l2dPlaced ? "auto" : "off");
        info.l2dCol             = l->m_l2dCol;
        info.l2dRow             = l->m_l2dRow;
        info.l2dX               = l->m_l2dOffset.x;
        info.l2dY               = l->m_l2dOffset.y;
        info.l2dAzDeg           = l->m_l2dAzDeg;
        info.l2dElDeg           = l->m_l2dElDeg;
        if (l->m_l2dUserPinned) {
            if (auto mon = l->m_monitor.lock()) {
                info.l2dX = mon->m_position.x;
                info.l2dY = mon->m_position.y;
            }
        }
        if (l->m_reqResolution) {
            info.w = (int)l->m_reqResolution->x;
            info.h = (int)l->m_reqResolution->y;
        }
        if (l->m_reqRefresh)
            info.refresh = *l->m_reqRefresh;
        // WP X1/X3: the declaration (main thread wrote it) and what the frame thread last actually
        // submitted. Both are plain atomics — no refcount, no string crosses a thread here.
        // `stereo` is the split, `producer` is WHO made the panes — the two are reported separately
        // because they answer different questions: "sbs"/"depth" is the depth desktop doing its job,
        // "sbs"/"content" is a client's packed frame, and "sbs"/"depth" with quads 1 is a pair the
        // budget refused or the kill switch flattened.
        const auto DECL = OpenXR::Stereo::unpackDecl(l->m_stereoPairDecl.load(std::memory_order_acquire));
        info.stereo     = Render::Stereo::layoutToString(DECL.layout);
        info.producer   = OpenXR::Stereo::producerToString(DECL.producer);
        info.chrome     = l->m_chromeLive.load(std::memory_order_relaxed);
        info.quads      = (int)l->m_quadsSubmitted.load(std::memory_order_relaxed);
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
// 2D-plane sync (report 12 WP-S2): keep Hyprland's native monitor-layout plane in sync with where
// the XR quads actually float, so the cursor leaves XR-main's right edge and lands on XR-side
// exactly where your eyes expect. Everything here is MAIN THREAD: the pure projection lives in
// XRLayout2D (WP-S1), and the pose snapshot is taken under m_layersMu exactly as layoutDump() takes
// it — the frame thread already publishes CXRAnchor::lastWorld() for status, so this needs no new
// cross-thread plumbing and does zero refcount work off-main.
// ---------------------------------------------------------------------------------------------

bool COpenXRManager::layout2DEnabled() {
    static auto PEN = CConfigValue<Hyprlang::INT>("openxr:layout2d:enabled");
    return *PEN != 0;
}

OpenXR::SXRLayout2DConfig COpenXRManager::readLayout2DConfig() {
    // MAIN THREAD ONLY — two of these are STRING config values, which the frame thread may never
    // touch (XRMonitorLayer.hpp threading rule). Nothing here ever runs off-main.
    static auto PPXDEG   = CConfigValue<Hyprlang::FLOAT>("openxr:layout2d:px_per_degree");
    static auto PPXM     = CConfigValue<Hyprlang::FLOAT>("openxr:layout2d:px_per_meter");
    static auto PVERT    = CConfigValue<std::string>("openxr:layout2d:vertical");
    static auto POVERLAP = CConfigValue<Hyprlang::INT>("openxr:layout2d:min_overlap_px");
    static auto PROWMRG  = CConfigValue<Hyprlang::FLOAT>("openxr:layout2d:row_merge_deg");
    static auto PHYST    = CConfigValue<Hyprlang::FLOAT>("openxr:layout2d:reorder_hysteresis_deg");

    OpenXR::SXRLayout2DConfig cfg;
    cfg.pxPerDegree          = (float)*PPXDEG;
    cfg.pxPerMeter           = (float)*PPXM;
    cfg.vertical             = OpenXR::xrParseLayout2DVertical(*PVERT).value_or(OpenXR::XR_L2D_VERT_ELEVATION);
    cfg.minOverlapPx         = (int)*POVERLAP;
    cfg.rowMergeDeg          = (float)*PROWMRG;
    cfg.reorderHysteresisDeg = (float)*PHYST;
    return cfg;
}

OpenXR::eXRLayout2DAttach COpenXRManager::readLayout2DAttach() {
    static auto PATTACH = CConfigValue<std::string>("openxr:layout2d:attach");
    return OpenXR::xrParseLayout2DAttach(*PATTACH).value_or(OpenXR::XR_L2D_ATTACH_RIGHT);
}

void COpenXRManager::releaseLayout2DRuleOffset(const std::string& name) {
    if (!Config::monitorRuleMgr())
        return;

    // XR-owned offsets are only ever persisted in the exact-name rule installed by
    // registerDeclaredMonitorRule. Read that raw rule rather than get(mon): the monitor is already
    // gone on external-destroy path B, and matching a selector requires the live monitor object.
    const auto& rules = Config::monitorRuleMgr()->all();
    const auto  found = std::ranges::find_if(rules, [&](const auto& r) { return r.m_name == name && r.m_offsetOwnedByXR; });
    if (found == rules.end())
        return;

    Config::CMonitorRule rule = *found;
    rule.m_offset             = Vector2D{-INT32_MAX, -INT32_MAX};
    rule.m_offsetOwnedByXR    = false;
    Config::monitorRuleMgr()->add(std::move(rule));
}

void COpenXRManager::releaseLayout2DPlacements() {
    // MAIN THREAD. Clear every offset the sync engine owns, back to the {-INT32_MAX,-INT32_MAX}
    // "auto" sentinel arrange() treats as append-right. Idempotent: a no-op once nothing is placed.
    bool                     changed = false;
    std::vector<std::string> released;
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers) {
            if (!l->m_l2dPlaced)
                continue;
            l->m_l2dPlaced = false;
            released.push_back(l->m_monitorName);
            if (auto mon = l->m_monitor.lock()) {
                mon->m_activeMonitorRule.m_offset          = Vector2D{-INT32_MAX, -INT32_MAX};
                mon->m_activeMonitorRule.m_offsetOwnedByXR = false;
            }
            changed = true;
        }
    }
    m_l2dPrev.clear();
    m_l2dPlacedCount = 0;
    m_l2dRows = m_l2dWidth = m_l2dHeight = 0;
    for (const auto& name : released)
        releaseLayout2DRuleOffset(name);
    if (!changed)
        return;
    Log::logger->log(Log::DEBUG, "[OPENXR] 2D-plane sync off — XR monitors handed back to auto placement");
    State::monitorLayoutController()->scheduleRecheck();
}

void COpenXRManager::latchLayout2DReference() {
    // §3a: world-anchored monitors are measured against a LATCHED "desk orientation", never the live
    // head yaw — turning your chair must not re-map your mouse. Re-latched only at moments that mean
    // "this is how I'm sitting now": session start, `center`/recenter, an explicit `sync-layout`.
    const auto ctx = currentVerbContext();
    if (!ctx.viewValid)
        return; // no tracking yet — keep whatever we had (possibly nothing; the sync then no-ops)

    // The reference yaw in the projection's RIGHT-POSITIVE convention, straight from the forward
    // vector. NOT qYawOf(), which is atan2(-f.x, -f.z) — the negation — and would mirror the layout.
    const OpenXR::Vec3 fwd = OpenXR::poseForward(ctx.view.rot);
    const float        horiz = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);

    m_l2dRef.eye   = ctx.view.pos;
    m_l2dRef.yaw   = horiz < 1e-4F ? m_l2dRef.yaw : std::atan2(fwd.x, -fwd.z); // looking straight up/down: keep the last yaw
    m_l2dRef.valid = true;
    Log::logger->log(Log::DEBUG, "[OPENXR] 2D-plane sync: reference frame latched (eye [{:.2f}, {:.2f}, {:.2f}], yaw {:.1f} deg)", m_l2dRef.eye.x, m_l2dRef.eye.y, m_l2dRef.eye.z,
                     m_l2dRef.yaw * 57.29578F);
}

void COpenXRManager::requestLayout2DSync() {
    // The debounced funnel every trigger uses. Re-arming (rather than only arming) is what makes a
    // burst — five monitors declared in a config, or a flurry of grab releases — cost ONE relayout:
    // the timer only fires once the events stop. NEVER call syncLayout2D() straight from a trigger.
    // NOTE: this deliberately does NOT check layout2DEnabled(). Turning the feature off at runtime
    // has to reach syncLayout2D so it can hand every monitor back to the auto path — an offset we
    // wrote would otherwise sit in the rule forever.
    if (!g_pEventLoopManager || m_l2dFrozen)
        return;

    static auto PDEB = CConfigValue<Hyprlang::INT>("openxr:layout2d:debounce_ms");
    const auto  dur  = std::chrono::milliseconds(std::max(0, (int)*PDEB));

    if (!m_l2dTimer) {
        m_l2dTimer = makeShared<CEventLoopTimer>(
            dur, [](SP<CEventLoopTimer>, void*) {
                // Capture NOTHING and re-check the global: a deferred callback must never run
                // against a destroyed manager (same discipline as requestEffectEval).
                if (g_pOpenXRManager)
                    g_pOpenXRManager->onLayout2DSyncDue();
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_l2dTimer);
    } else
        m_l2dTimer->updateTimeout(dur);
}

void COpenXRManager::onLayout2DSyncDue() {
    if (m_l2dTimer)
        m_l2dTimer->updateTimeout(std::nullopt); // one-shot; requestLayout2DSync re-arms
    syncLayout2D(false);
}

void COpenXRManager::syncLayout2D(bool force) {
    // MAIN THREAD. One pass: snapshot poses -> project -> attach the block -> write the offsets ->
    // let the ordinary arrange() pipeline do placement, xdg-output and monitor.layoutChanged.
    if (m_l2dFrozen && !force)
        return;
    if (!layout2DEnabled()) {
        // Switched off (config or `hyprctl keyword openxr:layout2d:enabled 0`). Give every monitor we
        // placed back to the ordinary append-right path instead of leaving it pinned at an offset
        // nothing maintains any more — an off switch that leaves the layout frozen where it was is
        // not an off switch.
        releaseLayout2DPlacements();
        return;
    }

    if (!m_l2dRef.valid)
        latchLayout2DReference();
    if (!m_l2dRef.valid) {
        // No tracked head pose has ever been seen (no session, or tracking not up). Falling back to
        // today's auto-append-right is the correct behaviour, not an error: we have nothing to
        // measure azimuths against, and guessing would produce a layout that changes the moment
        // tracking arrives.
        return;
    }

    const auto cfg    = readLayout2DConfig();
    const auto attach = readLayout2DAttach();

    std::vector<OpenXR::SXRLayout2DInput> in;
    std::vector<PXRLAYER>                 chosen;
    bool                                  carrying = false;
    int                                   pinned   = 0;
    {
        std::scoped_lock lock(m_layersMu);

        // FREEZE while anything is being carried (§4, "NEVER mid-grab"). Moving the plane under a
        // live carry would yank the cursor and re-tile windows in the middle of a drag, and the pose
        // we'd measure is the transient grip pose, not where the monitor is going to end up. Retry
        // after the debounce instead of dropping the request, so the release still gets its relayout
        // even if the release event itself is missed.
        for (auto& l : m_layers) {
            if (l->m_pendingRemoval.load(std::memory_order_acquire))
                continue;
            if (l->m_anchor.grabbed() || l->m_anchor.gazeGrabbed()) {
                carrying = true;
                break;
            }
        }

        if (!carrying) {
            for (auto& l : m_layers) {
                if (l->m_pendingRemoval.load(std::memory_order_acquire))
                    continue;

                // An adopted pre-existing output (m_createdByXR == false) is one of the user's REAL
                // monitors that an xrmonitor line mirrors into XR — its 2D position belongs to the
                // physical layout and must never be driven from a quad pose. Same guard as
                // setMonitorsPlugged's.
                if (!l->m_createdByXR)
                    continue;

                // An explicit user `monitor=` offset OWNS that monitor's position: the sync engine
                // never fights it (§4 per-monitor opt-out). arrange() places explicit-position
                // monitors first, and the attach seam below puts our block clear of them.
                if (l->m_l2dUserPinned) {
                    ++pinned;
                    continue;
                }

                // A controller-locked quad is a hand-held palette, not a desktop (§3, DEVICE
                // excluded) — it has no stable place in a mouse-crossing plane.
                const auto& st = l->m_anchor.state();
                if (st.mode == OpenXR::XR_ANCHOR_DEVICE)
                    continue;

                // Only monitors that are actually in the layout: a monitor held UNPLUGGED (headset
                // doffed, research/18) is not in monitorState()->monitors(), so reserving a box for
                // it would leave a hole the cursor falls into.
                auto mon = l->m_monitor.lock();
                if (!mon || !mon->m_enabled)
                    continue;

                OpenXR::SXRLayout2DInput m;
                m.name = l->m_monitorName;
                // Same live-vs-declared choice layoutDump() makes, for the same reasons: a LOCAL
                // monitor reports its live solved world pose, but an ADAPTIVE one reports its SAVED
                // desk pose even while roaming — the desk pose is the persistent identity, and
                // mapping the mouse to wherever a monitor followed you to would churn the plane
                // every time you stood up.
                const bool useLive = st.mode == OpenXR::XR_ANCHOR_LOCAL && !st.adaptive.enabled && l->m_anchor.hasLastWorld();
                m.pose             = useLive ? l->m_anchor.lastWorld() : st.anchorPose;
                // head/body offsets are already expressed in their own (stable) follow frame and are
                // merged onto the same plane (§3b): the 2D map represents the arrangement as seen
                // from the reference pose, where the desk frame and the view frame coincide.
                m.followFrame      = st.mode == OpenXR::XR_ANCHOR_HEAD || st.mode == OpenXR::XR_ANCHOR_BODY;
                // LOGICAL size — the unit CMonitorPositionController::arrange works in.
                m.w                = (int)mon->m_size.x;
                m.h                = (int)mon->m_size.y;

                in.push_back(m);
                chosen.push_back(l);
            }
        }
    }

    if (carrying) {
        requestLayout2DSync();
        return;
    }
    if (in.empty()) {
        releaseLayout2DPlacements();
        return;
    }

    const auto res = OpenXR::xrProjectLayout2D(in, m_l2dRef, cfg, m_l2dPrev);

    // ---- attach the block to the physical/pinned monitors (§2.5) ----
    // Physical outputs have NO room pose — they aren't in XR space — so we can only arrange XR
    // monitors among themselves and translate the finished block to a seam. Only EXPLICITLY
    // positioned monitors may feed that computation; see xrLayout2DAttachOrigin for why (arrange()
    // appends auto monitors downstream of our block, so measuring one would measure our own previous
    // output and the layout would march rightwards a block-width per event).
    std::vector<OpenXR::SXRLayout2DAnchorBox> anchors;
    for (const auto& mon : State::monitorState()->monitors()) {
        if (std::ranges::any_of(chosen, [&](const PXRLAYER& l) { return l->m_monitorName == mon->m_name; }))
            continue; // one of ours: it is being placed by this very pass
        if (!mon->explicitPosition().has_value())
            continue; // auto: its position is downstream of ours
        anchors.push_back(OpenXR::SXRLayout2DAnchorBox{(int)mon->m_position.x, (int)mon->m_position.y, (int)mon->m_size.x, (int)mon->m_size.y});
    }

    int ox = 0, oy = 0;
    OpenXR::xrLayout2DAttachOrigin(anchors, res.width, res.height, attach, ox, oy);
    const Vector2D origin{(double)ox, (double)oy};

    // ---- write the offsets ----
    // The report's "cleanest integration" (§5): set each XR monitor's rule offset (so
    // explicitPosition() reports it and every future arrange() honours it) and let the existing
    // pipeline do the rest. moveTo() then shifts floating windows by the delta, re-arranges the
    // monitor's LAYER SURFACES (c3bdf3aa — layers cache GLOBAL geometry and would otherwise be
    // stranded outside their own monitor), relayouts tiled windows, and re-clamps the cursor.
    bool                     changed = false;
    std::vector<std::string> released;
    {
        std::scoped_lock lock(m_layersMu);

        // Hand back any monitor we used to place but no longer do (it re-anchored to a controller,
        // or the user pinned it). Leaving our stale offset on it would freeze it at a position
        // nothing maintains any more; clearing to the "auto" sentinel returns it to the ordinary
        // append-right path, which is exactly what an unmanaged XR monitor should do.
        for (auto& l : m_layers) {
            if (!l->m_l2dPlaced || std::ranges::any_of(chosen, [&](const PXRLAYER& c) { return c == l; }))
                continue;
            l->m_l2dPlaced = false;
            released.push_back(l->m_monitorName);
            if (auto mon = l->m_monitor.lock()) {
                mon->m_activeMonitorRule.m_offset          = Vector2D{-INT32_MAX, -INT32_MAX};
                mon->m_activeMonitorRule.m_offsetOwnedByXR = false;
            }
            changed = true;
        }

        for (size_t i = 0; i < chosen.size(); ++i) {
            const auto&    slot = res.slots[i];
            const Vector2D pos{origin.x + (double)slot.x, origin.y + (double)slot.y};
            auto&          l = chosen[i];

            if (!l->m_l2dPlaced || l->m_l2dOffset != pos)
                changed = true;

            l->m_l2dPlaced = true;
            l->m_l2dOffset = pos;
            l->m_l2dCol    = slot.col;
            l->m_l2dRow    = slot.row;
            l->m_l2dAzDeg  = slot.azDeg;
            l->m_l2dElDeg  = slot.elDeg;

            if (auto mon = l->m_monitor.lock()) {
                // "Nothing moved" must be judged against the COMPOSITOR, not against our own memory
                // of the last pass. Applying a monitor rule replaces m_activeMonitorRule wholesale
                // (CMonitor::applyMonitorRule takes the config-derived rule by value), so anything
                // that re-applies rules — a reload, the rule manager's scheduled re-apply, a mode
                // change — silently drops the offset we wrote here and the monitors fall back to
                // plain append-right. Judged from our own m_l2dOffset the next pass computes the
                // same number, concludes nothing moved, skips the recheck, and the fallback layout
                // stands for the rest of the session: a monitor floating to your left keeps a
                // layout x to your right, and no later sync ever repairs it (observed in the XR
                // suite: engine offsets 0/2048, monitors at 2596/1572, stable across nine forced
                // syncs). Noticing the disagreement here is what makes the sync self-healing.
                if (mon->m_activeMonitorRule.m_offset != pos || mon->m_position != pos)
                    changed = true;
                mon->m_activeMonitorRule.m_offset          = pos;
                mon->m_activeMonitorRule.m_offsetOwnedByXR = true;
            }
        }
    }

    for (const auto& name : released)
        releaseLayout2DRuleOffset(name);

    m_l2dPrev        = OpenXR::xrLayout2DPrevOf(res);
    m_l2dPlacedCount = (int)chosen.size();
    m_l2dRows        = res.rows;
    m_l2dWidth       = res.width;
    m_l2dHeight      = res.height;

    if (!changed)
        return; // the common case: a trigger fired but nothing actually moved (hysteresis held)

    Log::logger->log(Log::DEBUG, "[OPENXR] 2D-plane sync: placed {} monitor(s) in {} row(s), block {}x{} at [{}, {}]{}", chosen.size(), res.rows, res.width, res.height, origin.x,
                     origin.y, pinned > 0 ? std::format(" ({} pinned by monitor= rules)", pinned) : "");

    State::monitorLayoutController()->scheduleRecheck();
    if (g_pEventManager)
        g_pEventManager->postEvent(SHyprIPCEvent{"xrlayout2dsync", std::format("{}", chosen.size())});
}

COpenXRManager::SXRLayout2DStatus COpenXRManager::layout2DStatus() {
    SXRLayout2DStatus s;
    s.enabled = layout2DEnabled();
    s.frozen  = m_l2dFrozen;
    if (!s.enabled)
        return s;
    const auto cfg  = readLayout2DConfig();
    s.refValid      = m_l2dRef.valid;
    s.refYawDeg     = m_l2dRef.yaw * 57.29578F;
    s.placed        = m_l2dPlacedCount;
    s.rows          = m_l2dRows;
    s.width         = m_l2dWidth;
    s.height        = m_l2dHeight;
    s.vertical      = OpenXR::xrLayout2DVerticalName(cfg.vertical);
    s.attach        = OpenXR::xrLayout2DAttachName(readLayout2DAttach());
    s.pxPerDegree   = cfg.pxPerDegree;
    return s;
}

// ---- ray-cast cursor edge crossing (task #139, XRCursorCross.hpp) --------------------------------

void COpenXRManager::clearCrossingSubmissionState() {
    // MAIN THREAD, with the frame thread absent/joined or the global view gate closed. Submission is
    // session/frame state; quad geometry and partial cursor pressure derived from it must cross
    // neither a view gate nor a session boundary.
    {
        std::scoped_lock lock(m_layersMu);
        for (auto& l : m_layers)
            l->m_quadsSubmitted.store(0, std::memory_order_relaxed);
    }
    m_crossQuads.clear();
    m_crossQuadLayers.clear();
    m_crossQuadsMs = 0;
    m_crossPush.reset();
}

void COpenXRManager::refreshCrossQuads(int64_t nowMs) {
    // MAIN THREAD. See the header for why only the 3D half is cached and why the TTL exists.
    if (m_crossQuadsMs != 0 && nowMs - m_crossQuadsMs < OpenXR::XR_CROSS_GEOM_TTL_MS)
        return;
    m_crossQuadsMs = nowMs;
    m_crossQuads.clear();
    m_crossQuadLayers.clear();

    std::scoped_lock lock(m_layersMu);
    for (auto& l : m_layers) {
        if (l->m_pendingRemoval.load(std::memory_order_acquire))
            continue;
        if (l->m_quadsSubmitted.load(std::memory_order_relaxed) == 0)
            continue;
        const MONITORID id = l->m_monitorId.load(std::memory_order_acquire);
        if (id < 0)
            continue;
        // No solved world pose = we do not know where this quad IS this session (never submitted, or
        // the session has not produced a frame yet). Guessing from the persistent anchorPose would be
        // wrong for every follow mode, where anchorPose is an OFFSET in a frame we would have to
        // re-solve, so such a monitor simply is not a crossing candidate.
        if (!l->m_anchor.hasLastWorld())
            continue;
        const auto& st = l->m_anchor.state();
        // Same exclusion the 2D sync makes (syncLayout2D): a device-anchored quad is a hand-held
        // palette that rides your controller, not a place in the room to send a cursor.
        if (st.mode == OpenXR::XR_ANCHOR_DEVICE)
            continue;
        // Content (not chrome-expanded) metres, derived exactly as CXRAnchor::solve does, so the
        // rectangle we intersect is the rectangle of desktop pixels the user sees.
        //
        // "Exactly as solve does" is load-bearing and used to be wrong here: solve feeds itself
        // presentedPaneSize(), so widthMeters is ONE PANE's width, while m_contentSize is the whole
        // packed mode. Dividing the two mixes the units and the crossing rectangle comes out at half
        // its height on every paired monitor — a ray aimed at the bottom of a quad would cross to
        // nothing. Latent while only a fullscreen 3D film paired (WP X1); the steady state once
        // every XR monitor is a depth pack (WP X3).
        const auto           PANEPX = Render::Stereo::presentedPaneSize(l->m_contentSize, layerDecl(*l).layout);
        const double         pxW    = PANEPX.x > 0.0 ? PANEPX.x : 1.0;
        const double         pxH    = PANEPX.y > 0.0 ? PANEPX.y : 1.0;

        OpenXR::SXRCrossQuad q;
        q.id      = id;
        q.pose    = l->m_anchor.lastWorld();
        q.wMeters = st.widthMeters;
        q.hMeters = st.widthMeters * (float)(pxH / pxW);
        m_crossQuads.push_back(q);
        m_crossQuadLayers.push_back(l);
    }
}

void COpenXRManager::resetCursorCrossing() {
    // MAIN THREAD. Absolute/external cursor warps do not pass through redirectCursorCrossing, so
    // CPointerManager calls this to prevent a pre-warp edge gesture carrying into the new place.
    m_crossPush.reset();
}

std::optional<Vector2D> COpenXRManager::redirectCursorCrossing(const Vector2D& oldPos, const Vector2D& newPos) {
    // MAIN THREAD (CPointerManager::move). Every early return means "leave it to the 2D layout".
    if (m_cursorCrossMode.load(std::memory_order_relaxed) != OpenXR::XR_CURSORCROSS_RAYCAST) {
        m_crossPush.reset();
        return std::nullopt;
    }
    if (!m_running.load(std::memory_order_acquire)) {
        m_crossPush.reset();
        return std::nullopt;
    }
    if (!m_monitorViewVisible.load(std::memory_order_acquire)) {
        m_crossPush.reset();
        return std::nullopt;
    }
    if (oldPos == newPos)
        return std::nullopt;

    // The exact predicate CPointerManager::onMonitorLayoutChange uses to build the boxes the cursor
    // is clamped to. Anything it excludes is not part of the layout the crossing is happening in.
    const auto& mons     = State::monitorState()->monitors();
    const auto  inLayout = [](const PHLMONITOR& m) { return m && m->m_enabled && !m->isMirror() && m->m_output; };
    const auto  byId     = [&](MONITORID id) -> PHLMONITOR {
        for (auto const& m : mons) {
            if (inLayout(m) && m->m_id == id)
                return m;
        }
        return nullptr;
    };

    // 1. The monitor being LEFT. oldPos is a previously-clamped cursor position, so it is inside
    //    some box; if this motion does not take it out of that box there is no crossing to decide.
    PHLMONITOR src;
    for (auto const& m : mons) {
        if (!inLayout(m))
            continue;
        if (m->logicalBox().containsPoint(oldPos)) {
            src = m;
            break;
        }
    }
    if (!src) {
        m_crossPush.reset();
        return std::nullopt;
    }
    const CBox    srcBox = src->logicalBox();
    const int64_t nowMs  = (int64_t)Time::millis(Time::steadyNow());
    const auto exitUV = OpenXR::xrAccumulateCrossPush(m_crossPush, src->m_id, srcBox, oldPos, newPos, nowMs, OpenXR::XR_CROSS_PUSH_TIMEOUT_MS, OpenXR::XR_CROSS_MAX_OVERSHOOT_UV);
    if (!exitUV)
        return std::nullopt;

    // 2. Where the head is NOW. A ray cast from a stale head is a ray cast from where the user is
    //    not, which is worse than the layout answer — so staleness falls back rather than guesses.
    OpenXR::SXRPoseSample head;
    if (!newestPoseSample(head) || !head.viewValid) {
        m_crossPush.reset();
        return std::nullopt;
    }
    if (nowMs - head.timestampMs > OpenXR::XR_CROSS_POSE_MAX_AGE_MS) {
        m_crossPush.reset();
        return std::nullopt;
    }

    // 3. Is the monitor we are leaving an XR quad at all? Crossing OFF a physical output keeps the
    //    2D behaviour: a physical output has no pose in the room, so there is no plane to exit from.
    refreshCrossQuads(nowMs);
    const OpenXR::SXRCrossQuad* srcQuad = nullptr;
    for (size_t i = 0; i < m_crossQuads.size(); ++i) {
        const auto& q     = m_crossQuads[i];
        const auto  layer = m_crossQuadLayers[i].lock();
        if (!layer || layer->m_quadsSubmitted.load(std::memory_order_relaxed) == 0)
            continue;
        if (q.id == src->m_id) {
            srcQuad = &q;
            break;
        }
    }
    if (!srcQuad || !(srcQuad->wMeters > 0.F) || !(srcQuad->hMeters > 0.F)) {
        m_crossPush.reset();
        return std::nullopt;
    }

    // 4. Candidates: every OTHER XR quad that is currently a real, enabled monitor in the layout.
    //    (Physical outputs are never candidates — nothing to intersect. That asymmetry is by
    //    construction and documented: crossing between XR and physical monitors stays 2D.)
    std::vector<OpenXR::SXRCrossQuad> cands;
    cands.reserve(m_crossQuads.size());
    for (size_t i = 0; i < m_crossQuads.size(); ++i) {
        const auto& q     = m_crossQuads[i];
        const auto  layer = m_crossQuadLayers[i].lock();
        if (!layer || layer->m_quadsSubmitted.load(std::memory_order_relaxed) == 0)
            continue;
        if (q.id == srcQuad->id || !byId(q.id))
            continue;
        cands.push_back(q);
    }
    if (cands.empty()) {
        m_crossPush.reset();
        return std::nullopt;
    }

    // 5. The exit point on the source quad's EXTENDED plane, and the line of sight through it.
    const OpenXR::Vec3 through = OpenXR::quadPointFromUV(srcQuad->pose, srcQuad->wMeters, srcQuad->hMeters, (float)exitUV->x, (float)exitUV->y);
    const auto         pick    = OpenXR::xrPickCrossTarget(head.headPos, through, cands, srcQuad->id, OpenXR::XR_CROSS_TOLERANCE_DEG);
    if (!pick.ok)
        return std::nullopt; // nothing over there — the layout's answer is as good as any

    // 6. The hit point, in the target's layout pixels. Its box is read LIVE (never from the cached
    //    snapshot) so a 2D-sync relayout between motion events cannot land us on stale coordinates.
    auto tgt = byId(pick.id);
    if (!tgt) {
        m_crossPush.reset();
        return std::nullopt;
    }
    const Vector2D land = OpenXR::xrCrossEntryPoint(tgt->logicalBox(), pick.u, pick.v, OpenXR::XR_CROSS_ENTRY_INSET_PX);
    Log::logger->log(Log::TRACE, "[OPENXR] cursor ray-crossing {} -> {} at uv {:.3f},{:.3f} ({:.2f}m{})", src->m_name, tgt->m_name, pick.u, pick.v, pick.t,
                     pick.tolerated ? ", tolerated" : "");
    m_crossPush.reset();
    return land;
}

std::expected<void, std::string> COpenXRManager::cmdSyncLayout(const std::string& args) {
    // At most one token; trimmed here rather than through splitWs, which is defined further down
    // with the rest of the verb helpers.
    std::string arg = args;
    while (!arg.empty() && std::isspace((unsigned char)arg.front()))
        arg.erase(arg.begin());
    while (!arg.empty() && std::isspace((unsigned char)arg.back()))
        arg.pop_back();

    if (arg.empty()) {
        if (!layout2DEnabled())
            return std::unexpected<std::string>("2D-plane sync is off (set openxr:layout2d:enabled = true)");
        // An explicit sync is also the natural "re-sync my layout to how I'm sitting NOW" moment, so
        // it re-latches the desk orientation before projecting (§4) — same as `center`.
        latchLayout2DReference();
        if (!m_l2dRef.valid)
            return std::unexpected<std::string>("no head tracking yet — cannot latch a reference frame");
        syncLayout2D(/*force=*/true);
        return {};
    }
    if (arg == "freeze") {
        m_l2dFrozen = true;
        if (m_l2dTimer)
            m_l2dTimer->updateTimeout(std::nullopt);
        return {};
    }
    if (arg == "thaw") {
        m_l2dFrozen = false;
        requestLayout2DSync();
        return {};
    }
    return std::unexpected<std::string>("sync-layout: unknown argument '" + arg + "' (expected freeze|thaw)");
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
    // doc 05 §xrrule: local <-> head/body/device IS a docked <-> follow transition.
    if (modeChanged)
        requestEffectEval();
    // report 12 §3: re-anchoring changes WHICH frame the monitor is measured in (world vs follow vs
    // excluded-entirely for device), and an explicit offset moves it outright.
    requestLayout2DSync();
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

    const auto ctx = currentVerbContext();
    {
        std::scoped_lock lock(m_layersMu);
        if (!layer->m_anchor.applyMove(OpenXR::Vec3{*dx, *dy, *dz}, ctx))
            return std::unexpected<std::string>("move: head tracking (or controller for device anchors) unavailable");
    }
    requestLayout2DSync(); // report 12 §4: a pose verb moved a quad -> re-derive the plane (debounced)
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
    {
        std::scoped_lock lock(m_layersMu);
        layer->m_anchor.applyRotate(*yaw * DEG2RAD, pitch * DEG2RAD, ctx);
    }
    requestLayout2DSync();
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdAlpha(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 2)
        return std::unexpected<std::string>("alpha: expected <name|active> <0..1|auto>");

    const std::string& target = tokens[0];
    PXRLAYER           layer  = target == "active" ? resolveSelected() : layerByName(target);
    if (!layer)
        return std::unexpected<std::string>(target == "active" ? "no XR monitor selected" : "no XR monitor named '" + target + "'");

    const std::string val = tokens[1];
    std::optional<float> want;
    if (val != "auto") {
        auto f = parseFloatArg(val);
        if (!f || *f < 0.F || *f > 1.F)
            return std::unexpected<std::string>("alpha: expected a number in 0..1, or `auto` to hand the monitor back to the rules");
        want = *f;
    }

    {
        std::scoped_lock lock(m_layersMu);
        layer->m_manualFx.alpha = want; // nullopt == `auto` == back under rule control
    }
    // Re-resolve now: the manual layer sits ABOVE the rules, and clearing it must immediately fall
    // back to whatever the rules say for the monitor's CURRENT situation.
    requestEffectEval();
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdBlackAlpha(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 2)
        return std::unexpected<std::string>("blackalpha: expected <name|active> <0..1|off|auto>");

    const std::string& target = tokens[0];
    PXRLAYER           layer  = target == "active" ? resolveSelected() : layerByName(target);
    if (!layer)
        return std::unexpected<std::string>(target == "active" ? "no XR monitor selected" : "no XR monitor named '" + target + "'");

    const std::string    val = tokens[1];
    std::optional<float> want;
    if (val == "off")
        want = 1.F; // off == every pixel opaque == no keying
    else if (val != "auto") {
        auto f = parseFloatArg(val);
        if (!f || *f < 0.F || *f > 1.F)
            return std::unexpected<std::string>("blackalpha: expected a number in 0..1, `off`, or `auto` to hand the monitor back to the rules");
        want = *f;
    }

    {
        std::scoped_lock lock(m_layersMu);
        layer->m_manualFx.blackAlpha = want;
    }
    requestEffectEval();
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

    {
        std::scoped_lock lock(m_layersMu);
        layer->m_anchor.applyScale(isDelta, *f);
        layer->m_sizeMeters = layer->m_anchor.state().widthMeters;
    }
    requestLayout2DSync();
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

    const auto ctx = currentVerbContext();
    {
        std::scoped_lock lock(m_layersMu);
        if (!layer->m_anchor.applyDistance(*dm, ctx))
            return std::unexpected<std::string>("distance: head tracking unavailable or no current pose");
    }
    requestLayout2DSync();
    return {};
}

std::expected<void, std::string> COpenXRManager::cmdCenter() {
    auto layer = resolveSelected();
    if (!layer)
        return std::unexpected<std::string>("no XR monitor selected");

    static auto PDEFDIST = CConfigValue<Hyprlang::FLOAT>("openxr:default_distance");
    const auto  ctx      = currentVerbContext();
    {
        std::scoped_lock lock(m_layersMu);
        if (!layer->m_anchor.applyCenter(ctx, (float)*PDEFDIST))
            return std::unexpected<std::string>("center: head tracking unavailable");
    }
    // report 12 §3a/§4: `center` is a "this is how I'm sitting now" moment, so it RE-LATCHES the desk
    // orientation the whole world-monitor projection is measured against, then re-derives the plane.
    latchLayout2DReference();
    requestLayout2DSync();
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
    requestLayout2DSync();
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
    requestEffectEval(); // doc 05 §xrrule: adaptive on/off can change the docked/follow state
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
    requestEffectEval(); // doc 05 §xrrule: forced dock -> anchorstate docked
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
    requestEffectEval(); // doc 05 §xrrule: forced undock -> anchorstate follow
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

std::expected<void, std::string> COpenXRManager::cmdView(const std::string& args) {
    const auto tokens = splitWs(args);
    if (tokens.size() != 1)
        return std::unexpected<std::string>("view: expected on|off|toggle");
    const std::string& arg = tokens[0];
    if (arg != "on" && arg != "off" && arg != "toggle")
        return std::unexpected<std::string>("view: expected on|off|toggle");
    if (!sessionExists())
        return std::unexpected<std::string>("view: no OpenXR session");

    const bool wasVisible = m_monitorViewVisible.load(std::memory_order_acquire);
    const bool visible    = arg == "toggle" ? !wasVisible : arg == "on";
    m_monitorViewVisible.store(visible, std::memory_order_release);
    requestEffectEval();

    if (!visible) {
        // Status should reflect the gate immediately instead of retaining last frame's quad count or
        // hover until the frame thread reaches its next zero-layer submission.
        clearCrossingSubmissionState();
        setHoveredMonitor("");

        // …and so must the GRAB state, which is the one piece the frame thread cannot clean up for
        // itself. Hiding the view empties the frame loop's presentation set, so CXRInput is handed
        // no target for a monitor being carried and takes its liveness branch, which force-releases
        // only its own bookkeeping — it was written for a layer DESTROYED mid-grab, where there is
        // nothing left to re-anchor. Under the latch the layer is very much alive, so without this
        // its CXRAnchor keeps m_grabbed/m_resizing set forever: the monitor returns on `view on`
        // still device-locked to a hand, status reports it grabbed, and viewpoint eligibility
        // (which requires !grabbed()) is blocked for the rest of the session. End them here, on the
        // main thread, re-anchoring each quad from the pose it was last displayed at.
        //
        // currentVerbContext() takes m_layersMu, so it must be read BEFORE the lock below (same
        // ordering as cmdGazeRelease). A gaze carry is deliberately left alone: its state is not
        // force-cleared by anything, and `openxr gazerelease` still ends it while the view is
        // hidden.
        const auto            CTX  = currentVerbContext();
        const auto            TUNE = readAnchorTuning();
        OpenXR::SXRSolveInput in;
        in.view      = CTX.view;
        in.gripLeft  = CTX.gripLeft;
        in.gripRight = CTX.gripRight;

        std::vector<std::string> aborted;
        {
            std::scoped_lock lock(m_layersMu);
            for (auto& l : m_layers) {
                if (l->m_anchor.abortGrab(in, TUNE))
                    aborted.push_back(l->m_monitorName);
            }
        }
        for (const auto& name : aborted)
            Log::logger->log(Log::DEBUG, "[OPENXR] monitor view hidden mid-grab: released '{}' where it was last displayed", name);
    }

    Log::logger->log(Log::DEBUG, "[OPENXR] monitor view {}", visible ? "shown" : "hidden");
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
    // hypxrvoice GAP 4 — the ray/quad hit on the stable candidate, computed by gazeSelectPass this
    // same frame on this same thread. Plain floats, so the ring stays a POD the main thread can copy.
    s.gazeHitValid  = m_gazeHitValid;
    s.gazeHitPoint  = m_gazeHitPoint;
    s.gazeHitDist   = m_gazeHitDist;

    std::scoped_lock lock(m_poseRingMu);
    m_poseRing.push(s);
}

bool COpenXRManager::newestPoseSample(OpenXR::SXRPoseSample& out) {
    // MAIN THREAD, m_poseRingMu only — see the header for why this is not gazeSampleNow().
    std::scoped_lock lock(m_poseRingMu);
    if (m_poseRing.empty())
        return false;
    out = m_poseRing.newest();
    return true;
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
    out.hitValid         = s.gazeHitValid && out.selected;
    out.hitPoint         = s.gazeHitPoint;
    out.hitDist          = s.gazeHitDist;
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
    out.hitValid             = s.gazeHitValid && out.selected;
    out.hitPoint             = s.gazeHitPoint;
    out.hitDist              = s.gazeHitDist;
    if (s.gazeMonitorId >= 0)
        if (auto l = layerByMonitorID((MONITORID)s.gazeMonitorId))
            out.gazeName = l->m_monitorName;
    return out;
}

#endif
