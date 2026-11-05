#include "XRDispatch.hpp"

#include <expected>

#ifdef HAVE_OPENXR
#include "OpenXRManager.hpp"
#endif

using namespace Config::Actions;

#ifdef HAVE_OPENXR
// Every verb failure is EXECUTION_FAILED — what hl.dsp.xrmonitor reported when it went through the
// legacy dispatcher table (any SDispatchResult error became C_EXECFAIL).
static ActionResult fail(std::string message) {
    return actionError(std::move(message), eActionErrorLevel::ERROR, eActionErrorCode::EXECUTION_FAILED);
}

static ActionResult fromExpected(std::expected<void, std::string>&& r) {
    if (r)
        return {};
    return fail(r.error());
}
#endif

ActionResult OpenXR::xrmonitorAction(const std::string& args) {
#ifndef HAVE_OPENXR
    return actionError("Hyprland was built without OpenXR support", eActionErrorLevel::ERROR, eActionErrorCode::UNAVAILABLE);
#else
    if (!g_pOpenXRManager)
        return actionError("OpenXR manager not initialized", eActionErrorLevel::ERROR, eActionErrorCode::UNAVAILABLE);

    // verb + unsplit rest (rest keeps its spaces, e.g. "XR-test 1280x720 anchor:local ...").
    const auto        sp   = args.find(' ');
    const std::string verb = sp == std::string::npos ? args : args.substr(0, sp);
    const std::string rest = sp == std::string::npos ? std::string{} : args.substr(sp + 1);

    if (verb.empty())
        return fail("xrmonitor: expected a verb (create|destroy|select|...)");

    if (verb == "create") {
        auto r = g_pOpenXRManager->cmdCreate(rest);
        if (!r)
            return fail(r.error());
        return {};
    }
    if (verb == "destroy")
        return fromExpected(g_pOpenXRManager->cmdDestroy(rest));
    if (verb == "select")
        return fromExpected(g_pOpenXRManager->cmdSelect(rest));

    // Pose-mutation verbs (WP5, doc 03 §5 / doc 05 §3.1).
    if (verb == "anchor")
        return fromExpected(g_pOpenXRManager->cmdAnchor(rest));
    if (verb == "move")
        return fromExpected(g_pOpenXRManager->cmdMove(rest));
    if (verb == "rotate")
        return fromExpected(g_pOpenXRManager->cmdRotate(rest));
    if (verb == "scale")
        return fromExpected(g_pOpenXRManager->cmdScale(rest));
    if (verb == "distance")
        return fromExpected(g_pOpenXRManager->cmdDistance(rest));
    if (verb == "center")
        return fromExpected(g_pOpenXRManager->cmdCenter());
    // docs/openxr/02 "Curved monitors": curve <radius m>|auto|flat on the selected monitor.
    if (verb == "curve")
        return fromExpected(g_pOpenXRManager->cmdCurve(rest));
    // doc 03 §8.4: "bring my monitors to me" — rigidly re-seat the whole anchor:local group around
    // the current head. cmdReseat returns a message rather than void (the hyprctl transport prints
    // it); a dispatcher has nowhere to put a success string, so it is logged there and dropped here.
    if (verb == "reseat") {
        auto r = g_pOpenXRManager->cmdReseat();
        if (!r)
            return fail(r.error());
        return {};
    }
    // hypxrvoice GAP 2: drop a named monitor at a resolved LOCAL_FLOOR point (facing the headset).
    if (verb == "place")
        return fromExpected(g_pOpenXRManager->cmdPlace(rest));

    // Adaptive anchoring verbs (research/13 §6.3).
    if (verb == "adaptive")
        return fromExpected(g_pOpenXRManager->cmdAdaptive(rest));
    if (verb == "dock")
        return fromExpected(g_pOpenXRManager->cmdDock(rest));
    if (verb == "undock")
        return fromExpected(g_pOpenXRManager->cmdUndock());
    if (verb == "roam")
        return fromExpected(g_pOpenXRManager->cmdRoam(rest));

    // Gaze grab + conditional hand input (research/16). gazegrab with no arg TOGGLES (grab/release);
    // with a <name> (hypxrvoice GAP 1) it begins a carry on the NAMED monitor. gazepush is designed
    // for repeating binds; handinput toggles hand input on/off at the keyboard.
    if (verb == "gazegrab")
        return fromExpected(g_pOpenXRManager->cmdGazeGrab(rest));
    if (verb == "gazerelease")
        return fromExpected(g_pOpenXRManager->cmdGazeRelease());
    if (verb == "gazepush")
        return fromExpected(g_pOpenXRManager->cmdGazePush(rest));
    if (verb == "handinput")
        return fromExpected(g_pOpenXRManager->cmdHandInput(rest));
    if (verb == "view")
        return fromExpected(g_pOpenXRManager->cmdView(rest));

    // 2D-plane sync (report 12 WP-S2): re-derive Hyprland's 2D monitor layout from where the quads
    // actually float, so mouse crossing and directional focus match what you see. Bindable, so
    // "re-sync my mouse mapping to how I'm sitting now" is one keystroke; `sync freeze|thaw` pauses
    // and resumes the automatic recompute while you rearrange quads.
    if (verb == "sync")
        return fromExpected(g_pOpenXRManager->cmdSyncLayout(rest));

    return fail("unknown xrmonitor verb '" + verb + "'");
#endif
}
