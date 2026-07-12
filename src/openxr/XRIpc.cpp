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
    // Read-only observability for the idle-inhibit predicate (doc 05 §6.1). There is otherwise
    // no queryable surface for "is the compositor's idle-inhibit bit currently raised because of
    // XR" — CIdleNotifyProtocol::isInhibited is private with no getter, and it's a fold of every
    // inhibitor source, not XR-specific anyway. This mirrors shouldInhibitIdle()'s own predicate
    // (openxr:inhibit_idle && FOCUSED) rather than adding a new getter to IdleNotify.hpp, keeping
    // the touched surface to this one file (WP12 test infra needs it to assert idle-inhibit
    // end-to-end without polling wall-clock idle timers).
    const bool         INHIBITING_IDLE = g_pOpenXRManager->shouldInhibitIdle();

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
            "plugged": {},
            "contentPath": "{}",
            "adaptive": {{
                "enabled": {},
                "phase": "{}",
                "roamMode": "{}",
                "seatDistM": {:.3f},
                "transitionT": {:.3f}
            }}
        }})#",
                                m.name, m.id, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.quatX, m.quatY, m.quatZ, m.quatW, m.grabbed ? "true" : "false",
                                m.grabKind, m.hovered ? "true" : "false", m.plugged ? "true" : "false", m.contentPath, m.adaptiveEnabled ? "true" : "false", m.adaptivePhase,
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
    "inhibitingIdle": {},
    "input": {{
        "left": {{ "kind": "{}", "gesture": "{}", "filtered": {} }},
        "right": {{ "kind": "{}", "gesture": "{}", "filtered": {} }}
    }},
    "monitors": [{}{}]
}}
)#",
                           STATE, RUNTIME, SYSTEM, RTGPU, BLEND, OVERLAY ? "true" : "false", INHIBITING_IDLE ? "true" : "false", HANDS[0].hands ? "hands" : "controllers", HANDS[0].gesture,
                           HANDS[0].filtered ? "true" : "false", HANDS[1].hands ? "hands" : "controllers", HANDS[1].gesture, HANDS[1].filtered ? "true" : "false",
                           MONS.empty() ? "" : "\n", mons);
    }

    std::string out = std::format("state: {}\nruntime: {}\nsystem: {}\nruntime gpu: {}\nblend mode: {}\noverlay: {}\nidle inhibited: {}\ninput: left {}, right {}\n", STATE, RUNTIME,
                                  SYSTEM, RTGPU.empty() ? "unknown" : RTGPU, BLEND, OVERLAY ? "yes" : "no", INHIBITING_IDLE ? "yes" : "no", handLabel(HANDS[0]), handLabel(HANDS[1]));
    for (const auto& m : MONS) {
        out += std::format("monitor {} (ID {}): {}x{}@{:.2f} size {:.2f}m anchor {} pos [{:.2f}, {:.2f}, {:.2f}] grabbed: {} ({}) hovered: {} plugged: {} content: {}", m.name,
                           m.id, m.w, m.h, m.refresh, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.grabbed ? "yes" : "no", m.grabKind, m.hovered ? "yes" : "no",
                           m.plugged ? "yes" : "no", m.contentPath);
        if (m.adaptiveEnabled)
            out += std::format(" adaptive: {} (roam {}, seat {:.2f}m)", m.adaptivePhase, m.adaptiveRoamMode, m.adaptiveSeatDist);
        out += "\n";
    }
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

    return std::format("unknown openxr subcommand '{}'. Valid: status, enable, disable, create, destroy, select, anchor, move, rotate, scale, distance, center, adaptive, dock, "
                       "undock, roam, layout",
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
