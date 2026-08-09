#include "hyprctlCompat.hpp"
#include "shared.hpp"
#include "SafeKill.hpp"
#include "RuntimeIsolation.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <csignal>
#include <cerrno>
#include <print>
#include <hyprutils/memory/Casts.hpp>
using namespace Hyprutils::Memory;

// Both of these resolve $XDG_RUNTIME_DIR on every call, which is what makes CRuntimeIsolation work
// at all: engaging it mid-process silently moves every subsequent lookup — enumeration and IPC
// alike — into the run's private directory.
std::vector<SInstanceData> instances() {
    return Harness::scanInstances(Harness::currentRegistryDir());
}

std::string getFromSocket(const std::string& cmd) {
    const auto SERVERSOCKET = socket(AF_UNIX, SOCK_STREAM, 0);

    auto       t = timeval{.tv_sec = 5, .tv_usec = 0};
    setsockopt(SERVERSOCKET, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(struct timeval));

    if (SERVERSOCKET < 0) {
        std::println("socket: Couldn't open a socket (1)");
        return "";
    }

    sockaddr_un serverAddress = {0};
    serverAddress.sun_family  = AF_UNIX;

    std::string socketPath = Harness::currentRegistryDir() + "/" + HIS + "/.socket.sock";

    strncpy(serverAddress.sun_path, socketPath.c_str(), sizeof(serverAddress.sun_path) - 1);

    if (connect(SERVERSOCKET, rc<sockaddr*>(&serverAddress), SUN_LEN(&serverAddress)) < 0) {
        std::println("Couldn't connect to {}. (3)", socketPath);
        return "";
    }

    auto sizeWritten = write(SERVERSOCKET, cmd.c_str(), cmd.length());

    if (sizeWritten < 0) {
        std::println("Couldn't write (4)");
        return "";
    }

    std::string reply        = "";
    char        buffer[8192] = {0};

    sizeWritten = read(SERVERSOCKET, buffer, 8192);

    if (sizeWritten < 0) {
        if (errno == EWOULDBLOCK)
            std::println("Hyprland IPC didn't respond in time");
        std::println("Couldn't read (5)");
        return "";
    }

    reply += std::string(buffer, sizeWritten);

    while (sizeWritten == 8192) {
        sizeWritten = read(SERVERSOCKET, buffer, 8192);
        if (sizeWritten < 0) {
            std::println("Couldn't read (5)");
            return "";
        }
        reply += std::string(buffer, sizeWritten);
    }

    close(SERVERSOCKET);

    return reply;
}
