#pragma once

// Per-window stereo CONTENT — the pure part (research/24 §4, §5.2, §5.3, F7).
//
// A stereo OUTPUT (src/output/StereoPacking.hpp, WP F1) presents one logical monitor packed from N
// identical panes. This header is the other half: how a single window whose buffer already carries
// BOTH eyes packed into one frame is sampled differently per pane, so the two panes stop being
// identical and the packed frame is un-packed onto the user's face.
//
// Everything here is a pure function of (layout, eye) so tests/render/StereoContent.cpp exercises
// the very expressions the renderer runs, not a copy (the StereoPacking.hpp discipline).
//
// The crop is the whole producer: while compositing pane i, a stereo-declared window's primary
// surface samples half of its UV range instead of all of it. Everything else on the monitor keeps
// the same UVs in every pane and is therefore bit-identical between the eyes.

#include "../helpers/math/Math.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace Render::Stereo {

    // The declared packing of the CONTENT inside one window's buffer. Deliberately the same
    // vocabulary as the `monitor = …, stereo:` output token and as the xdg-toplevel-tag convention
    // (§4.2), because a user who learns one has learned all three.
    enum eContentLayout : uint8_t {
        CONTENT_OFF = 0, // mono. Also what an unmatched / suppressed window resolves to.
        CONTENT_SBS,     // full side-by-side: the frame is 2x wide, each half is a correct-aspect eye
        CONTENT_HSBS,    // half side-by-side: normal width, each half horizontally squeezed by 2
        CONTENT_TAB,     // full over-under: the frame is 2x tall
        CONTENT_HTAB,    // half over-under: normal height, each half vertically squeezed by 2
        CONTENT_AUTO,    // read the window's xdg-toplevel-tag (§4.2) — the cooperative channel
    };

    // §4.3's negative heuristic, as a per-window switch. The heuristic exists because half-cropping
    // a windowed desktop is the failure mode users report as "the compositor broke", so a rule that
    // matched on a guess (class/title) only engages while the window owns the screen. An explicit
    // declaration — the client's own tag, or `always` written by hand — is not a guess and skips it.
    enum eContentGate : uint8_t {
        GATE_FULLSCREEN = 0, // default: engage only while fullscreen-on-this-monitor
        GATE_ALWAYS,         // the manual verb: engage wherever the window is
    };

    struct SDeclaration {
        eContentLayout layout = CONTENT_OFF;
        eContentGate   gate   = GATE_FULLSCREEN;

        bool           operator==(const SDeclaration&) const = default;
    };

    // The window rule stores one integer (the rule engine's value variant has no room for a bespoke
    // struct, and the renderer must read this per surface per frame without touching a string).
    inline int64_t packDeclaration(const SDeclaration& decl) {
        return static_cast<int64_t>(decl.layout) | (static_cast<int64_t>(decl.gate) << 8);
    }

    inline SDeclaration unpackDeclaration(int64_t packed) {
        const auto LAYOUT = static_cast<uint8_t>(packed & 0xFF);
        const auto GATE   = static_cast<uint8_t>((packed >> 8) & 0xFF);
        return {
            .layout = LAYOUT > CONTENT_AUTO ? CONTENT_OFF : static_cast<eContentLayout>(LAYOUT),
            .gate   = GATE > GATE_ALWAYS ? GATE_FULLSCREEN : static_cast<eContentGate>(GATE),
        };
    }

    constexpr const char* layoutToString(eContentLayout layout) {
        switch (layout) {
            case CONTENT_SBS: return "sbs";
            case CONTENT_HSBS: return "hsbs";
            case CONTENT_TAB: return "tab";
            case CONTENT_HTAB: return "htab";
            case CONTENT_AUTO: return "auto";
            default: return "off";
        }
    }

    // One layout token. `mono` is spelled out because that is the word the tag convention uses for
    // "explicitly not stereo" (§4.2) and a client that says so must be able to suppress a rule.
    inline std::optional<eContentLayout> layoutFromString(std::string_view token) {
        if (token == "off" || token == "mono" || token == "none" || token == "0")
            return CONTENT_OFF;
        if (token == "sbs")
            return CONTENT_SBS;
        if (token == "hsbs")
            return CONTENT_HSBS;
        if (token == "tab")
            return CONTENT_TAB;
        if (token == "htab")
            return CONTENT_HTAB;
        if (token == "auto")
            return CONTENT_AUTO;
        return std::nullopt;
    }

    // `windowrule = stereo <layout> [always|fullscreen]` — the raw is everything after the effect
    // name. Kept pure so the .conf handler and the Lua table binding parse identically (F8).
    inline std::expected<SDeclaration, std::string> parseDeclaration(std::string_view raw) {
        SDeclaration result;

        size_t       i         = 0;
        auto         nextToken = [&]() -> std::string_view {
            while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t'))
                ++i;
            const size_t START = i;
            while (i < raw.size() && raw[i] != ' ' && raw[i] != '\t')
                ++i;
            return raw.substr(START, i - START);
        };

        const auto LAYOUT_TOKEN = nextToken();
        if (LAYOUT_TOKEN.empty())
            return std::unexpected(std::string{"stereo rule needs a layout (off, sbs, hsbs, tab, htab, auto)"});

        const auto LAYOUT = layoutFromString(LAYOUT_TOKEN);
        if (!LAYOUT)
            return std::unexpected(std::format("stereo rule has unknown layout \"{}\" (off, sbs, hsbs, tab, htab, auto)", LAYOUT_TOKEN));

        result.layout = *LAYOUT;

        if (const auto GATE_TOKEN = nextToken(); !GATE_TOKEN.empty()) {
            if (GATE_TOKEN == "always")
                result.gate = GATE_ALWAYS;
            else if (GATE_TOKEN == "fullscreen")
                result.gate = GATE_FULLSCREEN;
            else
                return std::unexpected(std::format("stereo rule has unknown scope \"{}\" (always, fullscreen)", GATE_TOKEN));

            if (!nextToken().empty())
                return std::unexpected(std::string{"stereo rule takes at most a layout and a scope"});
        }

        return result;
    }

    // The cooperative channel (§4.2, F6): xdg-toplevel-tag-v1, which is already implemented and
    // already matchable as `xdg_tag`. THE GRAMMAR, which is what a client (the Dead Space mod) must
    // emit verbatim:
    //
    //     stereo:sbs   stereo:hsbs   stereo:tab   stereo:htab   stereo:mono
    //
    // Exactly one tag, lowercase, no whitespace, no suffixes. `stereo:mono` is the explicit
    // not-stereo declaration and resolves to CONTENT_OFF (it suppresses a `stereo auto` rule rather
    // than failing to match it). Anything else — no tag, a tag with another prefix, an unknown
    // layout, or `stereo:auto` (a client cannot delegate back to itself) — is not a declaration.
    inline std::optional<eContentLayout> layoutFromTag(std::string_view tag) {
        constexpr std::string_view PREFIX = "stereo:";
        if (!tag.starts_with(PREFIX))
            return std::nullopt;

        const auto LAYOUT = layoutFromString(tag.substr(PREFIX.size()));
        if (!LAYOUT || *LAYOUT == CONTENT_AUTO)
            return std::nullopt;

        return LAYOUT;
    }

    inline bool splitsHorizontally(eContentLayout layout) {
        return layout == CONTENT_SBS || layout == CONTENT_HSBS;
    }

    inline bool splitsVertically(eContentLayout layout) {
        return layout == CONTENT_TAB || layout == CONTENT_HTAB;
    }

    // 2 for every packed layout, 1 for mono. CONTENT_AUTO is unresolved and therefore not packed —
    // it must be turned into a concrete layout (from the tag) before it reaches the renderer.
    inline int viewCount(eContentLayout layout) {
        return splitsHorizontally(layout) || splitsVertically(layout) ? 2 : 1;
    }

    // §5.2's `k`: the constant that turns PANE pixels into the presented aspect —
    //     presentedAspect(H/W) = paneH / (paneW · k),   k = 1 full, k = 2 half   (axes swap for TAB)
    //
    // On the FLAT presenter this is deliberately unused, and that is a finding rather than an
    // omission: there the destination is the window's own box, which the user sized, so cropping
    // half the buffer across an unchanged box already un-squeezes a half-packed frame by exactly 2
    // and leaves a full-packed frame at 1:1. `sbs` and `hsbs` are the SAME geometric operation on a
    // window; they differ only in how many source samples each eye gets. The constant matters where
    // the destination is DERIVED from content pixels — the XR quad pair (WP X1) — which is why it
    // lives here, next to the layouts, rather than being invented again there.
    inline float aspectFactor(eContentLayout layout) {
        return layout == CONTENT_HSBS || layout == CONTENT_HTAB ? 2.F : 1.F;
    }

    struct SUVRange {
        Vector2D tl = {0, 0};
        Vector2D br = {1, 1};

        bool     operator==(const SUVRange&) const = default;
    };

    // THE PRODUCER (§5.3, F7). `base` is whatever UV range the surface already resolved to
    // (wp_viewporter source box, undersized-texture expansion, …) — the crop composes with it
    // instead of replacing it, so a stereo window that also uses a viewporter source still works.
    //
    // Eye 0 is the LEFT eye and takes the left/top half; eye 1 is the RIGHT eye. That is the
    // left-first convention of both file-level vocabularies (§4.5) and of the pane order the output
    // packs in (pane 0 is the left half of the scanout). Identity for a mono layout.
    inline SUVRange cropForEye(const SUVRange& base, eContentLayout layout, int eye) {
        if (viewCount(layout) < 2)
            return base;

        const int      VIEW = std::clamp(eye, 0, 1);
        const Vector2D SPAN = base.br - base.tl;

        SUVRange       out = base;
        if (splitsHorizontally(layout)) {
            out.tl.x = base.tl.x + SPAN.x * 0.5 * VIEW;
            out.br.x = out.tl.x + SPAN.x * 0.5;
        } else {
            out.tl.y = base.tl.y + SPAN.y * 0.5 * VIEW;
            out.br.y = out.tl.y + SPAN.y * 0.5;
        }

        return out;
    }
}
