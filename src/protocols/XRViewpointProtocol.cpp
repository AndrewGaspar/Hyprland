#include "XRViewpointProtocol.hpp"

#include "core/Compositor.hpp"
#include "core/Subcompositor.hpp"
#include "../openxr/XRViewpointEligibility.hpp"
#ifdef HAVE_OPENXR
#include "../openxr/OpenXRManager.hpp"
#endif
#include <hyprutils/memory/Casts.hpp>

using namespace Hyprutils::Memory;

static constexpr uint32_t KNOWN_LAYOUTS      = HYPXR_VIEWPOINT_V1_LAYOUT_SBS | HYPXR_VIEWPOINT_V1_LAYOUT_HSBS | HYPXR_VIEWPOINT_V1_LAYOUT_TAB | HYPXR_VIEWPOINT_V1_LAYOUT_HTAB;
static constexpr uint32_t KNOWN_CAPABILITIES = HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED | HYPXR_VIEWPOINT_V1_CAPABILITY_MONOTONIC_TIMESTAMPS;
static constexpr uint32_t AVAILABLE_LAYOUTS  = HYPXR_VIEWPOINT_V1_LAYOUT_SBS;
static constexpr uint32_t AVAILABLE_CAPS     = HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED;

static void               requestReevaluation() {
#ifdef HAVE_OPENXR
    if (g_pOpenXRManager)
        g_pOpenXRManager->requestEffectEval();
#endif
}

CXRViewpointResource::CXRViewpointResource(UP<CHypxrViewpointV1>&& resource, SP<CWLSurfaceResource> surface) : m_resource(std::move(resource)), m_surface(surface) {
    if UNLIKELY (!m_resource->resource())
        return;

    m_resource->setDestroy([this](CHypxrViewpointV1*) { destroy(); });
    m_resource->setOnDestroy([this](CHypxrViewpointV1*) { destroy(); });

    m_resource->setSetCapabilities([this](CHypxrViewpointV1* resource, hypxrViewpointV1Layout layouts, hypxrViewpointV1Capability capabilities) {
        if (!m_surface)
            return;

        if ((sc<uint32_t>(layouts) & ~KNOWN_LAYOUTS) != 0 || (sc<uint32_t>(capabilities) & ~KNOWN_CAPABILITIES) != 0) {
            resource->error(HYPXR_VIEWPOINT_V1_ERROR_INVALID_CAPABILITIES, "set_capabilities contains unknown layout or capability bits");
            return;
        }

        m_layouts      = sc<uint32_t>(layouts);
        m_capabilities = sc<uint32_t>(capabilities);
        if (m_epoch != 0)
            invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
        requestReevaluation();
    });
    m_resource->setSetEnabled([this](CHypxrViewpointV1* resource, uint32_t enabled) {
        if (!m_surface)
            return;

        if (enabled > 1) {
            resource->error(HYPXR_VIEWPOINT_V1_ERROR_INVALID_STATE, "set_enabled accepts only zero or one");
            return;
        }

        m_enabled = enabled != 0;
        if (!m_enabled)
            invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_DISABLED);
        else
            requestReevaluation();
    });
    m_resource->setRendered([this](CHypxrViewpointV1* resource, uint32_t epochHi, uint32_t epochLo, uint32_t sampleHi, uint32_t sampleLo) {
        if (!m_surface)
            return;

        const uint64_t EPOCH  = OpenXR::joinViewpointU64({.hi = epochHi, .lo = epochLo});
        const uint64_t SAMPLE = OpenXR::joinViewpointU64({.hi = sampleHi, .lo = sampleLo});
        if (OpenXR::viewpointRenderedDisposition(m_epoch, EPOCH) == OpenXR::eXRViewpointRenderedDisposition::XR_VIEWPOINT_RENDERED_IGNORE_STALE)
            return; // an in-flight report crossed a deactivation on the wire; the client could not have known
        if (!m_issuedSamples.consume(SAMPLE)) {
            resource->error(HYPXR_VIEWPOINT_V1_ERROR_INVALID_SAMPLE, "rendered references an unknown or already consumed sample for the current epoch");
            return;
        }

        m_surface->stageViewpointAssociation({.epoch = EPOCH, .sample = SAMPLE});
    });

    m_listeners.surfaceDestroyed = m_surface->m_events.destroy.listen([this] { surfaceDestroyed(); });
    m_listeners.map              = m_surface->m_events.map.listen([] { requestReevaluation(); });
    m_listeners.unmap            = m_surface->m_events.unmap.listen([this] {
        invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);
        requestReevaluation();
    });
    m_listeners.stateCommit      = m_surface->m_events.stateCommit.listen([this](WP<SSurfaceState> state) {
        if (!state)
            return;
        if (OpenXR::viewpointSurfaceStateRequiresReevaluation(state->updated.bits.transform, state->updated.bits.scale, state->updated.bits.viewport, state->updated.bits.offset,
                                                              state->updated.bits.buffer && m_surface && state->bufferSize != m_surface->m_current.bufferSize))
            requestReevaluation();
    });
    m_listeners.newSubsurface    = m_surface->m_events.newSubsurface.listen([this](const auto& subsurface) {
        watchSubsurface(subsurface, 0);
        requestReevaluation();
    });
    for (const auto& subsurface : m_surface->m_subsurfaces) {
        if (subsurface)
            watchSubsurface(subsurface.lock(), 0);
    }

#ifdef HAVE_OPENXR
    m_resource->sendCapabilities(sc<hypxrViewpointV1Layout>(AVAILABLE_LAYOUTS), sc<hypxrViewpointV1Capability>(AVAILABLE_CAPS));
#else
    m_resource->sendCapabilities(HYPXR_VIEWPOINT_V1_LAYOUT_NONE, HYPXR_VIEWPOINT_V1_CAPABILITY_NONE);
#endif
}

bool CXRViewpointResource::good() const {
    return m_resource->resource();
}

SP<CWLSurfaceResource> CXRViewpointResource::surface() const {
    return m_surface.lock();
}

bool CXRViewpointResource::requested() const {
    return m_surface && m_enabled && (m_layouts & AVAILABLE_LAYOUTS) != 0 && (m_capabilities & AVAILABLE_CAPS) == AVAILABLE_CAPS;
}

bool CXRViewpointResource::enabled() const {
    return m_enabled;
}

bool CXRViewpointResource::subsurfaceTreeObservable() const {
    return m_subsurfaceTreeObservable;
}

uint64_t CXRViewpointResource::token() const {
    return m_token;
}

uint64_t CXRViewpointResource::activate(uint64_t token, uint64_t geometryId, uint32_t widthUM, uint32_t heightUM) {
    if (!requested() || token == 0 || geometryId == 0 || widthUM == 0 || heightUM == 0)
        return 0;

    if (m_epoch != 0 && m_token == token && m_geometryId == geometryId)
        return m_epoch;

    if (m_epoch != 0)
        invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_ELIGIBLE);

    uint64_t next = 0;
    if (!OpenXR::nextViewpointEpoch(m_epochCounter, next))
        return 0;

    m_epochCounter = next;
    m_epoch        = next;
    m_geometryId   = geometryId;
    m_token        = token;
    m_issuedSamples.clear();
    m_lastInactiveReason.reset();

    const auto EPOCH    = OpenXR::splitViewpointU64(m_epoch);
    const auto GEOMETRY = OpenXR::splitViewpointU64(m_geometryId);
    m_resource->sendActive(EPOCH.hi, EPOCH.lo, GEOMETRY.hi, GEOMETRY.lo, widthUM, heightUM, HYPXR_VIEWPOINT_V1_LAYOUT_SBS, HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED);
    return m_epoch;
}

void CXRViewpointResource::invalidate(hypxrViewpointV1InactiveReason reason) {
    if (m_epoch == 0 && m_lastInactiveReason == reason)
        return;

    const auto EPOCH = OpenXR::splitViewpointU64(m_epoch);
    m_resource->sendInactive(EPOCH.hi, EPOCH.lo, reason);
    if (m_surface && m_epoch != 0)
        m_surface->clearViewpointAssociations(m_epoch);
    m_epoch      = 0;
    m_geometryId = 0;
    m_token      = 0;
    m_issuedSamples.clear();
    m_lastInactiveReason = reason;
}

void CXRViewpointResource::sendSample(uint64_t token, const OpenXR::SXRViewpointEncodedSample& sample) {
    if (m_epoch == 0 || token == 0 || token != m_token || OpenXR::joinViewpointU64(sample.geometryId) != m_geometryId ||
        !m_issuedSamples.issue(OpenXR::joinViewpointU64(sample.serial)))
        return;

    const auto  EPOCH = OpenXR::splitViewpointU64(m_epoch);
    const auto& LEFT  = sample.viewPositions[0];
    const auto& RIGHT = sample.viewPositions[1];
    m_resource->sendSample(EPOCH.hi, EPOCH.lo, sample.serial.hi, sample.serial.lo, sample.geometryId.hi, sample.geometryId.lo, 0, 0, 0, 0, LEFT.x, LEFT.y, LEFT.z, RIGHT.x, RIGHT.y,
                           RIGHT.z, sample.viewCount, HYPXR_VIEWPOINT_V1_SAMPLE_FLAG_POSITIONS_VALID);
}

void CXRViewpointResource::destroy() {
    if (m_surface && m_epoch != 0)
        m_surface->clearViewpointAssociations(m_epoch);
    m_epoch      = 0;
    m_geometryId = 0;
    m_token      = 0;
    m_issuedSamples.clear();
    requestReevaluation();
    PROTO::xrViewpoint->destroyViewpoint(this);
}

void CXRViewpointResource::surfaceDestroyed() {
    if (!m_surface)
        return;

    invalidate(HYPXR_VIEWPOINT_V1_INACTIVE_REASON_SURFACE_DESTROYED);
    m_subsurfaceWatches.clear();
    m_surface.reset();
}

void CXRViewpointResource::watchSubsurface(SP<CWLSubsurfaceResource> subsurface, size_t depth) {
    if (!subsurface || !subsurface->m_surface)
        return;

    std::erase_if(m_subsurfaceWatches, [](const auto& watch) { return !watch->subsurface; });
    if (std::ranges::any_of(m_subsurfaceWatches, [&](const auto& watch) { return watch->subsurface == subsurface; }))
        return;

    if (!OpenXR::viewpointSubsurfaceWatchWithinBudget(depth, m_subsurfaceWatches.size())) {
        m_subsurfaceTreeObservable = false;
        return;
    }

    const auto SURFACE = subsurface->m_surface.lock();
    if (!SURFACE)
        return;

    auto watch                 = makeUnique<SSubsurfaceWatch>();
    watch->subsurface          = subsurface;
    watch->subsurfaceDestroyed = subsurface->m_events.destroy.listen([] { requestReevaluation(); });
    watch->surfaceDestroyed    = SURFACE->m_events.destroy.listen([] { requestReevaluation(); });
    watch->map                 = SURFACE->m_events.map.listen([] { requestReevaluation(); });
    watch->unmap               = SURFACE->m_events.unmap.listen([] { requestReevaluation(); });
    watch->newSubsurface       = SURFACE->m_events.newSubsurface.listen([this, depth](const auto& child) {
        watchSubsurface(child, depth + 1);
        requestReevaluation();
    });
    m_subsurfaceWatches.emplace_back(std::move(watch));

    for (const auto& child : SURFACE->m_subsurfaces) {
        if (child)
            watchSubsurface(child.lock(), depth + 1);
    }
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

void CXRViewpointProtocol::forEachViewpoint(const std::function<void(CXRViewpointResource&)>& fn) {
    for (auto& [surface, viewpoint] : m_viewpoints) {
        if (viewpoint)
            fn(*viewpoint);
    }
}

void CXRViewpointProtocol::deliverSample(uint64_t token, const OpenXR::SXRViewpointEncodedSample& sample) {
    forEachViewpoint([&](CXRViewpointResource& viewpoint) {
        if (viewpoint.token() == token)
            viewpoint.sendSample(token, sample);
    });
}
