#pragma once

#include "../../defines.hpp"
#include "PassElement.hpp"

class CGradientValueData;

namespace Render {
    class ITexture;

    class CRenderPass {
      public:
        bool    empty() const;
        bool    single() const;
        bool    needsLiveBlur(CRenderContext& ctx);
        bool    needsPrecomputeBlur(CRenderContext& ctx);

        void    add(UP<IPassElement>&& elem);
        void    clear();
        void    removeAllOfType(const std::string& type);

        CRegion render(CRenderContext& ctx, const CRegion& damage_);

        // research/24 §3.3/§5.3 (WP S1): re-execute the pass THIS FRAME, unchanged, into whatever
        // framebuffer is bound now. The stereo producer uses it for the second eye: the elements
        // already know what they draw, the only thing that differs is the stereo eye in render
        // data, so a second composite is a replay rather than a second scene build (which would
        // double-send frame callbacks and re-run every layout query).
        //
        // Deliberately reuses render()'s simplify/blur decisions and its per-element damage — the
        // eye changes UVs, never geometry — and deliberately does NOT re-discard: presentFeedback
        // for an invisible surface is a per-frame protocol event, not a per-composite one.
        CRegion replay(CRenderContext& ctx);

      private:
        CRegion              m_damage;
        std::vector<CRegion> m_occludedRegions;
        CRegion              m_totalLiveBlurRegion;

        struct SPassElementData {
            CRegion          elementDamage;
            UP<IPassElement> element;
            bool             discard = false;
        };

        std::vector<SPassElementData> m_passElements;

        void                          simplify(CRenderContext& ctx, bool willBlur, const CRegion& liveBlurRegion);
        void                          planBackdropScopes(CRenderContext& ctx);
        void                          renderDebugData(CRenderContext& ctx);

        struct {
            bool         present = false;
            SP<ITexture> keyboardFocusText, pointerFocusText, lastWindowText;
        } m_debugData;
    };
}
