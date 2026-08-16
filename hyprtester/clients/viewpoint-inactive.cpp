// Headless client for the experimental viewpoint protocol's inactive lifecycle. It proves the
// global can create an object, advertises the active SBS/pair-latched contract, reports an enable
// attempt without a negotiated layout as unsupported, and becomes inert after surface destruction.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>

#include <hypxr-viewpoint-v1.hpp>
#include <wayland-client.h>
#include <wayland.hpp>

#include <hyprutils/memory/Casts.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

using namespace Hyprutils::Memory;

struct SState {
    wl_display*                               display = nullptr;
    CSharedPointer<CCWlRegistry>              registry;
    wl_compositor*                            compositor = nullptr;
    CSharedPointer<CCHypxrViewpointManagerV1> manager;
    CSharedPointer<CCHypxrViewpointV1>        viewpoint;
    wl_surface*                               surface = nullptr;

    uint32_t                                  capabilitiesEvents = 0;
    hypxrViewpointV1Layout                    layouts            = HYPXR_VIEWPOINT_V1_LAYOUT_SBS;
    hypxrViewpointV1Capability                capabilities       = HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED;
    uint32_t                                  disabledEvents     = 0;
    uint32_t                                  unsupportedEvents  = 0;
    uint32_t                                  destroyedEvents    = 0;
};

static bool roundtrip(SState& state) {
    return wl_display_roundtrip(state.display) >= 0;
}

static bool bindGlobals(SState& state) {
    state.registry = makeShared<CCWlRegistry>(rc<wl_proxy*>(wl_display_get_registry(state.display)));
    state.registry->setGlobal([&state](CCWlRegistry* registry, uint32_t name, const char* interface, uint32_t version) {
        const std::string INTERFACE = interface;
        if (INTERFACE == "wl_compositor")
            state.compositor = sc<wl_compositor*>(wl_registry_bind(rc<wl_registry*>(registry->resource()), name, &wl_compositor_interface, std::min(version, 6U)));
        else if (INTERFACE == "hypxr_viewpoint_manager_v1")
            state.manager = makeShared<CCHypxrViewpointManagerV1>(
                rc<wl_proxy*>(wl_registry_bind(rc<wl_registry*>(registry->resource()), name, &hypxr_viewpoint_manager_v1_interface, std::min(version, 1U))));
    });
    state.registry->setGlobalRemove([](CCWlRegistry*, uint32_t) {});

    return roundtrip(state) && state.compositor && state.manager;
}

static bool createViewpoint(SState& state) {
    state.surface   = wl_compositor_create_surface(state.compositor);
    state.viewpoint = makeShared<CCHypxrViewpointV1>(state.manager->sendGetViewpoint(rc<wl_proxy*>(state.surface)));
    return state.surface && state.viewpoint && state.viewpoint->resource();
}

static bool exerciseInactiveLifecycle(SState& state) {
    if (!createViewpoint(state))
        return false;

    state.viewpoint->setCapabilities([&state](CCHypxrViewpointV1*, hypxrViewpointV1Layout layouts, hypxrViewpointV1Capability capabilities) {
        ++state.capabilitiesEvents;
        state.layouts      = layouts;
        state.capabilities = capabilities;
    });
    state.viewpoint->setInactive([&state](CCHypxrViewpointV1*, uint32_t epochHi, uint32_t epochLo, hypxrViewpointV1InactiveReason reason) {
        if (epochHi != 0 || epochLo != 0)
            return;
        if (reason == HYPXR_VIEWPOINT_V1_INACTIVE_REASON_DISABLED)
            ++state.disabledEvents;
        else if (reason == HYPXR_VIEWPOINT_V1_INACTIVE_REASON_NOT_SUPPORTED)
            ++state.unsupportedEvents;
        else if (reason == HYPXR_VIEWPOINT_V1_INACTIVE_REASON_SURFACE_DESTROYED)
            ++state.destroyedEvents;
    });

    if (!roundtrip(state) || state.capabilitiesEvents != 1 || state.layouts != HYPXR_VIEWPOINT_V1_LAYOUT_SBS || state.capabilities != HYPXR_VIEWPOINT_V1_CAPABILITY_PAIR_LATCHED)
        return false;

    state.viewpoint->sendSetEnabled(0);
    if (!roundtrip(state) || state.disabledEvents != 1)
        return false;

    state.viewpoint->sendSetEnabled(1);
    if (!roundtrip(state) || state.unsupportedEvents != 1)
        return false;

    wl_surface_destroy(state.surface);
    state.surface = nullptr;
    if (!roundtrip(state) || state.destroyedEvents != 1)
        return false;

    // Even otherwise-invalid requests become inert after wl_surface destruction.
    state.viewpoint->sendSetCapabilities(sc<hypxrViewpointV1Layout>(UINT32_MAX), sc<hypxrViewpointV1Capability>(UINT32_MAX));
    state.viewpoint->sendSetEnabled(2);
    state.viewpoint->sendRendered(0, 0, 0, 1);
    return roundtrip(state) && state.disabledEvents == 1 && state.unsupportedEvents == 1 && state.destroyedEvents == 1;
}

[[noreturn]] static void finishProtocolErrorTest(SState& state, const wl_interface& expectedInterface, uint32_t expectedCode, const char* success) {
    const wl_interface* interface = nullptr;
    uint32_t            objectId  = 0;
    const uint32_t      code      = wl_display_get_protocol_error(state.display, &interface, &objectId);
    const bool          passed =
        wl_display_get_error(state.display) == EPROTO && interface && objectId != 0 && std::strcmp(interface->name, expectedInterface.name) == 0 && code == expectedCode;

    // A protocol error makes the display unusable; exiting directly avoids generated wrapper
    // destructors attempting destructor requests on the intentionally failed connection.
    std::println("{}", passed ? success : "error: wrong protocol error");
    std::fflush(stdout);
    std::_Exit(passed ? 0 : 1);
}

[[noreturn]] static void exerciseInvalidCapabilities(SState& state) {
    if (!createViewpoint(state) || !roundtrip(state)) {
        std::println("error: invalid-capabilities setup");
        std::fflush(stdout);
        std::_Exit(1);
    }

    state.viewpoint->sendSetCapabilities(sc<hypxrViewpointV1Layout>(1U << 31), HYPXR_VIEWPOINT_V1_CAPABILITY_NONE);
    if (roundtrip(state)) {
        std::println("error: invalid capabilities accepted");
        std::fflush(stdout);
        std::_Exit(1);
    }

    finishProtocolErrorTest(state, hypxr_viewpoint_v1_interface, HYPXR_VIEWPOINT_V1_ERROR_INVALID_CAPABILITIES, "viewpoint invalid capabilities rejected");
}

[[noreturn]] static void exerciseDuplicate(SState& state) {
    if (!createViewpoint(state) || !roundtrip(state)) {
        std::println("error: duplicate setup");
        std::fflush(stdout);
        std::_Exit(1);
    }

    state.manager->sendGetViewpoint(rc<wl_proxy*>(state.surface));
    if (roundtrip(state)) {
        std::println("error: duplicate viewpoint accepted");
        std::fflush(stdout);
        std::_Exit(1);
    }

    finishProtocolErrorTest(state, hypxr_viewpoint_manager_v1_interface, HYPXR_VIEWPOINT_MANAGER_V1_ERROR_ALREADY_CONSTRUCTED, "viewpoint duplicate rejected");
}

[[noreturn]] static void exerciseInvalidEnabled(SState& state) {
    if (!createViewpoint(state) || !roundtrip(state)) {
        std::println("error: invalid-enabled setup");
        std::fflush(stdout);
        std::_Exit(1);
    }

    state.viewpoint->sendSetEnabled(2);

    if (roundtrip(state)) {
        std::println("error: invalid state accepted");
        std::fflush(stdout);
        std::_Exit(1);
    }

    finishProtocolErrorTest(state, hypxr_viewpoint_v1_interface, HYPXR_VIEWPOINT_V1_ERROR_INVALID_STATE, "viewpoint invalid enabled rejected");
}

// A `rendered` report that arrives while the viewpoint is inactive — or for an epoch that is no
// longer current — is the benign wire race, NOT a protocol violation: deactivation is an
// asynchronous compositor->client event, so a report the client had already finished and queued can
// always cross it. The compositor answered that with a fatal error until 1805e965, and the live
// consequence was severe: doffing the headset deactivated the viewpoint (reason xr_inactive) while
// the demo's last report was in flight, the connection was killed, and a fullscreen window simply
// vanished from the user's desktop. This case pins the tolerant contract, in the shape the doff
// actually produced.
[[noreturn]] static void exerciseInactiveRendered(SState& state) {
    if (!createViewpoint(state) || !roundtrip(state)) {
        std::println("error: inactive-rendered setup");
        std::fflush(stdout);
        std::_Exit(1);
    }

    state.viewpoint->sendRendered(0, 0, 0, 1); // epoch 0: reported after a deactivation
    state.viewpoint->sendRendered(0, 7, 0, 3); // an epoch the compositor never issued to this client

    const bool alive = roundtrip(state) && wl_display_get_error(state.display) == 0;

    // _Exit for the same reason the rejection cases use it: the generated wrappers would otherwise
    // run destructor requests during teardown, which is noise on a connection whose whole point was
    // to prove nothing killed it.
    std::println("{}", alive ? "viewpoint inactive rendered tolerated" : "error: inactive rendered killed the connection");
    std::fflush(stdout);
    std::_Exit(alive ? 0 : 1);
}

int main(int argc, char** argv) {
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        std::println("error: display");
        return 1;
    }

    bool passed = false;
    {
        SState state{.display = display};
        if (!bindGlobals(state)) {
            std::println("error: globals");
            std::fflush(stdout);
            std::_Exit(1);
        }

        if (argc == 2 && std::strcmp(argv[1], "--invalid-capabilities") == 0)
            exerciseInvalidCapabilities(state);
        if (argc == 2 && std::strcmp(argv[1], "--duplicate") == 0)
            exerciseDuplicate(state);
        if (argc == 2 && std::strcmp(argv[1], "--invalid-enabled") == 0)
            exerciseInvalidEnabled(state);
        if (argc == 2 && std::strcmp(argv[1], "--inactive-rendered") == 0)
            exerciseInactiveRendered(state);

        passed = exerciseInactiveLifecycle(state);

        if (state.viewpoint)
            state.viewpoint->sendDestroy();
        if (state.manager)
            state.manager->sendDestroy();
        if (state.surface)
            wl_surface_destroy(state.surface);
        if (state.compositor)
            wl_compositor_destroy(state.compositor);
    }

    wl_display_disconnect(display);
    std::println("{}", passed ? "viewpoint inactive lifecycle ok" : "error: viewpoint inactive lifecycle");
    return passed ? 0 : 1;
}
