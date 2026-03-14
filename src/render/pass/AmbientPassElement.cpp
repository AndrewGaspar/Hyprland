#include "AmbientPassElement.hpp"
#include "../Renderer.hpp"

CAmbientPassElement::CAmbientPassElement(const SAmbientData& data) : m_data(data) {
    ;
}

bool CAmbientPassElement::needsLiveBlur() {
    return false;
}

bool CAmbientPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CAmbientPassElement::boundingBox() {
    return m_data.monitorBox.copy().scale(1.F / g_pHyprRenderer->m_renderData.pMonitor->m_scale).round();
}

CRegion CAmbientPassElement::opaqueRegion() {
    return {};
}
