#include "ProcIdentity.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

using namespace Desktop;

std::string Proc::cmdlineFromRaw(std::string_view raw) {
    // argv is NUL-separated with a trailing NUL, and a process may leave embedded empty args. Drop
    // the trailing NULs first so the fold cannot produce a trailing space, then fold the separators.
    while (!raw.empty() && raw.back() == '\0')
        raw.remove_suffix(1);

    std::string out{raw};
    std::ranges::replace(out, '\0', ' ');
    return out;
}

Proc::SIdentity Proc::readIdentity(pid_t pid) {
    SIdentity ident;

    if (pid <= 0)
        return ident;

    // /proc/<pid>/exe is a symlink to the running image. read_symlink rather than canonical(): a
    // deleted binary resolves to "<path> (deleted)" and canonicalizing it would throw, where the
    // raw link is still the most useful thing to match on.
    std::error_code ec;
    const auto      EXE = std::filesystem::read_symlink(std::format("/proc/{}/exe", pid), ec);
    if (!ec)
        ident.exe = EXE.string();

    // cmdline is a single read of a small proc file. It is legitimately EMPTY for a kernel thread,
    // and unreadable for a process that is gone or owned by another user — both cache as empty.
    std::ifstream file(std::format("/proc/{}/cmdline", pid), std::ios::binary);
    if (file.is_open()) {
        std::ostringstream ss;
        ss << file.rdbuf();
        ident.cmdline = cmdlineFromRaw(ss.str());
    }

    return ident;
}

namespace {
    Proc::PIdentityReader g_identityReader = nullptr;
}

Proc::SIdentity Proc::identityFor(pid_t pid) {
    return g_identityReader ? g_identityReader(pid) : readIdentity(pid);
}

void Proc::setIdentityReaderForTesting(PIdentityReader reader) {
    g_identityReader = std::move(reader);
}
