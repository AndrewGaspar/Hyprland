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

    // THE PRECEDENCE TABLE (§4.2, §4.3, §4.5), as a pure function of the rule and what the client
    // said — so the whole matrix is exercised without a window, a surface or a compositor (WP S2).
    //
    // `tagged` is the client's own declaration: nullopt for "said nothing at all", CONTENT_OFF for
    // the explicit `stereo:mono`. The two are NOT the same input and do not produce the same answer.
    //
    // The return is split rather than resolved because the fullscreen query is the expensive half
    // and the caller runs per surface per frame: this decides WHETHER the gate applies, and the
    // caller asks the fullscreen controller only when it does.
    struct SResolution {
        eContentLayout layout = CONTENT_OFF; // what to crop with, once `gated` is satisfied
        bool           gated  = false;       // §4.3: engage only while the window owns the screen

        bool           operator==(const SResolution&) const = default;
    };

    inline SResolution resolveDeclaration(const SDeclaration& rule, std::optional<eContentLayout> tagged) {
        // `stereo off` is the user's off switch and beats everything, a client's tag included.
        if (rule.layout == CONTENT_OFF)
            return {};

        // `auto` delegates to the client. Every other layout is a human instruction and therefore
        // outranks the tag — §4.5's ecosystem warning is that correct-LOOKING content metadata is a
        // liability, so the last human instruction has to win.
        const auto LAYOUT = rule.layout == CONTENT_AUTO ? tagged.value_or(CONTENT_OFF) : rule.layout;
        if (LAYOUT == CONTENT_OFF)
            return {};

        // Two things are not guesses and skip §4.3's gate: `always` (written by hand) and a client
        // that declared its own packing. `stereo:mono` is a declaration of NOT-stereo, so it is not
        // one of them — it can only ever suppress.
        const bool CLIENT_DECLARED = tagged.has_value() && *tagged != CONTENT_OFF;
        return {.layout = LAYOUT, .gated = rule.gate != GATE_ALWAYS && !CLIENT_DECLARED};
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

    // ONE eye's pixels inside a packed content buffer — the box §5.2 derives the presented aspect
    // from. Identity for a mono layout.
    inline Vector2D contentPaneSize(const Vector2D& bufferSize, eContentLayout layout) {
        if (splitsHorizontally(layout))
            return {bufferSize.x / 2, bufferSize.y};
        if (splitsVertically(layout))
            return {bufferSize.x, bufferSize.y / 2};
        return bufferSize;
    }

    // §5.2's rule applied end to end: the presented HEIGHT/WIDTH of one eye, given the packed buffer
    // it came out of. The squeezed axis is un-squeezed by `k` — for a side-by-side pack that is the
    // pane's width, for an over-under pack its height ("axes swapped for TAB").
    //
    // The invariant worth stating out loud, because it is the entire reason the half layouts must be
    // DECLARED rather than measured: every packing of the same picture lands on the same number
    // here. A 3840x1080 `sbs` buffer, a 1920x1080 `hsbs` buffer, a 1920x2160 `tab` buffer and a
    // 1920x1080 `htab` buffer all present 1080/1920 — and the last two are pixel-for-pixel the same
    // SIZE as each other and as a mono 1920x1080 frame, so no pixel analysis can tell them apart.
    //
    // Consumer: the XR quad pair (WP X1), whose height is derived from content pixels. The flat
    // presenter never calls it, for the reason aspectFactor above spells out.
    inline float presentedAspect(const Vector2D& bufferSize, eContentLayout layout) {
        const auto PANE = contentPaneSize(bufferSize, layout);
        const auto K    = static_cast<double>(aspectFactor(layout));
        const auto W    = splitsVertically(layout) ? PANE.x : PANE.x * K;
        const auto H    = splitsVertically(layout) ? PANE.y * K : PANE.y;
        return static_cast<float>(H / std::max(1.0, W));
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

    // §5.6, both directions. A ray or pointer hit is computed against ONE PANE — the half of the
    // image an eye actually shows — while everything downstream of it (absolute pointer injection,
    // which is keyed by monitor name plus a 0..1 UV) wants a coordinate in the whole packed image.
    // Get this backwards and the cursor is offset by half a screen.
    //
    // Both are expressed through cropForEye so there is exactly ONE definition of where the halves
    // are: §5.6's `u = u_pane / 2` (left) and `0.5 + u_pane / 2` (right) are these with a full base
    // range. Identity for a mono layout in both directions, and exact inverses of each other.
    //
    // Consumer: the XR pointer un-mapping (WP X1/X3). It is named for the CONTENT producer on
    // purpose — §5.6's real warning is that the DEPTH producer (D2) needs the other mapping
    // entirely, because there both panes are the same desktop, so the un-map is the identity and it
    // is the disparity that must come off instead. The mapping belongs to the producer, not to the
    // presenter, and a function called "stereoUnmap" would have invited exactly that mistake.
    inline Vector2D paneUVToContentUV(const Vector2D& paneUV, eContentLayout layout, int eye, const SUVRange& base = {}) {
        const auto CROP = cropForEye(base, layout, eye);
        return CROP.tl + (CROP.br - CROP.tl) * paneUV;
    }

    inline Vector2D contentUVToPaneUV(const Vector2D& contentUV, eContentLayout layout, int eye, const SUVRange& base = {}) {
        const auto     CROP = cropForEye(base, layout, eye);
        const Vector2D SPAN = CROP.br - CROP.tl;
        return {
            SPAN.x == 0 ? 0 : (contentUV.x - CROP.tl.x) / SPAN.x,
            SPAN.y == 0 ? 0 : (contentUV.y - CROP.tl.y) / SPAN.y,
        };
    }
}
