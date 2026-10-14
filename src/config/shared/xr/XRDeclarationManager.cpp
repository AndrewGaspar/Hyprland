#include "XRDeclarationManager.hpp"

#include <algorithm>

using namespace Config;

UP<CXRDeclarationManager>& Config::xrDeclarationMgr() {
    static UP<CXRDeclarationManager> p = makeUnique<CXRDeclarationManager>();
    return p;
}

void CXRDeclarationManager::clear() {
    m_monitors.clear();
    m_rules.clear();
    m_defaults.clear();
}

void CXRDeclarationManager::addMonitor(SXRMonitorParams&& params) {
    const auto WHEN = params.m_when.describe();
    std::erase_if(m_monitors, [&](const SXRMonitorParams& p) { return p.m_name == params.m_name && p.m_when.describe() == WHEN; });
    m_monitors.emplace_back(std::move(params));
}

void CXRDeclarationManager::addRule(OpenXR::SXRRule&& rule) {
    m_rules.emplace_back(std::move(rule));
}

void CXRDeclarationManager::addDefaults(OpenXR::SXRDefaultsBlock&& block) {
    m_defaults.emplace_back(std::move(block));
}
