#pragma once

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <print>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

// One choke point for every signal hyprtester sends, and for every "is it still alive?" probe.
//
// kill(2) reads a non-positive pid as a BROADCAST: -1 signals every process the invoking user is
// permitted to signal, 0 signals the caller's entire process group, and < -1 signals a process
// group. The harness sources its pids from two places that legitimately hand back -1:
//
//   * Hyprland's `/layers` reply. CLayerSurface::getPID() (src/desktop/view/LayerSurface.cpp)
//     returns -1 for a layer surface whose wl_client is already gone, which is routine while
//     surfaces are being torn down — killAllLayers()'s own comment records having seen exactly
//     that ("we'll end up with layers with pid -1 if they are all removed at the same time").
//   * CProcess::pid(), which is -1 until a spawn has succeeded.
//
// Feeding either straight into kill() turns a test-harness cleanup into `kill(-1, SIGTERM)`: a
// SIGTERM to the developer's whole login session. That is not hypothetical. On 2026-08-09 it took
// down a live Hyprland desktop twice, ~12s into a hyprtester run each time — uwsm, dbus-broker,
// the compositor, the session's daemons and the harness's own shell all caught SIGTERM inside the
// same millisecond, out of band from systemd (a systemd-driven stop is ordered and staggered; a
// single kill(-1) is not).
//
// Nothing in hyprtester ever wants a broadcast, so refusing one costs nothing and removes the
// entire failure class.
namespace Safe {
    // True when `pid` names one specific process we may signal. Anything else (a broadcast, a
    // process group, an unstarted CProcess) is refused.
    inline bool validPid(pid_t pid) {
        return pid > 0;
    }

    // Send `sig` to exactly one process. Returns true if the signal was delivered.
    inline bool signalPid(pid_t pid, int sig) {
        if (!validPid(pid)) {
            std::println(stderr, "[ hyprtester ] refusing to send signal {} to pid {}: a non-positive pid is a BROADCAST, not a process", sig, static_cast<long long>(pid));
            return false;
        }
        return ::kill(pid, sig) == 0;
    }

    // Liveness probe. False for any pid we would refuse to signal, so a bogus pid never reads as
    // "alive" (the raw `kill(pid, 0) == 0` form answers true for both 0 and -1).
    inline bool pidAlive(pid_t pid) {
        if (!validPid(pid))
            return false;
        errno = 0;
        return ::kill(pid, 0) == 0 || errno != ESRCH;
    }

    // Every pid in an IPC reply, in wire order, for entries introduced by `key` (e.g. "pid: " in
    // `/layers`). Values that do not parse come back as -1 rather than being silently dropped, so
    // the caller always hands the result to signalPid() and the guard above is what decides.
    //
    // The reply is Hyprland's own text format: `key` is followed by an optionally signed decimal
    // that ends at the first character that cannot continue it.
    inline std::vector<pid_t> pidsFromReply(const std::string& reply, std::string_view key) {
        std::vector<pid_t> out;
        if (key.empty())
            return out;

        for (auto pos = reply.find(key); pos != std::string::npos; pos = reply.find(key, pos + key.size())) {
            const auto  VALSTART = pos + key.size();
            std::size_t end      = VALSTART;
            if (end < reply.size() && (reply[end] == '-' || reply[end] == '+'))
                ++end;
            const auto DIGITSTART = end;
            while (end < reply.size() && reply[end] >= '0' && reply[end] <= '9')
                ++end;

            if (end == DIGITSTART) { // no digits at all
                out.push_back(-1);
                continue;
            }

            try {
                out.push_back(static_cast<pid_t>(std::stoll(reply.substr(VALSTART, end - VALSTART))));
            } catch (...) { out.push_back(-1); } // out of range for a pid_t: not a process either
        }
        return out;
    }
}
