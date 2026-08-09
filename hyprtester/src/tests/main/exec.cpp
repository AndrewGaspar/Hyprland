#include "tests.hpp"
#include "../../shared.hpp"
#include "../../hyprctlCompat.hpp"
#include <chrono>
#include <format>
#include <thread>
#include <hyprutils/os/Process.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include "../shared.hpp"

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;

#define UP CUniquePointer
#define SP CSharedPointer

const static auto SLEEP_DURATIONS = std::array{1, 10};

namespace {
    // `exec_cmd` is fire-and-forget: the dispatcher answers "ok" the moment CExecutor::spawnRawProc
    // has forked, and the child still has to exec /bin/sh and let sh exec into `sleep` before a
    // process by that name exists. Reading pgrep in the same breath as the reply is therefore a
    // race — one the developer's idle box happens to win and the (nested, loaded) test container
    // loses EVERY time: pgrep came up empty for both durations there, which reads exactly like
    // "spawning is broken". Poll instead of assuming. Genuinely broken spawning still fails the
    // test: nothing ever shows up and we give in after the whole budget.
    constexpr auto SPAWN_BUDGET = std::chrono::seconds(5);

    // Number of pids pgrep printed, and the first one. Two `sleep`s at once is ambiguous (we cannot
    // tell which is ours), so the caller keeps waiting for the field to clear rather than guessing;
    // std::stoull alone would silently take the first of them.
    size_t parsePids(const std::string& out, pid_t& firstOut) {
        size_t count = 0;
        for (size_t pos = 0; pos < out.size();) {
            const auto end = out.find('\n', pos);
            const auto len = (end == std::string::npos ? out.size() : end) - pos;
            if (len > 0) {
                try {
                    const pid_t pid = (pid_t)std::stoull(out.substr(pos, len));
                    if (count++ == 0)
                        firstOut = pid;
                } catch (...) { /* not a pid line — ignore */ }
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        return count;
    }

    // The pid of the one running `sleep`, or -1 if exactly one never shows up within the budget.
    // `-x` so the pattern is matched against the WHOLE process name (plain `pgrep sleep` also
    // matches, say, a `sleepd`).
    pid_t waitForTheSleep(std::chrono::milliseconds budget) {
        const auto  start    = std::chrono::steady_clock::now();
        const auto  deadline = start + budget;
        std::string out;
        while (true) {
            out             = Tests::execAndGet("pgrep -x sleep");
            pid_t      pid  = -1;
            const auto PIDS = parsePids(out, pid);
            if (PIDS == 1) {
                const auto MS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                NLog::log("{}sleep spawned as pid {}, visible {}ms after the dispatch returned", Colors::YELLOW, pid, MS);
                return pid;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        NLog::log("{}Sleep was not spawned or several sleeps are running after {}ms: pgrep returned '{}'", Colors::RED, budget.count(), out);
        return -1;
    }

    // GONE, not merely "done sleeping": Safe::pidAlive answers true for a zombie, which is the exact
    // leak this is here to catch, so the child has to disappear from the process table (be reaped by
    // the compositor) rather than just stop running.
    bool waitUntilReaped(pid_t pid, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (Tests::processAlive(pid)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    }
}

TEST_CASE(processSpawning) {
    for (const auto duration : SLEEP_DURATIONS) {
        // Note: POSIX sleep does not support fractional seconds, so
        // can't sleep for less than 1 second.
        OK(getFromSocket(std::format("/dispatch hl.dsp.exec_cmd('sleep {}')", duration)));

        // Ensure that sleep is our child
        const pid_t sleepPid = waitForTheSleep(SPAWN_BUDGET);
        if (sleepPid < 0)
            continue;

        const std::string sleepPidS       = std::to_string(sleepPid);
        const std::string sleepParentComm = Tests::execAndGet("cat \"/proc/$(ps -o ppid:1= -p " + sleepPidS + ")/comm\"");
        NLog::log("{}Expecting that sleep's parent is Hyprland", Colors::YELLOW);
        EXPECT_CONTAINS(sleepParentComm, "Hyprland");

        // Ensure that sleep did not become a zombie. The budget is the sleep itself plus slack: the
        // child exits on its own schedule, and waking at exactly `duration` is another race.
        EXPECT(waitUntilReaped(sleepPid, std::chrono::seconds(duration) + std::chrono::seconds(5)), true);

        // Test succeeded
        return;
    }

    FAIL_TEST_SILENT();
}
