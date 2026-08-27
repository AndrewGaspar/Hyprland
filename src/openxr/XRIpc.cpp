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
    // WP-XR1: the openxr:runtime_json override forced into XR_RUNTIME_JSON for this session (empty =
    // loader default / active_runtime.json). Lets the XREAL flat/XR toggle confirm the active runtime.
    const std::string RTJSON  = g_pOpenXRManager->runtimeJson();
    // Kernel-taint tripwire (doc 01): why bring-up is being REFUSED, empty when nothing is. The
    // ordinary UNAVAILABLE reasons are transient and already covered by the state line's "waiting
    // for ..." hint; this one needs a reboot, so it gets its own line — and only when it applies,
    // so a healthy session's status is byte-identical to before.
    const std::string BLOCKED = g_pOpenXRManager->blockedReason();
    const std::string BLEND   = g_pOpenXRManager->blendModeName();
    const bool        OVERLAY = g_pOpenXRManager->isOverlay();
    const bool        MONITOR_VIEW = g_pOpenXRManager->monitorViewVisible();
    // report 09: luma-keyed transparency ("black-as-alpha"). Reports the CONFIGURED value, the
    // EFFECTIVE one (forced off unless the blend mode composites over passthrough/additive) and the
    // knee, so "why is my desktop still opaque" is answerable in one command.
    const auto        BLACK   = g_pOpenXRManager->blackAlphaStatus();
    const auto        MONS    = g_pOpenXRManager->monitorInfos();
    // report-18 addendum: the plugged-state follow mode and, when the headset has just been
    // doffed under `visible` mode, the ms remaining before the anti-flap grace-unplug fires (-1
    // when no unplug is pending).
    const std::string FOLLOW      = g_pOpenXRManager->monitorFollowModeName();
    const int         UNPLUG_PEND = g_pOpenXRManager->monitorUnplugPendingMs();
    // doc 03 §8.4: what the HEADSET's own recenter button currently does to the monitor group —
    // "hold" (stay put in the room) or "follow" (come to me). The single most confusing thing about
    // recentering is that pressing it can correctly do nothing, so say which contract is in force.
    const std::string RECENTER = g_pOpenXRManager->recenterPolicyName();
    // report-19: the user-presence signal driving the `visible`-mode plug gate — "yes"/"no" when the
    // runtime exposes XR_EXT_user_presence, "unknown" before the first event, "unsupported" otherwise.
    const std::string PRESENCE = g_pOpenXRManager->presenceStatusString();
    // report-20 issue D: surface the RAW visibility signal next to presence so the combined plug gate
    // (needs BOTH) is diagnosable in one command. "yes"/"no"/"n/a".
    const std::string VISIBLE = g_pOpenXRManager->visibleStatusString();
    // report-20 issue B1: dormant re-probe hint — what we're waiting for + ms until the next probe.
    const std::string REPROBE_WAIT = g_pOpenXRManager->reprobeWaitString();
    const int         REPROBE_MS   = g_pOpenXRManager->reprobePendingMs();
    // Event-driven re-probe (don-the-headset dead-air fix): is the inotify watch on the runtime socket
    // dir currently armed? When true, the socket appearing fires an immediate probe (the backoff timer
    // is just the fallback), so the up-to-30s donning stall is gone.
    const bool         REPROBE_WATCH = g_pOpenXRManager->reprobeWatchArmed();
    // Sticky `hyprctl openxr disable` (reload-storm containment, 2026-08-21). Reported because a
    // "disabled" state that no config edit will lift is otherwise indistinguishable from
    // `openxr:enabled = 0`, and the only way out of it is the `enable` verb.
    const bool MANUAL_DISABLE = g_pOpenXRManager->manuallyDisabled();
    // Read-only observability for the idle-inhibit predicate (doc 05 §6.1). There is otherwise
    // no queryable surface for "is the compositor's idle-inhibit bit currently raised because of
    // XR" — CIdleNotifyProtocol::isInhibited is private with no getter, and it's a fold of every
    // inhibitor source, not XR-specific anyway. This mirrors shouldInhibitIdle()'s own predicate
    // rather than adding a new getter to IdleNotify.hpp, keeping the touched surface to this one file
    // (WP12 test infra needs it to assert idle-inhibit end-to-end without polling wall-clock idle
    // timers). research/20 phase 2: report the resolved MODE alongside, so "why is it not inhibiting"
    // is answerable in one command (mode + visible + presence are now all on the same status page).
    const bool         INHIBITING_IDLE = g_pOpenXRManager->shouldInhibitIdle();
    const std::string  INHIBIT_MODE    = g_pOpenXRManager->idleInhibitModeName();

    // research/16 Part A/B: conditional hand-input gate + gaze grab state.
    const auto        HANDIN = g_pOpenXRManager->handInputStatus();
    const auto        GAZE   = g_pOpenXRManager->gazeStatus();
    // hypxrvoice GAP 3: the concrete monitor `active` resolves to (explicit select > last hovered >
    // focused-if-XR), so a voice daemon can name the target in feedback without replicating the order.
    const std::string SELECTED = g_pOpenXRManager->selectedName();
    // WP-G5: per-hand active input device (hands vs controllers) + the hand grab gesture.
    const auto        HANDS = g_pOpenXRManager->handInputInfos();
    // report 12: 2D-plane sync — is the layout plane being derived from the 3D arrangement, and from
    // which latched reference frame? `hyprctl openxr layout` is deliberately NOT affected by any of
    // this: it still dumps paste-ready `xrmonitor =` lines describing the 3D arrangement.
    const auto        L2D   = g_pOpenXRManager->layout2DStatus();
    // doc 03 §8.3: is the compositor currently remembering where the user has put their monitors.
    const bool        RESTORE_CAPTURE = g_pOpenXRManager->restoreCaptureActive();
    // research/28 M0/W4: the runtime's ROOM frame — STAGE, located in our own reference space — and
    // how often it has moved. Read-only instrumentation for the attended probe that gates Track W;
    // nothing is anchored in STAGE yet. "unavailable" on a runtime that does not offer one.
    const auto        WORLD = g_pOpenXRManager->worldFrameStatus();
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
            "stereo": "{}",
            "stereoProducer": "{}",
            "chrome": {},
            "quads": {},
            "adaptive": {{
                "enabled": {},
                "phase": "{}",
                "roamMode": "{}",
                "seatDistM": {:.3f},
                "transitionT": {:.3f}
            }},
            "anchorState": "{}",
            "restore": {{
                "restorable": {},
                "offset": [{:.3f}, {:.3f}, {:.3f}],
                "staged": {},
                "stagedOffset": [{:.3f}, {:.3f}, {:.3f}],
                "lastCommit": "{}"
            }},
            "layout2d": {{
                "source": "{}",
                "col": {},
                "row": {},
                "x": {:.0f},
                "y": {:.0f},
                "azDeg": {:.2f},
                "elDeg": {:.2f}
            }},
            "transparency": {{
                "alpha": {:.3f},
                "alphaTarget": {:.3f},
                "alphaSource": "{}",
                "blackAlpha": {:.3f},
                "blackAlphaTarget": {:.3f},
                "blackAlphaSource": "{}",
                "knee": {:.3f},
                "kneeSource": "{}",
                "transitioning": {}
            }}
        }})#",
                                m.name, m.id, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.quatX, m.quatY, m.quatZ, m.quatW, m.grabbed ? "true" : "false",
                                m.grabKind, m.hovered ? "true" : "false", m.region, m.plugged ? "true" : "false", m.contentPath, m.linear ? "true" : "false", m.stereo, m.producer, m.chrome ? "true" : "false", m.quads, m.adaptiveEnabled ? "true" : "false", m.adaptivePhase,
                                m.adaptiveRoamMode, m.adaptiveSeatDist, m.adaptiveT, m.anchorState, m.restorable ? "true" : "false", m.restoreX, m.restoreY, m.restoreZ,
                                m.staged ? "true" : "false", m.stagedX, m.stagedY, m.stagedZ, m.commit, m.l2dSource, m.l2dCol, m.l2dRow, m.l2dX, m.l2dY, m.l2dAzDeg, m.l2dElDeg, m.fxAlpha, m.fxAlphaTarget, m.fxAlphaSrc, m.fxBlackAlpha, m.fxBlackAlphaTarget,
                                m.fxBlackAlphaSrc, m.fxKnee, m.fxKneeSrc, m.fxTransitioning ? "true" : "false");
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
    "runtimeJson": "{}",
    "blocked": "{}",
    "blendMode": "{}",
    "blackAlpha": {{ "configured": {:.3f}, "effective": {:.3f}, "knee": {:.3f}, "active": {}, "gatedOff": {} }},
    "overlay": {},
    "monitorView": "{}",
    "selected": "{}",
    "monitorsFollowSession": "{}",
    "monitorUnplugPendingMs": {},
    "recenterPolicy": "{}",
    "userPresence": "{}",
    "restoreCapture": {},
    "worldFrame": {{ "available": {}, "located": {}, "tracked": {}, "pose": [{:.3f}, {:.3f}, {:.3f}], "yawDeg": {:.1f}, "probes": {}, "changes": {}, "unresolvable": {} }},
    "visible": "{}",
    "reprobeWaiting": "{}",
    "reprobePendingMs": {},
    "reprobeWatching": {},
    "manualDisable": {},
    "inhibitingIdle": {},
    "idleInhibitMode": "{}",
    "handInput": {{ "mode": "{}", "state": "{}" }},
    "gaze": {{ "source": "{}", "hoveredMonitor": {}, "hoveredName": "{}", "carrying": {}, "carryMonitor": "{}", "distM": {:.3f} }},
    "input": {{
        "left": {{ "kind": "{}", "gesture": "{}", "filtered": {} }},
        "right": {{ "kind": "{}", "gesture": "{}", "filtered": {} }}
    }},
    "layout2d": {{
        "enabled": {}, "frozen": {}, "referenceLatched": {}, "referenceYawDeg": {:.1f},
        "placed": {}, "rows": {}, "blockWidth": {}, "blockHeight": {},
        "vertical": "{}", "attach": "{}", "pxPerDegree": {:.1f}
    }},
    "monitors": [{}{}]
}}
)#",
                           STATE, RUNTIME, SYSTEM, RTGPU, RTJSON, BLOCKED, BLEND, BLACK.configured, BLACK.effective, BLACK.knee, BLACK.active ? "true" : "false",
                           BLACK.gatedOff ? "true" : "false", OVERLAY ? "true" : "false", MONITOR_VIEW ? "shown" : "hidden", SELECTED, FOLLOW, UNPLUG_PEND, RECENTER, PRESENCE,
                           RESTORE_CAPTURE ? "true" : "false", WORLD.available ? "true" : "false", WORLD.located ? "true" : "false", WORLD.tracked ? "true" : "false", WORLD.x,
                           WORLD.y, WORLD.z, WORLD.yawDeg, WORLD.probes, WORLD.changes, WORLD.unresolvable, VISIBLE, REPROBE_WAIT, REPROBE_MS,
                           REPROBE_WATCH ? "true" : "false", MANUAL_DISABLE ? "true" : "false",
                           INHIBITING_IDLE ? "true" : "false", INHIBIT_MODE, HANDIN.mode, HANDIN.state, GAZE.source, GAZE.hoveredMonitor, GAZE.hoveredName,
                           GAZE.carrying ? "true" : "false", GAZE.carryMonitor, GAZE.dist, HANDS[0].hands ? "hands" : "controllers", HANDS[0].gesture,
                           HANDS[0].filtered ? "true" : "false", HANDS[1].hands ? "hands" : "controllers", HANDS[1].gesture, HANDS[1].filtered ? "true" : "false",
                           L2D.enabled ? "true" : "false", L2D.frozen ? "true" : "false", L2D.refValid ? "true" : "false", L2D.refYawDeg, L2D.placed, L2D.rows, L2D.width,
                           L2D.height, L2D.vertical, L2D.attach, L2D.pxPerDegree, MONS.empty() ? "" : "\n", mons);
    }

    const std::string FOLLOWLINE = UNPLUG_PEND >= 0 ? std::format("{} (unplug in {}ms)", FOLLOW, UNPLUG_PEND) : FOLLOW;
    // report-20 issue B1: append the dormant re-probe hint to the state line, e.g.
    // "unavailable (waiting for headset, retrying in 1800ms)".
    const std::string STATELINE = MANUAL_DISABLE ?
        std::format("{} (manually disabled — `hyprctl openxr enable` to resume)", STATE) :
        REPROBE_WAIT.empty() ?
        STATE :
        std::format("{} (waiting for {}, retrying in {}ms{})", STATE, REPROBE_WAIT, REPROBE_MS < 0 ? 0 : REPROBE_MS, REPROBE_WATCH ? ", watching socket" : "");
    const std::string GAZELINE = GAZE.carrying ? std::format("carrying {} at {:.2f}m", GAZE.carryMonitor, GAZE.dist)
                                                : (GAZE.hoveredMonitor >= 0 ? std::format("looking at {}", GAZE.hoveredName) : "idle");
    // "black alpha: 0.20 (knee 0.10)" when the luma key is live; "off" when unconfigured; and an
    // explicit reason when a configured value is being ignored because the blend mode is opaque.
    const std::string BLACKLINE = BLACK.active ? std::format("{:.2f} (knee {:.2f})", BLACK.effective, BLACK.knee)
        : BLACK.gatedOff                       ? std::format("off — {:.2f} ignored under blend mode {}", BLACK.configured, BLEND)
                                               : "off";
    // report 12: one line for the whole 2D-plane-sync engine. "off" is the historic append-right
    // behavior; "frozen" means auto-recompute is paused (`openxr sync-layout thaw` resumes);
    // "waiting for tracking" means it is on but has never had a head pose to latch a reference from.
    const std::string L2DLINE = !L2D.enabled ? "off"
        : !L2D.refValid        ? "on (waiting for tracking)"
                               : std::format("{}{} monitor(s) in {} row(s), block {}x{}, ref yaw {:.0f} deg, {} px/deg, vertical {}, attach {}", L2D.frozen ? "frozen — " : "",
                                             L2D.placed, L2D.rows, L2D.width, L2D.height, L2D.refYawDeg, L2D.pxPerDegree, L2D.vertical, L2D.attach);
    // Emitted directly UNDER the state line (which stays first, as bar scripts expect) and only
    // when something is actually blocking — a healthy session's status is byte-identical to before.
    // It sits above the runtime details because when it is set those are all empty and meaningless.
    const std::string BLOCKEDLINE = BLOCKED.empty() ? "" : std::format("blocked: {}\n", BLOCKED);
    // research/28 M0: the room frame, in the seat frame's coordinates. At session start the two
    // coincide, so this line reads ~[0,0,0] yaw 0 and every later reading is the drift between "the
    // room" and "the origin the runtime re-establishes on every recenter". `changes` counts the
    // headset's own STAGE redefinitions — the ones our wivrn-xg fork forwards and the compositor used
    // to discard silently (W4); `unresolvable` counts those that carried no delta, i.e. the ones that
    // mean world content is now suspect.
    const std::string WORLDLINE = !WORLD.available ?
        std::string("unavailable (this runtime offers no STAGE space)") :
        std::format("[{:.2f}, {:.2f}, {:.2f}] yaw {:.1f} deg ({}{}), {} probe(s), {} change(s), {} unresolvable", WORLD.x, WORLD.y, WORLD.z, WORLD.yawDeg,
                    WORLD.located ? "located" : "NOT locatable", WORLD.located && !WORLD.tracked ? ", inferred" : "", WORLD.probes, WORLD.changes, WORLD.unresolvable);
    std::string       out =
        std::format("state: {}\n{}runtime: {}\nsystem: {}\nruntime gpu: {}\nruntime json: {}\nblend mode: {}\nblack alpha: {}\noverlay: {}\nmonitor view: {}\nselected: {}\nmonitors "
                    "follow session: {}\nrecenter policy: {}\nvisible: "
                    "{}\npresence: {}\nidle "
                    "inhibited: {} (mode {})\nplacement capture: {}\nworld frame (STAGE): {}\nhand input: {} "
                    "({})\ngaze ({}): {}\ninput: left {}, right {}\n2d-plane sync: {}\n",
                    STATELINE, BLOCKEDLINE, RUNTIME, SYSTEM, RTGPU.empty() ? "unknown" : RTGPU, RTJSON.empty() ? "(loader default)" : RTJSON, BLEND, BLACKLINE, OVERLAY ? "yes" : "no",
                    MONITOR_VIEW ? "shown" : "hidden", SELECTED.empty() ? "(none)" : SELECTED, FOLLOWLINE, RECENTER, VISIBLE, PRESENCE, INHIBITING_IDLE ? "yes" : "no", INHIBIT_MODE, RESTORE_CAPTURE ? "on" : "off",
                    WORLDLINE, HANDIN.state, HANDIN.mode, GAZE.source, GAZELINE, handLabel(HANDS[0]), handLabel(HANDS[1]), L2DLINE);
    for (const auto& m : MONS) {
        out += std::format("monitor {} (ID {}): {}x{}@{:.2f} size {:.2f}m anchor {} pos [{:.2f}, {:.2f}, {:.2f}] grabbed: {} ({}) hovered: {} ({}) plugged: {} content: {}{}", m.name,
                           m.id, m.w, m.h, m.refresh, m.sizeMeters, m.anchorMode, m.posX, m.posY, m.posZ, m.grabbed ? "yes" : "no", m.grabKind, m.hovered ? "yes" : "no", m.region,
                           m.plugged ? "yes" : "no", m.contentPath, m.linear ? " (linear)" : "");
        if (m.adaptiveEnabled)
            out += std::format(" adaptive: {} (roam {}, seat {:.2f}m)", m.adaptivePhase, m.adaptiveRoamMode, m.adaptiveSeatDist);
        // WP X1: only mentioned when a monitor is actually in stereo, so a mono session's status is
        // byte-identical to before. `quads` alongside it is the tell for a pair the budget refused.
        if (m.stereo != "off")
            out += std::format(" stereo: {}/{} ({} quad{}{})", m.stereo, m.producer, m.quads, m.quads == 1 ? "" : "s", m.chrome ? "" : ", chrome off");
        out += "\n";
        // Situational transparency (doc 05 §xrrule): the effective values + WHERE each came from, so
        // "why is this monitor ghosted" is answerable in one command. `-> x` shows a live transition.
        const std::string ALINE = m.fxTransitioning && m.fxAlpha != m.fxAlphaTarget ? std::format("{:.2f} -> {:.2f}", m.fxAlpha, m.fxAlphaTarget) : std::format("{:.2f}", m.fxAlpha);
        const std::string BLINE = m.fxBlackAlpha >= 1.f && m.fxBlackAlphaTarget >= 1.f
            ? std::string("off")
            : (m.fxTransitioning && m.fxBlackAlpha != m.fxBlackAlphaTarget ? std::format("{:.2f} -> {:.2f}", m.fxBlackAlpha, m.fxBlackAlphaTarget)
                                                                          : std::format("{:.2f}", m.fxBlackAlpha));
        out += std::format("  {}: alpha {} ({}), blackalpha {} ({}, knee {:.2f}), anchorstate {}\n", m.name, ALINE, m.fxAlphaSrc, BLINE, m.fxBlackAlphaSrc, m.fxKnee, m.anchorState);
        // doc 03 §8.3: will this monitor come back where the user left it after a session restart, and
        // from what offset. Only meaningful for anchor:local — the leashed modes ride the user anyway.
        // research/28 H2: the MEMORY first (that is what a re-seat plants), then the live staging
        // value and the last commit verdict — "staged but refused" and "nothing captured at all" are
        // different diagnoses and used to look identical from here.
        if (m.anchorMode == "local") {
            out += m.restorable ? std::format("  {}: restore [{:.2f}, {:.2f}, {:.2f}] (head-relative)", m.name, m.restoreX, m.restoreY, m.restoreZ)
                                : std::format("  {}: restore none — a new session re-seats it from its declared rig", m.name);
            out += m.staged ? std::format(", staged [{:.2f}, {:.2f}, {:.2f}], last commit {}\n", m.stagedX, m.stagedY, m.stagedZ, m.commit)
                            : std::format(", nothing staged, last commit {}\n", m.commit);
        }
        // report 12: where the 2D plane put it, and from what angles — "auto" = the projection owns
        // it, "pinned" = an explicit user monitor= offset does, "off" = append-right as before.
        if (L2D.enabled && m.l2dSource != "off")
            out += std::format("  {}: 2d [{:.0f}, {:.0f}] col {} row {} ({}) from az {:.1f} deg, el {:.1f} deg\n", m.name, m.l2dX, m.l2dY, m.l2dCol, m.l2dRow, m.l2dSource,
                               m.l2dAzDeg, m.l2dElDeg);
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
        // hypxrvoice GAP 4: OPTIONAL hit fields, emitted only when the gaze ray actually met the
        // SELECTED monitor's quad. `hitPoint` is LOCAL_FLOOR meters — the exact space
        // `openxr place <name> at x,y,z` consumes, so a voice daemon feeds it straight back with no
        // conversion. Absent (the daemon falls back to its head-forward projection) when nothing is
        // selected, or on a mid-dwell sample that still names a monitor the ray has already left.
        std::string hit;
        if (G.hitValid)
            hit = std::format(R"#(,
        "hitPoint": [{:.4f}, {:.4f}, {:.4f}],
        "hitDistM": {:.4f})#",
                              G.hitPoint.x, G.hitPoint.y, G.hitPoint.z, G.hitDist);
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
        "dwellSec": {:.3f}{}
    }}{}
}}
)#",
                           G.timestampMs, G.viewValid ? "true" : "false", G.headPos.x, G.headPos.y, G.headPos.z, G.headRot.x, G.headRot.y, G.headRot.z, G.headRot.w, G.headForward.x,
                           G.headForward.y, G.headForward.z, G.gazeMonitorId, G.gazeName, G.selected ? "true" : "false", G.dwell, hit, query);
    }

    if (!G.ok)
        return "gaze: no XR frame recorded yet\n";
    const std::string GAZELINE = G.selected ? std::format("looking at {} (id {}, dwell {:.2f}s)", G.gazeName.empty() ? "?" : G.gazeName, G.gazeMonitorId, G.dwell)
                                            : "looking at passthrough (no monitor)";
    std::string       out       = std::format("gaze: {}\n  timestampMs: {}  viewValid: {}\n  head pos [{:.3f}, {:.3f}, {:.3f}]  forward [{:.3f}, {:.3f}, {:.3f}]\n", GAZELINE,
                                              G.timestampMs, G.viewValid ? "yes" : "no", G.headPos.x, G.headPos.y, G.headPos.z, G.headForward.x, G.headForward.y, G.headForward.z);
    if (G.hitValid)
        out += std::format("  hit [{:.3f}, {:.3f}, {:.3f}] at {:.3f}m\n", G.hitPoint.x, G.hitPoint.y, G.hitPoint.z, G.hitDist);
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
        // userEnable(), not start(): it clears the sticky manual-disable latch and the probe floor
        // so an explicit enable always gets a fresh attempt (reload-storm containment, 2026-08-21).
        g_pOpenXRManager->userEnable();
        switch (g_pOpenXRManager->state()) {
            case XR_STATE_UNAVAILABLE: return "OpenXR runtime unavailable";
            case XR_STATE_DISABLED: return "failed to start OpenXR session";
            default: return "ok";
        }
    }

    if (SUBCOMMAND == "disable") {
        // userDisable(), not stop(): the disable must SURVIVE config reloads. A bare stop() left
        // openxr:enabled = 1 in the config, so the next reload saw DISABLED + enabled and started
        // straight back up — under the 1 Hz reload storm of 2026-08-21 the disable lasted under a
        // second and there was no way to turn XR off at all.
        g_pOpenXRManager->userDisable();
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
    // doc 03 §8.4: the deliberate "bring my monitors to me". Reports what it did (or why it did
    // nothing) rather than "ok" — a re-seat that no-ops is invisible from inside the headset.
    if (SUBCOMMAND == "reseat") {
        auto r = g_pOpenXRManager->cmdReseat();
        return r ? *r : r.error();
    }
    // hypxrvoice GAP 2: drop a named monitor at a resolved LOCAL_FLOOR point (facing the headset).
    if (SUBCOMMAND == "place") {
        auto r = g_pOpenXRManager->cmdPlace(ARGS);
        return r ? "ok" : r.error();
    }

    // Situational transparency manual overrides (doc 05 §xrrule). `auto` hands the monitor back to
    // the rules; anything else is sticky and outranks every rule until it is cleared.
    if (SUBCOMMAND == "alpha") {
        auto r = g_pOpenXRManager->cmdAlpha(ARGS);
        return r ? "ok" : r.error();
    }
    if (SUBCOMMAND == "blackalpha") {
        auto r = g_pOpenXRManager->cmdBlackAlpha(ARGS);
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

    // Gaze grab + conditional hand input (research/16). gazegrab with no arg toggles on the dwell
    // candidate; with a <name> (hypxrvoice GAP 1) it begins a carry on the NAMED monitor.
    if (SUBCOMMAND == "gazegrab") {
        auto r = g_pOpenXRManager->cmdGazeGrab(ARGS);
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
    if (SUBCOMMAND == "view") {
        auto r = g_pOpenXRManager->cmdView(ARGS);
        return r ? "ok" : r.error();
    }

    // 2D-plane sync (report 12 WP-S2). Bare = re-latch the desk orientation and re-derive the plane
    // now; freeze/thaw pauses and resumes the automatic recompute while you rearrange quads.
    if (SUBCOMMAND == "sync-layout") {
        auto r = g_pOpenXRManager->cmdSyncLayout(ARGS);
        return r ? "ok" : r.error();
    }

    // hypxrvoice WP-V1: read-only head-ray + timestamped gaze history query.
    if (SUBCOMMAND == "gaze")
        return openxrGaze(format, ARGS);

    return std::format("unknown openxr subcommand '{}'. Valid: status, enable, disable, create, destroy, select, anchor, move, rotate, scale, distance, center, reseat, place, "
                       "alpha, blackalpha, adaptive, dock, undock, roam, gazegrab, gazerelease, gazepush, handinput, view, gaze, layout, sync-layout",
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
