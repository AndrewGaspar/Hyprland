#include "XRIpc.hpp"
#ifdef HAVE_OPENXR

#include <format>
#include <string>

#include <hyprutils/string/VarList.hpp>

#include "OpenXRManager.hpp"
#include "../debug/HyprCtl.hpp"
#include "../SharedDefs.hpp"

using namespace Hyprutils::String;

// hyprctl openxr status  (default when no subcommand). The bar-pollable surface (doc 05 §4.3).
static std::string openxrStatus(eHyprCtlOutputFormat format) {
    const auto        STATE   = COpenXRManager::stateToString(g_pOpenXRManager->state());
    const std::string RUNTIME = g_pOpenXRManager->runtimeName();
    const std::string SYSTEM  = g_pOpenXRManager->systemName();
    const std::string RTGPU   = g_pOpenXRManager->runtimeGpu();
    const std::string BLEND   = g_pOpenXRManager->blendModeName();
    const bool        OVERLAY = g_pOpenXRManager->isOverlay();
    const auto        MONS    = g_pOpenXRManager->monitorInfos();
    // report-18 addendum: the plugged-state follow mode and, when the headset has just been
    // doffed under `visible` mode, the ms remaining before the anti-flap grace-unplug fires (-1
    // when no unplug is pending).
    const std::string FOLLOW      = g_pOpenXRManager->monitorFollowModeName();
    const int         UNPLUG_PEND = g_pOpenXRManager->monitorUnplugPendingMs();
    // report-19: the user-presence signal driving the `visible`-mode plug gate — "yes"/"no" when the
    // runtime exposes XR_EXT_user_presence, "unknown" before the first event, "unsupported" otherwise.
    const std::string PRESENCE = g_pOpenXRManager->presenceStatusString();
    // report-20 issue D: surface the RAW visibility signal next to presence so the combined plug gate
    // (needs BOTH) is diagnosable in one command. "yes"/"no"/"n/a".
    const std::string VISIBLE = g_pOpenXRManager->visibleStatusString();
    // report-20 issue B1: dormant re-probe hint — what we're waiting for + ms until the next probe.
    const std::string REPROBE_WAIT = g_pOpenXRManager->reprobeWaitString();
    const int         REPROBE_MS   = g_pOpenXRManager->reprobePendingMs();
    // Read-only observability for the idle-inhibit predicate (doc 05 §6.1). There is otherwise
    // no queryable surface for "is the compositor's idle-inhibit bit currently raised because of
    // XR" — CIdleNotifyProtocol::isInhibited is private with no getter, and it's a fold of every
    // inhibitor source, not XR-specific anyway. This mirrors shouldInhibitIdle()'s own predicate
    // (openxr:inhibit_idle && FOCUSED) rather than adding a new getter to IdleNotify.hpp, keeping
    // the touched surface to this one file (WP12 test infra needs it to assert idle-inhibit
    // end-to-end without polling wall-clock idle timers).
    const bool         INHIBITING_IDLE = g_pOpenXRManager->shouldInhibitIdle();

    // research/16 Part A/B: conditional hand-input gate + gaze grab state.
    const auto        HANDIN = g_pOpenXRManager->handInputStatus();
    const auto        GAZE   = g_pOpenXRManager->gazeStatus();
    // WP-G5: per-hand active input device (hands vs controllers) + the hand grab gesture.
    const auto        HANDS = g_pOpenXRManager->handInputInfos();
    auto              handLabel = [](const COpenXRManager::SXRHandInputInfo& hi) -> std::string {
        if (!hi.hands)
            return "controllers";
        return hi.filtered ? std::format("hands ({}, filtered)", hi.gesture) : std::format("hands ({})", hi.gesture);
    };

    if (format == FORMAT_JSON) {
        std::string mons;
        for (size_t i = 0; i < MONS.size(); ++i) {
            const auto& m = MONS[i];
            mons += std::format(R"#(        {{
            "name": "{}",
            "id": {},
            "size_m": {:.2f},
            "anchor": {{
                "mode": "{}",
                "pose": {{
                    "pos": [{:.3f}, {:.3f}, {:.3f}],
                    "quat": [{:.4f}, {:.4f}, {:.4f}, {:.4f}]
                }}
            }},
            "grabbed": {},
            "grabKind": "{}",
            "hovered": {},
            "region": "{}",
            "plugged": {},
            "contentPath": "{}",
            "linear": {},
            "adaptive": {{
                "enabled": {},
                "phase": "{}",
                "roamMode": "{}",
                "seatDistM": {:.3f},
                "transitionT": {:.3f}
            }}
        }})#",
                                m.name, m.id, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.quatX, m.quatY, m.quatZ, m.quatW, m.grabbed ? "true" : "false",
                                m.grabKind, m.hovered ? "true" : "false", m.region, m.plugged ? "true" : "false", m.contentPath, m.linear ? "true" : "false", m.adaptiveEnabled ? "true" : "false", m.adaptivePhase,
                                m.adaptiveRoamMode, m.adaptiveSeatDist, m.adaptiveT);
            if (i + 1 < MONS.size())
                mons += ",\n";
            else
                mons += "\n";
        }

        return std::format(R"#({{
    "state": "{}",
    "runtimeName": "{}",
    "systemName": "{}",
    "runtimeGpu": "{}",
    "blendMode": "{}",
    "overlay": {},
    "monitorsFollowSession": "{}",
    "monitorUnplugPendingMs": {},
    "userPresence": "{}",
    "visible": "{}",
    "reprobeWaiting": "{}",
    "reprobePendingMs": {},
    "inhibitingIdle": {},
    "handInput": {{ "mode": "{}", "state": "{}" }},
    "gaze": {{ "source": "{}", "hoveredMonitor": {}, "hoveredName": "{}", "carrying": {}, "carryMonitor": "{}", "distM": {:.3f} }},
    "input": {{
        "left": {{ "kind": "{}", "gesture": "{}", "filtered": {} }},
        "right": {{ "kind": "{}", "gesture": "{}", "filtered": {} }}
    }},
    "monitors": [{}{}]
}}
)#",
                           STATE, RUNTIME, SYSTEM, RTGPU, BLEND, OVERLAY ? "true" : "false", FOLLOW, UNPLUG_PEND, PRESENCE, VISIBLE, REPROBE_WAIT, REPROBE_MS, INHIBITING_IDLE ? "true" : "false",
                           HANDIN.mode, HANDIN.state, GAZE.source, GAZE.hoveredMonitor, GAZE.hoveredName, GAZE.carrying ? "true" : "false", GAZE.carryMonitor, GAZE.dist,
                           HANDS[0].hands ? "hands" : "controllers", HANDS[0].gesture,
                           HANDS[0].filtered ? "true" : "false", HANDS[1].hands ? "hands" : "controllers", HANDS[1].gesture, HANDS[1].filtered ? "true" : "false",
                           MONS.empty() ? "" : "\n", mons);
    }

    const std::string FOLLOWLINE = UNPLUG_PEND >= 0 ? std::format("{} (unplug in {}ms)", FOLLOW, UNPLUG_PEND) : FOLLOW;
    // report-20 issue B1: append the dormant re-probe hint to the state line, e.g.
    // "unavailable (waiting for headset, retrying in 1800ms)".
    const std::string STATELINE = REPROBE_WAIT.empty() ? STATE : std::format("{} (waiting for {}, retrying in {}ms)", STATE, REPROBE_WAIT, REPROBE_MS < 0 ? 0 : REPROBE_MS);
    const std::string GAZELINE = GAZE.carrying ? std::format("carrying {} at {:.2f}m", GAZE.carryMonitor, GAZE.dist)
                                                : (GAZE.hoveredMonitor >= 0 ? std::format("looking at {}", GAZE.hoveredName) : "idle");
    std::string       out = std::format(
        "state: {}\nruntime: {}\nsystem: {}\nruntime gpu: {}\nblend mode: {}\noverlay: {}\nmonitors follow session: {}\nvisible: {}\npresence: {}\nidle inhibited: {}\nhand input: {} "
        "({})\ngaze ({}): {}\ninput: left {}, right {}\n",
        STATELINE, RUNTIME, SYSTEM, RTGPU.empty() ? "unknown" : RTGPU, BLEND, OVERLAY ? "yes" : "no", FOLLOWLINE, VISIBLE, PRESENCE, INHIBITING_IDLE ? "yes" : "no",
        HANDIN.state, HANDIN.mode, GAZE.source, GAZELINE, handLabel(HANDS[0]), handLabel(HANDS[1]));
    for (const auto& m : MONS) {
        out += std::format("monitor {} (ID {}): {}x{}@{:.2f} size {:.2f}m anchor {} pos [{:.2f}, {:.2f}, {:.2f}] grabbed: {} ({}) hovered: {} ({}) plugged: {} content: {}{}", m.name,
                           m.id, m.w, m.h, m.refresh, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.grabbed ? "yes" : "no", m.grabKind, m.hovered ? "yes" : "no", m.region,
                           m.plugged ? "yes" : "no", m.contentPath, m.linear ? " (linear)" : "");
        if (m.adaptiveEnabled)
            out += std::format(" adaptive: {} (roam {}, seat {:.2f}m)", m.adaptivePhase, m.adaptiveRoamMode, m.adaptiveSeatDist);
        out += "\n";
    }
    return out;
}

// hypxrvoice WP-V1: `hyprctl openxr gaze [at <ms> | --at-ms <ms>]`. Returns the current head ray +
// dwell-stable gaze candidate, OR — with a monotonic-clock timestamp — the nearest ring entry to
// that instant, so a voice daemon can resolve "drop this monitor HERE" against where the head was
// pointed AT SPEECH ONSET, not at parse time (docs/openxr/research/VOICE-CONTROL.md). The timestamp
// is CLOCK_MONOTONIC ms — the same clock the daemon reads via clock_gettime(CLOCK_MONOTONIC); see
// the CLOCK CONTRACT comment on SXRPoseRing in XRMath.hpp.
static std::string openxrGaze(eHyprCtlOutputFormat format, const std::string& args) {
    // Parse an optional target timestamp: "at <ms>" or "--at-ms <ms>" (also bare "<ms>").
    CVarList    gv(args, 0, ' ');
    bool        haveAt = false;
    int64_t     atMs   = 0;
    std::string tok0   = gv[0];
    std::string tsStr;
    if (tok0 == "at" || tok0 == "--at-ms")
        tsStr = gv[1];
    else if (!tok0.empty())
        tsStr = tok0; // bare number

    if (!tsStr.empty()) {
        try {
            atMs   = (int64_t)std::stoll(tsStr);
            haveAt = true;
        } catch (...) {
            return std::format("invalid timestamp '{}' — expected monotonic-clock milliseconds (CLOCK_MONOTONIC)", tsStr);
        }
    }

    const auto G = haveAt ? g_pOpenXRManager->gazeSampleAt(atMs) : g_pOpenXRManager->gazeSampleNow();

    if (format == FORMAT_JSON) {
        if (!G.ok)
            return "{\n    \"ok\": false,\n    \"reason\": \"no XR frame recorded yet\"\n}\n";
        std::string query;
        if (G.matched)
            query = std::format(R"#(,
    "query": {{
        "requestedTimestampMs": {},
        "matchedTimestampMs": {},
        "ageMs": {}
    }})#",
                                G.requestedTimestampMs, G.matchedTimestampMs, G.ageMs);
        return std::format(R"#({{
    "ok": true,
    "timestampMs": {},
    "viewValid": {},
    "head": {{
        "pos": [{:.4f}, {:.4f}, {:.4f}],
        "quat": [{:.5f}, {:.5f}, {:.5f}, {:.5f}],
        "forward": [{:.4f}, {:.4f}, {:.4f}]
    }},
    "gaze": {{
        "monitorId": {},
        "name": "{}",
        "selected": {},
        "dwellSec": {:.3f}
    }}{}
}}
)#",
                           G.timestampMs, G.viewValid ? "true" : "false", G.headPos.x, G.headPos.y, G.headPos.z, G.headRot.x, G.headRot.y, G.headRot.z, G.headRot.w, G.headForward.x,
                           G.headForward.y, G.headForward.z, G.gazeMonitorId, G.gazeName, G.selected ? "true" : "false", G.dwell, query);
    }

    if (!G.ok)
        return "gaze: no XR frame recorded yet\n";
    const std::string GAZELINE = G.selected ? std::format("looking at {} (id {}, dwell {:.2f}s)", G.gazeName.empty() ? "?" : G.gazeName, G.gazeMonitorId, G.dwell)
                                            : "looking at passthrough (no monitor)";
    std::string       out       = std::format("gaze: {}\n  timestampMs: {}  viewValid: {}\n  head pos [{:.3f}, {:.3f}, {:.3f}]  forward [{:.3f}, {:.3f}, {:.3f}]\n", GAZELINE,
                                              G.timestampMs, G.viewValid ? "yes" : "no", G.headPos.x, G.headPos.y, G.headPos.z, G.headForward.x, G.headForward.y, G.headForward.z);
    if (G.matched)
        out += std::format("  query: requested {}  matched {}  age {}ms\n", G.requestedTimestampMs, G.matchedTimestampMs, G.ageMs);
    return out;
}

static std::string openxrRequest(eHyprCtlOutputFormat format, std::string request) {
    if (!g_pOpenXRManager)
        return "OpenXR manager not initialized";

    CVarList          vars(request, 0, ' ');
    const std::string SUBCOMMAND = vars[1]; // vars[0] == "openxr"
    const std::string ARGS       = vars.join(" ", 2);

    if (SUBCOMMAND.empty() || SUBCOMMAND == "status")
        return openxrStatus(format);

    if (SUBCOMMAND == "enable") {
        g_pOpenXRManager->start();
        switch (g_pOpenXRManager->state()) {
            case XR_STATE_UNAVAILABLE: return "OpenXR runtime unavailable";
            case XR_STATE_DISABLED: return "failed to start OpenXR session";
            default: return "ok";
        }
    }

    if (SUBCOMMAND == "disable") {
        g_pOpenXRManager->stop();
        return "ok";
    }

    // Monitor lifecycle verbs (WP4). Same manager funnel as the xrmonitor dispatcher (doc 05 §4).
    if (SUBCOMMAND == "create") {
        auto r = g_pOpenXRManager->cmdCreate(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "destroy") {
        auto r = g_pOpenXRManager->cmdDestroy(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "select") {
        auto r = g_pOpenXRManager->cmdSelect(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "layout")
        return g_pOpenXRManager->layoutDump();

    // Pose-mutation verbs (WP5). Same manager funnel as the xrmonitor dispatcher (doc 05 §3/§4).
    if (SUBCOMMAND == "anchor") {
        auto r = g_pOpenXRManager->cmdAnchor(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "move") {
        auto r = g_pOpenXRManager->cmdMove(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "rotate") {
        auto r = g_pOpenXRManager->cmdRotate(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "scale") {
        auto r = g_pOpenXRManager->cmdScale(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "distance") {
        auto r = g_pOpenXRManager->cmdDistance(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "center") {
        auto r = g_pOpenXRManager->cmdCenter();
        return r ? "ok" : r.error();
    }

    // Adaptive anchoring verbs (research/13 §6.3).
    if (SUBCOMMAND == "adaptive") {
        auto r = g_pOpenXRManager->cmdAdaptive(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "dock") {
        auto r = g_pOpenXRManager->cmdDock(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "undock") {
        auto r = g_pOpenXRManager->cmdUndock();
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "roam") {
        auto r = g_pOpenXRManager->cmdRoam(ARGS);
        return r ? "ok" : r.error();
    }

    // Gaze grab + conditional hand input (research/16).
    if (SUBCOMMAND == "gazegrab") {
        auto r = g_pOpenXRManager->cmdGazeGrab();
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "gazerelease") {
        auto r = g_pOpenXRManager->cmdGazeRelease();
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "gazepush") {
        auto r = g_pOpenXRManager->cmdGazePush(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "handinput") {
        auto r = g_pOpenXRManager->cmdHandInput(ARGS);
        return r ? "ok" : r.error();
    }

    // hypxrvoice WP-V1: read-only head-ray + timestamped gaze history query.
    if (SUBCOMMAND == "gaze")
        return openxrGaze(format, ARGS);

    return std::format("unknown openxr subcommand '{}'. Valid: status, enable, disable, create, destroy, select, anchor, move, rotate, scale, distance, center, adaptive, dock, "
                       "undock, roam, gazegrab, gazerelease, gazepush, handinput, gaze, layout",
                       SUBCOMMAND);
}

CXRIpc::CXRIpc() = default;

CXRIpc::~CXRIpc() {
    if (m_openxrCommand && g_pHyprCtl)
        g_pHyprCtl->unregisterCommand(m_openxrCommand);
}

void CXRIpc::registerCommands() {
    if (!g_pHyprCtl)
        return;

    // exact = false: "openxr" carries subcommands in the request string (like "output").
    m_openxrCommand = g_pHyprCtl->registerCommand(SHyprCtlCommand{"openxr", false, openxrRequest});
}

#endif
