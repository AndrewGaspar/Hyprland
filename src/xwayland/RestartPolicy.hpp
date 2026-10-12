#pragma once

#include <chrono>
#include <cstddef>
#include <deque>

// Decides whether a dead Xwayland may be respawned. One protocol error or crash should cost the session a
// restart, not its X11 support — but an Xwayland that dies on every start must not respawn forever.
class CXWaylandRestartPolicy {
  public:
    using clock = std::chrono::steady_clock;

    CXWaylandRestartPolicy(size_t maxRestarts = 5, std::chrono::seconds window = std::chrono::seconds{60});

    // Records a restart attempt at `now` and returns whether it is allowed: at most maxRestarts within any
    // trailing `window`. A refused attempt is not recorded.
    bool allowRestart(clock::time_point now);

  private:
    size_t                        m_maxRestarts = 5;
    std::chrono::seconds          m_window{60};
    std::deque<clock::time_point> m_recent;
};
