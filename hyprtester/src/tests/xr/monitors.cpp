#include "tests.hpp"

#ifdef WITH_XR_TESTS

#include "../../hyprctlCompat.hpp"
#include "../../Log.hpp"
#include "../../xr/xr_helpers.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {
    // Gate shared by every test in this file: focused, falling back to a short visible check
    // (WP10's session.cpp caveat) so we don't burn the whole per-test budget on a dead session.
    bool gateUp() {
        if (XR::waitForXrState("focused", std::chrono::milliseconds(15000)))
            return true;
        return XR::waitForXrState("visible", std::chrono::milliseconds(2000));
    }

    // RAII: dump artifacts iff the test ended up failed (docs §5.2), and always clean up any
    // runtime monitors we created, regardless of how the test exits.
    struct SArtifactGuard {
        const bool&              failed;
        std::string              testName;
        std::vector<std::string> monitorNames;
        ~SArtifactGuard() {
            for (auto& n : monitorNames)
                if (!n.empty())
                    getFromSocket("/openxr destroy " + n);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
}

// xr_monitor_create_destroy — WP11 (doc 06 §6 row 2): create via `hyprctl openxr create`,
// verify presence in both j/monitors and j/openxr (with size_m ~ openxr:default_size), destroy,
// verify absence from both.
TEST_CASE(xr_monitor_create_destroy) {
    XR_SKIP_IF_UNAVAILABLE();
    SArtifactGuard guard{this->failed, name(), {}};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon = XR::monitorName(11);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorNames.push_back(mon);

    const std::string nameMarker = "\"name\": \"" + mon + "\"";
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);

    // size_m ≈ openxr:default_size (1.6 default; xr-test.conf doesn't override it).
    const std::string openxrJson = getFromSocket("j/openxr");
    const auto        pos        = XR::findAfter(openxrJson, nameMarker);
    ASSERT_NOT(pos, std::string::npos);
    const float sizeM = XR::toFloatOr(XR::fieldAfter(openxrJson, pos, "size_m"), -1.f);
    EXPECT_MAX_DELTA(sizeM, 1.6, 0.05);

    ASSERT(getFromSocket("/openxr destroy " + mon), std::string("ok"));
    guard.monitorNames.clear(); // already destroyed; don't try again in the guard

    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return !r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return !r.contains(nameMarker); }, std::chrono::milliseconds(10000)),
           true);

    NLog::green("xr_monitor_create_destroy: create+destroy round-trip verified in both j/monitors and j/openxr");
}

// xr_config_declared — WP11 (doc 06 §6 row 3, extended per the roadmap's explicit ask for
// update-in-place + removal coverage; doc 06 marks the file-swap flavor "out of scope v1" but
// it's cheap and the roadmap wants it).
//
// (a) the two config-declared fixtures (xr-test.conf) exist with their configured anchors;
// (b) a runtime monitor + `/reload` -> declared fixtures survive with stable ids (no
//     destroy/create flicker), the runtime monitor is untouched;
// (c) rewrite xr-test.conf to change XR-conf-a's size and drop XR-conf-b, `/reload` ->
//     XR-conf-a updates IN PLACE (same id, new size_m), XR-conf-b is destroyed, the runtime
//     monitor from (b) is still untouched.
//
// The config file is always restored (and reloaded again) by the RAII guard, regardless of
// test outcome, so a crash mid-test never leaves xr-test.conf modified on disk.
TEST_CASE(xr_config_declared) {
    XR_SKIP_IF_UNAVAILABLE();

    static const std::string CONF_PATH = "xr-test.conf";

    std::string original;
    {
        std::ifstream in(CONF_PATH);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            original = ss.str();
        }
    }

    struct SConfigGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        std::string originalContent;
        bool        modified = false;
        ~SConfigGuard() {
            if (modified && !originalContent.empty()) {
                std::ofstream out(CONF_PATH, std::ios::trunc);
                if (out)
                    out << originalContent;
                getFromSocket("/reload");
            }
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SConfigGuard guard{this->failed, name(), "", original, false};

    if (original.empty()) {
        XR::logSkip(name(), "could not read xr-test.conf from the cwd (hyprtester must run from hyprtester/)");
        return;
    }

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    // (a) initial declared fixtures, correct anchors.
    std::string status = getFromSocket("j/openxr");
    ASSERT_CONTAINS(status, "\"name\": \"XR-conf-a\"");
    ASSERT_CONTAINS(status, "\"name\": \"XR-conf-b\"");

    const auto posA0 = XR::findAfter(status, "\"name\": \"XR-conf-a\"");
    const auto posB0 = XR::findAfter(status, "\"name\": \"XR-conf-b\"");
    EXPECT(XR::fieldAfter(status, posA0, "mode"), std::string("local"));
    EXPECT(XR::fieldAfter(status, posB0, "mode"), std::string("head"));

    const std::string idA = XR::fieldAfter(status, posA0, "id");
    const std::string idB = XR::fieldAfter(status, posB0, "id");
    ASSERT_NOT(idA, std::string(""));
    ASSERT_NOT(idB, std::string(""));

    // (b) runtime monitor + reload -> declared survive untouched (same id), runtime survives.
    const std::string mon = XR::monitorName(12);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    ASSERT(getFromSocket("/reload"), std::string("ok"));

    std::string statusAfterReload;
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   statusAfterReload = r;
                   return r.contains("\"name\": \"XR-conf-a\"") && r.contains("\"name\": \"XR-conf-b\"") && r.contains("\"name\": \"" + mon + "\"");
               },
               std::chrono::milliseconds(10000)),
           true);

    EXPECT(XR::fieldAfter(statusAfterReload, XR::findAfter(statusAfterReload, "\"name\": \"XR-conf-a\""), "id"), idA);
    EXPECT(XR::fieldAfter(statusAfterReload, XR::findAfter(statusAfterReload, "\"name\": \"XR-conf-b\""), "id"), idB);
    NLog::green("xr_config_declared: declared fixtures survived reload with stable ids; runtime monitor untouched");

    // (c) rewrite the config: change XR-conf-a's size, drop XR-conf-b entirely.
    std::string modified;
    {
        std::istringstream iss(original);
        std::string        line;
        while (std::getline(iss, line)) {
            const auto p = line.find_first_not_of(" \t");
            if (p != std::string::npos && line.compare(p, 9, "xrmonitor") == 0)
                continue; // drop both original xrmonitor declarations; we re-add XR-conf-a below
            modified += line + "\n";
        }
        modified += "xrmonitor = XR-conf-a, 1280x720@60, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.9\n";
    }
    {
        std::ofstream out(CONF_PATH, std::ios::trunc);
        ASSERT(!!out, true);
        out << modified;
    }
    guard.modified = true;

    ASSERT(getFromSocket("/reload"), std::string("ok"));

    std::string statusFinal;
    ASSERT(XR::waitForJson(
               "j/openxr",
               [&](const std::string& r) {
                   statusFinal = r;
                   return r.contains("\"name\": \"XR-conf-a\"") && !r.contains("\"name\": \"XR-conf-b\"") && r.contains("\"name\": \"" + mon + "\"");
               },
               std::chrono::milliseconds(10000)),
           true);

    // XR-conf-a updated in place: same id, new size.
    const auto posAFinal = XR::findAfter(statusFinal, "\"name\": \"XR-conf-a\"");
    EXPECT(XR::fieldAfter(statusFinal, posAFinal, "id"), idA);
    const float sizeFinal = XR::toFloatOr(XR::fieldAfter(statusFinal, posAFinal, "size_m"), -1.f);
    EXPECT_MAX_DELTA(sizeFinal, 1.9, 0.05);

    // XR-conf-b fully gone (destroyed, headless output removed too). The layer leaves
    // j/openxr synchronously on reconcile, but the underlying output's removal goes through the
    // frame-thread removal barrier (WP3/WP4), so give j/monitors a bounded moment to catch up.
    ASSERT(XR::waitForJson(
               "j/monitors", [&](const std::string& r) { return !r.contains("\"name\": \"XR-conf-b\""); }, std::chrono::milliseconds(10000)),
           true);

    NLog::green("xr_config_declared: declaration change updated XR-conf-a in place and destroyed XR-conf-b; runtime monitor untouched throughout");
}

// xr_mirror — WP11 (doc 06 §6 row 9): zero-new-compositor-code mirroring via the ordinary
// `monitor =` keyword's `mirror` verb (CMonitor::setMirror).
TEST_CASE(xr_mirror) {
    XR_SKIP_IF_UNAVAILABLE();

    struct SGuard {
        const bool& failed;
        std::string testName;
        std::string monitorName;
        bool        createdHeadless2 = false;
        ~SGuard() {
            if (createdHeadless2)
                getFromSocket("/output destroy HEADLESS-2");
            if (!monitorName.empty())
                getFromSocket("/openxr destroy " + monitorName);
            if (failed)
                XR::dumpXrArtifacts(testName);
        }
    };
    SGuard guard{this->failed, name(), "", false};

    if (!gateUp()) {
        XR::logSkip(name(), "session never reached focused/visible (known env instability)");
        return;
    }

    const std::string mon = XR::monitorName(13);
    ASSERT(getFromSocket("/openxr create " + mon + " 1280x720"), std::string("ok"));
    guard.monitorName = mon;
    ASSERT(XR::waitForJson(
               "j/openxr", [&](const std::string& r) { return r.contains("\"name\": \"" + mon + "\""); }, std::chrono::milliseconds(10000)),
           true);

    const std::string xrStatus = getFromSocket("j/openxr");
    const std::string xrId     = XR::fieldAfter(xrStatus, XR::findAfter(xrStatus, "\"name\": \"" + mon + "\""), "id");
    ASSERT_NOT(xrId, std::string(""));

    const std::string createReply = getFromSocket("/output create headless HEADLESS-2");
    ASSERT(createReply == "ok" || createReply == "Name already taken", true);
    guard.createdHeadless2 = true;

    // The headless output takes a moment to actually connect/register before it's queryable or
    // matchable by a `monitor =` rule; wait for it before applying the mirror keyword. Use
    // "monitors all" throughout this test: a monitor actively mirroring another is excluded
    // from the plain `monitors()` list (CMonitorStateTracker erases `isMirror()` entries) and
    // only shows up via `allMonitors()` (`hyprctl monitors all`).
    ASSERT(XR::waitForJson(
               "j/monitors all", [&](const std::string& r) { return r.contains("\"name\": \"HEADLESS-2\""); }, std::chrono::milliseconds(10000)),
           true);

    ASSERT(getFromSocket("/keyword monitor HEADLESS-2, preferred, auto, 1, mirror, " + mon), std::string("ok"));

    ASSERT(XR::waitForJson(
               "j/monitors all",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, "\"name\": \"HEADLESS-2\"");
                   return p != std::string::npos && XR::fieldAfter(r, p, "mirrorOf") == xrId;
               },
               std::chrono::milliseconds(10000)),
           true);
    NLog::green("xr_mirror: HEADLESS-2 mirrorOf == {} (the XR monitor's id)", xrId);

    // Unset mirroring, restoring HEADLESS-2 to a normal output (doc 06 §6 row 9).
    ASSERT(getFromSocket("/keyword monitor HEADLESS-2, preferred, auto, 1"), std::string("ok"));
    ASSERT(XR::waitForJson(
               "j/monitors",
               [&](const std::string& r) {
                   const auto p = XR::findAfter(r, "\"name\": \"HEADLESS-2\"");
                   return p != std::string::npos && XR::fieldAfter(r, p, "mirrorOf") == "none";
               },
               std::chrono::milliseconds(10000)),
           true);
}

#endif // WITH_XR_TESTS
