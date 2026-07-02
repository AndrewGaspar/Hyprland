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
    const auto        MONS    = g_pOpenXRManager->monitorInfos();

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
            "hovered": {}
        }})#",
                                m.name, m.id, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.quatX, m.quatY, m.quatZ, m.quatW, m.grabbed ? "true" : "false",
                                m.hovered ? "true" : "false");
            if (i + 1 < MONS.size())
                mons += ",\n";
            else
                mons += "\n";
        }

        return std::format(R"#({{
    "state": "{}",
    "runtimeName": "{}",
    "systemName": "{}",
    "monitors": [{}{}]
}}
)#",
                           STATE, RUNTIME, SYSTEM, MONS.empty() ? "" : "\n", mons);
    }

    std::string out = std::format("state: {}\nruntime: {}\nsystem: {}\n", STATE, RUNTIME, SYSTEM);
    for (const auto& m : MONS) {
        out += std::format("monitor {} (ID {}): {}x{}@{:.2f} size {:.2f}m anchor {} pos [{:.2f}, {:.2f}, {:.2f}] grabbed: {} hovered: {}\n", m.name, m.id, m.w, m.h, m.refresh,
                           m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.grabbed ? "yes" : "no", m.hovered ? "yes" : "no");
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

    // Pose-mutation verbs land in WP5 (anchoring engine + verbs).
    if (SUBCOMMAND == "anchor" || SUBCOMMAND == "move" || SUBCOMMAND == "rotate" || SUBCOMMAND == "scale" || SUBCOMMAND == "distance" || SUBCOMMAND == "center")
        return std::format("openxr subcommand '{}' is not implemented until WP5", SUBCOMMAND);

    return std::format("unknown openxr subcommand '{}'. Valid: status, enable, disable, create, destroy, select, layout", SUBCOMMAND);
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
