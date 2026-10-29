#pragma once

// The `xrmonitor` dispatcher (doc 05 §3), front-end free.
//
// Compiled unconditionally (no OpenXR headers, no HAVE_OPENXR guard in this header): without
// OpenXR the action returns a clean "built without OpenXR" error instead of the verb vanishing,
// so a config that binds hl.dsp.xrmonitor(...) still loads on such a build.
//
// Every verb funnels into a COpenXRManager method — the exact same methods the `hyprctl openxr`
// subcommands call (one implementation, two transports). Up to 0.56 this switch lived in the
// legacy DispatcherTranslator and the Lua binding reached it through the keybind manager's
// dispatcher table; v0.57 deleted both, so it lives here and hl.dsp.xrmonitor calls it directly.
// Main thread only.

#include <string>

#include "../config/shared/actions/ConfigActions.hpp"

namespace OpenXR {
    // `args` is "<verb> [rest]" — the text a bind passes, e.g. "gazepush 0.1" or
    // "create XR-test 1280x720 anchor:local ...". The rest keeps its spaces.
    Config::Actions::ActionResult xrmonitorAction(const std::string& args);
}
