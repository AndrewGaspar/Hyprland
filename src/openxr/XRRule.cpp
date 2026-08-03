#include "XRRule.hpp"

// Compiled unconditionally (no OpenXR headers, no HAVE_OPENXR guard) — see the header. Only std +
// RE2 (the same regex engine Hyprland's windowrules use) here.

#include <algorithm>
#include <cctype>
#include <format>

#include <re2/re2.h>

using namespace OpenXR;

namespace {
    std::string trim(std::string_view s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
            --e;
        return std::string(s.substr(b, e - b));
    }

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // Whitespace split that respects double quotes, so a condition value may contain spaces:
    //   focustitle:"Mozilla Firefox"  ->  one token  focustitle:Mozilla Firefox
    // The quotes are stripped; everything else is verbatim (regexes are NOT unescaped).
    std::vector<std::string> tokenize(std::string_view s) {
        std::vector<std::string> out;
        std::string              cur;
        bool                     inQuote = false, any = false;
        for (char c : s) {
            if (c == '"') {
                inQuote = !inQuote;
                any     = true;
                continue;
            }
            if (!inQuote && std::isspace(static_cast<unsigned char>(c))) {
                if (any)
                    out.push_back(cur);
                cur.clear();
                any = false;
                continue;
            }
            cur += c;
            any = true;
        }
        if (any)
            out.push_back(cur);
        return out;
    }

    std::expected<float, std::string> parseFloat(const std::string& s, const std::string& what) {
        const std::string t = trim(s);
        if (t.empty())
            return std::unexpected(what + " is empty");
        try {
            size_t idx = 0;
            float  v   = std::stof(t, &idx);
            if (idx != t.size())
                return std::unexpected(std::format("invalid number for {}: '{}'", what, t));
            return v;
        } catch (...) { return std::unexpected(std::format("invalid number for {}: '{}'", what, t)); }
    }

    std::expected<float, std::string> parseUnit(const std::string& s, const std::string& what) {
        auto v = parseFloat(s, what);
        if (!v)
            return v;
        if (*v < 0.F || *v > 1.F)
            return std::unexpected(std::format("{} must be between 0 and 1 (got {})", what, trim(s)));
        return v;
    }

    std::expected<bool, std::string> parseBool(const std::string& s, const std::string& what) {
        const std::string t = lower(trim(s));
        if (t == "1" || t == "true" || t == "yes" || t == "on")
            return true;
        if (t == "0" || t == "false" || t == "no" || t == "off")
            return false;
        return std::unexpected(std::format("{} must be 0 or 1 (got '{}')", what, trim(s)));
    }

    // Compile a user regex. RE2 is what Hyprland's own rule match engine uses, so the accepted
    // syntax is identical; see xrRuleMatches for the PartialMatch semantics.
    std::expected<std::shared_ptr<const re2::RE2>, std::string> compileRe(const std::string& pattern, const std::string& what) {
        // log_errors off: a bad pattern is reported through the config-error path with the offending
        // rule named, not as a bare RE2 line on stderr that no one can trace back to a config line.
        re2::RE2::Options opts;
        opts.set_log_errors(false);
        auto re = std::make_shared<re2::RE2>(pattern, opts);
        if (!re->ok())
            return std::unexpected(std::format("invalid regex for {}: '{}' ({})", what, pattern, re->error()));
        return std::shared_ptr<const re2::RE2>(std::move(re));
    }
}

const char* OpenXR::xrAnchorStateName(eXRAnchorState s) {
    switch (s) {
        case XR_ANCHORSTATE_FOLLOW: return "follow";
        case XR_ANCHORSTATE_CARRIED: return "carried";
        default: return "docked";
    }
}

std::optional<eXRAnchorState> OpenXR::xrParseAnchorState(const std::string& s) {
    const std::string t = lower(trim(s));
    if (t == "docked")
        return XR_ANCHORSTATE_DOCKED;
    if (t == "follow" || t == "following" || t == "roaming")
        return XR_ANCHORSTATE_FOLLOW;
    if (t == "carried" || t == "grabbed")
        return XR_ANCHORSTATE_CARRIED;
    return std::nullopt;
}

const char* OpenXR::xrEffectSourceName(eXREffectSource s) {
    switch (s) {
        case XR_EFFSRC_RULE: return "rule";
        case XR_EFFSRC_MANUAL: return "manual";
        default: return "default";
    }
}

std::expected<SXRRule, std::string> OpenXR::parseXRRuleLine(const std::string& args) {
    // Split on the FIRST comma only: `<effects>, <conditions>`. No comma at all = effects with no
    // conditions (matches every monitor) — the honest reading of "omitted condition = wildcard".
    const auto        c1     = args.find(',');
    const std::string fxStr  = trim(c1 == std::string::npos ? args : args.substr(0, c1));
    const std::string cndStr = c1 == std::string::npos ? std::string{} : trim(args.substr(c1 + 1));

    SXRRule           rule;
    rule.raw = trim(args);

    // ---- effects: space-separated `name value` pairs ----
    const auto fxTok = tokenize(fxStr);
    if (fxTok.empty())
        return std::unexpected<std::string>("xrrule: no effects given — expected `<effects>, <conditions>`, e.g. `xrrule = alpha 0.55, anchorstate:follow`");

    for (size_t i = 0; i < fxTok.size(); i += 2) {
        const std::string name = lower(fxTok[i]);
        if (i + 1 >= fxTok.size())
            return std::unexpected(std::format("xrrule: effect '{}' is missing its value", fxTok[i]));
        const std::string val = fxTok[i + 1];

        if (name == "alpha") {
            if (rule.effects.alpha)
                return std::unexpected<std::string>("xrrule: 'alpha' given twice");
            auto v = parseUnit(val, "alpha");
            if (!v)
                return std::unexpected("xrrule: " + v.error());
            rule.effects.alpha = *v;
        } else if (name == "blackalpha") {
            if (rule.effects.blackAlpha)
                return std::unexpected<std::string>("xrrule: 'blackalpha' given twice");
            if (lower(trim(val)) == "off")
                rule.effects.blackAlpha = 1.F; // off == every pixel opaque == no keying
            else {
                auto v = parseUnit(val, "blackalpha");
                if (!v)
                    return std::unexpected("xrrule: " + v.error());
                rule.effects.blackAlpha = *v;
            }
        } else if (name == "blackalpha_knee" || name == "blackalphaknee") {
            if (rule.effects.blackKnee)
                return std::unexpected<std::string>("xrrule: 'blackalpha_knee' given twice");
            auto v = parseFloat(val, "blackalpha_knee");
            if (!v)
                return std::unexpected("xrrule: " + v.error());
            rule.effects.blackKnee = std::clamp(*v, XR_BLACK_ALPHA_KNEE_MIN, 1.F);
        } else
            return std::unexpected(std::format("xrrule: unknown effect '{}' (valid: alpha, blackalpha, blackalpha_knee)", fxTok[i]));
    }

    // ---- conditions: space-separated `key:value` pairs, ALL must match ----
    for (const auto& tok : tokenize(cndStr)) {
        const auto colon = tok.find(':');
        if (colon == std::string::npos || colon == 0)
            return std::unexpected(std::format("xrrule: condition '{}' is not `key:value`", tok));
        const std::string key = lower(tok.substr(0, colon));
        const std::string val = tok.substr(colon + 1);
        if (val.empty())
            return std::unexpected(std::format("xrrule: condition '{}' has an empty value", key));

        if (key == "monitor") {
            if (rule.conds.monitorRe)
                return std::unexpected<std::string>("xrrule: 'monitor' given twice");
            auto re = compileRe(val, "monitor");
            if (!re)
                return std::unexpected("xrrule: " + re.error());
            rule.conds.monitorRe = *re;
        } else if (key == "focusclass") {
            if (rule.conds.focusClassRe)
                return std::unexpected<std::string>("xrrule: 'focusclass' given twice");
            auto re = compileRe(val, "focusclass");
            if (!re)
                return std::unexpected("xrrule: " + re.error());
            rule.conds.focusClassRe = *re;
        } else if (key == "focustitle") {
            if (rule.conds.focusTitleRe)
                return std::unexpected<std::string>("xrrule: 'focustitle' given twice");
            auto re = compileRe(val, "focustitle");
            if (!re)
                return std::unexpected("xrrule: " + re.error());
            rule.conds.focusTitleRe = *re;
        } else if (key == "anchorstate") {
            if (rule.conds.anchorState)
                return std::unexpected<std::string>("xrrule: 'anchorstate' given twice");
            auto st = xrParseAnchorState(val);
            if (!st)
                return std::unexpected(std::format("xrrule: unknown anchorstate '{}' (valid: docked, follow, carried)", val));
            rule.conds.anchorState = *st;
        } else if (key == "fullscreen") {
            if (rule.conds.fullscreen)
                return std::unexpected<std::string>("xrrule: 'fullscreen' given twice");
            auto b = parseBool(val, "fullscreen");
            if (!b)
                return std::unexpected("xrrule: " + b.error());
            rule.conds.fullscreen = *b;
        } else
            return std::unexpected(std::format("xrrule: unknown condition '{}' (valid: monitor, anchorstate, focusclass, focustitle, fullscreen)", key));
    }

    return rule;
}

bool OpenXR::xrRuleMatches(const SXRRule& rule, const SXRRuleContext& ctx) {
    const auto& c = rule.conds;

    // PartialMatch (search), NOT FullMatch: `focusclass:^steam_app_` must match "steam_app_620" and
    // `focusclass:(mpv|vlc)` must match "org.mpv.mpv". Anchor with ^...$ for an exact match. This is
    // a deliberate, documented divergence from Hyprland's windowrule regexes (doc 05 §xrrule).
    if (c.monitorRe && !re2::RE2::PartialMatch(ctx.monitorName, *c.monitorRe))
        return false;

    if (c.anchorState && *c.anchorState != ctx.anchorState)
        return false;

    // A condition ON the focused window cannot be satisfied by a monitor that has no window.
    if (c.focusClassRe && (!ctx.hasFocus || !re2::RE2::PartialMatch(ctx.focusClass, *c.focusClassRe)))
        return false;
    if (c.focusTitleRe && (!ctx.hasFocus || !re2::RE2::PartialMatch(ctx.focusTitle, *c.focusTitleRe)))
        return false;

    // fullscreen:0 DOES match an empty monitor (nothing fullscreen there) — it is a property of the
    // monitor's situation, not a claim that a window exists.
    if (c.fullscreen && *c.fullscreen != ctx.fullscreen)
        return false;

    return true;
}

SXRResolvedEffects OpenXR::xrResolveEffects(const SXREffects& defaults, const std::vector<SXRRule>& rules, const SXRRuleContext& ctx, const SXREffects& manual) {
    SXRResolvedEffects out;

    // Layer 1 — defaults.
    if (defaults.alpha)
        out.alpha = *defaults.alpha;
    if (defaults.blackAlpha)
        out.blackAlpha = *defaults.blackAlpha;
    if (defaults.blackKnee)
        out.blackKnee = *defaults.blackKnee;

    // Layer 2 — rules, in config order; a later match overrides PER EFFECT.
    for (const auto& r : rules) {
        if (!xrRuleMatches(r, ctx))
            continue;
        if (r.effects.alpha) {
            out.alpha    = *r.effects.alpha;
            out.alphaSrc = XR_EFFSRC_RULE;
        }
        if (r.effects.blackAlpha) {
            out.blackAlpha    = *r.effects.blackAlpha;
            out.blackAlphaSrc = XR_EFFSRC_RULE;
        }
        if (r.effects.blackKnee) {
            out.blackKnee    = *r.effects.blackKnee;
            out.blackKneeSrc = XR_EFFSRC_RULE;
        }
    }

    // Layer 3 — manual override (`hyprctl openxr alpha|blackalpha`), sticky until cleared to `auto`.
    if (manual.alpha) {
        out.alpha    = *manual.alpha;
        out.alphaSrc = XR_EFFSRC_MANUAL;
    }
    if (manual.blackAlpha) {
        out.blackAlpha    = *manual.blackAlpha;
        out.blackAlphaSrc = XR_EFFSRC_MANUAL;
    }
    if (manual.blackKnee) {
        out.blackKnee    = *manual.blackKnee;
        out.blackKneeSrc = XR_EFFSRC_MANUAL;
    }

    out.alpha      = std::clamp(out.alpha, 0.F, 1.F);
    out.blackAlpha = std::clamp(out.blackAlpha, 0.F, 1.F);
    out.blackKnee  = std::clamp(out.blackKnee, XR_BLACK_ALPHA_KNEE_MIN, 1.F);
    return out;
}
