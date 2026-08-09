#pragma once

#include <optional>
#include <wayland-server.h>
#include <vector>
#include <format>
#include <expected>
#include <utility>
#include <hyprutils/os/FileDescriptor.hpp>
#include "../SharedDefs.hpp"
#include "../macros.hpp"

struct SCallstackFrameInfo {
    void*       adr = nullptr;
    std::string desc;
};

struct SWorkspaceIDName {
    WORKSPACEID id = WORKSPACE_INVALID;
    std::string name;
    bool        isAutoIDd = false;
};

std::string                             absolutePath(const std::string&, const std::string&);
std::string                             escapeJSONStrings(const std::string& str);
bool                                    isDirection(std::string_view);
SWorkspaceIDName                        getWorkspaceIDNameFromString(const std::string&);
std::optional<std::string>              cleanCmdForWorkspace(const std::string&, std::string);
float                                   vecToRectDistanceSquared(const Vector2D& vec, const Vector2D& p1, const Vector2D& p2);
std::string                             execAndGet(const char*);
int64_t                                 getPPIDof(int64_t pid);
std::optional<float>                    getPlusMinusKeywordResult(std::string in, float relative);
double                                  normalizeAngleRad(double ang);
std::vector<SCallstackFrameInfo>        getBacktrace();
[[noreturn]] void                       throwError(const std::string& err);
Hyprutils::OS::CFileDescriptor          allocateSHMFile(size_t len);
bool                                    allocateSHMFilePair(size_t size, Hyprutils::OS::CFileDescriptor& rw_fd_ptr, Hyprutils::OS::CFileDescriptor& ro_fd_ptr);
float                                   stringToPercentage(const std::string& VALUE, const float REL);
bool                                    isNvidiaDriverVersionAtLeast(int threshold);
std::expected<std::string, std::string> binaryNameForWlClient(wl_client* client);
std::expected<std::string, std::string> binaryNameForPid(pid_t pid);
std::string                             deviceNameToInternalString(const std::string& in);
std::string                             getSystemLibraryVersion(const std::string& name);
std::string                             getBuiltSystemLibraryNames();
bool                                    truthy(const std::string& str);
// Send a signal to exactly one process. kill(2) reads a non-positive pid as a BROADCAST — -1 is
// every process we may signal, 0 is our own process group (for a compositor started by a session
// manager, that is the session) — and the pid accessors we feed it hand back exactly those values
// for a client that has gone away: CWindow::getPID() and CLayerSurface::getPID() return -1 once the
// surface has no owner ("happens at unmap"), CANRManager::SANRData::getPID() returns 0. Force-killing
// a window that is mid-unmap must close that window or nothing, never the session. Returns false
// when the pid was refused.
bool killPid(pid_t pid, int sig);

template <typename... Args>
[[deprecated("use std::format instead")]] std::string getFormat(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}
