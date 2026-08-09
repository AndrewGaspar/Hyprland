// The structural half of the 2026-08-09 forensics, pinned.
//
// HarnessSafeKill.cpp guards what a hyprtester run may *do* (never broadcast a signal, never drive
// an instance that isn't the pid we spawned). These tests guard what a run can even *see*: with
// the isolation engaged, the developer's live session is not in the instance registry the harness
// enumerates, so the "harness picked the wrong compositor" family of bugs has nothing to pick.
//
// Everything here works on temp directories and this process's own environment. Nothing signals
// anything, and nothing touches a real runtime dir.

#include "../../hyprtester/src/RuntimeIsolation.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
    // Restores the three variables the isolation moves, whatever a test does to them.
    class CEnvGuard {
      public:
        CEnvGuard() {
            for (const auto* name : NAMES) {
                const char* v = std::getenv(name);
                m_saved.emplace_back(name, v ? std::optional<std::string>{v} : std::nullopt);
            }
        }

        ~CEnvGuard() {
            for (const auto& [name, value] : m_saved) {
                if (value)
                    setenv(name, value->c_str(), 1);
                else
                    unsetenv(name);
            }
        }

      private:
        static constexpr const char*                                    NAMES[] = {"XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE"};
        std::vector<std::pair<const char*, std::optional<std::string>>> m_saved;
    };

    // A directory shaped like a real $XDG_RUNTIME_DIR with one compositor registered in it. The
    // lock names THIS process, so Harness::scanInstances' liveness filter keeps it: as far as the
    // scan can tell, this is a live session.
    struct SFakeSession {
        std::string dir;
        std::string signature = "deadbeef_1754700000_12345";

        explicit SFakeSession(const std::string& name) {
            dir = std::filesystem::temp_directory_path() / ("hyprtester-gtest-host-" + name + "-" + std::to_string(getpid()));
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(Harness::registryDir(dir) + "/" + signature);
            std::ofstream lock(Harness::registryDir(dir) + "/" + signature + "/hyprland.lock");
            lock << getpid() << "\n"
                 << "wayland-9\n";
        }

        ~SFakeSession() {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };
}

TEST(HarnessRuntimeIsolation, registryDirIsTheDirectoryTheHarnessEnumerates) {
    // Must agree with CCompositor's own m_hyprTempDataRoot ($XDG_RUNTIME_DIR + "/hypr"), or the
    // harness and the compositor under test would disagree about where the run lives.
    EXPECT_EQ(Harness::registryDir("/run/user/1000"), "/run/user/1000/hypr");

    CEnvGuard guard;
    setenv("XDG_RUNTIME_DIR", "/run/user/4242", 1);
    EXPECT_EQ(Harness::currentRuntimeDir(), "/run/user/4242");
    EXPECT_EQ(Harness::currentRegistryDir(), "/run/user/4242/hypr");

    // No runtime dir in the environment: the documented per-uid default, never a relative path.
    unsetenv("XDG_RUNTIME_DIR");
    EXPECT_EQ(Harness::currentRuntimeDir(), "/run/user/" + std::to_string(getuid()));
}

TEST(HarnessRuntimeIsolation, isolationParentStaysInsideTheSunPathBudget) {
    // A short, usable $TMPDIR is honoured...
    const std::string SHORT = "/tmp";
    EXPECT_EQ(Harness::isolationParent(SHORT), "/tmp");

    // ...and everything else falls back to /tmp, including the case that made this a rule: the
    // real runtime dir is usable and writable, but nesting in it overflows sun_path.
    EXPECT_EQ(Harness::isolationParent(""), "/tmp");
    EXPECT_EQ(Harness::isolationParent("/run/user/1000"), "/tmp");
    EXPECT_EQ(Harness::isolationParent("/tmp/does-not-exist-" + std::to_string(getpid())), "/tmp");
    EXPECT_EQ(Harness::isolationParent("/proc/self/cmdline"), "/tmp"); // exists, but not a directory
}

// The regression that this file's first live run actually hit: a private dir nested in
// /run/user/1000 made the compositor log "Socket2 path is too long. (2) IPC will not work."
TEST(HarnessRuntimeIsolation, aPrivateRunDirLeavesRoomForTheCompositorsIpcSockets) {
    // The arithmetic must agree with EventManager's own check: instancePath + "/.socket2.sock"
    // must fit in sizeof(sockaddr_un::sun_path) - 1.
    EXPECT_EQ(Harness::worstCaseSocketPathLen("/run/user/1000/hyprtester-613568-VNmIw0"), 121u);
    EXPECT_GT(Harness::worstCaseSocketPathLen("/run/user/1000/hyprtester-613568-VNmIw0"), Harness::SUN_PATH_USABLE);

    // the baseline (a session's own runtime dir) fits, which is why this never bit before
    EXPECT_LE(Harness::worstCaseSocketPathLen("/run/user/1000"), Harness::SUN_PATH_USABLE);

    // ...and so does what we hand out
    EXPECT_LE(Harness::privateDirLen(Harness::isolationParent("")), Harness::MAX_RUNTIME_DIR_LEN);

    CEnvGuard                  guard;
    Harness::CRuntimeIsolation iso;
    ASSERT_TRUE(iso.engage());
    EXPECT_LE(iso.dir().size(), Harness::MAX_RUNTIME_DIR_LEN);
    EXPECT_LE(Harness::worstCaseSocketPathLen(iso.dir()), Harness::SUN_PATH_USABLE);
}

TEST(HarnessRuntimeIsolation, safeToRemoveRefusesAnythingWeDidNotName) {
    const std::string HOST = "/run/user/1000";

    EXPECT_TRUE(Harness::safeToRemove("/tmp/ht-123-AbCdEf", HOST));
    EXPECT_TRUE(Harness::safeToRemove("/tmp/hyprtester-xr-4242", HOST));

    // the paths a teardown bug would otherwise reach
    EXPECT_FALSE(Harness::safeToRemove(HOST, HOST)); // the session's own runtime dir
    EXPECT_FALSE(Harness::safeToRemove("/", HOST));
    EXPECT_FALSE(Harness::safeToRemove("/tmp", HOST));
    EXPECT_FALSE(Harness::safeToRemove("/run/user", HOST));
    EXPECT_FALSE(Harness::safeToRemove("", HOST));
    EXPECT_FALSE(Harness::safeToRemove("ht-123", HOST));                   // relative
    EXPECT_FALSE(Harness::safeToRemove("/tmp/ht-1/../../home/ajg", HOST)); // traversal
    EXPECT_FALSE(Harness::safeToRemove("/run/user/1000/hypr", HOST));      // the registry itself
    EXPECT_FALSE(Harness::safeToRemove("/home/ajg/code/Hyprland", HOST));  // a name we never gave
    EXPECT_FALSE(Harness::safeToRemove("/tmp/ht-1/nested", HOST));         // only the dir we made, not a child
}

// The property this whole file exists for.
TEST(HarnessRuntimeIsolation, engagedRunCannotEnumerateTheLiveSession) {
    CEnvGuard    guard;
    SFakeSession host("enumerate");

    setenv("XDG_RUNTIME_DIR", host.dir.c_str(), 1);
    setenv("HYPRLAND_INSTANCE_SIGNATURE", host.signature.c_str(), 1);
    setenv("WAYLAND_DISPLAY", "wayland-9", 1);

    // Without isolation the failure is representable: the live session IS what the harness
    // enumerates, and instances().back() is it.
    {
        const auto SEEN = Harness::scanInstances(Harness::currentRegistryDir());
        ASSERT_EQ(SEEN.size(), 1u);
        EXPECT_EQ(SEEN.back().id, host.signature);
        EXPECT_EQ(SEEN.back().pid, static_cast<uint64_t>(getpid()));
        EXPECT_EQ(SEEN.back().wlSocket, "wayland-9");
    }

    Harness::CRuntimeIsolation iso;
    ASSERT_TRUE(iso.engage());

    // ...and with it, the same call sees nothing at all. Not "sees it and skips it" — the lock
    // is not under the directory being read.
    EXPECT_TRUE(Harness::scanInstances(Harness::currentRegistryDir()).empty());
    EXPECT_NE(Harness::currentRuntimeDir(), host.dir);
    EXPECT_EQ(Harness::currentRuntimeDir(), iso.dir());
    EXPECT_TRUE(std::filesystem::is_directory(iso.dir()));

    // The registry we now read is not the one the session registered in — the whole property.
    EXPECT_NE(Harness::currentRegistryDir(), Harness::registryDir(host.dir));
    EXPECT_FALSE(iso.dir().starts_with(host.dir)); // and not nested in it either (sun_path budget)

    // Nothing a child spawns can inherit the session's identity or its socket...
    EXPECT_EQ(std::getenv("HYPRLAND_INSTANCE_SIGNATURE"), nullptr);
    EXPECT_EQ(std::getenv("WAYLAND_DISPLAY"), nullptr);
    // ...but both are kept, because ourInstance() and the --xr nested fallback need them by name.
    EXPECT_EQ(iso.hostInstanceSignature(), host.signature);
    EXPECT_EQ(iso.hostWaylandDisplay(), "wayland-9");
    EXPECT_EQ(iso.hostRuntimeDir(), host.dir);

    const std::string RUNDIR = iso.dir();
    iso.release();

    // Released: the environment is exactly as it was, and the dir is gone.
    EXPECT_FALSE(std::filesystem::exists(RUNDIR));
    EXPECT_EQ(Harness::currentRuntimeDir(), host.dir);
    ASSERT_NE(std::getenv("HYPRLAND_INSTANCE_SIGNATURE"), nullptr);
    EXPECT_EQ(std::string{std::getenv("HYPRLAND_INSTANCE_SIGNATURE")}, host.signature);
    ASSERT_NE(std::getenv("WAYLAND_DISPLAY"), nullptr);
    EXPECT_EQ(std::string{std::getenv("WAYLAND_DISPLAY")}, "wayland-9");

    // The live session survived untouched — its lock is exactly where it was.
    EXPECT_EQ(Harness::scanInstances(Harness::currentRegistryDir()).size(), 1u);
}

TEST(HarnessRuntimeIsolation, releaseRestoresVariablesThatWereNotSetToNotSet) {
    CEnvGuard    guard;
    SFakeSession host("unset");

    setenv("XDG_RUNTIME_DIR", host.dir.c_str(), 1);
    unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    unsetenv("WAYLAND_DISPLAY");

    Harness::CRuntimeIsolation iso;
    ASSERT_TRUE(iso.engage());
    EXPECT_TRUE(iso.hostInstanceSignature().empty());
    EXPECT_TRUE(iso.hostWaylandDisplay().empty());
    iso.release();

    // An unset variable comes back unset, not as an empty string — an empty WAYLAND_DISPLAY is a
    // different (and broken) state from no WAYLAND_DISPLAY.
    EXPECT_EQ(std::getenv("HYPRLAND_INSTANCE_SIGNATURE"), nullptr);
    EXPECT_EQ(std::getenv("WAYLAND_DISPLAY"), nullptr);
}

TEST(HarnessRuntimeIsolation, fixedPathIsolationIsTheSameMechanism) {
    // What --xr uses: a known name, so the container runner can collect it after a failure.
    CEnvGuard    guard;
    SFakeSession host("fixed");
    setenv("XDG_RUNTIME_DIR", host.dir.c_str(), 1);

    const std::string          FIXED = "/tmp/ht-gt-" + std::to_string(getpid());

    Harness::CRuntimeIsolation iso;
    ASSERT_TRUE(iso.engage(FIXED));
    EXPECT_EQ(iso.dir(), FIXED);
    EXPECT_EQ(Harness::currentRuntimeDir(), FIXED);
    EXPECT_LE(Harness::worstCaseSocketPathLen(FIXED), Harness::SUN_PATH_USABLE);
    EXPECT_TRUE(Harness::scanInstances(Harness::currentRegistryDir()).empty());

    // A second engage on a live object is refused rather than leaking the first dir.
    EXPECT_FALSE(iso.engage("/tmp/ht-gt-other"));
    EXPECT_EQ(iso.dir(), FIXED);

    iso.release();
    EXPECT_FALSE(std::filesystem::exists(FIXED));
}

TEST(HarnessRuntimeIsolation, preserveKeepsTheEvidenceButStillRestoresTheEnvironment) {
    CEnvGuard    guard;
    SFakeSession host("preserve");
    setenv("XDG_RUNTIME_DIR", host.dir.c_str(), 1);

    Harness::CRuntimeIsolation iso;
    ASSERT_TRUE(iso.engage());
    const std::string RUNDIR = iso.dir();

    // stand in for the compositor log a failed run leaves behind
    { std::ofstream(RUNDIR + "/hyprland.log") << "boom\n"; }

    iso.preserve();
    iso.release();

    EXPECT_TRUE(std::filesystem::exists(RUNDIR + "/hyprland.log"));
    EXPECT_EQ(Harness::currentRuntimeDir(), host.dir);

    std::error_code ec;
    std::filesystem::remove_all(RUNDIR, ec);
}

TEST(HarnessRuntimeIsolation, destructorReleasesAnUnreleasedIsolation) {
    CEnvGuard    guard;
    SFakeSession host("dtor");
    setenv("XDG_RUNTIME_DIR", host.dir.c_str(), 1);

    std::string rundir;
    {
        Harness::CRuntimeIsolation iso;
        ASSERT_TRUE(iso.engage());
        rundir = iso.dir();
        // no release() — an early `return 1` out of main() must still clean up, which is what the
        // static object in main.cpp relies on (std::exit runs static destructors too).
    }

    EXPECT_FALSE(std::filesystem::exists(rundir));
    EXPECT_EQ(Harness::currentRuntimeDir(), host.dir);
}
