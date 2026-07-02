#pragma once

#ifdef WITH_XR_TESTS

#include "monado_remote_wire.hpp"

#include <chrono>
#include <functional>

// TCP 4242 client speaking the Monado remote-driver wire protocol (docs §4).
//
// Usage is read-modify-write-one-struct-per-tick: the client holds a full
// device snapshot (`m_data`); setters mutate it; pulse() sets the header and
// writes the whole struct. There are no deltas.
//
// With P_OVERRIDE_ACTIVE_CONFIG=remote the devices enumerate as Valve Index
// controllers + an HMD.
class CRemoteClient {
  public:
    enum eSide : uint8_t {
        SIDE_LEFT = 0,
        SIDE_RIGHT,
    };

    CRemoteClient() = default;
    ~CRemoteClient();

    // Takes over the accepted TCP socket from the orchestrator (docs §4.2).
    // Reads one full r_remote_data, validates magic + size. Returns false on ANY
    // mismatch => the whole suite must SKIP (ABI drift is a re-pin task, not a
    // Hyprland regression). Also false on a hard socket error.
    bool connectAndValidate(int fd);

    bool connected() const {
        return m_fd >= 0 && m_valid;
    }

    // --- setters (mutate the snapshot only; no I/O) ---
    void setHeadPose(MonadoWire::xrt_vec3 pos, MonadoWire::xrt_quat q);
    void setControllerPose(eSide s, MonadoWire::xrt_vec3 pos, MonadoWire::xrt_quat q);
    void setControllerActive(eSide s, bool active);
    void setTrigger(eSide s, float v);   // trigger_value.x (+ trigger_click at >=0.9)
    void setSqueeze(eSide s, float v);   // squeeze_value.x
    void setThumbstick(eSide s, float x, float y);
    void setHandCurl(eSide s, float curl);

    // header = R_HEADER_VALUE; write the whole struct. The only setter that does I/O.
    bool pulse();

    // Interpolate over [dur], calling pulse() at ~hz. The workhorse for scripted
    // motion (leash-spring / grab tests).
    void animate(std::function<void(MonadoWire::r_remote_data&, float t01)> f, std::chrono::milliseconds dur, int hz = 60);

    MonadoWire::r_remote_data&                    data() {
        return m_data;
    }

  private:
    MonadoWire::r_remote_controller_data&         ctrl(eSide s) {
        return s == SIDE_LEFT ? m_data.left : m_data.right;
    }

    bool readFull(void* buf, size_t len);
    bool writeFull(const void* buf, size_t len);

    int                       m_fd    = -1;
    bool                      m_valid = false;
    MonadoWire::r_remote_data m_data  = {};
};

#endif // WITH_XR_TESTS
