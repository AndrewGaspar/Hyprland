#include "xr_helpers.hpp"

#ifdef WITH_XR_TESTS

#include "../Log.hpp"
#include "../hyprctlCompat.hpp"
#include "../shared.hpp" // HIS

#include <unistd.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <thread>

namespace XR {

    bool shouldSkip(std::string& reasonOut) {
        if (g_ctx.wireMismatch) {
            reasonOut = "monado remote wire ABI mismatch (vendored @c2ddab59, service reports otherwise)";
            return true;
        }
        if (!g_ctx.available) {
            reasonOut = g_ctx.skipReason.empty() ? "monado-service not found" : g_ctx.skipReason;
            return true;
        }
        return false;
    }

    void logSkip(const std::string& testName, const std::string& reason) {
        // TAP-style line so external harnesses can grep it (docs §5.3).
        NLog::yellow("SKIP: {} — {}", testName, reason);
        NLog::yellow("ok - {} # SKIP {}", testName, reason);
    }

    bool waitForJson(const std::string& cmd, std::function<bool(const std::string&)> pred, std::chrono::milliseconds timeout, std::chrono::milliseconds interval) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            const std::string reply = getFromSocket(cmd);
            if (pred(reply))
                return true;
            std::this_thread::sleep_for(interval);
        } while (std::chrono::steady_clock::now() < deadline);

        // one final check right at/after the deadline
        return pred(getFromSocket(cmd));
    }

    bool waitForXrState(const std::string& state, std::chrono::milliseconds timeout) {
        const std::string needle = std::string("\"state\": \"") + state + "\"";
        return waitForJson("j/openxr", [&](const std::string& reply) { return reply.contains(needle); }, timeout);
    }

    std::string monitorName(int n) {
        return std::string("XR-t") + std::to_string(getpid()) + "-" + std::to_string(n);
    }

    static void writeTail(const std::string& src, const std::string& dst, size_t maxLines) {
        std::ifstream in(src);
        if (!in)
            return;
        std::vector<std::string> lines;
        std::string              line;
        while (std::getline(in, line))
            lines.push_back(line);
        std::ofstream out(dst);
        if (!out)
            return;
        const size_t from = lines.size() > maxLines ? lines.size() - maxLines : 0;
        for (size_t i = from; i < lines.size(); ++i)
            out << lines[i] << "\n";
    }

    static void writeString(const std::string& dst, const std::string& contents) {
        std::ofstream out(dst);
        if (out)
            out << contents;
    }

    void dumpXrArtifacts(const std::string& testName) {
        const std::string runId  = g_ctx.runId.empty() ? ("xr-" + std::to_string(getpid())) : g_ctx.runId;
        const std::string outDir = std::string("artifacts/") + runId + "/" + testName;

        std::error_code ec;
        std::filesystem::create_directories(outDir, ec);

        writeString(outDir + "/monitors.json", getFromSocket("j/monitors"));
        writeString(outDir + "/openxr.json", getFromSocket("j/openxr"));

        if (!g_ctx.monadoLog.empty())
            writeTail(g_ctx.monadoLog, outDir + "/monado.log", 200);

        if (!HIS.empty()) {
            const auto* xdg = getenv("XDG_RUNTIME_DIR");
            if (xdg) {
                const std::string hlLog = std::string(xdg) + "/hypr/" + HIS + "/hyprland.log";
                writeTail(hlLog, outDir + "/hyprland.log", 200);
            }
        }

        NLog::red("Artifacts written to {}/", std::filesystem::absolute(outDir, ec).string());
    }

    size_t findAfter(const std::string& hay, const std::string& marker, size_t from) {
        return hay.find(marker, from);
    }

    std::string fieldAfter(const std::string& json, size_t from, const std::string& key) {
        if (from == std::string::npos)
            return "";
        const std::string needle = "\"" + key + "\":";
        const auto        kpos   = json.find(needle, from);
        if (kpos == std::string::npos)
            return "";

        size_t i = kpos + needle.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t'))
            ++i;
        if (i >= json.size())
            return "";

        if (json[i] == '"') {
            const auto end = json.find('"', i + 1);
            if (end == std::string::npos)
                return "";
            return json.substr(i + 1, end - i - 1);
        }

        if (json[i] == '[') {
            const auto end = json.find(']', i);
            if (end == std::string::npos)
                return "";
            return json.substr(i, end - i + 1);
        }

        size_t end = i;
        while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']' && json[end] != '\n')
            ++end;
        while (end > i && std::isspace(static_cast<unsigned char>(json[end - 1])))
            --end;
        return json.substr(i, end - i);
    }

    std::vector<float> parseFloatArray(const std::string& arr) {
        std::vector<float> out;
        std::string        cur;
        for (char c : arr) {
            if (c == '[' || c == ']')
                continue;
            if (c == ',') {
                if (!cur.empty()) {
                    out.push_back(toFloatOr(cur, 0.f));
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (!cur.empty())
            out.push_back(toFloatOr(cur, 0.f));
        return out;
    }

    float toFloatOr(const std::string& s, float fallback) {
        try {
            size_t idx = 0;
            float  v   = std::stof(s, &idx);
            return v;
        } catch (...) { return fallback; }
    }

} // namespace XR

#endif // WITH_XR_TESTS
