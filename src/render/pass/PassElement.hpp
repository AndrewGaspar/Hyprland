#pragma once

#include "../../defines.hpp"
#include <vector>

enum ePassElementType : uint8_t {
    EK_UNKNOWN = 0,
    EK_BORDER,
    EK_CLEAR,
    EK_FRAMEBUFFER,
    EK_PRE_BLUR,
    EK_RECT,
    EK_HINTS,
    EK_SHADOW,
    EK_SURFACE,
    EK_TEXTURE,
    EK_TEXTURE_MATTE,
    EK_INNER_GLOW,
    EK_TRANSFORMED_WINDOW,
    EK_CUSTOM,
};

class IPassElement {
  public:
    virtual ~IPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw();
    //
    virtual bool                needsLiveBlur()       = 0;
    virtual bool                needsPrecomputeBlur() = 0;
    virtual const char*         passName()            = 0;
    virtual ePassElementType    type()                = 0;
    virtual void                discard();
    virtual bool                undiscardable();
    virtual std::optional<CBox> boundingBox();  // in monitor-local logical coordinates
    virtual CRegion             opaqueRegion(); // in monitor-local logical coordinates
    virtual bool                disableSimplification();

    // research/24 §5.3 (WP S1): may CRenderPass::replay() run this element a SECOND time in the same
    // frame? True for everything that only paints — a second eye repaints the same geometry with
    // different UVs. False for an element whose output is a per-frame side effect computed from what
    // the framebuffer held BEFORE the pass reached it, because by replay time the framebuffer holds
    // the finished first eye and the "before" is gone.
    virtual bool replayable();

    // cached results, computed once per frame in CRenderPass::render()
    bool needsLiveBlurCached       = false;
    bool needsPrecomputeBlurCached = false;
};
