#include "MonitorRuleManager.hpp"

#include "../../../debug/log/Logger.hpp"
#include "../../../protocols/OutputManagement.hpp"
#include "../../../protocols/DRMLease.hpp"
#include "../../../output/Monitor.hpp"
#include "../../../Compositor.hpp"
#include "../../../render/Renderer.hpp"
#include "../../../event/EventBus.hpp"
#include "../../../managers/eventLoop/EventLoopManager.hpp"
#include "../../../managers/fullscreen/FullscreenController.hpp"
#include "../../../state/MonitorLayoutController.hpp"
#include "../../../state/MonitorState.hpp"

#include <ranges>
#include <hyprutils/utils/ScopeGuard.hpp>

using namespace Config;

UP<CMonitorRuleManager>& Config::monitorRuleMgr() {
    static UP<CMonitorRuleManager> p = makeUnique<CMonitorRuleManager>();
    return p;
}

CMonitorRuleManager::CMonitorRuleManager() {
    m_listeners.preChecksRender = Event::bus()->m_events.render.preChecks.listen([this](PHLMONITOR m) {
        if (m_reloadScheduled)
            ensureMonitorStatus();

        m_reloadScheduled = false;
    });
}

void CMonitorRuleManager::clear() {
    m_rules.clear();
}

void CMonitorRuleManager::add(CMonitorRule&& x) {
    std::erase_if(m_rules, [&x](const auto& e) { return e.m_name == x.m_name; });
    m_rules.emplace_back(std::move(x));

    scheduleReload();
}

CMonitorRule CMonitorRuleManager::get(const PHLMONITOR PMONITOR) {
    auto applyWlrOutputConfig = [PMONITOR](CMonitorRule rule) -> CMonitorRule {
        const auto CONFIG = PROTO::outputManagement->getOutputStateFor(PMONITOR);

        if (!CONFIG)
            return rule;

        Log::logger->log(Log::DEBUG, "CConfigManager::getMonitorRuleFor: found a wlr_output_manager override for {}", PMONITOR->m_name);

        Log::logger->log(Log::DEBUG, " > overriding enabled: {} -> {}", !rule.m_disabled, !CONFIG->enabled);
        rule.m_disabled = !CONFIG->enabled;

        if ((CONFIG->committedProperties & OUTPUT_HEAD_COMMITTED_MODE) || (CONFIG->committedProperties & OUTPUT_HEAD_COMMITTED_CUSTOM_MODE)) {
            Log::logger->log(Log::DEBUG, " > overriding mode: {:.0f}x{:.0f}@{:.2f}Hz -> {:.0f}x{:.0f}@{:.2f}Hz", rule.m_resolution.x, rule.m_resolution.y, rule.m_refreshRate,
                             CONFIG->resolution.x, CONFIG->resolution.y, CONFIG->refresh / 1000.F);
            rule.m_resolution  = CONFIG->resolution;
            rule.m_refreshRate = CONFIG->refresh / 1000.F;
        }

        if (CONFIG->committedProperties & OUTPUT_HEAD_COMMITTED_POSITION) {
            Log::logger->log(Log::DEBUG, " > overriding offset: {:.0f}, {:.0f} -> {:.0f}, {:.0f}", rule.m_offset.x, rule.m_offset.y, CONFIG->position.x, CONFIG->position.y);
            rule.m_offset = CONFIG->position;
        }

        if (CONFIG->committedProperties & OUTPUT_HEAD_COMMITTED_TRANSFORM) {
            Log::logger->log(Log::DEBUG, " > overriding transform: {} -> {}", sc<uint8_t>(rule.m_transform), sc<uint8_t>(CONFIG->transform));
            rule.m_transform = CONFIG->transform;
        }

        if (CONFIG->committedProperties & OUTPUT_HEAD_COMMITTED_SCALE) {
            Log::logger->log(Log::DEBUG, " > overriding scale: {} -> {}", sc<uint8_t>(rule.m_scale), sc<uint8_t>(CONFIG->scale));
            rule.m_scale = CONFIG->scale;
        }

        if (CONFIG->committedProperties & OUTPUT_HEAD_COMMITTED_ADAPTIVE_SYNC) {
            Log::logger->log(Log::DEBUG, " > overriding vrr: {} -> {}", rule.m_vrr.value_or(0), CONFIG->adaptiveSync);
            rule.m_vrr = sc<int>(CONFIG->adaptiveSync);
        }

        return rule;
    };

    if (PMONITOR->m_isUnsafeFallback) {
        CMonitorRule fallbackRule;
        fallbackRule.m_autoDir    = DIR_AUTO_RIGHT;
        fallbackRule.m_name       = PMONITOR->m_name;
        fallbackRule.m_resolution = Vector2D{1920, 1080};
        fallbackRule.m_offset     = Vector2D{-INT32_MAX, -INT32_MAX};
        fallbackRule.m_scale      = 1;
        return fallbackRule;
    }

    for (auto const& r : m_rules | std::views::reverse) {
        if (PMONITOR->matchesStaticSelector(r.m_name))
            return applyWlrOutputConfig(r);
    }

    Log::logger->log(Log::WARN, "No rule found for {}, trying to use the first.", PMONITOR->m_name);

    for (auto const& r : m_rules) {
        if (r.m_name.empty())
            return applyWlrOutputConfig(r);
    }

    Log::logger->log(Log::WARN, "No rules configured. Using the default hardcoded one.");

    CMonitorRule fallbackRule;
    fallbackRule.m_autoDir    = eAutoDirs::DIR_AUTO_RIGHT;
    fallbackRule.m_name       = "";
    fallbackRule.m_resolution = Vector2D{};
    fallbackRule.m_offset     = Vector2D{-INT32_MAX, -INT32_MAX};
    fallbackRule.m_scale      = -1;
    return applyWlrOutputConfig(fallbackRule);
}

const std::vector<CMonitorRule>& CMonitorRuleManager::all() {
    return m_rules;
}

std::vector<CMonitorRule>& CMonitorRuleManager::allMut() {
    return m_rules;
}

void CMonitorRuleManager::scheduleReload() {
    if (m_reloadScheduled)
        return;

    m_reloadScheduled = true;
}

void CMonitorRuleManager::ensureMonitorStatus() {
    std::vector<PHLMONITOR>       monsForRefresh;

    Hyprutils::Utils::CScopeGuard x([this] { m_events.stateReloaded.emit(); });

    for (auto const& m : State::monitorState()->allMonitors()) {
        if (!m || !m->m_output || m->m_isUnsafeFallback)
            continue;

        // report-20 issue A: an XR virtual monitor that COpenXRManager is holding UNPLUGGED (disabled)
        // is owned by the XR plug lifecycle. Its config rule says "enabled" (there is no
        // `monitor=NAME,disable` line), so applying that rule here would re-enable it (and the
        // re-enable line below would onConnect() it), re-materializing the phantom monitor the XR
        // manager just unplugged. Leave a disabled XR-managed output completely alone; the XR manager
        // re-plugs it (onConnect) on the next visibility/presence edge. Enabled XR monitors fall
        // through to normal rule application (mode/scale tweaks still apply while plugged).
        if (m->m_xrManagedPlug && !m->m_enabled)
            continue;

        // A connector a lease client currently holds ACTIVE (m_isBeingLeased — e.g. Monado direct mode
        // owns DP-5's CRTC via wp_drm_lease_v1 / VkDisplayKHR) must NEVER be reconfigured here.
        // applyMonitorRule()/onConnect() below would issue a DRM modeset/commit on a connector owned by
        // the lessee; the kernel/aquamarine commit blocks against the held lease and wedges the compositor's
        // main thread → full-system freeze (the XREAL V2.2 direct-mode hang). Leave a leased output entirely
        // alone: its active rule + standing lease offer persist, so direct mode SURVIVES a config reload that
        // dropped the runtime `lease` flag (e.g. an Omarchy theme change re-applying monitor= from the file).
        // The output only returns to desktop once the lease is actually released (Monado exits), which
        // re-drives this pass (see CDRMLeaseResource's destroy listener → scheduleReload()).
        if (m->m_isBeingLeased)
            continue;

        auto rule = get(m);

        auto cmp = rule.compare(m->m_activeMonitorRule);

        if (cmp == COMPARISON_FULL_MATCH)
            continue;

        m->m_splash = nullptr;

        monsForRefresh.emplace_back(m);

        if (cmp == COMPARISON_SOFT_MISMATCH) {
            m->applyMonitorRuleSoft(Config::CMonitorRule{rule});
            continue;
        }

        if (!m->applyMonitorRule(Config::CMonitorRule{rule})) {
            Log::logger->log(Log::ERR, "[MonitorRuleManager] failed to apply rule to {}!", m->m_name);
            continue;
        }
    }

    m_reloadScheduled = false;

    if (monsForRefresh.empty())
        return;

    for (const auto& m : monsForRefresh) {
        if (!m->m_output)
            continue;

        // Desired states for this output. m_enabled tracks "configured as a desktop". A leasable output
        // (HypXRland XREAL V2.2 `lease` flag) is neither disabled nor a desktop: it is offered to
        // drm-lease-v1 and its m_enabled stays false while it is being leased/offered.
        const bool WANT_LEASE   = m->m_activeMonitorRule.m_lease && !m->m_activeMonitorRule.m_disabled;
        const bool WANT_DESKTOP = !m->m_activeMonitorRule.m_disabled && !m->m_activeMonitorRule.m_lease;

        // If this output should no longer be leased (returning to desktop, or being disabled), withdraw any
        // standing lease offer first. reclaim() fast-returns for outputs that were never offered, so this
        // does nothing for ordinary monitors — the non-lease reconfigure path is byte-identical to stock.
        if (!WANT_LEASE) {
            for (auto& [name, lease] : PROTO::lease) {
                if (lease)
                    lease->reclaim(m);
            }
        }

        if (m->m_enabled && !WANT_DESKTOP) {
            // Leaving the desktop (disabled OR switching to lease/direct-XR): tear it down — this relocates
            // its workspaces to another monitor and frees the CRTC/connector so it can be leased.
            m->onDisconnect();
            // For a leasable output, re-enter connect so CMonitor::onConnect's lease gate offers the
            // connector to lease clients (it early-returns there without configuring a desktop).
            if (WANT_LEASE)
                m->onConnect(true);
        } else if (!m->m_enabled && WANT_DESKTOP) {
            // Becoming (or returning to) a normal desktop output. The stale lease offer, if any, was already
            // withdrawn above, so no lease client can grab the connector we are about to scan out on.
            m->onConnect(true);
        }
    }

    for (auto const& w : Desktop::windowState()->windows()) {
        w->updateSurfaceScaleTransformDetails();
    }

    State::monitorLayoutController()->arrange();
    State::monitorLayoutController()->checkOverlapsAndNotify();

    for (const auto& m : monsForRefresh) {
        if (!m->m_output)
            continue;

        g_pHyprRenderer->arrangeLayersForMonitor(m->m_id);
    }

    Event::bus()->m_events.monitor.layoutChanged.emit();
}

void CMonitorRuleManager::ensureVRR(PHLMONITOR pMonitor) {
    static auto PVRR = CConfigValue<Config::INTEGER>("misc:vrr");

    static auto ensureVRRForDisplay = [&](PHLMONITOR m) -> void {
        if (!m->m_output || m->m_createdByUser)
            return;

        const auto USEVRR = m->m_activeMonitorRule.m_vrr.has_value() ? m->m_activeMonitorRule.m_vrr.value() : *PVRR;

        if (USEVRR == 0) {
            if (m->m_vrrActive) {
                m->m_output->state->resetExplicitFences();
                m->m_output->state->setAdaptiveSync(false);

                if (!m->m_state.commit())
                    Log::logger->log(Log::ERR, "Couldn't commit output {} in ensureVRR -> false", m->m_output->name);
            }
            m->m_vrrActive = false;
            return;
        }

        const auto PWORKSPACE = m->m_activeWorkspace;

        if (USEVRR == 1) {
            bool wantVRR = true;
            if (PWORKSPACE && Fullscreen::controller()->getFullscreenModes(PWORKSPACE).internal == Fullscreen::FSMODE_FULLSCREEN)
                wantVRR = !Fullscreen::controller()->getFullscreenWindow(PWORKSPACE)->m_ruleApplicator->noVRR().valueOrDefault();

            if (wantVRR) {
                if (!m->m_vrrActive) {
                    m->m_output->state->resetExplicitFences();
                    m->m_output->state->setAdaptiveSync(true);

                    if (!m->m_state.test()) {
                        Log::logger->log(Log::DEBUG, "Pending output {} does not accept VRR.", m->m_output->name);
                        m->m_output->state->setAdaptiveSync(false);
                    }

                    if (!m->m_state.commit())
                        Log::logger->log(Log::ERR, "Couldn't commit output {} in ensureVRR -> true", m->m_output->name);
                }
                m->m_vrrActive = true;
            } else {
                if (m->m_vrrActive) {
                    m->m_output->state->resetExplicitFences();
                    m->m_output->state->setAdaptiveSync(false);

                    if (!m->m_state.commit())
                        Log::logger->log(Log::ERR, "Couldn't commit output {} in ensureVRR -> false", m->m_output->name);
                }
                m->m_vrrActive = false;
            }
            return;
        } else if (USEVRR == 2 || USEVRR == 3) {

            bool wantVRR = Fullscreen::controller()->getFullscreenModes(PWORKSPACE).internal == Fullscreen::FSMODE_FULLSCREEN;
            if (wantVRR && Fullscreen::controller()->getFullscreenWindow(PWORKSPACE)->m_ruleApplicator->noVRR().valueOrDefault())
                wantVRR = false;

            if (wantVRR && USEVRR == 3) {
                const auto contentType = Fullscreen::controller()->getFullscreenWindow(PWORKSPACE)->getContentType();
                wantVRR                = contentType == NContentType::CONTENT_TYPE_GAME || contentType == NContentType::CONTENT_TYPE_VIDEO;
            }

            if (wantVRR) {
                /* fullscreen */
                m->m_vrrActive = true;

                if (!m->m_output->state->state().adaptiveSync) {
                    m->m_output->state->setAdaptiveSync(true);

                    if (!m->m_state.test()) {
                        Log::logger->log(Log::DEBUG, "Pending output {} does not accept VRR.", m->m_output->name);
                        m->m_output->state->setAdaptiveSync(false);
                    }
                }
            } else {
                m->m_vrrActive = false;

                m->m_output->state->setAdaptiveSync(false);
            }
        }
    };

    if (pMonitor) {
        ensureVRRForDisplay(pMonitor);
        return;
    }

    for (auto const& m : State::monitorState()->monitors()) {
        ensureVRRForDisplay(m);
    }
}
