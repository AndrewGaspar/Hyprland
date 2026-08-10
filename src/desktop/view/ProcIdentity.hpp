#pragma once

// The two strings a window rule can match a *process* by (task #143): the resolved target of
// /proc/<pid>/exe and the process's own argv, read from /proc/<pid>/cmdline.
//
// Why both, when class and title already exist: a Wine/Proton game has neither of the two identities
// its user thinks it has. Its class is Steam's or the wrapper's, its title is whatever the game
// draws, and its exe is `.../wine-preloader` for every Wine app on the system. The one string that
// actually names the game is its argv — verified on a live Proton install:
//
//     exe     = /home/ajg/.steam/.../dist/bin/wine-preloader
//     cmdline = Z:\home\ajg\Games\Ishimura\game\Dead Space.exe
//
// so `match:cmdline` is the useful identity for Wine, and `match:exe` for everything native.
//
// Both are read ONCE per window and cached by the window that owns the pid, on the main thread.
// They are client-controlled — a process picks its own argv[0] and can be exec'd from any path — so
// they carry exactly the trust level of class and title: convenient identity, not authentication.

#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace Desktop::Proc {

    struct SIdentity {
        std::string exe;     // resolved /proc/<pid>/exe, empty when unreadable
        std::string cmdline; // /proc/<pid>/cmdline with NULs folded to single spaces, empty when unreadable
    };

    // pure: /proc/<pid>/cmdline is a NUL-separated argv blob with a trailing NUL. Fold it into the
    // one string a regex can be written against — single spaces between args, no trailing space.
    // Kept separate from the file read so the interesting half is testable without a process.
    std::string cmdlineFromRaw(std::string_view raw);

    // Read both from /proc. Never throws and never reports failure: a process that exited between
    // the map and this read caches as empty, and an empty string simply never matches a rule. A
    // matcher that errors on a dead pid would be worse than one that quietly does not fire.
    SIdentity readIdentity(pid_t pid);

    // The seam. Everything in the tree goes through this; tests install a fake so the matcher can be
    // driven without spawning a process. Passing nullptr restores the real /proc reader.
    using PIdentityReader = std::function<SIdentity(pid_t)>;
    SIdentity identityFor(pid_t pid);
    void      setIdentityReaderForTesting(PIdentityReader reader);
}
