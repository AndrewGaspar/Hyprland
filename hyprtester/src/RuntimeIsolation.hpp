#pragma once

// Where a hyprtester run's compositor state lives — and why it must not be where the developer's
// session keeps its own.
//
// Every piece of Hyprland discovery is keyed off $XDG_RUNTIME_DIR: a compositor registers itself
// as $XDG_RUNTIME_DIR/hypr/<signature>/ (lock file + IPC sockets + log), and the harness
// enumerates exactly that directory to find "the instance under test". Run the suite from inside
// a live Hyprland session with the session's runtime dir and both compositors sit in the same
// registry — so a compositor that dies before registering, a slow lock write, or any future
// "pick the newest instance" shortcut points every getFromSocket(), every window/layer kill and
// the closing `/dispatch exit` at the developer's desktop. That is not hypothetical: on
// 2026-08-09 a run selected instances().back() and drove the live session with it.
//
// SafeKill.hpp is the guard half of the fix (refuse the signals that incident sent, match our
// instance by pid). This is the structural half: give the run its own $XDG_RUNTIME_DIR so the
// live session is not in the registry the harness reads at all, and the failure class stops
// being representable rather than merely being caught.
//
// `--xr` has always done this — it needs an isolated-but-shared runtime dir for Monado's IPC
// socket — so this file is that mechanism factored out of runXrSuite() and handed to the
// ordinary path too. Both paths now differ only in where the directory lives and how loudly its
// name is documented.

#include "SafeKill.hpp"
#include "hyprctlCompat.hpp" // SInstanceData

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace Harness {

    inline std::string envOr(const char* name, const std::string& fallback = "") {
        const char* v = std::getenv(name);
        return v ? std::string{v} : fallback;
    }

    // $XDG_RUNTIME_DIR as it reads RIGHT NOW — deliberately not cached, because engaging the
    // isolation below changes it mid-process and every later lookup must follow.
    inline std::string currentRuntimeDir() {
        if (const auto XDG = envOr("XDG_RUNTIME_DIR"); !XDG.empty())
            return XDG;
        return "/run/user/" + std::to_string(getuid());
    }

    // The instance registry: the one directory whose contents decide which compositor the harness
    // talks to. Isolation is exactly the statement "ours is not the session's".
    inline std::string registryDir(std::string_view runtimeDir) {
        return std::string{runtimeDir} + "/hypr";
    }

    inline std::string currentRegistryDir() {
        return registryDir(currentRuntimeDir());
    }

    // Every live compositor registered under `registry`. Split out of instances() so the isolation
    // property is testable: point this at an isolated dir and the session's lock is not merely
    // ignored, it is not enumerated.
    inline std::vector<SInstanceData> scanInstances(const std::string& registry) {
        std::vector<SInstanceData> result;

        try {
            if (!std::filesystem::exists(registry))
                return {};
        } catch (std::exception& e) { return {}; }

        for (const auto& el : std::filesystem::directory_iterator(registry)) {
            if (!std::filesystem::exists(el.path() / "hyprland.lock"))
                continue;

            // read lock
            SInstanceData* data = &result.emplace_back();
            data->id            = el.path().filename().string();
            data->pid           = 0; // 0 never names a process here; see the erase below

            try {
                data->time = std::stoull(data->id.substr(data->id.find_first_of('_') + 1, data->id.find_last_of('_') - (data->id.find_first_of('_') + 1)));
            } catch (std::exception& e) {
                // a directory whose name we cannot parse is not an instance — drop it rather than
                // leaving a zero-filled entry in the list for callers to trip over
                result.pop_back();
                continue;
            }

            // read file
            std::ifstream ifs(el.path().string() + "/hyprland.lock");

            int           i = 0;
            for (std::string line; std::getline(ifs, line); ++i) {
                if (i == 0) {
                    try {
                        data->pid = std::stoull(line);
                    } catch (std::exception& e) { continue; }
                } else if (i == 1) {
                    data->wlSocket = line;
                } else
                    break;
            }

            ifs.close();
        }

        // Safe::pidAlive rejects a pid of 0 (an unreadable lock file) instead of probing the caller's
        // own process group, which the raw kill(0, 0) form would report as "alive".
        std::erase_if(result, [&](const auto& el) { return !Safe::pidAlive(static_cast<pid_t>(el.pid)); });

        std::ranges::sort(result, [&](const auto& a, const auto& b) { return a.time < b.time; });

        return result;
    }

    // How long a runtime dir may be before it breaks the compositor's IPC.
    //
    // AF_UNIX's sun_path is 108 bytes, and Hyprland has spent nearly all of it before the runtime
    // dir gets a say: the socket lives at <runtimeDir>/hypr/<signature>/.socket2.sock, where the
    // signature is <40-char commit>_<10-digit time>_<10-digit random> = 62 characters. That leaves
    // 107 - (len("/hypr") + 1 + 62 + len("/.socket2.sock")) = 25 characters for the runtime dir.
    //
    // This is why the private dir goes in /tmp with a short name instead of nesting inside the real
    // $XDG_RUNTIME_DIR: /run/user/1000 is already 14 of those 25, and
    // /run/user/1000/hyprtester-<pid>-XXXXXX overflows — the compositor then comes up with socket2
    // refused outright ("Socket2 path is too long") and the command socket silently truncated by
    // HyprCtl's snprintf, i.e. a compositor the harness cannot talk to.
    inline constexpr size_t SUN_PATH_USABLE     = 107; // sizeof(sockaddr_un::sun_path) - 1
    inline constexpr size_t SIGNATURE_LEN       = 62;  // <40-char commit>_<10-digit time>_<10-digit random>
    inline constexpr size_t INSTANCE_TAIL_LEN   = 5 /* /hypr */ + 1 /* / */ + SIGNATURE_LEN + 14 /* /.socket2.sock */;
    inline constexpr size_t MAX_RUNTIME_DIR_LEN = SUN_PATH_USABLE - INSTANCE_TAIL_LEN;

    // The longest IPC socket path a compositor living in `runtimeDir` can end up wanting.
    inline size_t worstCaseSocketPathLen(std::string_view runtimeDir) {
        return runtimeDir.size() + INSTANCE_TAIL_LEN;
    }

    // A private dir is "<parent>/ht-<pid>-XXXXXX": short on purpose, because every character here
    // comes out of the budget above.
    inline size_t privateDirLen(std::string_view parent) {
        return parent.size() + std::string_view{"/ht--XXXXXX"}.size() + 7 /* worst-case pid width */;
    }

    // Where private run dirs live: /tmp (or $TMPDIR when it is usable and short enough), never the
    // real $XDG_RUNTIME_DIR — see the budget above. /tmp is also what `--xr` has always used, and
    // the "XDG_RUNTIME_DIR looks non-standard" line CCompositor prints for it is expected.
    inline std::string isolationParent(const std::string& tmpdir) {
        std::error_code ec;
        if (!tmpdir.empty() && privateDirLen(tmpdir) <= MAX_RUNTIME_DIR_LEN && std::filesystem::is_directory(tmpdir, ec) && ::access(tmpdir.c_str(), W_OK | X_OK) == 0)
            return tmpdir;
        return "/tmp";
    }

    // remove_all() aimed at the wrong path is the same class of accident this file exists to
    // prevent, so teardown has its own guard: only an absolute path that carries the name we gave
    // it, and is not the host's runtime dir, is ever removed.
    inline bool safeToRemove(std::string_view dir, std::string_view hostRuntimeDir) {
        if (dir.empty() || dir.front() != '/')
            return false;
        if (dir.contains(".."))
            return false;
        if (dir == "/" || dir == "/tmp" || dir == "/run" || dir == "/run/user")
            return false;
        if (!hostRuntimeDir.empty() && dir == hostRuntimeDir)
            return false;

        // we only ever delete a directory we named
        const auto             SLASH = dir.rfind('/');
        const std::string_view BASE  = dir.substr(SLASH + 1);
        return BASE.starts_with("ht-") || BASE.starts_with("hyprtester-");
    }

    // The run's private $XDG_RUNTIME_DIR, as an RAII object: engage() creates it and repoints this
    // process at it, the destructor (or an explicit release()) puts the environment back and takes
    // the directory with it.
    //
    // Repointing *this process* is the whole trick — hyprtester's own IPC resolves the runtime dir
    // from the environment on every call, and Hyprutils' CProcess hands our environment to every
    // child it spawns, so the compositor under test, its clients and any hyprctl the tests invoke
    // all land in the same private dir without a single explicit plumb.
    class CRuntimeIsolation {
      public:
        CRuntimeIsolation()                                    = default;
        CRuntimeIsolation(const CRuntimeIsolation&)            = delete;
        CRuntimeIsolation& operator=(const CRuntimeIsolation&) = delete;
        ~CRuntimeIsolation() {
            release();
        }

        // Create the private dir and point the process at it.
        //
        // `fixedPath` empty  -> mkdtemp under isolationParent(): unguessable, collision-free,
        //                       and gone again at the end of the run.
        // `fixedPath` set    -> that exact path, recreated. `--xr` uses it so the container runner
        //                       can collect `/tmp/hyprtester-xr-*` after a failure.
        bool engage(const std::string& fixedPath = "") {
            if (m_engaged)
                return false;

            m_hostRuntimeDir           = envOr("XDG_RUNTIME_DIR");
            m_hostWaylandDisplay       = envOr("WAYLAND_DISPLAY");
            m_hostInstanceSignature    = envOr("HYPRLAND_INSTANCE_SIGNATURE");
            m_hadHostRuntimeDir        = std::getenv("XDG_RUNTIME_DIR") != nullptr;
            m_hadHostWaylandDisplay    = std::getenv("WAYLAND_DISPLAY") != nullptr;
            m_hadHostInstanceSignature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr;

            std::error_code ec;
            std::string     dir;

            if (!fixedPath.empty()) {
                std::filesystem::remove_all(fixedPath, ec);
                std::filesystem::create_directories(fixedPath, ec);
                if (!std::filesystem::is_directory(fixedPath, ec)) {
                    std::println(stderr, "[ hyprtester ] could not create the isolated run dir {}", fixedPath);
                    return false;
                }
                dir = fixedPath;
            } else {
                const std::string PARENT = isolationParent(envOr("TMPDIR"));
                std::string       tmpl   = PARENT + "/ht-" + std::to_string(getpid()) + "-XXXXXX";
                std::vector<char> buf(tmpl.begin(), tmpl.end());
                buf.push_back('\0');
                if (!::mkdtemp(buf.data())) {
                    std::println(stderr, "[ hyprtester ] could not create an isolated run dir under {}", PARENT);
                    return false;
                }
                dir = buf.data();
            }

            ::chmod(dir.c_str(), 0700);

            // Not fatal — the compositor still starts — but socket2 will be refused and the command
            // socket silently truncated, so say so rather than letting the run fail obscurely.
            if (worstCaseSocketPathLen(dir) > SUN_PATH_USABLE)
                std::println(stderr, "[ hyprtester ] run dir {} is {} chars; over {} the compositor's IPC socket paths do not fit in sun_path", dir, dir.size(),
                             MAX_RUNTIME_DIR_LEN);

            // The one thing this class promises. If it somehow does not hold, running is worse
            // than not running: the harness would be enumerating the developer's session.
            if (registryDir(dir) == registryDir(m_hostRuntimeDir)) {
                std::println(stderr, "[ hyprtester ] isolated run dir {} resolves to the session's instance registry — refusing", dir);
                if (safeToRemove(dir, m_hostRuntimeDir))
                    std::filesystem::remove_all(dir, ec);
                return false;
            }

            setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);

            // Nothing may inherit the identity or the socket of the session we were launched from.
            // A child that needs either gets it handed over explicitly (see the pass-through table
            // in docs/openxr/06-testing.md §3.1) — never by accident, and never silently pointing
            // at the developer's compositor because a variable survived.
            unsetenv("HYPRLAND_INSTANCE_SIGNATURE");
            unsetenv("WAYLAND_DISPLAY");

            m_dir     = dir;
            m_engaged = true;
            return true;
        }

        // Make the session's Wayland socket reachable from inside the private dir, and say what to
        // call it.
        //
        // Aquamarine's Wayland backend is the FALLBACK for when DRM is unavailable — which is the
        // normal case for a suite launched from inside a graphical session, because libseat cannot
        // take the seat the live compositor is holding and the headless backend cannot come up
        // alone (`CBackend::create() failed!`). So the compositor under test nests. It always has;
        // it just did so silently, on an inherited WAYLAND_DISPLAY pointing into a shared runtime
        // dir. This makes it explicit: a Wayland client resolves `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`,
        // so the socket (and its lock, which is how the compositor knows the name is taken) are
        // symlinked into the private dir, and the name is handed to the child by `addEnv` rather
        // than left lying in the environment for anything else to pick up.
        //
        // Returns the display name to hand over, or "" when there is no session to nest in — a tty
        // or a CI VM, where DRM or headless works on its own and nothing should be borrowed.
        std::string linkHostWaylandSocket() const {
            if (!m_engaged || m_hostWaylandDisplay.empty() || m_hostRuntimeDir.empty())
                return "";

            std::error_code   ec;
            const std::string SRC = m_hostRuntimeDir + "/" + m_hostWaylandDisplay;
            const std::string DST = m_dir + "/" + m_hostWaylandDisplay;

            if (!std::filesystem::exists(SRC, ec))
                return "";

            std::filesystem::remove(DST, ec);
            std::filesystem::create_symlink(SRC, DST, ec);
            if (ec)
                return "";

            if (std::filesystem::exists(SRC + ".lock", ec)) {
                std::filesystem::remove(DST + ".lock", ec);
                std::filesystem::create_symlink(SRC + ".lock", DST + ".lock", ec);
            }

            return m_hostWaylandDisplay;
        }

        // Keep the directory at release() — the compositor log inside it is usually the only
        // evidence of why a run failed. Successful runs never leave anything behind.
        void preserve() {
            m_preserve = true;
        }

        void release() {
            if (!m_engaged)
                return;
            m_engaged = false;

            // Restore the environment first: whatever happens to the directory, this process must
            // stop claiming to live in it.
            restore("XDG_RUNTIME_DIR", m_hostRuntimeDir, m_hadHostRuntimeDir);
            restore("WAYLAND_DISPLAY", m_hostWaylandDisplay, m_hadHostWaylandDisplay);
            restore("HYPRLAND_INSTANCE_SIGNATURE", m_hostInstanceSignature, m_hadHostInstanceSignature);

            if (m_preserve) {
                std::println("[ hyprtester ] run dir preserved for inspection: {}", m_dir);
                return;
            }

            if (!safeToRemove(m_dir, m_hostRuntimeDir)) {
                std::println(stderr, "[ hyprtester ] refusing to remove run dir {}: it is not a directory we created", m_dir);
                return;
            }

            std::error_code ec;
            std::filesystem::remove_all(m_dir, ec);
        }

        bool engaged() const {
            return m_engaged;
        }

        const std::string& dir() const {
            return m_dir;
        }

        // Captured at engage() and then removed from the environment. Callers that legitimately
        // need a piece of the real session — the `--xr` nested-Wayland fallback wants the host's
        // socket, ourInstance() wants the signature it must never drive — read it from here and
        // plumb it explicitly.
        const std::string& hostRuntimeDir() const {
            return m_hostRuntimeDir;
        }

        const std::string& hostWaylandDisplay() const {
            return m_hostWaylandDisplay;
        }

        const std::string& hostInstanceSignature() const {
            return m_hostInstanceSignature;
        }

      private:
        static void restore(const char* name, const std::string& value, bool wasSet) {
            if (wasSet)
                setenv(name, value.c_str(), 1);
            else
                unsetenv(name);
        }

        std::string m_dir;
        std::string m_hostRuntimeDir;
        std::string m_hostWaylandDisplay;
        std::string m_hostInstanceSignature;
        bool        m_hadHostRuntimeDir        = false;
        bool        m_hadHostWaylandDisplay    = false;
        bool        m_hadHostInstanceSignature = false;
        bool        m_engaged                  = false;
        bool        m_preserve                 = false;
    };
}
