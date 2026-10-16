#include "XRPlaneSource.hpp"

#include <format>

namespace OpenXR {
    void IXRPlaneSource::invalidate() {
        ;
    }

    void IXRPlaneSource::shutdown(bool) {
        ;
    }

    void CXRSyntheticPlaneSource::invalidate() {
        m_dirty = true;
    }

    bool CXRSyntheticPlaneSource::setSpec(const std::string& spec) {
        if (spec == m_spec && m_error.empty())
            return true;
        m_spec   = spec;
        m_dirty  = true;
        auto res = xrParseSyntheticPlanes(spec);
        if (!res) {
            m_error = res.error();
            m_planes.clear();
            return false;
        }
        m_error  = "";
        m_planes = std::move(*res);
        ++m_generation;
        return true;
    }

    size_t CXRSyntheticPlaneSource::planeCount() const {
        return m_planes.size();
    }

    const char* CXRSyntheticPlaneSource::name() const {
        return "synthetic";
    }

    eXRPlanePoll CXRSyntheticPlaneSource::poll(const SXRPlanePollContext& ctx, std::vector<SXRPlane>& out) {
        // Re-located every poll, by construction: the declared coordinates ARE the current LOCAL_FLOOR,
        // so the only thing that can move them is the LOCAL fallback's floor shift.
        if (!m_dirty && ctx.floorShift == m_lastShift)
            return XR_PLANE_POLL_UNCHANGED;
        m_dirty     = false;
        m_lastShift = ctx.floorShift;
        out         = m_planes;
        for (auto& p : out) {
            p.pose.pos.y += ctx.floorShift;
            p.generation = m_generation;
        }
        return XR_PLANE_POLL_UPDATED;
    }

    std::string CXRSyntheticPlaneSource::status() const {
        if (!m_error.empty())
            return std::format("invalid openxr:surfaces_synthetic — {}", m_error);
        if (m_planes.empty())
            return "none declared";
        return std::format("{} declared plane(s)", m_planes.size());
    }

    bool CXRSurfaceSources::setSyntheticSpec(const std::string& spec) {
        return m_synthetic.setSpec(spec);
    }

    void CXRSurfaceSources::setRuntimeSource(UP<IXRPlaneSource>&& src) {
        m_runtime = std::move(src);
        m_runtimePlanes.clear();
        m_dirty = true;
    }

    void CXRSurfaceSources::detachRuntime(bool runtimeLost) {
        if (m_runtime)
            m_runtime->shutdown(runtimeLost);
        setRuntimeSource(nullptr);
    }

    void CXRSurfaceSources::invalidate() {
        m_synthetic.invalidate();
        if (m_runtime)
            m_runtime->invalidate();
        m_dirty = true;
    }

    void CXRSurfaceSources::clear() {
        m_synthPlanes.clear();
        m_runtimePlanes.clear();
        m_merged = SXRPlaneSnapshot{};
        invalidate();
    }

    bool CXRSurfaceSources::poll(const SXRPlanePollContext& ctx, SXRPlaneSnapshot& out) {
        bool changed = m_dirty;
        m_dirty      = false;

        std::vector<SXRPlane> fresh;
        switch (m_synthetic.poll(ctx, fresh)) {
            case XR_PLANE_POLL_UPDATED:
                m_synthPlanes = std::move(fresh);
                changed       = true;
                break;
            case XR_PLANE_POLL_UNAVAILABLE:
                changed = changed || !m_synthPlanes.empty();
                m_synthPlanes.clear();
                break;
            case XR_PLANE_POLL_UNCHANGED: break;
        }
        fresh.clear();
        switch (m_runtime ? m_runtime->poll(ctx, fresh) : XR_PLANE_POLL_UNAVAILABLE) {
            case XR_PLANE_POLL_UPDATED:
                m_runtimePlanes = std::move(fresh);
                changed         = true;
                break;
            case XR_PLANE_POLL_UNAVAILABLE:
                changed = changed || !m_runtimePlanes.empty();
                m_runtimePlanes.clear();
                break;
            case XR_PLANE_POLL_UNCHANGED: break;
        }
        if (!changed)
            return false;

        SXRPlaneSnapshot s;
        for (const auto* list : {&m_synthPlanes, &m_runtimePlanes}) {
            for (const auto& p : *list) {
                if (s.n >= XR_PLANE_MAX)
                    break;
                s.planes[s.n++] = p;
            }
        }
        s.generation = ++m_generation;
        m_merged     = s;
        out          = s;
        return true;
    }

    const SXRPlaneSnapshot& CXRSurfaceSources::merged() const {
        return m_merged;
    }

    std::string CXRSurfaceSources::syntheticStatus() const {
        return m_synthetic.status();
    }

    std::string CXRSurfaceSources::runtimeStatus() const {
        return m_runtime ? std::format("{}: {}", m_runtime->name(), m_runtime->status()) : std::string("none (no session)");
    }
}
