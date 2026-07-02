#include "XRIpc.hpp"
#ifdef HAVE_OPENXR

#include <format>
#include <string>

#include <hyprutils/string/VarList.hpp>

#include "OpenXRManager.hpp"
#include "../debug/HyprCtl.hpp"
#include "../SharedDefs.hpp"

using namespace Hyprutils::String;

// hyprctl openxr status  (default when no subcommand). The bar-pollable surface.
// monitors[] is empty until the monitor layer lands (WP3/WP4).
static std::string openxrStatus(eHyprCtlOutputFormat format) {
    const auto        STATE   = COpenXRManager::stateToString(g_pOpenXRManager->state());
    const std::string RUNTIME = g_pOpenXRManager->runtimeName();
    const std::string SYSTEM  = g_pOpenXRManager->systemName();

    if (format == FORMAT_JSON) {
        return std::format(R"#({{
    "state": "{}",
    "runtimeName": "{}",
    "systemName": "{}",
    "monitors": []
}}
)#",
                           STATE, RUNTIME, SYSTEM);
    }

    return std::format("state: {}\nruntime: {}\nsystem: {}\n", STATE, RUNTIME, SYSTEM);
}

static std::string openxrRequest(eHyprCtlOutputFormat format, std::string request) {
    if (!g_pOpenXRManager)
        return "OpenXR manager not initialized";

    CVarList          vars(request, 0, ' ');
    const std::string SUBCOMMAND = vars[1]; // vars[0] == "openxr"

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

    // Monitor create/destroy/select and the pose-mutation verbs (create, destroy, select,
    // anchor, move, rotate, scale, distance, center, layout) land in WP4/WP5.
    if (SUBCOMMAND == "create" || SUBCOMMAND == "destroy" || SUBCOMMAND == "select" || SUBCOMMAND == "anchor" || SUBCOMMAND == "move" ||
        SUBCOMMAND == "rotate" || SUBCOMMAND == "scale" || SUBCOMMAND == "distance" || SUBCOMMAND == "center" || SUBCOMMAND == "layout")
        return std::format("openxr subcommand '{}' is not implemented yet", SUBCOMMAND);

    return std::format("unknown openxr subcommand '{}'. Valid: status, enable, disable", SUBCOMMAND);
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
