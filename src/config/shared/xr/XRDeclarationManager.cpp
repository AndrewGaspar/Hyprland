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
}

void CXRDeclarationManager::addMonitor(SXRMonitorParams&& params) {
    std::erase_if(m_monitors, [&](const SXRMonitorParams& p) { return p.m_name == params.m_name; });
    m_monitors.emplace_back(std::move(params));
}

void CXRDeclarationManager::addRule(OpenXR::SXRRule&& rule) {
    m_rules.emplace_back(std::move(rule));
}
