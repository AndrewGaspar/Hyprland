#include "PreBlurElement.hpp"

CPreBlurElement::CPreBlurElement() = default;

bool CPreBlurElement::needsLiveBlur() {
    return false;
}

bool CPreBlurElement::needsPrecomputeBlur() {
    return false;
}

bool CPreBlurElement::disableSimplification() {
    return true;
}

bool CPreBlurElement::undiscardable() {
    return true;
}

// research/24 §5.3 (WP S1): NOT on the second eye. This element blurs whatever the main framebuffer
// held when the pass reached it — by construction the wallpaper and the bottom layers, because it is
// added after them and before the first window (Renderer.cpp, "pre window pass"). A replay runs it
// again against a framebuffer that already holds the FINISHED first eye, which would fill
// m_blurFB with a blur of the whole composited desktop and then latch it (m_blurFBDirty is cleared
// on the way out), so every optimized/xray blur would sample it — in the second eye AND in every
// later frame. The result is eye-independent anyway: the crop only ever touches a window's own
// surface, never the background this samples.
bool CPreBlurElement::replayable() {
    return false;
}
