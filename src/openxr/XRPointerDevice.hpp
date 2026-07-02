#pragma once
#ifdef HAVE_OPENXR

// CXRPointerDevice — the synthetic ray pointer (docs/openxr/04-input.md §8). An IPointer
// subclass registered via g_pInputManager->newMouse so ray-quad hits become ordinary Hyprland
// pointer input: motionAbsolute (bound to the hit XR monitor), button, axis, frame. Modeled
// directly on hyprtester's CTestMouse — the proven synthetic-device pattern. It carries no
// backend device (aq() == nullptr), so setupMouse's libinput branch is skipped, and — being
// attached through CPointerManager::attachPointer like a real mouse — it resets idle timers for
// free (doc 04 §11) with zero explicit onActivity() calls in src/openxr/.
//
// Lives entirely on the MAIN thread: created/destroyed by COpenXRManager on session start/stop
// and openxr:pointer toggles, driven by the frame->main queue drain (dispatchInputEvent).

#include "../devices/IPointer.hpp"

class CXRPointerDevice : public IPointer {
  public:
    static SP<CXRPointerDevice> create() {
        auto p          = SP<CXRPointerDevice>(new CXRPointerDevice());
        p->m_self       = p;
        p->m_deviceName = "xr-pointer";
        p->m_hlName     = "xr-pointer"; // overwritten by setupMouse via getNameForNewDevice
        return p;
    }

    virtual bool isVirtual() {
        return false; // mirrors CTestMouse: keeps per-device config (device[xr-pointer]{...})
    }

    virtual SP<Aquamarine::IPointer> aq() {
        return nullptr; // no backend device; setupMouse's libinput branch is guarded off
    }

    void destroy() {
        m_events.destroy.emit();
    }

  private:
    CXRPointerDevice() = default;
};

#endif // HAVE_OPENXR
