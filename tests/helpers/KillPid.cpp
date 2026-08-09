// killPid() is the compositor's guard against turning a force-kill into a session kill.
//
// CWindow::getPID() returns -1 once the surface has no owner ("happens at unmap"), and
// CANRManager::SANRData::getPID() returns 0 when neither surface is set. Both were fed straight to
// kill(2), where -1 means "every process this user owns" and 0 means "this process group" — so
// pressing forcekillactive on a window that was mid-unmap, or answering an ANR dialog for a client
// that had already gone, SIGKILLed the whole session rather than the window.

#include <helpers/MiscFunctions.hpp>

#include <gtest/gtest.h>

#include <csignal>
#include <unistd.h>

TEST(Helpers, killPidRefusesBroadcasts) {
    // Signal 0 delivers nothing, so a regression cannot hurt the test runner — but it would still
    // flip these to true.
    EXPECT_FALSE(killPid(-1, 0));  // every process we may signal
    EXPECT_FALSE(killPid(0, 0));   // our own process group
    EXPECT_FALSE(killPid(-77, 0)); // process group 77

    // the two values the real accessors hand back for a client that has gone away
    EXPECT_FALSE(killPid(-1, SIGKILL));
    EXPECT_FALSE(killPid(0, SIGKILL));
}

TEST(Helpers, killPidStillSignalsOneRealProcess) {
    EXPECT_TRUE(killPid(getpid(), 0));
}
