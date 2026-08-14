#pragma once

#include <unordered_map>
#include <vector>
#include <optional>

#include "WaylandProtocol.hpp"
#include "hypxr-viewpoint-v1.hpp"
#include "../openxr/XRViewpointLedger.hpp"
#include "../openxr/XRViewpointTransport.hpp"
#include "../helpers/signal/Signal.hpp"

class CWLSurfaceResource;
class CWLSubsurfaceResource;

class CXRViewpointResource {
  public:
    CXRViewpointResource(UP<CHypxrViewpointV1>&& resource, SP<CWLSurfaceResource> surface);

    bool                   good() const;

    SP<CWLSurfaceResource> surface() const;
    bool                   requested() const;
    bool                   enabled() const;
    bool                   subsurfaceTreeObservable() const;
    uint64_t               token() const;

    uint64_t               activate(uint64_t token, uint64_t geometryId, uint32_t widthUM, uint32_t heightUM);
    void                   invalidate(hypxrViewpointV1InactiveReason reason);
    void                   sendSample(uint64_t token, const OpenXR::SXRViewpointEncodedSample& sample);

  private:
    void destroy();
    void surfaceDestroyed();
    void watchSubsurface(SP<CWLSubsurfaceResource> subsurface, size_t depth);

    struct SSubsurfaceWatch {
        WP<CWLSubsurfaceResource> subsurface;
        CHyprSignalListener       subsurfaceDestroyed;
        CHyprSignalListener       surfaceDestroyed;
        CHyprSignalListener       map;
        CHyprSignalListener       unmap;
        CHyprSignalListener       newSubsurface;
    };

    UP<CHypxrViewpointV1>                         m_resource;
    WP<CWLSurfaceResource>                        m_surface;
    uint32_t                                      m_layouts                  = 0;
    uint32_t                                      m_capabilities             = 0;
    bool                                          m_enabled                  = false;
    uint64_t                                      m_epochCounter             = 0;
    uint64_t                                      m_epoch                    = 0;
    uint64_t                                      m_geometryId               = 0;
    uint64_t                                      m_token                    = 0;
    bool                                          m_subsurfaceTreeObservable = true;
    std::optional<hypxrViewpointV1InactiveReason> m_lastInactiveReason;
    OpenXR::CXRViewpointIssuedSampleLedger        m_issuedSamples;
    std::vector<UP<SSubsurfaceWatch>>             m_subsurfaceWatches;

    struct {
        CHyprSignalListener surfaceDestroyed;
        CHyprSignalListener map;
        CHyprSignalListener unmap;
        CHyprSignalListener stateCommit;
        CHyprSignalListener newSubsurface;
    } m_listeners;

    friend class CXRViewpointProtocol;
};

class CXRViewpointProtocol : public IWaylandProtocol {
  public:
    CXRViewpointProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

    void         forEachViewpoint(const std::function<void(CXRViewpointResource&)>& fn);
    void         deliverSample(uint64_t token, const OpenXR::SXRViewpointEncodedSample& sample);

  private:
    void                                                                 getViewpoint(CHypxrViewpointManagerV1* manager, uint32_t id, wl_resource* surfaceResource);
    void                                                                 destroyManager(CHypxrViewpointManagerV1* manager);
    void                                                                 destroyViewpoint(CXRViewpointResource* viewpoint);

    std::vector<UP<CHypxrViewpointManagerV1>>                            m_managers;
    std::unordered_map<WP<CWLSurfaceResource>, UP<CXRViewpointResource>> m_viewpoints;

    friend class CXRViewpointResource;
};

namespace PROTO {
    inline UP<CXRViewpointProtocol> xrViewpoint;
};
