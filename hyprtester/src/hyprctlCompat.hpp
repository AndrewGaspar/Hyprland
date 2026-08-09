#pragma once

// Safe::signalPid / Safe::pidAlive. Pulled in here because every test TU already includes this
// header, and the harness must never reach for raw kill(2) — see SafeKill.hpp.
#include "SafeKill.hpp"

#include <vector>
#include <string>
#include <cstdint>

struct SInstanceData {
    std::string id;
    uint64_t    time;
    uint64_t    pid;
    std::string wlSocket;
    bool        valid = true;
};

std::vector<SInstanceData> instances();
std::string                getFromSocket(const std::string& cmd);