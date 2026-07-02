// Minimal ext-idle-notify-v1 client for the OpenXR integration test suite (WP12,
// docs/openxr/06-testing.md §6 row 8). Binds wl_seat + ext_idle_notifier_v1, requests one
// idle notification with a 1000ms timeout that obeys inhibitors, and prints "idled"/"resumed"
// lines to stdout (line-buffered, flushed immediately) as they arrive. No window is needed —
// idle notification is a seat-wide concept, not tied to surface focus.
//
// The test harness spawns this as a plain child process and polls its stdout pipe for those
// lines (see hyprtester/src/tests/xr/idle.cpp); there is no query/response protocol here (unlike
// pointer-scroll's stdin-driven queries) since idled/resumed are asynchronous, unsolicited
// events.

#include <sys/poll.h>
#include <print>
#include <string>

#include <wayland-client.h>
#include <wayland.hpp>
#include <ext-idle-notify-v1.hpp>

#include <hyprutils/memory/SharedPtr.hpp>

using namespace Hyprutils::Memory;

struct SWlState {
    wl_display*                            display = nullptr;
    CSharedPointer<CCWlRegistry>           registry;
    CSharedPointer<CCWlSeat>               wlSeat;
    CSharedPointer<CCExtIdleNotifierV1>    idleNotifier;
    CSharedPointer<CCExtIdleNotificationV1> notification;
};

static bool shouldExit = false;

template <typename... Args>
//NOLINTNEXTLINE
static void clientLog(std::format_string<Args...> fmt, Args&&... args) {
    std::println("{}", std::format(fmt, std::forward<Args>(args)...));
    std::fflush(stdout);
}

static bool bindRegistry(SWlState& state) {
    state.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(state.display));

    state.registry->setGlobal([&](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        const std::string NAME = name;
        if (NAME == "wl_seat")
            state.wlSeat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &wl_seat_interface, 1));
        else if (NAME == "ext_idle_notifier_v1")
            state.idleNotifier = makeShared<CCExtIdleNotifierV1>((wl_proxy*)wl_registry_bind((wl_registry*)state.registry->resource(), id, &ext_idle_notifier_v1_interface, 1));
    });
    state.registry->setGlobalRemove([](CCWlRegistry* r, uint32_t id) {});

    wl_display_roundtrip(state.display);

    if (!state.wlSeat || !state.idleNotifier) {
        clientLog("Failed to get wl_seat/ext_idle_notifier_v1 from Hyprland");
        return false;
    }

    return true;
}

int main(int argc, char** argv) {
    SWlState state;

    state.display = wl_display_connect(nullptr);
    if (!state.display) {
        clientLog("Failed to connect to wayland display");
        return -1;
    }

    if (!bindRegistry(state))
        return -1;

    // 1000ms timeout, obeying inhibitors (docs §6 row 8's spec). get_idle_notification (not
    // get_input_idle_notification) is the obeys-inhibitors request, matching hypridle's own
    // default usage per docs/openxr/05-ipc-config.md §7.3.
    state.notification = makeShared<CCExtIdleNotificationV1>(state.idleNotifier->sendGetIdleNotification(1000, state.wlSeat->resource()));
    if (!state.notification->resource()) {
        clientLog("Failed to create idle notification");
        return -1;
    }
    state.notification->setIdled([](CCExtIdleNotificationV1* p) { clientLog("idled"); });
    state.notification->setResumed([](CCExtIdleNotificationV1* p) { clientLog("resumed"); });

    clientLog("started");

    struct pollfd fds[1] = {{.fd = wl_display_get_fd(state.display), .events = POLLIN}};
    while (!shouldExit && poll(fds, 1, 50) != -1) {
        wl_display_flush(state.display);

        if (fds[0].revents & POLLIN) {
            if (wl_display_prepare_read(state.display) == 0) {
                wl_display_read_events(state.display);
                wl_display_dispatch_pending(state.display);
            } else
                wl_display_dispatch(state.display);
        }

        int ret = 0;
        do {
            ret = wl_display_dispatch_pending(state.display);
            wl_display_flush(state.display);
        } while (ret > 0);
    }

    wl_display* display = state.display;
    state               = {};
    wl_display_disconnect(display);
    return 0;
}
