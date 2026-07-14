#include <openxr/XRMonitorConfig.hpp>

#include <gtest/gtest.h>

using namespace OpenXR;

// resolveRuntimeJsonEnv (XRMonitorConfig.cpp) is the pure decision behind openxr:runtime_json
// (WP-XR1): given the process's ORIGINAL XR_RUNTIME_JSON, the value currently in the environment, and
// the requested config value, it returns the MINIMAL environ mutation. Minimality is load-bearing —
// setenv in a threaded process is only safe because steady-state reprobes resolve to NOOP and never
// touch `environ`. These tests pin the truth table so COpenXRManager::start() can apply it blindly.

static SRuntimeJsonEnvAction set(const std::string& v) {
    return {XR_RTJSON_SET, v};
}
static SRuntimeJsonEnvAction unset() {
    return {XR_RTJSON_UNSET, ""};
}
static SRuntimeJsonEnvAction noop() {
    return {XR_RTJSON_NOOP, ""};
}

// ---- config non-empty: force the override ----

TEST(XRRuntimeJson, OverrideFromCleanEnv) {
    // No original, nothing set, config asks for the xreal manifest -> SET it.
    EXPECT_EQ(resolveRuntimeJsonEnv("/x/xreal.json", false, "", false, ""), set("/x/xreal.json"));
}

TEST(XRRuntimeJson, OverrideReplacesLoginRuntime) {
    // Login had WiVRn's manifest; config forces xreal -> SET (replace).
    EXPECT_EQ(resolveRuntimeJsonEnv("/x/xreal.json", true, "/w/wivrn.json", true, "/w/wivrn.json"), set("/x/xreal.json"));
}

TEST(XRRuntimeJson, OverrideAlreadyAppliedIsNoop) {
    // Steady-state reprobe: env already holds the override -> NOOP (must not churn environ).
    EXPECT_EQ(resolveRuntimeJsonEnv("/x/xreal.json", false, "", true, "/x/xreal.json"), noop());
    EXPECT_EQ(resolveRuntimeJsonEnv("/x/xreal.json", true, "/w/wivrn.json", true, "/x/xreal.json"), noop());
}

TEST(XRRuntimeJson, OverrideChangesToDifferentManifest) {
    // Config changed from one override to another between starts -> SET the new one.
    EXPECT_EQ(resolveRuntimeJsonEnv("/x/b.json", false, "", true, "/x/a.json"), set("/x/b.json"));
}

// ---- config empty: restore whatever the process launched with ----

TEST(XRRuntimeJson, EmptyWithNoOriginalAndCleanEnvIsNoop) {
    // Never had a runtime override, none set now -> nothing to do.
    EXPECT_EQ(resolveRuntimeJsonEnv("", false, "", false, ""), noop());
}

TEST(XRRuntimeJson, EmptyRestoresUnsetAfterOverride) {
    // We previously forced an override (env is set), original had none, config cleared -> UNSET.
    EXPECT_EQ(resolveRuntimeJsonEnv("", false, "", true, "/x/xreal.json"), unset());
}

TEST(XRRuntimeJson, EmptyRestoresOriginalLoginValue) {
    // Login had WiVRn; we overrode to xreal; config cleared -> SET back to the login value.
    EXPECT_EQ(resolveRuntimeJsonEnv("", true, "/w/wivrn.json", true, "/x/xreal.json"), set("/w/wivrn.json"));
}

TEST(XRRuntimeJson, EmptyWithOriginalAlreadyRestoredIsNoop) {
    // Login had WiVRn, env already holds it, config empty -> NOOP (do not re-set the same value).
    EXPECT_EQ(resolveRuntimeJsonEnv("", true, "/w/wivrn.json", true, "/w/wivrn.json"), noop());
}

TEST(XRRuntimeJson, EmptyWithOriginalButEnvMissingReSets) {
    // Login had WiVRn but something unset it since; config empty -> restore the original.
    EXPECT_EQ(resolveRuntimeJsonEnv("", true, "/w/wivrn.json", false, ""), set("/w/wivrn.json"));
}

// ---- reversibility: the flat<->XR toggle round-trips with no residual state ----

TEST(XRRuntimeJson, ToggleRoundTripsFromLoginRuntime) {
    // Start: login = WiVRn, env = WiVRn.
    const std::string login = "/w/wivrn.json";
    // Toggle XR: config -> xreal.
    auto a = resolveRuntimeJsonEnv("/x/xreal.json", true, login, true, login);
    EXPECT_EQ(a, set("/x/xreal.json"));
    // env now = xreal. Toggle FLAT: config -> "" restores login.
    auto b = resolveRuntimeJsonEnv("", true, login, true, "/x/xreal.json");
    EXPECT_EQ(b, set(login));
    // env now = login again == start. Idempotent flat re-toggle is a NOOP.
    EXPECT_EQ(resolveRuntimeJsonEnv("", true, login, true, login), noop());
}

TEST(XRRuntimeJson, ToggleRoundTripsFromCleanLogin) {
    // Start: no login runtime, env clean.
    auto a = resolveRuntimeJsonEnv("/x/xreal.json", false, "", false, "");
    EXPECT_EQ(a, set("/x/xreal.json"));
    // Toggle flat: restore to "unset".
    EXPECT_EQ(resolveRuntimeJsonEnv("", false, "", true, "/x/xreal.json"), unset());
}
