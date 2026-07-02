#pragma once

#ifdef WITH_XR_TESTS

#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/UniquePtr.hpp>

#include <string>

// Owns the monado-service process for the whole `--xr` run (started once in
// main, torn down after cleanupAndReport). See docs/openxr/06-testing.md §3.
class CMonadoOrchestrator {
  public:
    CMonadoOrchestrator();
    ~CMonadoOrchestrator();

    // Resolve the binary, create the isolated run dir, launch monado-service and
    // poll for readiness (IPC socket + TCP 4242). Returns true when the service
    // is up and accepting; false => unavailable (suite SKIPs). Never throws.
    bool start();

    // SIGTERM -> wait 3s -> SIGKILL. Removes the run dir unless keepArtifacts.
    void teardown(bool keepArtifacts);

    // True once start() has brought a service up. False means "no monado-service
    // found or it failed to become ready" => the SKIP path.
    bool available() const {
        return m_available;
    }

    // The isolated, shared XDG_RUNTIME_DIR (/tmp/hyprtester-xr-<pid>). Set as
    // XDG_RUNTIME_DIR for BOTH monado-service and the Hyprland-under-test.
    const std::string& runtimeDir() const {
        return m_runtimeDir;
    }

    // Manifest handed to Hyprland as XR_RUNTIME_JSON (<monado-build>/openxr_monado-dev.json).
    const std::string& runtimeManifest() const {
        return m_manifest;
    }

    // <run-dir>/monado.log — service stdout/stderr, captured for artifacts.
    const std::string& logPath() const {
        return m_logPath;
    }

    // The accepted TCP connection to 127.0.0.1:4242 from the readiness probe.
    // Ownership transfers to RemoteClient (docs §3.2 / §4.2). -1 if not connected.
    int takeRemoteFd() {
        const int fd = m_remoteFd;
        m_remoteFd   = -1;
        return fd;
    }

  private:
    bool resolveBinary();  // fills m_binary + m_manifest, false if not found
    bool pollReadiness();  // 100ms poll, 10s timeout; connects m_remoteFd

    Hyprutils::Memory::CUniquePointer<Hyprutils::OS::CProcess> m_proc;

    std::string m_binary;
    std::string m_manifest;
    std::string m_runtimeDir;
    std::string m_logPath;

    int         m_remoteFd  = -1;
    bool        m_available = false;
};

#endif // WITH_XR_TESTS
