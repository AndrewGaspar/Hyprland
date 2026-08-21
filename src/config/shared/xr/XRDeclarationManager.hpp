#pragma once

#include <string>
#include <vector>

#include "../../../helpers/memory/Memory.hpp"
#include "../../../openxr/XRMonitorConfig.hpp"
#include "../../../openxr/XRRule.hpp"

namespace Config {
    // What the config DECLARED about XR: the `xrmonitor` monitors to materialize and the `xrrule`
    // transparency rules to evaluate. COpenXRManager reconciles the live world against this after
    // every reload.
    //
    // This lives out here, next to CMonitorRuleManager and CWorkspaceRuleManager, for the same
    // reason those do: there are two config front ends (the classic keyword and Lua's
    // hl.xr_monitor / hl.xr_rule) and only ONE of them exists in a given session — Config::mgr() is
    // either the legacy manager or the Lua one, never both. Hanging the declared set off the legacy
    // manager (where it started) made it invisible to a Lua config, which is exactly why the XR
    // session could not be expressed in Lua at all. A front-end-independent store fixes that
    // without either front end knowing the other exists.
    //
    // Main thread only. The XR manager COPIES out of here rather than holding a reference, so a
    // reload rebuilding these vectors can never pull the ground out from under an in-flight
    // evaluation.
    class CXRDeclarationManager {
      public:
        CXRDeclarationManager()  = default;
        ~CXRDeclarationManager() = default;

        // Called at the top of every reload, before the config is re-parsed. Both front ends
        // rebuild from scratch.
        void clear();

        // `xrmonitor` is NAME-KEYED: a later declaration of the same name replaces the earlier one
        // within a single parse (doc 05 §2). Insertion order is otherwise preserved.
        void addMonitor(SXRMonitorParams&& params);

        // `xrrule` has NO key: every rule is evaluated and CONFIG ORDER is load-bearing — a later
        // matching rule overrides an earlier one PER EFFECT (doc 05 §xrrule). Append only.
        void addRule(OpenXR::SXRRule&& rule);

        const std::vector<SXRMonitorParams>& monitors() const {
            return m_monitors;
        }

        const std::vector<OpenXR::SXRRule>& rules() const {
            return m_rules;
        }

      private:
        std::vector<SXRMonitorParams> m_monitors;
        std::vector<OpenXR::SXRRule>  m_rules;
    };

    UP<CXRDeclarationManager>& xrDeclarationMgr();
};
