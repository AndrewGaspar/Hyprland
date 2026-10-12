// Xwayland is respawned when it dies under a running compositor (a protocol error, a crash). The policy
// bounds that: one death costs a restart, an Xwayland that dies on every start does not respawn forever.

#include <xwayland/RestartPolicy.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(XWaylandRestartPolicy, AllowsASingleRestart) {
    CXWaylandRestartPolicy policy;
    EXPECT_TRUE(policy.allowRestart(CXWaylandRestartPolicy::clock::time_point{} + 1h));
}

TEST(XWaylandRestartPolicy, RefusesACrashLoopWithinTheWindow) {
    CXWaylandRestartPolicy policy{3, 60s};
    const auto             T0 = CXWaylandRestartPolicy::clock::time_point{} + 1h;

    EXPECT_TRUE(policy.allowRestart(T0));
    EXPECT_TRUE(policy.allowRestart(T0 + 1s));
    EXPECT_TRUE(policy.allowRestart(T0 + 2s));
    EXPECT_FALSE(policy.allowRestart(T0 + 3s));
    EXPECT_FALSE(policy.allowRestart(T0 + 59s)); // refused attempts do not extend the window
}

TEST(XWaylandRestartPolicy, ForgetsRestartsOlderThanTheWindow) {
    CXWaylandRestartPolicy policy{2, 60s};
    const auto             T0 = CXWaylandRestartPolicy::clock::time_point{} + 1h;

    EXPECT_TRUE(policy.allowRestart(T0));
    EXPECT_TRUE(policy.allowRestart(T0 + 10s));
    EXPECT_FALSE(policy.allowRestart(T0 + 30s));

    // the first restart ages out at T0 + 60s, the second at T0 + 70s
    EXPECT_TRUE(policy.allowRestart(T0 + 60s));
    EXPECT_FALSE(policy.allowRestart(T0 + 65s));
    EXPECT_TRUE(policy.allowRestart(T0 + 70s));
}

TEST(XWaylandRestartPolicy, ADeathAfterALongHealthyRunIsAlwaysRestarted) {
    CXWaylandRestartPolicy policy{1, 60s};
    const auto             T0 = CXWaylandRestartPolicy::clock::time_point{} + 1h;

    EXPECT_TRUE(policy.allowRestart(T0));
    EXPECT_FALSE(policy.allowRestart(T0 + 5s));
    EXPECT_TRUE(policy.allowRestart(T0 + 24h));
}
