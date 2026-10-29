#pragma once
#ifdef HAVE_OPENXR

#include "../helpers/memory/Memory.hpp"

#include <string>

namespace IPC::Socket1 {
    struct SCommand;
}

namespace OpenXR {
    // Post an `event>>data` line on socket2. A no-op until the compositor's event loop exists and
    // after socket2 is torn down -- the same window 0.56's `if (g_pEventManager)` guards covered,
    // which is what keeps the gtests (no compositor) from instantiating the socket. Main thread only.
    void postIPCEvent(std::string event, std::string data);
}

// Owns the "openxr" hyprctl command registration. Lives in src/openxr/ so the XR
// footprint inside src/ipc/s1/Commands.cpp stays at zero. All work here runs on the main thread.
class CXRIpc {
  public:
    CXRIpc();
    ~CXRIpc();

    void registerCommands();

  private:
    SP<IPC::Socket1::SCommand> m_openxrCommand;
};

#endif
