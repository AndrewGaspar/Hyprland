#include "SessionLockManager.hpp"
#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"
#include "../protocols/FractionalScale.hpp"
#include "../protocols/SessionLock.hpp"
#include "../render/Renderer.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/SessionLock.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/input/InputManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../state/MonitorState.hpp"
#include <algorithm>
#include <ranges>

SSessionLockSurface::SSessionLockSurface(SP<CSessionLockSurface> surface_) : surface(surface_) {
    pWlrSurface = surface->surface();

    listeners.map = surface_->m_events.map.listen([this] {
        mapped = true;

        g_pInputManager->simulateMouseMovement();

        // A lock surface that maps while nothing holds keyboard focus has to take it. The pointer
        // pass above only assigns focus when the cursor happens to sit on a monitor whose lock
        // surface is ALREADY mapped, and on a multi-output plug (the XR quads arrive as one batch)
        // it usually is not — which leaves the lock with no keyboard focus, i.e. eating every key.
        g_pSessionLockManager->refocusKeyboard();

        const auto PMONITOR = State::monitorState()->query().id(iMonitorID).run();

        if (PMONITOR)
            g_pHyprRenderer->damageMonitor(PMONITOR);
    });

    listeners.destroy = surface_->m_events.destroy.listen([this] {
        // NOTE: no focus clear here, on purpose. focusState()->surface() hands back a temporary, so
        // the .reset() that used to stand here cleared nothing — and removeSessionLockSurface()'s
        // "focus is already gone" gate then read the dying surface as a live focus and skipped the
        // handoff entirely. It drops us from the list before deciding instead, so the handoff only
        // ever considers surfaces that still exist.
        g_pSessionLockManager->removeSessionLockSurface(this);
    });

    listeners.commit = surface_->m_events.commit.listen([this] {
        const auto PMONITOR = State::monitorState()->query().id(iMonitorID).run();

        if (mapped && !Desktop::focusState()->surface()) {
            g_pInputManager->simulateMouseMovement();

            // ...and if the pointer pass still could not place focus (the cursor sits on a monitor
            // whose lock surface is not up yet), place it here rather than waiting for a motion
            // that may never come — under a lock the user has no reason to move the mouse.
            g_pSessionLockManager->refocusKeyboard();
        }

        if (PMONITOR)
            g_pHyprRenderer->damageMonitor(PMONITOR);
    });
}

CSessionLockManager::CSessionLockManager() {
    m_listeners.newLock = PROTO::sessionLock->m_events.newLock.listen([this](const auto& lock) { this->onNewSessionLock(lock); });
}

void CSessionLockManager::onNewSessionLock(SP<CSessionLock> pLock) {
    static auto PALLOWRELOCK = CConfigValue<Config::INTEGER>("misc:allow_session_lock_restore");

    if (PROTO::sessionLock->isLocked() && !*PALLOWRELOCK && g_pCompositor->m_startLockedCommand.empty()) {
        LOGM(Log::DEBUG, "Cannot re-lock, misc:allow_session_lock_restore is disabled");
        pLock->sendDenied();
        return;
    }

    if (m_sessionLock && !clientDenied() && !clientLocked())
        return; // Not allowing to relock in case the old lock is still in a limbo

    LOGM(Log::DEBUG, "Session got locked by {:x}", (uintptr_t)pLock.get());

    m_sessionLock       = makeUnique<SSessionLock>();
    m_sessionLock->lock = pLock;
    m_sessionLock->lockTimer.reset();

    m_sessionLock->listeners.newSurface = pLock->m_events.newLockSurface.listen([this](const SP<CSessionLockSurface>& surface) {
        const auto PMONITOR = surface->monitor();

        const auto NEWSURFACE  = m_sessionLock->vSessionLockSurfaces.emplace_back(makeShared<SSessionLockSurface>(surface));
        NEWSURFACE->iMonitorID = PMONITOR->m_id;
        PROTO::fractional->sendScale(surface->surface(), PMONITOR->m_scale);

        NEWSURFACE->view = Desktop::View::CSessionLock::create(surface);
    });

    m_sessionLock->listeners.unlock = pLock->m_events.unlockAndDestroy.listen([this] {
        m_events.unlock.emit();

        m_sessionLock.reset();
        g_pInputManager->refocus();

        for (auto const& m : State::monitorState()->monitors())
            g_pHyprRenderer->damageMonitor(m);
    });

    m_sessionLock->listeners.destroy = pLock->m_events.destroyed.listen([this] {
        m_sessionLock.reset();
        Desktop::focusState()->rawSurfaceFocus(nullptr);

        for (auto const& m : State::monitorState()->monitors())
            g_pHyprRenderer->damageMonitor(m);
    });

    m_events.lock.emit();

    Desktop::focusState()->rawSurfaceFocus(nullptr);
    g_pSeatManager->setGrab(nullptr);

    const bool NOACTIVEMONS = std::ranges::all_of(State::monitorState()->monitors(), [](const auto& m) { return !m->m_enabled || !m->m_dpmsStatus; });

    if (NOACTIVEMONS) {
        // Normally the locked event is sent after each output rendered a lock screen frame.
        // When there are no active outputs, send it right away.
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
        return;
    }

    m_sessionLock->sendLockedTimer = makeShared<CEventLoopTimer>(
        // Clients get sent the "locked" event after they submitted a lock frame for each output.
        // If they fail to do this, we send the "locked" event after a fixed amount of time here.
        // Previously we sent denied after this timeout, but that forcefully makes the client exit and the protocol doesn't require that anyways.
        std::chrono::seconds(5),
        [](auto, auto) {
            if (!g_pSessionLockManager || g_pSessionLockManager->clientLocked() || g_pSessionLockManager->clientDenied())
                return;

            if (!g_pSessionLockManager->m_sessionLock || !g_pSessionLockManager->m_sessionLock->lock)
                return;

            LOGM(Log::WARN,
                 "Sending locked after a 5 second timeout. This happens when we failed to render a lock frame from the client for every output. Lockdead frames may be shown.");
            g_pSessionLockManager->m_sessionLock->lock->sendLocked();
            g_pSessionLockManager->m_sessionLock->hasSentLocked = true;
        },
        nullptr);

    g_pEventLoopManager->addTimer(m_sessionLock->sendLockedTimer);
}

void CSessionLockManager::removeSendLockedTimer() {
    if (!m_sessionLock || !m_sessionLock->sendLockedTimer)
        return;

    g_pEventLoopManager->removeTimer(m_sessionLock->sendLockedTimer);
    m_sessionLock->sendLockedTimer.reset();
}

bool CSessionLockManager::isSessionLocked() {
    return PROTO::sessionLock->isLocked();
}

WP<SSessionLockSurface> CSessionLockManager::getSessionLockSurfaceForMonitor(uint64_t id) {
    if (!m_sessionLock)
        return {};

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (sls->iMonitorID == id) {
            if (sls->mapped)
                return sls;
            else
                return {};
        }
    }

    return {};
}

void CSessionLockManager::onLockscreenRenderedOnMonitor(uint64_t id) {
    if (!m_sessionLock || m_sessionLock->hasSentLocked || m_sessionLock->hasSentDenied)
        return;

    m_sessionLock->lockedMonitors.emplace(id);
    const bool LOCKED =
        std::ranges::all_of(State::monitorState()->monitors(), [this](auto m) { return !m->m_enabled || !m->m_dpmsStatus || m_sessionLock->lockedMonitors.contains(m->m_id); });

    if (LOCKED && m_sessionLock->lock->good()) {
        removeSendLockedTimer();
        m_sessionLock->lock->sendLocked();
        m_sessionLock->hasSentLocked = true;
    }
}

bool CSessionLockManager::isSurfaceSessionLock(SP<CWLSurfaceResource> pSurface) {
    // TODO: this has some edge cases when it's wrong (e.g. destroyed lock but not yet surfaces)
    // but can be easily fixed when I rewrite wlr_surface

    if (!m_sessionLock)
        return false;

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        if (sls->surface->surface() == pSurface)
            return true;
    }

    return false;
}

bool CSessionLockManager::anySessionLockSurfacesPresent() {
    return m_sessionLock && std::ranges::any_of(m_sessionLock->vSessionLockSurfaces, [](const auto& surf) { return surf->mapped; });
}

std::optional<size_t> SessionLock::pickFocusCandidate(const std::vector<SFocusCandidate>& candidates, std::optional<uint64_t> preferredMonitorID) {
    std::optional<size_t> fallback;

    for (size_t i = 0; i < candidates.size(); ++i) {
        const auto& CANDIDATE = candidates[i];

        if (!CANDIDATE.mapped || !CANDIDATE.focusable)
            continue;

        if (preferredMonitorID && CANDIDATE.monitorID == *preferredMonitorID)
            return i;

        if (!fallback)
            fallback = i;
    }

    return fallback;
}

// Make sure SOME live lock surface holds keyboard focus. A no-op when one already does, so it is
// safe to call from every event that can invalidate the answer: a surface mapping, a surface being
// removed, or a wl_surface dying under a live role object.
void CSessionLockManager::refocusKeyboard() {
    if (!m_sessionLock || !isSessionLocked())
        return;

    // Somebody live still holds it: a surface dying elsewhere must not yank focus away from the one
    // the user is typing into. This asks the LIST, not the focus state — a surface that is going
    // away has been dropped from the list before we run, so it no longer counts as a live holder,
    // which is the whole point (focusState() cannot tell us "this surface is about to die").
    const auto CURRENT = Desktop::focusState()->surface();
    if (CURRENT && std::ranges::any_of(m_sessionLock->vSessionLockSurfaces, [&CURRENT](const auto& sls) { return sls->surface && sls->surface->surface() == CURRENT; }))
        return;

    std::vector<SessionLock::SFocusCandidate> candidates;
    candidates.reserve(m_sessionLock->vSessionLockSurfaces.size());

    for (auto const& sls : m_sessionLock->vSessionLockSurfaces) {
        candidates.emplace_back(SessionLock::SFocusCandidate{
            .monitorID = sc<uint64_t>(sls->iMonitorID),
            .mapped    = sls->mapped,
            .focusable = sls->surface && sls->surface->surface(),
        });
    }

    const auto PMONITOR = Desktop::focusState()->monitor();
    const auto PICK     = SessionLock::pickFocusCandidate(candidates, PMONITOR ? std::optional<uint64_t>{sc<uint64_t>(PMONITOR->m_id)} : std::nullopt);

    if (!PICK)
        return;

    Desktop::focusState()->rawSurfaceFocus(m_sessionLock->vSessionLockSurfaces[*PICK]->surface->surface());
}

void CSessionLockManager::removeSessionLockSurface(SSessionLockSurface* pSLS) {
    if (!m_sessionLock)
        return;

    std::erase_if(m_sessionLock->vSessionLockSurfaces, [&](const auto& other) { return pSLS == other.get(); });

    // Drop first, decide after: a lock whose focused surface just went away has to hand focus to
    // one that survived, or every key for the rest of the session goes nowhere.
    refocusKeyboard();
}

bool CSessionLockManager::clientLocked() {
    return m_sessionLock && m_sessionLock->hasSentLocked;
}

bool CSessionLockManager::clientDenied() {
    return m_sessionLock && m_sessionLock->hasSentDenied;
}

void CSessionLockManager::clearSessionLock() {
    m_events.unlock.emit();
    m_sessionLock = {};
}

void CSessionLockManager::forceUnlock() {
    PROTO::sessionLock->forceUnlock();
    clearSessionLock();

    Desktop::focusState()->rawSurfaceFocus(nullptr);
    for (auto const& m : State::monitorState()->monitors())
        g_pHyprRenderer->damageMonitor(m);

    g_pInputManager->refocus();
}

void CSessionLockManager::forceLock() {
    PROTO::sessionLock->forceLock();
    m_events.lock.emit();
}

bool CSessionLockManager::shallConsiderLockMissing() {
    if (!m_sessionLock)
        return true;

    static auto LOCKDEAD_SCREEN_DELAY = CConfigValue<Config::INTEGER>("misc:lockdead_screen_delay");

    return m_sessionLock->lockTimer.getMillis() > *LOCKDEAD_SCREEN_DELAY;
}
