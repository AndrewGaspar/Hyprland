#pragma once

#include <unordered_map>
#include <vector>

#include "WaylandProtocol.hpp"
#include "hypxr-viewpoint-v1.hpp"
#include "../helpers/signal/Signal.hpp"

class CWLSurfaceResource;

class CXRViewpointResource {
  public:
    CXRViewpointResource(UP<CHypxrViewpointV1>&& resource, SP<CWLSurfaceResource> surface);

    bool good() const;

  private:
    void                   destroy();
    void                   surfaceDestroyed();

    UP<CHypxrViewpointV1>  m_resource;
    WP<CWLSurfaceResource> m_surface;

    struct {
        CHyprSignalListener surfaceDestroyed;
    } m_listeners;

    friend class CXRViewpointProtocol;
};

class CXRViewpointProtocol : public IWaylandProtocol {
  public:
    CXRViewpointProtocol(const wl_interface* iface, const int& ver, const std::string& name);

    virtual void bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id);

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
