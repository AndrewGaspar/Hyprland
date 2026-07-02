#pragma once
#ifdef HAVE_OPENXR

#include "../helpers/memory/Memory.hpp"

struct SHyprCtlCommand;

// Owns the "openxr" hyprctl command registration. Lives in src/openxr/ so the XR
// footprint inside HyprCtl.cpp stays at zero. All work here runs on the main thread.
class CXRIpc {
  public:
    CXRIpc();
    ~CXRIpc();

    void                registerCommands();

  private:
    SP<SHyprCtlCommand> m_openxrCommand;
};

#endif
