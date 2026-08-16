#include "../../Log.hpp"
#include "../shared.hpp"
#include "build.hpp"
#include "tests.hpp"

#include <hyprutils/os/Process.hpp>

using namespace Hyprutils::OS;

TEST_CASE(xrViewpointInactiveLifecycle) {
    CProcess client(binaryDir + "/viewpoint-inactive", {});
    client.addEnv("WAYLAND_DISPLAY", WLDISPLAY);

    ASSERT(client.runSync(), true);
    EXPECT_CONTAINS(client.stdOut(), "viewpoint inactive lifecycle ok");

    CProcess invalidCapabilities(binaryDir + "/viewpoint-inactive", {"--invalid-capabilities"});
    invalidCapabilities.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    ASSERT(invalidCapabilities.runSync(), true);
    EXPECT_CONTAINS(invalidCapabilities.stdOut(), "viewpoint invalid capabilities rejected");

    CProcess duplicate(binaryDir + "/viewpoint-inactive", {"--duplicate"});
    duplicate.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    ASSERT(duplicate.runSync(), true);
    EXPECT_CONTAINS(duplicate.stdOut(), "viewpoint duplicate rejected");

    CProcess invalidEnabled(binaryDir + "/viewpoint-inactive", {"--invalid-enabled"});
    invalidEnabled.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    ASSERT(invalidEnabled.runSync(), true);
    EXPECT_CONTAINS(invalidEnabled.stdOut(), "viewpoint invalid enabled rejected");

    // A rendered report crossing a deactivation on the wire must NOT be fatal (1805e965): the live
    // failure it caused was a doff killing the demo client's connection, taking a fullscreen window
    // with it. This case is the tolerance assertion, not a rejection one.
    CProcess inactiveRendered(binaryDir + "/viewpoint-inactive", {"--inactive-rendered"});
    inactiveRendered.addEnv("WAYLAND_DISPLAY", WLDISPLAY);
    ASSERT(inactiveRendered.runSync(), true);
    EXPECT_CONTAINS(inactiveRendered.stdOut(), "viewpoint inactive rendered tolerated");
}
