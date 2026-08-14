#include "XRViewpointProtocol.hpp"

#include "core/Compositor.hpp"
#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;

static constexpr uint32_t KNOWN_LAYOUTS      = HYPXR_VIEWPOINT_V1_LAYOUT_SBS | HYPXR_VIEWPOINT_V1_LAYOUT_HSBS | HYPXR_VIEWPOINT_V1_LAYOUT_TAB | HYPXR_VIEWPOINT_V1_LAYOUT_HTAB;
static constexpr uint32_t KNOWN_CAPABILITIES = HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED | HYPXR_VIEWPOINT_V1_CAPABILITY_MONOTONIC_TIMESTAMPS;

CXRViewpointResource::CXRViewpointResource(UP<CHypxrViewpointV1>&& resource, SP<CWLSurfaceResource> surface) : m_resource(std::move(resource)), m_surface(surface) {
    if UNLIKELY (!m_resource->resource())
        return;

    m_resource->setDestroy([this](CHypxrViewpointV1*) { destroy(); });
    m_resource->setOnDestroy([this](CHypxrViewpointV1*) { destroy(); });

    m_resource->setSetCapabilities([this](CHypxrViewpointV1* resource, hypxrViewpointV1Layout layouts, hypxrViewpointV1Capability capabilities) {
        if (!m_surface)
            return;

        if ((sc<uint32_t>(layouts) & ~KNOWN_LAYOUTS) != 0 || (sc<uint32_t>(capabilities) & ~KNOWN_CAPABILITIES) != 0)
            resource->error(HYPXR_VIEWPOINT_V1_ERROR_INVALID_CAPABILITIES, "set_capabilities contains unknown layout or capability bits");
    });
    m_resource->setSetEnabled([this](CHypxrViewpointV1* resource, uint32_t enabled) {
        if (!m_surface)
            return;

        if (enabled > 1) {
            resource->error(HYPXR_VIEWPOINT_V1_ERROR_INVALID_STATE, "set_enabled accepts only zero or one");
            return;
        }

        resource->sendInactive(0, 0, enabled ? HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_SUPPORTED : HYPXR_VIEWPOINT_V1_INACTIVE_REASON_DISABLED);
    });
    m_resource->setRendered([this](CHypxrViewpointV1* resource, uint32_t, uint32_t, uint32_t, uint32_t) {
        if (!m_surface)
            return;

        resource->error(HYPXR_VIEWPOINT_V1_ERROR_INVALID_STATE, "rendered called while viewpoint feedback is inactive");
    });

    m_listeners.surfaceDestroyed = m_surface->m_events.destroy.listen([this] { surfaceDestroyed(); });

    // Stage one is intentionally negotiation-only. Zero masks are the honest capability set until
    // authorization, sample transport, and per-commit latching land in later commits.
    m_resource->sendCapabilities(HYPXR_VIEWPOINT_V1_LAYOUT_NONE, HYPXR_VIEWPOINT_V1_CAPABILITY_NONE);
}

bool CXRViewpointResource::good() const {
    return m_resource->resource();
}

void CXRViewpointResource::destroy() {
    PROTO::xrViewpoint->destroyViewpoint(this);
}

void CXRViewpointResource::surfaceDestroyed() {
    if (!m_surface)
        return;

    m_surface.reset();
    m_resource->sendInactive(0, 0, HYPXR_VIEWPOINT_V1_INACTIVE_REASON_SURFACE_DESTROYED);
}

CXRViewpointProtocol::CXRViewpointProtocol(const wl_interface* iface, const int& ver, const std::string& name) : IWaylandProtocol(iface, ver, name) {
    ;
}

void CXRViewpointProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_managers.emplace_back(makeUnique<CHypxrViewpointManagerV1>(client, ver, id)).get();

    if UNLIKELY (!RESOURCE->resource()) {
        wl_client_post_no_memory(client);
        m_managers.pop_back();
        return;
    }

    RESOURCE->setDestroy([this](CHypxrViewpointManagerV1* manager) { destroyManager(manager); });
    RESOURCE->setOnDestroy([this](CHypxrViewpointManagerV1* manager) { destroyManager(manager); });
    RESOURCE->setGetViewpoint([this](CHypxrViewpointManagerV1* manager, uint32_t id, wl_resource* surface) { getViewpoint(manager, id, surface); });
}

void CXRViewpointProtocol::getViewpoint(CHypxrViewpointManagerV1* manager, uint32_t id, wl_resource* surfaceResource) {
    if (!surfaceResource) {
        manager->error(HYPXR_VIEWPOINT_MANAGER_V1_ERROR_INVALID_SURFACE, "get_viewpoint called without a wl_surface");
        return;
    }

    const auto SURFACE = CWLSurfaceResource::fromResource(surfaceResource);
    if (!SURFACE || SURFACE->client() != manager->client()) {
        manager->error(HYPXR_VIEWPOINT_MANAGER_V1_ERROR_INVALID_SURFACE, "get_viewpoint called with an invalid or foreign wl_surface");
        return;
    }

    if (m_viewpoints.contains(SURFACE)) {
        manager->error(HYPXR_VIEWPOINT_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED, "the wl_surface already has a viewpoint object");
        return;
    }

    const auto VIEWPOINT =
        m_viewpoints.emplace(SURFACE, makeUnique<CXRViewpointResource>(makeUnique<CHypxrViewpointV1>(manager->client(), manager->version(), id), SURFACE)).first->second.get();

    if LIKELY (VIEWPOINT->good())
        return;

    manager->noMemory();
    m_viewpoints.erase(SURFACE);
}

void CXRViewpointProtocol::destroyManager(CHypxrViewpointManagerV1* manager) {
    std::erase_if(m_managers, [manager](const auto& other) { return other.get() == manager; });
}

void CXRViewpointProtocol::destroyViewpoint(CXRViewpointResource* viewpoint) {
    std::erase_if(m_viewpoints, [viewpoint](const auto& entry) { return entry.second.get() == viewpoint; });
}
