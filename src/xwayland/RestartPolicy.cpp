#include "RestartPolicy.hpp"

CXWaylandRestartPolicy::CXWaylandRestartPolicy(size_t maxRestarts, std::chrono::seconds window) : m_maxRestarts(maxRestarts), m_window(window) {
    ;
}

bool CXWaylandRestartPolicy::allowRestart(clock::time_point now) {
    while (!m_recent.empty() && now - m_recent.front() >= m_window) {
        m_recent.pop_front();
    }

    if (m_recent.size() >= m_maxRestarts)
        return false;

    m_recent.emplace_back(now);
    return true;
}
