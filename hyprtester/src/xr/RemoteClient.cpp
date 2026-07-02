#include "RemoteClient.hpp"

#ifdef WITH_XR_TESTS

#include "../Log.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>

using namespace MonadoWire;

CRemoteClient::~CRemoteClient() {
    if (m_fd >= 0)
        close(m_fd);
}

bool CRemoteClient::readFull(void* buf, size_t len) {
    auto*  p    = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = recv(m_fd, p + done, len - done, 0);
        if (n == 0)
            return false; // peer closed
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

bool CRemoteClient::writeFull(const void* buf, size_t len) {
    const auto* p    = static_cast<const uint8_t*>(buf);
    size_t      done = 0;
    while (done < len) {
        const ssize_t n = send(m_fd, p + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

bool CRemoteClient::connectAndValidate(int fd) {
    m_fd = fd;
    if (m_fd < 0)
        return false;

    // Give the read a bounded timeout: the hub sends its current state to a new
    // connection (docs §4.2 step 2). If it doesn't within the window, fall back
    // to a zeroed template validated via the write+read path (step 3).
    timeval tv{.tv_sec = 3, .tv_usec = 0};
    setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    r_remote_data pkt = {};
    if (readFull(&pkt, sizeof(pkt))) {
        if (pkt.header != R_HEADER_VALUE) {
            NLog::yellow("RemoteClient: wire header mismatch (got {:#018x}, expected {:#018x}) — ABI drift", pkt.header, R_HEADER_VALUE);
            m_valid = false;
            return false;
        }
        m_data  = pkt; // keep as current-state template
        m_valid = true;
        NLog::green("RemoteClient: validated remote wire (header + {} byte struct)", sizeof(r_remote_data));
        return true;
    }

    // Fallback (step 3): the hub did not push an initial snapshot. Validate the
    // ABI by writing one struct and reading one back.
    NLog::yellow("RemoteClient: no initial snapshot; validating via write+read");
    m_data        = {};
    m_data.header = R_HEADER_VALUE;
    if (!writeFull(&m_data, sizeof(m_data))) {
        NLog::red("RemoteClient: failed to write handshake struct");
        m_valid = false;
        return false;
    }
    if (readFull(&pkt, sizeof(pkt)) && pkt.header == R_HEADER_VALUE) {
        m_data  = pkt;
        m_valid = true;
        NLog::green("RemoteClient: validated remote wire via write+read");
        return true;
    }

    NLog::yellow("RemoteClient: could not validate remote wire — ABI mismatch, suite will SKIP");
    m_valid = false;
    return false;
}

void CRemoteClient::setHeadPose(xrt_vec3 pos, xrt_quat q) {
    m_data.head.center.position    = pos;
    m_data.head.center.orientation = q;
    m_data.head.per_view_data_valid = false;
}

void CRemoteClient::setControllerPose(eSide s, xrt_vec3 pos, xrt_quat q) {
    ctrl(s).pose.position    = pos;
    ctrl(s).pose.orientation = q;
}

void CRemoteClient::setControllerActive(eSide s, bool active) {
    ctrl(s).active = active;
}

void CRemoteClient::setTrigger(eSide s, float v) {
    ctrl(s).trigger_value.x = v;
    ctrl(s).trigger_click   = v >= 0.9f;
}

void CRemoteClient::setSqueeze(eSide s, float v) {
    ctrl(s).squeeze_value.x = v;
    ctrl(s).squeeze_force.x = v;
}

void CRemoteClient::setThumbstick(eSide s, float x, float y) {
    ctrl(s).thumbstick.x = x;
    ctrl(s).thumbstick.y = y;
}

void CRemoteClient::setHandCurl(eSide s, float curl) {
    for (float& c : ctrl(s).hand_curl)
        c = curl;
    ctrl(s).hand_tracking_active = true;
}

bool CRemoteClient::pulse() {
    if (m_fd < 0)
        return false;
    m_data.header = R_HEADER_VALUE;
    return writeFull(&m_data, sizeof(m_data));
}

void CRemoteClient::animate(std::function<void(r_remote_data&, float t01)> f, std::chrono::milliseconds dur, int hz) {
    if (hz <= 0)
        hz = 60;
    const auto  start = std::chrono::steady_clock::now();
    const auto  step  = std::chrono::milliseconds(1000 / hz);
    const float durMs = static_cast<float>(dur.count());

    while (true) {
        const auto  now = std::chrono::steady_clock::now();
        const float el  = static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        const float t01 = durMs > 0.f ? std::min(el / durMs, 1.f) : 1.f;

        f(m_data, t01);
        pulse();

        if (t01 >= 1.f)
            break;
        std::this_thread::sleep_for(step);
    }
}

#endif // WITH_XR_TESTS
