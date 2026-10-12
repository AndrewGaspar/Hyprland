#include "XWayland.hpp"
#include "../Compositor.hpp"
#include "../debug/log/Logger.hpp"
#include "../helpers/fs/FsUtils.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"

CXWayland::CXWayland(const bool wantsEnabled) {
#ifndef NO_XWAYLAND
    // Disable Xwayland and clean up if the user disabled it.
    if (!wantsEnabled) {
        Log::logger->log(Log::DEBUG, "XWayland has been disabled, cleaning up...");
        for (auto& w : Desktop::windowState()->windows()) {
            if (!w->m_isX11)
                continue;
            w->sendClose();
        }
        unsetenv("DISPLAY");
        m_enabled = false;
        return;
    }

    if (!NFsUtils::executableExistsInPath("Xwayland")) {
        // If Xwayland doesn't exist, don't try to start it.
        Log::logger->log(Log::DEBUG, "Unable to find XWayland; not starting it.");
        return;
    }

    m_enabled = startServer();
#else
    Log::logger->log(Log::DEBUG, "Not starting XWayland: disabled at compile time");
#endif
}

bool CXWayland::startServer() {
#ifndef NO_XWAYLAND
    Log::logger->log(Log::DEBUG, "Starting up the XWayland server");

    m_server = makeUnique<CXWaylandServer>();

    if (!m_server->create()) {
        Log::logger->log(Log::ERR, "XWayland failed to start: it will not work.");
        return false;
    }

    return true;
#else
    return false;
#endif
}

void CXWayland::onServerDied(const char* reason) {
#ifndef NO_XWAYLAND
    if (g_pCompositor->m_isShuttingDown || m_restartPending)
        return;

    Log::logger->log(Log::ERR, "XWayland died ({}). X11 clients cannot connect until it is restarted.", reason);

    m_restartPending = true;
    // Never tear the server down from inside one of its own callbacks.
    g_pEventLoopManager->doLater([] {
        if (g_pXWayland)
            g_pXWayland->restartServer();
    });
#endif
}

void CXWayland::restartServer() {
#ifndef NO_XWAYLAND
    m_restartPending = false;

    if (g_pCompositor->m_isShuttingDown)
        return;

    // Destroying the server closes and unlinks the dead instance's X socket and lock, so X11 clients stop
    // queueing on a listener nobody will ever accept from.
    m_wm.reset();
    m_server.reset();

    if (!m_restartPolicy.allowRestart(CXWaylandRestartPolicy::clock::now())) {
        Log::logger->log(Log::CRIT, "XWayland keeps dying; giving up on restarting it. X11 applications will not work in this session.");
        unsetenv("DISPLAY");
        m_enabled = false;
        return;
    }

    Log::logger->log(Log::WARN, "Restarting XWayland");
    m_enabled = startServer();
    if (!m_enabled)
        unsetenv("DISPLAY");
#endif
}

void CXWayland::setCursor(unsigned char* pixData, uint32_t stride, const Vector2D& size, const Vector2D& hotspot) {
#ifndef NO_XWAYLAND
    if (!m_wm) {
        Log::logger->log(Log::ERR, "Couldn't set XCursor: no XWM yet");
        return;
    }

    m_wm->setCursor(pixData, stride, size, hotspot);
#endif
}

bool CXWayland::enabled() {
    return m_enabled;
}
