#include "XRDefaults.hpp"

// Compiled unconditionally (no OpenXR headers, no HAVE_OPENXR guard) — see the header.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <unordered_set>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/string/String.hpp>
#include <re2/re2.h>

using namespace OpenXR;
using Hyprutils::Memory::sc;

static bool isDigits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](unsigned char c) { return std::isdigit(c); });
}

static std::string fmtFloat(float v) {
    return std::format("{}", v);
}

std::string OpenXR::describeXRSystemContext(const SXRSystemContext& ctx) {
    if (!ctx.bound)
        return "no system bound yet";

    std::string leased;
    for (const auto& d : ctx.leasedDisplays) {
        if (!leased.empty())
            leased += "; ";
        leased += d.description.empty() ? d.shortDescription : d.description;
    }

    return std::format("system '{}', leased [{}], eye_gaze {}, hand_tracking {}, passthrough {}", ctx.systemName, leased, ctx.eyeGaze, ctx.handTracking, ctx.passthrough);
}

bool SXRWhen::empty() const {
    return !hmd && !eyeGaze && !handTracking && !passthrough;
}

std::string SXRWhen::describe() const {
    std::string out;
    auto        add = [&out](const std::string& kv) {
        if (!out.empty())
            out += ' ';
        out += kv;
    };

    if (hmd)
        add(std::format("hmd=\"{}\"", *hmd));
    if (eyeGaze)
        add(std::format("eye_gaze={}", *eyeGaze));
    if (handTracking)
        add(std::format("hand_tracking={}", *handTracking));
    if (passthrough)
        add(std::format("passthrough={}", *passthrough));
    return out;
}

std::expected<void, std::string> OpenXR::setXRWhenHmd(SXRWhen& when, const std::string& value) {
    const auto TRIMMED = Hyprutils::String::trim(value);
    if (TRIMMED.empty())
        return std::unexpected("when.hmd must not be empty");

    if (TRIMMED.starts_with("desc:")) {
        const auto PREFIX = Hyprutils::String::trim(TRIMMED.substr(5));
        if (PREFIX.empty())
            return std::unexpected("when.hmd: 'desc:' needs a description prefix");
        when.hmd     = value;
        when.hmdDesc = PREFIX;
        when.hmdRe.reset();
        return {};
    }

    // log_errors off: a bad pattern is reported through the config-error path, not as a bare RE2
    // line on stderr that nobody can trace back to a config line (same as xrrule).
    re2::RE2::Options opts;
    opts.set_log_errors(false);
    auto re = std::make_shared<re2::RE2>(value, opts);
    if (!re->ok())
        return std::unexpected(std::format("when.hmd: invalid regex '{}' ({})", value, re->error()));

    when.hmd = value;
    when.hmdDesc.clear();
    when.hmdRe = std::move(re);
    return {};
}

std::expected<void, std::string> OpenXR::setXRWhenCapability(SXRWhen& when, std::string_view key, bool value) {
    if (key == "eye_gaze")
        when.eyeGaze = value;
    else if (key == "hand_tracking")
        when.handTracking = value;
    else if (key == "passthrough")
        when.passthrough = value;
    else
        return std::unexpected(std::format("unknown when key '{}' (valid: {})", key, XR_WHEN_VALID_KEYS));
    return {};
}

bool OpenXR::matchXRWhen(const SXRWhen& when, const SXRSystemContext& ctx) {
    if (when.empty())
        return true;
    if (!ctx.bound)
        return false;

    if (when.hmd) {
        if (!when.hmdDesc.empty()) {
            const bool ANY = std::ranges::any_of(
                ctx.leasedDisplays, [&when](const SXRLeasedDisplay& d) { return d.description.starts_with(when.hmdDesc) || d.shortDescription.starts_with(when.hmdDesc); });
            if (!ANY)
                return false;
        } else if (!when.hmdRe || !re2::RE2::PartialMatch(ctx.systemName, *when.hmdRe))
            return false;
    }

    if (when.eyeGaze && *when.eyeGaze != ctx.eyeGaze)
        return false;
    if (when.handTracking && *when.handTracking != ctx.handTracking)
        return false;
    if (when.passthrough && *when.passthrough != ctx.passthrough)
        return false;

    return true;
}

std::expected<void, std::string> OpenXR::validateXRDefaultsBlock(const SXRDefaultsBlock& block) {
    if (block.scale && (*block.scale < 0.F || !std::isfinite(*block.scale)))
        return std::unexpected(std::format("scale must be >= 0 (0 = PPI guess), got {}", fmtFloat(*block.scale)));
    if (block.size && (*block.size <= 0.F || !std::isfinite(*block.size)))
        return std::unexpected(std::format("size must be > 0 meters, got {}", fmtFloat(*block.size)));
    if (block.distance && (*block.distance <= 0.F || !std::isfinite(*block.distance)))
        return std::unexpected(std::format("distance must be > 0 meters, got {}", fmtFloat(*block.distance)));
    if (block.resolution && (block.resolution->x <= 0 || block.resolution->y <= 0))
        return std::unexpected("mode must be a positive WxH");
    return {};
}

const char* OpenXR::xrDefaultSourceName(eXRDefaultSource s) {
    switch (s) {
        case XR_DEFSRC_BLOCK: return "xr_defaults";
        case XR_DEFSRC_PROFILE: return "profile";
        case XR_DEFSRC_EXPLICIT: return "explicit";
        default: return "global";
    }
}

bool OpenXR::xrDefaultsHasProfile(const std::vector<SXRDefaultsBlock>& blocks, const std::string& profile) {
    return std::ranges::any_of(blocks, [&profile](const SXRDefaultsBlock& b) { return b.profile == profile; });
}

std::expected<SXRResolvedDefaults, std::string> OpenXR::resolveXRDefaults(const std::vector<SXRDefaultsBlock>& blocks, const std::string& profile, const SXRSystemContext& ctx,
                                                                          const SXRDefaultsGlobals& globals, const SXRExplicitFields& explicitFields) {
    if (!profile.empty() && !xrDefaultsHasProfile(blocks, profile))
        return std::unexpected(std::format("unknown xr_defaults profile '{}'", profile));

    SXRResolvedDefaults out;
    out.resolution  = {.value = globals.resolution};
    out.refreshRate = {.value = globals.refreshRate};
    out.scale       = {.value = globals.scale};
    out.size        = {.value = globals.size};
    out.distance    = {.value = globals.distance};

    // Two passes over the same list: base blocks first, then the profile's, so a profile layered on
    // top always wins regardless of where in the config its blocks sit.
    auto apply = [&](bool profilePass) {
        const auto SRC = profilePass ? XR_DEFSRC_PROFILE : XR_DEFSRC_BLOCK;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto& b = blocks[i];
            if (profilePass ? (profile.empty() || b.profile != profile) : !b.profile.empty())
                continue;
            if (!matchXRWhen(b.when, ctx))
                continue;

            const int IDX = sc<int>(i);
            if (b.resolution) {
                out.resolution  = {.value = *b.resolution, .source = SRC, .block = IDX};
                out.refreshRate = {.value = b.refreshRate.value_or(60.F), .source = SRC, .block = IDX};
            }
            if (b.scale)
                out.scale = {.value = *b.scale, .source = SRC, .block = IDX};
            if (b.size)
                out.size = {.value = *b.size, .source = SRC, .block = IDX};
            if (b.distance)
                out.distance = {.value = *b.distance, .source = SRC, .block = IDX};
        }
    };
    apply(false);
    apply(true);

    if (explicitFields.resolution) {
        out.resolution  = {.value = *explicitFields.resolution, .source = XR_DEFSRC_EXPLICIT};
        out.refreshRate = {.value = explicitFields.refreshRate.value_or(60.F), .source = XR_DEFSRC_EXPLICIT};
    }
    if (explicitFields.size)
        out.size = {.value = *explicitFields.size, .source = XR_DEFSRC_EXPLICIT};

    return out;
}

std::pair<std::optional<Vector2D>, std::optional<float>> OpenXR::xrRequestedMode(const SXRResolvedDefaults& resolved, bool pinGlobalMode) {
    if (resolved.resolution.source == XR_DEFSRC_GLOBAL && !pinGlobalMode)
        return {std::nullopt, std::nullopt};
    return {resolved.resolution.value, resolved.refreshRate.value};
}

std::string OpenXR::mintXRMonitorName(const std::vector<std::string>& taken) {
    const std::unordered_set<std::string> TAKEN(taken.begin(), taken.end());
    for (size_t n = 2;; ++n) {
        auto name = std::format("XR-{}", n);
        if (!TAKEN.contains(name))
            return name;
    }
}

bool OpenXR::looksLikeXRMode(std::string_view token) {
    const auto AT   = token.find('@');
    const auto DIMS = token.substr(0, AT);
    const auto X    = DIMS.find('x');
    if (X == std::string_view::npos || !isDigits(DIMS.substr(0, X)) || !isDigits(DIMS.substr(X + 1)))
        return false;
    if (AT == std::string_view::npos)
        return true;

    // @Hz: digits with at most one '.'
    const auto HZ = token.substr(AT + 1);
    if (HZ.empty() || std::ranges::count(HZ, '.') > 1)
        return false;
    return std::ranges::all_of(HZ, [](unsigned char c) { return std::isdigit(c) || c == '.'; }) && HZ != ".";
}
