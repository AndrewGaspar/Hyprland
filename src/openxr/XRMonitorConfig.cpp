#include "XRMonitorConfig.hpp"

// Compiled unconditionally (no OpenXR headers, no HAVE_OPENXR guard) — see the header. Only
// std + hyprutils string helpers here.

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <vector>

#include <hyprutils/string/VarList2.hpp>

using namespace Hyprutils::String;

namespace {
    std::string trim(std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return std::string(s.substr(b, e - b));
    }

    // strip surrounding commas + whitespace from a whitespace-split token
    std::string stripToken(std::string_view s) {
        std::string t = trim(s);
        while (!t.empty() && (t.front() == ','))
            t.erase(t.begin());
        while (!t.empty() && (t.back() == ','))
            t.pop_back();
        return trim(t);
    }

    std::expected<int, std::string> parseInt(const std::string& s, const std::string& what) {
        const std::string t = trim(s);
        if (t.empty())
            return std::unexpected(what + " is empty");
        int v        = 0;
        auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
        if (ec != std::errc{} || p != t.data() + t.size())
            return std::unexpected("invalid integer for " + what + ": '" + t + "'");
        return v;
    }

    std::expected<float, std::string> parseFloat(const std::string& s, const std::string& what) {
        const std::string t = trim(s);
        if (t.empty())
            return std::unexpected(what + " is empty");
        try {
            size_t idx = 0;
            float  v   = std::stof(t, &idx);
            if (idx != t.size())
                return std::unexpected("invalid number for " + what + ": '" + t + "'");
            return v;
        } catch (...) { return std::unexpected("invalid number for " + what + ": '" + t + "'"); }
    }

    // "WxH" or "WxH@Hz"
    std::expected<void, std::string> parseMode(const std::string& modeRaw, SXRMonitorParams& out) {
        const std::string mode = trim(modeRaw);
        if (mode.empty())
            return std::unexpected<std::string>("missing mode (expected WxH[@Hz])");

        std::string wh = mode;
        std::string hz;
        if (const auto at = mode.find('@'); at != std::string::npos) {
            wh = mode.substr(0, at);
            hz = mode.substr(at + 1);
        }

        const auto x = wh.find('x');
        if (x == std::string::npos)
            return std::unexpected<std::string>("invalid mode '" + mode + "' (expected WxH[@Hz])");

        auto w = parseInt(wh.substr(0, x), "mode width");
        if (!w)
            return std::unexpected(w.error());
        auto h = parseInt(wh.substr(x + 1), "mode height");
        if (!h)
            return std::unexpected(h.error());
        if (*w <= 0 || *h <= 0)
            return std::unexpected<std::string>("mode dimensions must be positive");

        out.m_resolution = Vector2D{(double)*w, (double)*h};

        if (!hz.empty()) {
            auto r = parseFloat(hz, "refresh rate");
            if (!r)
                return std::unexpected(r.error());
            if (*r <= 0.f)
                return std::unexpected<std::string>("refresh rate must be positive");
            out.m_refreshRate = *r;
        }
        return {};
    }

    // Parse "x,y,z" into three floats.
    std::expected<void, std::string> parseVec3(const std::string& v, const std::string& what, float& x, float& y, float& z) {
        CVarList2 parts(std::string(v), 0, ',', true);
        if (parts.size() != 3)
            return std::unexpected<std::string>(what + " expects 3 comma-separated numbers, got '" + v + "'");
        auto px = parseFloat(std::string(parts[0]), what + ".x");
        if (!px)
            return std::unexpected(px.error());
        auto py = parseFloat(std::string(parts[1]), what + ".y");
        if (!py)
            return std::unexpected(py.error());
        auto pz = parseFloat(std::string(parts[2]), what + ".z");
        if (!pz)
            return std::unexpected(pz.error());
        x = *px;
        y = *py;
        z = *pz;
        return {};
    }

    // tokens: whitespace-split, comma-stripped tokens of the anchor-spec + trailing kv pairs.
    // tokens[0] must be the anchor mode token ("anchor:local" / "anchor:head" / "anchor:body" /
    // "anchor:device:left|right"). Fills out.m_anchor and out.m_sizeMeters.
    std::expected<void, std::string> parseAnchorAndKV(const std::vector<std::string>& tokens, SXRMonitorParams& out) {
        if (tokens.empty())
            return std::unexpected<std::string>("missing anchor spec (expected anchor:local|head|body|device:left|right)");

        OpenXR::SXRAnchorState anchor;
        bool                   gotPos = false;
        float                  posX = 0.f, posY = 0.f, posZ = 0.f;
        float                  yawDeg = 0.f, pitchDeg = 0.f;
        // Adaptive-anchoring tokens (research/13 §6.2). They decorate anchor:local only.
        bool                   sawAdaptive = false;
        float                  roamYawDeg  = 0.f;

        const std::string&     modeTok = tokens[0];
        if (modeTok == "anchor:local")
            anchor.mode = OpenXR::XR_ANCHOR_LOCAL;
        else if (modeTok == "anchor:head")
            anchor.mode = OpenXR::XR_ANCHOR_HEAD;
        else if (modeTok == "anchor:body")
            anchor.mode = OpenXR::XR_ANCHOR_BODY;
        else if (modeTok == "anchor:device:left") {
            anchor.mode   = OpenXR::XR_ANCHOR_DEVICE;
            anchor.device = OpenXR::XR_HAND_LEFT;
        } else if (modeTok == "anchor:device:right") {
            anchor.mode   = OpenXR::XR_ANCHOR_DEVICE;
            anchor.device = OpenXR::XR_HAND_RIGHT;
        } else if (modeTok.starts_with("anchor:device:"))
            return std::unexpected<std::string>("device side must be 'left' or 'right', got '" + modeTok.substr(std::string("anchor:device:").size()) + "'");
        else
            return std::unexpected<std::string>("unknown anchor mode '" + modeTok + "'");

        for (size_t i = 1; i < tokens.size(); ++i) {
            const std::string& tok   = tokens[i];
            const auto         colon = tok.find(':');
            if (colon == std::string::npos)
                return std::unexpected<std::string>("expected key:value, got '" + tok + "'");
            const std::string key = tok.substr(0, colon);
            const std::string val = tok.substr(colon + 1);

            if (key == "pos") {
                if (anchor.mode != OpenXR::XR_ANCHOR_LOCAL)
                    return std::unexpected<std::string>("'pos:' is only valid for anchor:local (use 'offset:' for head/body/device)");
                if (auto r = parseVec3(val, "pos", posX, posY, posZ); !r)
                    return std::unexpected(r.error());
                gotPos = true;
            } else if (key == "offset") {
                if (anchor.mode == OpenXR::XR_ANCHOR_LOCAL)
                    return std::unexpected<std::string>("'offset:' is only valid for head/body/device (use 'pos:' for anchor:local)");
                if (auto r = parseVec3(val, "offset", posX, posY, posZ); !r)
                    return std::unexpected(r.error());
                gotPos = true;
            } else if (key == "yaw") {
                auto r = parseFloat(val, "yaw");
                if (!r)
                    return std::unexpected(r.error());
                yawDeg = *r;
            } else if (key == "pitch") {
                auto r = parseFloat(val, "pitch");
                if (!r)
                    return std::unexpected(r.error());
                pitchDeg = *r;
            } else if (key == "size") {
                auto r = parseFloat(val, "size");
                if (!r)
                    return std::unexpected(r.error());
                if (*r <= 0.f)
                    return std::unexpected<std::string>("size must be positive");
                out.m_sizeMeters = *r;
            } else if (key == "adaptive") {
                sawAdaptive = true;
                if (val == "on" || val == "true" || val == "1")
                    anchor.adaptive.enabled = true;
                else if (val == "off" || val == "false" || val == "0")
                    anchor.adaptive.enabled = false;
                else
                    return std::unexpected<std::string>("adaptive must be on/off, got '" + val + "'");
            } else if (key == "roam") {
                sawAdaptive                  = true;
                anchor.adaptive.roamModeSet  = true;
                if (val == "head")
                    anchor.adaptive.roamMode = OpenXR::XR_ANCHOR_HEAD;
                else if (val == "body")
                    anchor.adaptive.roamMode = OpenXR::XR_ANCHOR_BODY;
                else
                    return std::unexpected<std::string>("roam must be 'head' or 'body', got '" + val + "'");
            } else if (key == "roam_offset") {
                sawAdaptive = true;
                float rx = 0.f, ry = 0.f, rz = 0.f;
                if (auto r = parseVec3(val, "roam_offset", rx, ry, rz); !r)
                    return std::unexpected(r.error());
                anchor.adaptive.roamOffset.pos = OpenXR::Vec3{rx, ry, rz};
                anchor.adaptive.hasRoamOffset  = true;
            } else if (key == "roam_yaw") {
                sawAdaptive = true;
                auto r      = parseFloat(val, "roam_yaw");
                if (!r)
                    return std::unexpected(r.error());
                roamYawDeg = *r;
            } else if (key == "leave") {
                sawAdaptive = true;
                auto r      = parseFloat(val, "leave");
                if (!r)
                    return std::unexpected(r.error());
                if (*r <= 0.f)
                    return std::unexpected<std::string>("leave radius must be positive");
                anchor.adaptive.leaveRadius = *r;
            } else if (key == "return") {
                sawAdaptive = true;
                auto r      = parseFloat(val, "return");
                if (!r)
                    return std::unexpected(r.error());
                if (*r <= 0.f)
                    return std::unexpected<std::string>("return radius must be positive");
                anchor.adaptive.returnRadius = *r;
            } else if (key == "carry") {
                sawAdaptive                   = true;
                anchor.adaptive.carryOverride = true;
                if (val == "on" || val == "true" || val == "1")
                    anchor.adaptive.carryOffset = true;
                else if (val == "off" || val == "false" || val == "0")
                    anchor.adaptive.carryOffset = false;
                else
                    return std::unexpected<std::string>("carry must be on/off, got '" + val + "'");
            } else
                return std::unexpected<std::string>("unknown key '" + key + "' in anchor spec");
        }

        if (anchor.mode == OpenXR::XR_ANCHOR_LOCAL && !gotPos)
            return std::unexpected<std::string>("anchor:local requires 'pos:x,y,z'");
        if (anchor.mode != OpenXR::XR_ANCHOR_LOCAL && !gotPos)
            return std::unexpected<std::string>("this anchor mode requires 'offset:x,y,z'");

        // Adaptive anchoring decorates anchor:local only (the desk pose is the persistent identity).
        if (sawAdaptive && anchor.mode != OpenXR::XR_ANCHOR_LOCAL)
            return std::unexpected<std::string>("adaptive/roam tokens are only valid on anchor:local");
        if (anchor.adaptive.returnRadius >= 0.f && anchor.adaptive.leaveRadius >= 0.f && anchor.adaptive.returnRadius >= anchor.adaptive.leaveRadius)
            return std::unexpected<std::string>("adaptive 'return' radius must be smaller than 'leave' radius (hysteresis)");

        anchor.anchorPose.pos = OpenXR::Vec3{posX, posY, posZ};

        // Build the stored rotation (doc 03 §7 deserialization): rot = qFromYaw ∘ qFromPitch.
        // head display orientation is lookAt-driven (§3.2) so its stored rot is left identity;
        // body is yaw-only (§3.3); local/device carry yaw+pitch.
        constexpr float DEG2RAD = 3.14159265358979323846f / 180.f;
        switch (anchor.mode) {
            case OpenXR::XR_ANCHOR_HEAD: anchor.anchorPose.rot = OpenXR::Quat{}; break;
            case OpenXR::XR_ANCHOR_BODY: anchor.anchorPose.rot = OpenXR::qFromYaw(yawDeg * DEG2RAD); break;
            default: anchor.anchorPose.rot = OpenXR::qMul(OpenXR::qFromYaw(yawDeg * DEG2RAD), OpenXR::qFromPitch(pitchDeg * DEG2RAD)); break;
        }

        // Roam-offset rotation: head is lookAt-driven (identity stored); body is yaw-only.
        anchor.adaptive.roamOffset.rot = anchor.adaptive.roamMode == OpenXR::XR_ANCHOR_HEAD ? OpenXR::Quat{} : OpenXR::qFromYaw(roamYawDeg * DEG2RAD);

        out.m_anchor         = anchor;
        out.m_anchorProvided = true;
        return {};
    }

    // Whitespace-split a string, strip surrounding commas, drop empties.
    std::vector<std::string> tokenizeAnchor(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream       iss(s);
        std::string              w;
        while (iss >> w) {
            std::string t = stripToken(w);
            if (!t.empty())
                out.push_back(t);
        }
        return out;
    }
}

std::string OpenXR::blendModeToString(eXRBlendMode mode) {
    switch (mode) {
        case XR_BLEND_OPAQUE: return "opaque";
        case XR_BLEND_ALPHA: return "alpha";
        case XR_BLEND_ADDITIVE: return "additive";
    }
    return "opaque";
}

OpenXR::SXRBlendModePick OpenXR::pickBlendMode(const std::vector<eXRBlendMode>& supported, const std::string& config) {
    // The runtime's preferred mode = its first-listed one (empty list defends against a
    // spec-noncompliant runtime by falling back to OPAQUE).
    const eXRBlendMode preferred = supported.empty() ? XR_BLEND_OPAQUE : supported.front();

    // "auto" (or any unrecognized value) => take the runtime preference, no fallback warning.
    std::optional<eXRBlendMode> want;
    if (config == "opaque")
        want = XR_BLEND_OPAQUE;
    else if (config == "alpha")
        want = XR_BLEND_ALPHA;
    else if (config == "additive")
        want = XR_BLEND_ADDITIVE;

    if (!want)
        return {preferred, false};

    // Explicit request: honor iff the runtime advertises it, else fall back with a flag.
    for (const auto m : supported)
        if (m == *want)
            return {*want, false};
    return {preferred, true};
}

OpenXR::eXRMonitorFollowMode OpenXR::parseMonitorFollowMode(const std::string& v) {
    // Lower-case + strip surrounding whitespace (hyprlang trims already, but be defensive).
    std::string s;
    s.reserve(v.size());
    for (const unsigned char c : v)
        if (!std::isspace(c))
            s += (char)std::tolower(c);

    if (s == "off" || s == "0" || s == "false" || s == "no" || s == "none")
        return XR_FOLLOW_OFF;
    if (s == "session" || s == "1" || s == "true" || s == "yes" || s == "existence" || s == "exists")
        return XR_FOLLOW_SESSION;
    if (s == "visible" || s == "focused" || s == "visibility" || s == "worn")
        return XR_FOLLOW_VISIBLE;
    return XR_FOLLOW_VISIBLE; // default (report-18 addendum: doffed/standby headset reads as unplugged)
}

OpenXR::eXRRecenterPolicy OpenXR::parseRecenterPolicy(const std::string& v) {
    // Same normalization as parseMonitorFollowMode: lower-case, strip whitespace.
    std::string s;
    s.reserve(v.size());
    for (const unsigned char c : v)
        if (!std::isspace(c))
            s += (char)std::tolower(c);

    // Deliberately NO boolean spellings: there is no "recenter on/off" to be compatible with, and
    // reading a stray `1` as "follow" would silently hand someone the mode this option exists to
    // make them opt into. Anything unrecognized keeps today's behavior.
    if (s == "follow" || s == "reseat" || s == "me")
        return XR_RECENTER_FOLLOW;
    return XR_RECENTER_HOLD;
}

const char* OpenXR::recenterPolicyName(eXRRecenterPolicy p) {
    return p == XR_RECENTER_FOLLOW ? "follow" : "hold";
}

bool OpenXR::wantXRMonitorsPlugged(eXRMonitorFollowMode mode, bool sessionUp, bool sessionVisible, bool presenceSupported, bool presenceKnown, bool userPresent) {
    // research/18 + report-18/19/20 addenda: instantaneous plugged predicate. See the header for the
    // full contract. OFF keeps XR monitors always plugged (pre-feature); SESSION plugs while a session
    // exists; VISIBLE gates on the real donned signal.
    //
    // report-20 (issue D): the VISIBLE gate is now the CONJUNCTION of BOTH available signals —
    // visibility AND, when the runtime exposes it, user presence. Presence alone was insufficient
    // because WiVRn's user_presence STICKS 'present' while the headset sits doffed in standby; the
    // session correctly drops VISIBLE->SYNCHRONIZED on doff, so requiring visibility too lets a doff
    // unplug even when presence is stuck. Symmetrically, presence-absent (if it ever fires) unplugs
    // even while a stale VISIBLE bit lingers. Both must currently agree.
    switch (mode) {
        case XR_FOLLOW_OFF: return true;
        case XR_FOLLOW_SESSION: return sessionUp;
        case XR_FOLLOW_VISIBLE: break;
    }
    if (!sessionUp)
        return false;      // VISIBLE mode still requires a live session
    if (!sessionVisible)
        return false;      // doffed / standby -> not visible -> unplug (even if presence sticks present)
    if (presenceSupported) // visible AND the runtime has a presence signal: require present too
        return presenceKnown && userPresent; // absent until the first presence event -> blip-proof
    return true;                             // no presence ext: visibility alone is the signal
}

std::optional<float> OpenXR::xrDefaultMonitorScale(bool createdByXR, bool ruleScaleExplicit, bool stereoOutput, const Vector2D& pixelSize, bool skipScaleChecks,
                                                   float configuredDefault) {
    // task #129. See the header for the precedence ladder. Every rejection is "do not own the
    // scale" — this function never returns the auto sentinel, so a caller can write its result
    // unconditionally when present and use field provenance to hand an old runtime value back.
    if (!createdByXR)         // adopted real monitor: its scale is the user's business
        return std::nullopt;
    if (ruleScaleExplicit)    // an explicit `monitor = NAME, ..., <scale>` outranks us
        return std::nullopt;
    if (stereoOutput)         // packed per-eye scanout wants 1.0, which getDefaultScale() already pins
        return std::nullopt;
    if (!(configuredDefault > XR_RULE_SCALE_AUTO_MAX)) // opted out (0) -> fall through to the PPI guess
        return std::nullopt;
    if (skipScaleChecks)      // debug:disable_scale_checks — the user took the wheel; no divisor gate
        return configuredDefault;
    if (pixelSize.x <= 0 || pixelSize.y <= 0) // mode not knowable yet: claim only what we can prove
        return std::nullopt;

    // The divisor gate, in exactly the arithmetic the consumer uses — Monitor::Stereo::
    // scaleValidationSize() is `pane / scale` with the float scale promoted to double, and
    // applyMonitorRule compares that against its own round(). Computing it any other way here would
    // let a value through that trips the ERR path we exist to avoid.
    const Vector2D LOGICAL = pixelSize / static_cast<double>(configuredDefault);
    if (LOGICAL != LOGICAL.round())
        return std::nullopt;
    return configuredDefault;
}

OpenXR::eXRIdleInhibitMode OpenXR::parseIdleInhibitMode(const std::string& v) {
    // Same normalization as parseMonitorFollowMode: lower-case, strip whitespace.
    std::string s;
    s.reserve(v.size());
    for (const unsigned char c : v)
        if (!std::isspace(c))
            s += (char)std::tolower(c);

    if (s == "off" || s == "0" || s == "false" || s == "no" || s == "none")
        return XR_INHIBIT_OFF;
    // Legacy `inhibit_idle = true/1` maps to FOCUSED, NOT to the new default: an existing config that
    // explicitly opted in keeps byte-for-byte the behavior it opted into (doc 05 §7). Only configs
    // that never mention inhibit_idle at all get the widened `present` default.
    if (s == "1" || s == "true" || s == "yes" || s == "focused" || s == "focus" || s == "on")
        return XR_INHIBIT_FOCUSED;
    if (s == "present" || s == "worn" || s == "presence" || s == "user_presence")
        return XR_INHIBIT_PRESENT;
    return XR_INHIBIT_PRESENT; // default (research/20 phase 2)
}

std::string OpenXR::idleInhibitModeToString(eXRIdleInhibitMode mode) {
    switch (mode) {
        case XR_INHIBIT_OFF: return "off";
        case XR_INHIBIT_FOCUSED: return "focused";
        case XR_INHIBIT_PRESENT: return "present";
    }
    return "present";
}

bool OpenXR::wantXRIdleInhibit(eXRIdleInhibitMode mode, bool sessionUp, bool sessionVisible, bool sessionFocused, bool presenceSupported, bool presenceKnown, bool userPresent) {
    // research/20 §5 option D (phase 2). See the header for the full contract + why PRESENT requires
    // BOTH visibility and presence on a presence-capable runtime (WiVRn's presence sticks in standby).
    switch (mode) {
        case XR_INHIBIT_OFF: return false;
        case XR_INHIBIT_FOCUSED: return sessionFocused;
        case XR_INHIBIT_PRESENT: break;
    }
    if (!sessionUp)
        return false; // no session -> nothing to inhibit for
    if (!presenceSupported)
        return sessionFocused; // no wear signal at all -> exactly the FOCUSED semantics
    return sessionVisible && presenceKnown && userPresent;
}

bool OpenXR::xrDeferFirstPlug(bool everPlugged, int64_t visibleSustainedMs, int64_t blipMs) {
    // First-plug settle guard (report-20 issue D): the FIRST plug of a session must wait until
    // visibility has been continuously sustained past the session-create blip window, so a runtime
    // that sprints to VISIBLE/FOCUSED at session creation (WiVRn does within ~40ms, even doffed)
    // cannot plug on that blip. This applies to the VISIBLE-side of the gate REGARDLESS of presence
    // support: at 10:57 WiVRn reported presence 'present' 0.5ms after session creation (a headset
    // possibly mid-don, indistinguishable from a spurious blip), so both signals agreeing is not on
    // its own enough for the first plug — visibility must also be sustained. Later plugs
    // (everPlugged) use the anti-flap grace instead and never defer.
    if (everPlugged)
        return false;
    return visibleSustainedMs < blipMs;
}

int64_t OpenXR::xrReprobeBackoffMs(int attempt, int64_t baseMs, int64_t capMs) {
    // report-17 WP-L3 / report-20 issue B1: growing backoff for re-probing the runtime while dormant
    // in UNAVAILABLE. attempt is the 0-based count of consecutive failed probes. Doubling from base,
    // clamped to cap: with base=2000, cap=30000 -> 2s, 4s, 8s, 16s, 30s(cap)... (the report's
    // "2s->5s->10s cap 30s" suggestion, approximated by a clean power-of-two schedule). Pure/gtested.
    if (baseMs <= 0)
        baseMs = 2000;
    if (capMs < baseMs)
        capMs = baseMs;
    int64_t v = baseMs;
    for (int i = 0; i < attempt && v < capMs; ++i)
        v = std::min<int64_t>(v * 2, capMs);
    return v;
}

std::vector<OpenXR::SXRReprobeWatch> OpenXR::xrReprobeWatchDirs(const std::string& runtimeDir) {
    // No XDG_RUNTIME_DIR -> the runtime socket location is unresolvable; caller relies on the timer.
    if (runtimeDir.empty())
        return {};
    std::string base = runtimeDir;
    while (base.size() > 1 && base.back() == '/') // normalize a trailing slash (env may carry one)
        base.pop_back();
    std::vector<SXRReprobeWatch> out;
    // [0] the runtime dir itself. Triggers: monado's socket lands here directly, and BOTH runtimes'
    // pid files land here — the pid file is the only don-time filesystem signal WiVRn emits (live
    // evidence: comp_ipc is created at service start and inherited by the forked server; wivrn.pid
    // is created/rewritten by the forked server exactly at headset-connect). "wivrn" subdir creation
    // is also watched (first service start of a boot creates it, then comp_ipc inside).
    out.push_back(SXRReprobeWatch{.dir = base, .triggerNames = {"monado_comp_ipc", "monado.pid", "wivrn.pid"}, .subdirNames = {"wivrn"}});
    // [1] the WiVRn subdir: comp_ipc appears inside at SERVICE start (not at don — see above).
    out.push_back(SXRReprobeWatch{.dir = base + "/wivrn", .triggerNames = {"comp_ipc"}, .subdirNames = {}});
    return out;
}

bool OpenXR::xrReprobeTriggerMatch(const SXRReprobeWatch& w, const std::string& name) {
    for (const auto& s : w.triggerNames)
        if (s == name)
            return true;
    return false;
}

std::vector<std::string> OpenXR::xrRuntimeSocketPaths(const std::string& runtimeDir) {
    if (runtimeDir.empty())
        return {};
    std::string base = runtimeDir;
    while (base.size() > 1 && base.back() == '/')
        base.pop_back();
    return {base + "/monado_comp_ipc", base + "/wivrn/comp_ipc"};
}

int64_t OpenXR::xrReprobeDelayMs(bool headsetWait, bool activityRecent, int attempt, int64_t baseMs, int64_t capMs) {
    // Backoff-policy decision table (live-evidence bug 1). HEADSET-class waits (runtime reachable,
    // headset absent — includes WiVRn's degraded pre-don mode) and windows of recent relevant
    // filesystem activity (the runtime is materializing) poll at the fixed base cadence; only a
    // truly absent, quiet runtime earns the grown backoff.
    if (baseMs <= 0)
        baseMs = 2000;
    if (headsetWait || activityRecent)
        return baseMs;
    return xrReprobeBackoffMs(attempt, baseMs, capMs);
}

bool OpenXR::xrReprobeSubdirMatch(const SXRReprobeWatch& w, const std::string& name) {
    for (const auto& s : w.subdirNames)
        if (s == name)
            return true;
    return false;
}

OpenXR::eForceLinearMode OpenXR::parseForceLinearMode(const std::string& s) {
    if (s == "on" || s == "true" || s == "1" || s == "yes")
        return XR_LINEAR_ON;
    if (s == "off" || s == "false" || s == "0" || s == "no")
        return XR_LINEAR_OFF;
    return XR_LINEAR_AUTO; // "auto" and anything unrecognized
}

bool OpenXR::shouldForceLinear(eForceLinearMode mode, bool gpusKnown, bool sameGpu) {
    switch (mode) {
        case XR_LINEAR_OFF: return false;
        case XR_LINEAR_ON: return true;
        case XR_LINEAR_AUTO:
        default:
            // Only force when we can positively confirm the XR EGL device differs from the buffer
            // allocator's device. An unresolved device (shared-display fallback, unstat-able fd) is
            // treated as same-GPU and left native — the fail-closed guard in start() already refuses
            // the truly dangerous wrong-GPU case, and forcing linear on a same-GPU setup just wastes
            // bandwidth (and, on NVIDIA, breaks entirely: its allocator can't hand back MOD_LINEAR for
            // a scanout buffer, so the frames are dropped and the panel goes black).
            if (!gpusKnown)
                return false;
            return !sameGpu;
    }
}

std::optional<uint64_t> OpenXR::parseKernelTaint(std::string_view contents) {
    // Trim ASCII whitespace both ends — the file is "<decimal>\n", but be liberal.
    const auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
    while (!contents.empty() && isWs(contents.front()))
        contents.remove_prefix(1);
    while (!contents.empty() && isWs(contents.back()))
        contents.remove_suffix(1);
    if (contents.empty())
        return std::nullopt;

    // Strictly unsigned decimal, and the WHOLE remaining token must be consumed: a value we do not
    // fully understand is treated as unreadable (fail open) rather than silently masked, so a
    // future kernel format change can never invent a TAINT_DIE bit that is not there.
    uint64_t   value = 0;
    const auto res   = std::from_chars(contents.data(), contents.data() + contents.size(), value, 10);
    if (res.ec != std::errc{} || res.ptr != contents.data() + contents.size())
        return std::nullopt;
    return value;
}

std::optional<uint64_t> OpenXR::readKernelTaint(const std::string& path) {
    // procfs entries are generated on read and report st_size 0, so read the first LINE rather than
    // sizing the file. Text mode, one getline: the content is "<decimal>\n" and nothing else.
    std::ifstream f(path);
    if (!f.is_open())
        return std::nullopt;
    std::string line;
    if (!std::getline(f, line))
        return std::nullopt;
    return parseKernelTaint(line);
}

OpenXR::SKernelTaintVerdict OpenXR::evaluateKernelTaint(std::optional<uint64_t> taint, bool ignore) {
    // Fail OPEN when we could not read/parse it — see the header. A tripwire that turns an
    // unreadable proc file into "no XR for you" would be a worse bug than the one it guards.
    if (!taint.has_value())
        return {};

    if ((*taint & XR_TAINT_DIE_MASK) == 0)
        return {};

    SKernelTaintVerdict out;
    out.oopsed = true;

    // One sentence of WHAT, one of WHY IT MATTERS HERE, one of WHAT TO DO. This exact string is
    // both the ERR log line and the `blocked:` line in `hyprctl openxr status`, so it has to stand
    // alone without the surrounding log context.
    out.reason = std::format("the kernel has taken an oops this boot (/proc/sys/kernel/tainted = {}, TAINT_DIE set); XR bring-up would enter a possibly-corrupt GPU driver "
                             "— reboot before using XR",
                             *taint);

    if (ignore) {
        out.blocked = false;
        out.reason += " [proceeding anyway: openxr:ignore_kernel_taint = 1]";
        return out;
    }

    out.blocked = true;
    out.reason += " (set openxr:ignore_kernel_taint = 1 to override)";
    return out;
}

OpenXR::SRuntimeJsonEnvAction OpenXR::resolveRuntimeJsonEnv(const std::string& configValue, bool originalPresent, const std::string& originalValue, bool currentPresent,
                                                           const std::string& currentValue) {
    // Desired environment state:
    //   configValue non-empty -> XR_RUNTIME_JSON should be exactly configValue (the override).
    //   configValue empty      -> restore the ORIGINAL (the runtime the process launched with): either
    //                             the original value, or "unset" if the process had none.
    const bool        wantPresent = !configValue.empty() || originalPresent;
    const std::string wantValue   = configValue.empty() ? originalValue : configValue;

    if (!wantPresent) {
        // Want no XR_RUNTIME_JSON at all. Unset only if something is currently set (else NOOP).
        return currentPresent ? SRuntimeJsonEnvAction{XR_RTJSON_UNSET, ""} : SRuntimeJsonEnvAction{XR_RTJSON_NOOP, ""};
    }

    // Want XR_RUNTIME_JSON == wantValue. Skip the environ mutation entirely if it already matches
    // (steady-state reprobes must not churn `environ` — that is the whole point of the guard).
    if (currentPresent && currentValue == wantValue)
        return {XR_RTJSON_NOOP, ""};
    return {XR_RTJSON_SET, wantValue};
}

std::string OpenXR::anchorModeToString(eXRAnchorMode mode, eXRHand device) {
    switch (mode) {
        case XR_ANCHOR_LOCAL: return "local";
        case XR_ANCHOR_HEAD: return "head";
        case XR_ANCHOR_BODY: return "body";
        case XR_ANCHOR_DEVICE: return device == XR_HAND_RIGHT ? "device:right" : "device:left";
    }
    return "local";
}

std::string OpenXR::anchorModeToString(const SXRAnchorState& state) {
    return anchorModeToString(state.mode, state.device);
}

namespace {
    // Derive yaw/pitch degrees from a pose's forward vector for serialization (doc 03 §7).
    void quatToYawPitchDeg(const OpenXR::Quat& q, float& yawDeg, float& pitchDeg) {
        const OpenXR::Vec3 f       = OpenXR::qRotate(q, OpenXR::Vec3{0.f, 0.f, -1.f});
        const float        pitch   = std::asin(std::clamp(f.y, -1.f, 1.f));
        const float        yaw     = (f.x * f.x + f.z * f.z < 1e-8f) ? 0.f : std::atan2(-f.x, -f.z);
        constexpr float    RAD2DEG = 180.f / 3.14159265358979323846f;
        yawDeg                     = yaw * RAD2DEG;
        pitchDeg                   = pitch * RAD2DEG;
    }
}

std::string OpenXR::serializeXRMonitorLine(const std::string& name, Vector2D resolution, std::optional<float> refreshHz, const SXRAnchorState& anchor, const SXRPose& pose,
                                            float sizeMeters) {
    std::string mode = std::format("{}x{}", (int)resolution.x, (int)resolution.y);
    if (refreshHz.has_value() && *refreshHz > 0.f)
        mode += std::format("@{:.0f}", *refreshHz);

    const auto& p = pose.pos;
    std::string spec;
    switch (anchor.mode) {
        case XR_ANCHOR_LOCAL: spec = std::format("anchor:local pos:{:.3f},{:.3f},{:.3f}", p.x, p.y, p.z); break;
        case XR_ANCHOR_HEAD: spec = std::format("anchor:head offset:{:.3f},{:.3f},{:.3f}", p.x, p.y, p.z); break;
        case XR_ANCHOR_BODY: spec = std::format("anchor:body offset:{:.3f},{:.3f},{:.3f}", p.x, p.y, p.z); break;
        case XR_ANCHOR_DEVICE:
            spec = std::format("anchor:device:{} offset:{:.3f},{:.3f},{:.3f}", anchor.device == XR_HAND_RIGHT ? "right" : "left", p.x, p.y, p.z);
            break;
    }

    // Rotation (doc 03 §7): head prints no rotation (lookAt-driven); body prints yaw only
    // (pitch/roll forced to 0); local/device print yaw and pitch (pitch omitted when
    // |pitch| < 0.05°). Roll is not representable and is intentionally dropped (v1 limitation).
    float yawDeg = 0.f, pitchDeg = 0.f;
    quatToYawPitchDeg(pose.rot, yawDeg, pitchDeg);
    if (anchor.mode != XR_ANCHOR_HEAD)
        spec += std::format(" yaw:{:.1f}", yawDeg);
    if ((anchor.mode == XR_ANCHOR_LOCAL || anchor.mode == XR_ANCHOR_DEVICE) && std::fabs(pitchDeg) >= 0.05f)
        spec += std::format(" pitch:{:.1f}", pitchDeg);

    // Adaptive anchoring tokens (research/13 §6.4). `pose` here is the SAVED dock pose (the caller
    // passes m_state.anchorPose for an adaptive monitor regardless of live phase), so a save while
    // roaming round-trips to the desk pose — the correct persistent identity. Seat + live phase are
    // ephemeral and not serialized.
    if (anchor.adaptive.enabled) {
        spec += " adaptive:on";
        // Only emit an explicit roam mode when the monitor overrode the global default, so a
        // save/reload keeps deferring to openxr:adaptive_roam_mode when it wasn't set.
        if (anchor.adaptive.roamModeSet)
            spec += std::format(" roam:{}", anchor.adaptive.roamMode == XR_ANCHOR_HEAD ? "head" : "body");
        if (anchor.adaptive.hasRoamOffset) {
            const auto& ro = anchor.adaptive.roamOffset.pos;
            spec += std::format(" roam_offset:{:.3f},{:.3f},{:.3f}", ro.x, ro.y, ro.z);
            float ry = 0.f, rp = 0.f;
            quatToYawPitchDeg(anchor.adaptive.roamOffset.rot, ry, rp);
            spec += std::format(" roam_yaw:{:.1f}", ry);
        }
        if (anchor.adaptive.leaveRadius >= 0.f)
            spec += std::format(" leave:{:.2f}", anchor.adaptive.leaveRadius);
        if (anchor.adaptive.returnRadius >= 0.f)
            spec += std::format(" return:{:.2f}", anchor.adaptive.returnRadius);
        if (anchor.adaptive.carryOverride)
            spec += std::format(" carry:{}", anchor.adaptive.carryOffset ? "on" : "off");
    }

    return std::format("xrmonitor = {}, {}, {}, size:{:.2f}", name, mode, spec, sizeMeters);
}

std::expected<void, std::string> OpenXR::parseXRMonitorMode(const std::string& mode, SXRMonitorParams& out) {
    return parseMode(mode, out);
}

std::expected<void, std::string> OpenXR::parseXRAnchorSpec(const std::vector<std::string>& tokens, SXRMonitorParams& out) {
    return parseAnchorAndKV(tokens, out);
}

std::vector<std::string> OpenXR::tokenizeXRAnchorSpec(const std::string& spec) {
    return tokenizeAnchor(spec);
}

std::expected<SXRMonitorParams, std::string> OpenXR::parseXRMonitorLine(const std::string& args) {
    // Slice on the first two commas only: name, mode, and the rest (anchor-spec + kv). The rest
    // is kept unsplit so commas inside pos:/offset: survive (doc 05 §2.2); within it, sub-tokens
    // are whitespace-separated key:value pairs.
    const auto c1 = args.find(',');
    if (c1 == std::string::npos)
        return std::unexpected<std::string>("xrmonitor: expected <name>, <mode>, <anchor-spec>");
    const auto c2 = args.find(',', c1 + 1);
    if (c2 == std::string::npos)
        return std::unexpected<std::string>("xrmonitor: expected <name>, <mode>, <anchor-spec>");

    const std::string name = trim(args.substr(0, c1));
    if (name.empty())
        return std::unexpected<std::string>("xrmonitor: missing monitor name");

    SXRMonitorParams params;
    params.m_name = name;

    if (auto r = parseMode(args.substr(c1 + 1, c2 - c1 - 1), params); !r)
        return std::unexpected("xrmonitor: " + r.error());

    const auto tokens = tokenizeAnchor(args.substr(c2 + 1));
    if (auto r = parseAnchorAndKV(tokens, params); !r)
        return std::unexpected("xrmonitor: " + r.error());

    return params;
}

std::expected<SXRMonitorParams, std::string> OpenXR::parseXRMonitorCreateArgs(const std::string& args) {
    // Space-separated: <name> [WxH[@Hz]] [anchor-spec]. Defaults are applied by the caller when
    // fields are absent (doc 05 §3.1).
    const auto tokens = tokenizeAnchor(args);
    if (tokens.empty())
        return std::unexpected<std::string>("create: missing monitor name");

    SXRMonitorParams params;
    params.m_name = tokens[0];

    size_t idx = 1;

    // Optional mode: a token containing 'x' and not starting with "anchor:" is a mode.
    if (idx < tokens.size() && !tokens[idx].starts_with("anchor:") && tokens[idx].find('x') != std::string::npos) {
        if (auto r = parseMode(tokens[idx], params); !r)
            return std::unexpected("create: " + r.error());
        ++idx;
    }

    // Optional anchor-spec: everything from here, first token must be "anchor:...".
    if (idx < tokens.size()) {
        std::vector<std::string> anchorTokens(tokens.begin() + idx, tokens.end());
        if (auto r = parseAnchorAndKV(anchorTokens, params); !r)
            return std::unexpected("create: " + r.error());
    }
    // else: no anchor spec — caller applies the default anchor:local placement.

    return params;
}
