# 06 — Testing: Unit Tests, Monado Orchestration, and the XR Integration Suite

This document specifies the complete test strategy for the OpenXR extension: an always-on
gtest unit tier and a local-only hyprtester integration tier that drives a real (headless)
Monado runtime with scripted head/controller input. It is self-contained; sibling docs are
cited where the behavior under test is designed (`02-virtual-monitors.md`, `03-anchoring.md`,
`04-input.md`, `05-ipc-config.md`).

The conventions here deliberately adopt the lessons from
`/home/ajg/code/omedora-4/omedora/testing.md` (a battle-tested headless-compositor test
harness): **TAP-style reporting with a CI exit code; a strict `waitFor*` / `assert*` split
(never assert on state you haven't waited for); artifact dumps on failure instead of
interactive debugging; unique per-run resource names so concurrent/repeated runs never
collide; and keeping the heavy runtime-dependent tier local-only until it has proven stable**
(omedora's L4 lesson: their GPU-dependent session tier stayed a local/pre-release gate, not a
per-PR CI gate — same posture here for anything needing `monado-service`).

---

## 1. Two-tier posture

| Tier | Framework | Lives in | Needs | Runs |
|---|---|---|---|---|
| **Unit** | gtest (`hyprland_gtests`) | `tests/xr/` | nothing (pure math/parsing, no XR runtime, no session) | always — every CI run, `-DWITH_TESTS=ON` |
| **Integration** | hyprtester | `hyprtester/src/xr/` + `hyprtester/src/tests/xr/` | built Hyprland with OpenXR, `monado-service`, a Vulkan device (lavapipe OK) | local only, behind CMake option `WITH_XR_TESTS` (default **OFF**); runtime-SKIPs (TAP `# SKIP`) when `monado-service` is not found |

### 1.1 Unit tier

Three files under `tests/xr/`, picked up automatically by the root `CMakeLists.txt`
(`file(GLOB_RECURSE TESTFILES "tests/*.cpp")`, ~line 688; linked against `hyprland_lib` +
`GTest::gtest_main`, discovered via `gtest_discover_tests`). They test code that is compiled
**unconditionally** (no `HAVE_OPENXR`, no OpenXR headers) — see `00-overview.md` build
gating and `05-ipc-config.md` §2.3:

- `tests/xr/anchor_math.cpp` — `src/openxr/XRAnchor.{hpp,cpp}` + `XRMath.hpp`: the
  critically-damped spring (ω = 2/leash_response) converges without overshoot and respects
  dt independence; yaw-frame extraction from a view quaternion (forward projected to XZ,
  including the near-vertical-gaze degenerate case); angular and positional deadzones
  (inside → no motion, outside → target re-acquired); grab offset composition
  `inv(gripPose)∘quadPose` round-trips (see `03-anchoring.md` for the derivations).
- `tests/xr/ray_intersect.cpp` — ray–quad intersection (`04-input.md`): hit → correct UV in
  [0,1]²; miss (behind, parallel, outside bounds); nearest-t selection across two
  overlapping quads; UV → output-local pixel mapping for a known mode.
- `tests/xr/parser.cpp` — `parseXRMonitorLine` (`05-ipc-config.md` §2.3): the four grammar
  examples, defaults, all four anchor modes, malformed-input errors.

Follow the existing style (`tests/helpers/*.cpp`, e.g. `tests/helpers/Color.cpp`):
plain `TEST(XRAnchor, springConverges) { ... }`, include the header under test by its
`src/`-relative path.

### 1.2 Integration tier — CMake gating

Root `CMakeLists.txt`, inside the existing `if(BUILD_TESTING OR WITH_TESTS)` block
(~line 679, where `add_subdirectory(hyprtester)` lives):

```cmake
option(WITH_XR_TESTS "Build the OpenXR integration test suite into hyprtester (needs a local Monado for running)" OFF)
```

`hyprtester/CMakeLists.txt` GLOBs all of `src/*.cpp` into one binary, so gating is by compile
definition, not file exclusion:

```cmake
if(WITH_XR_TESTS)
  target_compile_definitions(hyprtester PRIVATE WITH_XR_TESTS)
endif()
```

Every file under `hyprtester/src/xr/` and `hyprtester/src/tests/xr/` wraps its entire
contents in `#ifdef WITH_XR_TESTS`. The suite is additionally **runtime-gated**: even when
built, if `monado-service` can't be located/started, every XR test emits
`ok N - <name> # SKIP monado-service not found` and the run passes (omedora SKIP-gating
pattern — absence of the optional runtime is not a failure).

---

## 2. Directory layout

```
hyprtester/
├── xr-test.conf                     # Hyprland config for the XR suite (classic hyprlang)
└── src/
    ├── xr/                          # infrastructure (all #ifdef WITH_XR_TESTS)
    │   ├── MonadoOrchestrator.hpp/.cpp   # launch/ready-poll/teardown monado-service
    │   ├── RemoteClient.hpp/.cpp         # TCP 4242 client speaking the remote wire protocol
    │   ├── monado_remote_wire.hpp        # VENDORED wire structs (see §4) — pinned to a Monado commit
    │   └── xr_helpers.hpp/.cpp           # waitFor*/artifact helpers (see §5)
    └── tests/
        └── xr/                      # test cases (all #ifdef WITH_XR_TESTS)
            ├── tests.hpp            # group header: TEST_GROUP_NAME "xr", GROUP_TEST_CASE_STORAGE xrTestCases
            ├── session.cpp          # xr_session_up, xr_runtime_absent
            ├── monitors.cpp         # xr_monitor_create_destroy, xr_config_declared, xr_mirror
            ├── anchors.cpp          # xr_anchor_transitions
            ├── input.cpp            # xr_ray_click_routing, xr_select_hysteresis, xr_two_hand_pointer, xr_scroll, xr_menu_right_click
            ├── ray_live.cpp         # xr_ray_hover (WP7 bounded live smoke test, added alongside input.cpp's WP12 suite)
            ├── grab_live.cpp        # xr_grab_move (WP8 bounded live smoke test — NOT in input.cpp, despite doc 07's WP12 deliverable list saying so)
            ├── idle.cpp             # xr_idle_inhibit
            └── teardown.cpp         # xr_disable_teardown
```

**As built (WP13 reconciliation):** the file layout above is the actual one — `xr_grab_move`
lives in its own `grab_live.cpp` (not folded into `input.cpp` as WP12's roadmap entry implied),
and `xr_ray_hover` (a WP7 deliverable, predating WP12's input suite) lives in `ray_live.cpp`.
`input.cpp` itself grew three more `TEST_CASE`s beyond the three originally scoped for WP12
(`xr_select_hysteresis`, `xr_two_hand_pointer`, `xr_menu_right_click`) — see §6's table for all
15. There is no dedicated integration (or unit) test for the layer-count cap policy
(`02-virtual-monitors.md` "Layer-count limit") — it is implemented and code-reviewed but has no
automated coverage as of WP13; a 16th-layer creation/suspension test remains a gap for a future
WP, not something this doc set can currently claim passes.

Group mechanism: copy `hyprtester/src/tests/main/tests.hpp` exactly — the group header
defines `TEST_GROUP_NAME "xr"` and `GROUP_TEST_CASE_STORAGE xrTestCases`
(`inline std::vector<std::shared_ptr<CTestCase>> xrTestCases;`), each `.cpp` includes it and
uses `TEST_CASE(name)` from `hyprtester/src/shared.hpp` (which registers into both the global
`testCases` map and `xrTestCases`). Assertion macros come from the same header:
`EXPECT(expr, val)`, `EXPECT_OK(x)` (= `EXPECT(x, "ok")`), `EXPECT_CONTAINS(haystack,
needle)`, `ASSERT*` variants that also return from the test, `OK(x)`.

### 2.1 How the XR suite runs (its own Hyprland + Monado instance)

hyprtester launches **one** Hyprland at startup (`launchHyprland` in
`hyprtester/src/main.cpp:67` — `CProcess` on the binary with `--config <path>` and
`addEnv("HYPRLAND_HEADLESS_ONLY", "1")`, then IPC via `getFromSocket()` from
`hyprctlCompat.hpp`). The XR runtime env (`XR_RUNTIME_JSON`) must be present in Hyprland's
environment *at launch*, and Monado must be up *before* Hyprland starts its session — so the
XR suite is a **separate invocation**, selected by a new `--xr` flag in `main.cpp`:

1. `--xr` implies: config defaults to `hyprtester/xr-test.conf` (instead of `test.lua`), the
   test list defaults to `xrTestCases` only, and the standard groups are not run.
2. Before `launchHyprland`, construct `MonadoOrchestrator` and start it (§3). If it reports
   `unavailable` (no `monado-service` binary), remember that: all tests except
   `xr_runtime_absent` will SKIP (§5.3), and Hyprland is launched **without**
   `XR_RUNTIME_JSON` so `xr_runtime_absent` can assert the graceful-unavailable path.
3. `launchHyprland` gains an optional env map parameter; `--xr` passes
   `XR_RUNTIME_JSON=<manifest>` (§3.2) plus the shared `XDG_RUNTIME_DIR` (§3.1).
4. Skip the `preTestCleanup()` steps that assume the standard config (plugin load stays; the
   Lua-dispatch cursor/workspace resets are harmless but reference the standard setup — guard
   them on `!xrMode` if they fail against `xr-test.conf`).

`xr-test.conf` is a **classic hyprlang** config (the `xrmonitor` keyword is registered in the
legacy config manager, `05-ipc-config.md` §2.1). Contents:

```ini
monitor = HEADLESS-1, 1920x1080@60, 0x0, 1

openxr {
    enabled = 1
}

# declared-set fixtures for xr_config_declared (unique names come from runtime creation;
# these two are static because the config file is static)
xrmonitor = XR-conf-a, 1280x720@60, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.0
xrmonitor = XR-conf-b, 1024x768,    anchor:head offset:0.3,-0.1,-1.0, size:0.5
```

### 2.2 Launch-mode fallback: nested Wayland (as built, WP13 reconciliation)

The stock `HYPRLAND_HEADLESS_ONLY=1` launch (§2.1 step 1) can fail to bring up Aquamarine's
headless backend in an isolated sandbox that has no seat — observed in this project's own dev
environment. `hyprtester/src/main.cpp`'s `runXrSuite()` handles this with a fallback, not just
a hard failure:

1. Attempt the stock headless-only launch in the isolated `XDG_RUNTIME_DIR` (§3.1) and wait for
   the instance to come up (`waitForHyprlandInstance`, 15 s budget).
2. On failure: kill the process, symlink the **host's** `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`
   socket (and its `.lock` file) into the isolated run directory, set `WAYLAND_DISPLAY` to that
   name in the child's environment, and relaunch **without** `HYPRLAND_HEADLESS_ONLY` (i.e.
   nested inside the host's own Wayland session) — up to 2 retries.

**This changes the base output's name.** Under the stock headless-only launch the base monitor
from `monitor = HEADLESS-1, ...` above is `HEADLESS-1`; under the nested fallback it is
`WAYLAND-1` instead (Aquamarine's nested-Wayland backend names its output that way, regardless of
what the config's `monitor =` line names). The config line's literal name (`HEADLESS-1`) then
matches no real output when the fallback triggers, so that line's mode/position settings are
effectively inert on a nested-fallback run — this is a known, currently-unaddressed gap in
`xr-test.conf`, not a bug in the fallback logic itself. It does not affect the XR test cases
themselves: every XR monitor the suite creates goes through `createXRMonitor`/`/output create
headless` (both backend-independent), and `xr_mirror`'s `HEADLESS-2` target is likewise created
dynamically. Don't assert a hard-coded `HEADLESS-1` in new XR tests; if a test needs the base
monitor's name, query it rather than assuming.

---

## 3. MonadoOrchestrator

Owns the `monado-service` process for the whole `--xr` run (started once in `main`, torn down
after `cleanupAndReport`). Monado source of truth: the `subprojects/monado` git submodule,
pinned to the same commit as the vendored wire structs (§4.2); build it once with
`scripts/build-monado.sh` (or `cmake --build build-debug --target monado`; service binary
lands at `subprojects/monado/build/src/xrt/targets/service/monado-service`). Header-only
build deps are vendored too: `subprojects/eigen` (3.4.0) and `subprojects/vulkan-headers`
(v1.4.350) — only the Vulkan ICD loader library remains a system dependency.

### 3.1 Launch

Binary resolution order: `$HYPRTESTER_MONADO_SERVICE` (explicit override) →
`<repo>/subprojects/monado/build/src/xrt/targets/service/monado-service` (the vendored
submodule build; repo root baked in via the `HYPRTESTER_SOURCE_ROOT` compile definition) →
`monado-service` in `PATH`. If none exist → orchestrator state `unavailable` (suite SKIPs,
§2.1). Machine-specific launch knobs stay out of the tracked config: the harness passes a
generated wrapper config that sources `xr-test.conf`, then an optional untracked
`hyprtester/xr-test-local.conf`, then `openxr:gpu = $HYPRTESTER_XR_GPU` when that env var is
set (dual-GPU boxes: Hyprland must use the render node Monado's compositor picks, or
xrCreateSwapchain crosses GPUs and crashes inside Monado — see §7).

**The env override is authoritative, not merely first-priority (as built, WP13
reconciliation):** if `$HYPRTESTER_MONADO_SERVICE` is set but does not resolve to a usable
binary, `MonadoOrchestrator::resolveBinary()` does **not** fall through to the build-tree path or
`PATH` — it fails immediately and the orchestrator reports `unavailable` (suite SKIPs). This is
deliberate (source comment: "an explicit override is authoritative: if set, we use it exclusively
— resolve or fail, no silent fallback to a different monado") and is also the supported way to
force the no-monado/SKIP leg of the suite in development: point the var at a nonexistent path.

Environment for the child (via `CProcess::addEnv`, same API `launchHyprland` uses):

| Env | Value | Why |
|---|---|---|
| `XRT_COMPOSITOR_NULL` | `true` | headless null compositor — no HMD, no window; still needs a Vulkan device |
| `P_OVERRIDE_ACTIVE_CONFIG` | `remote` | select the remote driver: devices are scripted over TCP instead of real hardware |
| `XRT_NO_STDIN` | `1` | don't block on the service's interactive stdin |
| `XDG_RUNTIME_DIR` | `<run-dir>` (isolated-but-shared, below) | where the Monado IPC socket lands; **must equal Hyprland-under-test's `XDG_RUNTIME_DIR`** |
| `VK_DRIVER_FILES` | *(optional)* lavapipe ICD json, forwarded from `$HYPRTESTER_VK_DRIVER_FILES` if set | force software Vulkan on machines where the real GPU misbehaves; the null compositor is content with lavapipe |

**Isolated-but-shared `XDG_RUNTIME_DIR`**: create a fresh directory per run,
`/tmp/hyprtester-xr-<pid>/` (mode 0700), and set it as `XDG_RUNTIME_DIR` for **both**
`monado-service` and the launched Hyprland. Shared because the OpenXR client (inside
Hyprland) finds Monado's IPC socket at `$XDG_RUNTIME_DIR/monado_comp_ipc`; isolated so a
developer's real session (their own Hyprland socket, possibly their own Monado) is never
touched and concurrent runs can't collide. Stdout/stderr of the service are redirected to
`<run-dir>/monado.log` for artifact capture (§5.2).

### 3.2 Readiness

Poll every 100 ms, **10 s total timeout**, both conditions:

1. `connect()` on the unix socket `$XDG_RUNTIME_DIR/monado_comp_ipc` succeeds (then close —
   this only proves the service is accepting).
2. A TCP `connect()` to `127.0.0.1:4242` is **accepted** (the remote driver's scripting
   port; see §4). Keep this one — it becomes the `RemoteClient` connection.

On timeout: kill the service, mark `unavailable` (SKIP path), dump `monado.log` tail to the
test log. The runtime manifest handed to Hyprland as `XR_RUNTIME_JSON` is Monado's build-tree
manifest: `<monado-build>/openxr_monado-dev.json` (generated by Monado's build; verify the
name in the pinned checkout — it points the OpenXR loader's `active_runtime` resolution at
the just-started service).

### 3.3 Teardown

`SIGTERM` to the service pid → wait up to 3 s → `SIGKILL` + `waitpid`. Remove
`<run-dir>` unless any test failed (then leave it for inspection and print its path).
Teardown runs even when tests fail (hook it next to the existing
`kill(hyprlandProc->pid(), SIGKILL)` in `cleanupAndReport`).

---

## 4. RemoteClient and the vendored wire header

### 4.1 What gets vendored, and why

The remote driver's wire protocol is defined in
`subprojects/monado/src/xrt/drivers/remote/r_interface.h`. We do **not** link against
Monado (that would drag its headers/libs into hyprtester's build for everyone). Instead,
vendor the POD wire structs into `hyprtester/src/xr/monado_remote_wire.hpp`, **pinned to
Monado commit `c2ddab59dc41366fe520dc4e8abcfea257ecf0b8`** (record the commit hash in the
header's top comment; re-pin deliberately, never silently).

Vendor, translated to self-contained C++ (no Monado includes — that means also inlining the
handful of `xrt_defines.h` PODs the wire structs embed):

- `xrt_vec1 {float x;}`, `xrt_vec2 {float x,y;}`, `xrt_vec3 {float x,y,z;}`,
  `xrt_quat {float x,y,z,w;}`, `xrt_pose {xrt_quat orientation; xrt_vec3 position;}`,
  `xrt_fov {float angle_left, angle_right, angle_up, angle_down;}`
- `struct r_remote_controller_data` — exactly as in `r_interface.h:74-107`: `pose`,
  `linear_velocity`, `angular_velocity`, `float hand_curl[5]`, analogs (`trigger_value`,
  `squeeze_value`, `squeeze_force`, `thumbstick` (vec2), `trackpad_force`, `trackpad`
  (vec2)), then the bool block: `hand_tracking_active`, `active`, `system_click`,
  `system_touch`, `a_click`, `a_touch`, `b_click`, `b_touch`, `trigger_click`,
  `trigger_touch`, `thumbstick_click`, `thumbstick_touch`, `trackpad_touch`, `_pad0..2`
  (the comment in the source: "active(2) + bools(11) + pad(3) = 16" — the padding bools are
  load-bearing for layout, keep them).
- `struct r_head_data` — `views[2]` of `{xrt_fov fov; xrt_pose pose; uint32_t _pad;}`
  (48 bytes each per the source comment), then `xrt_pose center` (the OpenXR view space),
  `bool per_view_data_valid`, `bool _pad0,_pad1,_pad2` ("pose(16+12) bool(1) + pad(3) = 32").
- `struct r_remote_data` — `uint64_t header; r_head_data head; r_remote_controller_data
  left, right;` — this is the one struct written per tick.
- The magic: `#define`d in Monado as `R_HEADER_VALUE (*(uint64_t *)"mndrmt3\0")` — i.e. the
  little-endian u64 of the bytes `m n d r m t 3 \0`. Vendored as
  `constexpr uint64_t R_HEADER_VALUE = /* bytes "mndrmt3\0" */ 0x0033746D72646E6DULL;`
  (verify the constant with a `static_assert(std::bit_cast<...>)`-style check or a memcpy
  comparison against the string literal at compile/startup — do not hand-trust the hex).

Add `static_assert`s on the expected sizes computed from the pinned layout —
`sizeof(r_remote_controller_data) == 120`, `sizeof(r_head_data) == 128`,
`sizeof(r_remote_data) == 376` — **verify these numbers against the pinned checkout when
implementing** (e.g. a throwaway TU compiled against the real headers, or `pahole`); if they
differ, the vendored translation has a layout bug, fix it before anything else. These asserts
are the compile-time half of ABI-drift protection; the runtime half is the handshake below.

### 4.2 Connect handshake and ABI-drift → SKIP

The Monado remote hub (`r_hub.c` in the same directory) exchanges fixed-size
`r_remote_data` packets over the accepted TCP connection; `r_interface.h` exposes exactly
`r_remote_connection_read_one` / `r_remote_connection_write_one` (blocking full-struct
reads/writes). `RemoteClient::connect()`:

1. Take over the accepted TCP socket from the orchestrator (§3.2).
2. Read one full `r_remote_data` (the hub sends its current state to a new connection —
   **verify empirically at implementation time**; if it does not, fall back to step 3
   immediately and validate via the write path + a subsequent read).
3. Validate: `data.header == R_HEADER_VALUE` and the read delivered exactly
   `sizeof(r_remote_data)` bytes (a short/misaligned stream means struct-size drift).
4. **On any mismatch: mark the whole suite SKIP, not FAIL** — the vendored header is pinned
   to `c2ddab59dc41` and a newer installed Monado may legitimately have evolved the wire
   struct (plan risk #4). Emit
   `# SKIP monado remote wire ABI mismatch (vendored @c2ddab59, service reports otherwise)`
   for each test. A wire mismatch is a maintenance task (re-pin + re-vendor), not a Hyprland
   regression.

Keep the initial struct from step 2 as the client's current-state template: **usage is
read-modify-write-one-struct-per-tick** — mutate fields, set `header = R_HEADER_VALUE`,
`write_one`. Every write is a complete device snapshot; there are no deltas.

### 4.3 Device model and helper methods

With `P_OVERRIDE_ACTIVE_CONFIG=remote` the devices enumerate as **Valve Index controllers**
(`valve/index_controller` interaction profile) plus an HMD — which is why `04-input.md`'s
binding table includes valve/index: it is the profile the test suite exercises. Hand presence
is emulated via `hand_curl[5]` + `hand_tracking_active`; `active` toggles controller
presence entirely.

`RemoteClient` sketch (all setters mutate the template; only `pulse()` performs I/O):

```cpp
class RemoteClient {
  public:
    bool  connectAndValidate(int fd);           // §4.2; false => suite SKIP
    void  setHeadPose(Vec3 pos, Quat q);        // head.center; leave per_view_data_valid=false
    void  setControllerPose(Side s, Vec3 pos, Quat q);
    void  setControllerActive(Side s, bool active);
    void  setTrigger(Side s, float v);          // trigger_value.x (+ trigger_click at >=0.9 for realism)
    void  setSqueeze(Side s, float v);          // squeeze_value.x
    void  setThumbstick(Side s, float x, float y);
    void  setHandCurl(Side s, float curl);      // fills hand_curl[0..4], hand_tracking_active=true
    void  pulse();                              // header=R_HEADER_VALUE; write_one(struct)
    // convenience for scripted motion:
    void  animate(std::function<void(r_remote_data&, float t01)> f, std::chrono::milliseconds dur, int hz = 60);
};
```

`animate` is the workhorse for `xr_anchor_transitions` and `xr_grab_move`: it interpolates,
calling `pulse()` at ~60 Hz so the runtime sees continuous motion (a single teleport-jump
write is valid too, but leash-spring tests need a time series).

---

## 5. xr_helpers — waiting, asserting, artifacts, naming

### 5.1 Wait/assert split (omedora convention)

Never assert on asynchronous state directly; wait for it first, then assert. All waits poll
`getFromSocket()` (hyprtester's IPC channel) — state assertions over IPC, not pixels:

```cpp
// polls `hyprctl -j openxr` (i.e. getFromSocket("j/openxr")) until predicate or timeout
bool waitForJson(const std::string& cmd, std::function<bool(const std::string&)> pred,
                 std::chrono::milliseconds timeout = 5000ms, std::chrono::milliseconds interval = 100ms);

// specialization: state field of the openxr status JSON (05-ipc-config.md §4.3)
bool waitForXrState(const std::string& state, std::chrono::milliseconds timeout = 10000ms);
```

Typical use: `ASSERT(waitForXrState("focused"), true);` then `EXPECT_CONTAINS(...)` on the
settled JSON. Numeric pose assertions parse the JSON and use `EXPECT_MAX_DELTA` (already in
`shared.hpp`) with generous tolerances (leash convergence: 5 cm / 3°).

### 5.2 Artifact capture on failure

`dumpXrArtifacts(const std::string& testName)` — called by every test on its failure paths
(wrap in a small RAII guard that checks `this->failed` in its destructor). Writes to
`hyprtester/artifacts/<run-id>/<testName>/` where `<run-id>` = `xr-<pid>-<unixtime>`:

- `monitors.json` — `getFromSocket("j/monitors")`
- `openxr.json` — `getFromSocket("j/openxr")`
- `monado.log` — tail (last 200 lines) of `<run-dir>/monado.log` (§3.1)
- `hyprland.log` — tail of the Hyprland log
  (`$XDG_RUNTIME_DIR/hypr/$HIS/hyprland.log` under the run's `XDG_RUNTIME_DIR`)

A green run writes nothing (omedora rule). Print the artifact path in the failure message.

**Known latent gap (as built, WP13 reconciliation):** the compositor's own log filename depends
on the build type — `src/debug/log/Logger.cpp` writes to `hyprlandd.log` when `HYPRLAND_DEBUG`
is defined (true CMake `Debug` builds) and `hyprland.log` otherwise. The exact `--xr` build
command in §7 below does not pass `-DCMAKE_BUILD_TYPE=Debug` (a `build-debug`-named directory is
not itself a `Debug` build unless you also set that flag), so under that command the filename is
`hyprland.log` and `dumpXrArtifacts` reads the right file. `dumpXrArtifacts`
(`hyprtester/src/xr/xr_helpers.cpp`) hard-codes `"/hyprland.log"` unconditionally, though — if
`--xr` is ever run against a genuine `CMAKE_BUILD_TYPE=Debug` build, the real file is
`hyprlandd.log` and artifact capture will silently miss the compositor log. This was flagged as a
"WP6 bug to check" going into WP13 and remains unfixed as of this reconciliation pass: prefer a
non-Debug (Release/RelWithDebInfo, i.e. the plain `--xr` build command below) build for `--xr`
runs, or check both filenames by hand when debugging a Debug-build failure.

### 5.3 SKIP handling and TAP

hyprtester's reporting is pass/fail line-based (`runTests` in `main.cpp`). For the XR suite,
tests that cannot run (orchestrator `unavailable`, or ABI-mismatch §4.2) call a helper that
logs `SKIP: <name> — <reason>` via `NLog::yellow` and returns without touching `failed` —
i.e. a SKIP counts as a pass in the summary, with the reason visible in the log. Emit a
TAP-style line (`ok - <name> # SKIP <reason>`) in the same log call so external harnesses
can grep it.

### 5.4 Unique per-run names

Every monitor created *by a test at runtime* uses `XR-t<pid>-<n>` (pid of hyprtester,
`n` = per-test counter). Rationale (omedora): re-runs against a half-torn-down instance, or
two developers on one machine, must never trip "Name already taken". The two config-declared
monitors (`XR-conf-a/b`) are exempt — the config file is static, and the instance is
launched fresh per run.

---

## 6. Integration test cases

All tests below: group `xr`, `TEST_CASE(<name>)`. Precondition for all but
`xr_runtime_absent`: orchestrator available (else SKIP §5.3), and an implicit
`waitForXrState("focused")` gate — **with one caveat**: `xrSyncActions` only returns real
input when the session reaches FOCUSED (plan risk #3). WP10 must empirically verify the
remote driver + null compositor drives the session to FOCUSED; if it plateaus at VISIBLE,
the input-dependent tests (`xr_ray_click_routing`, `xr_grab_move`, `xr_scroll`) gate on
`waitForXrState("visible")` and are marked expected-SKIP until focused is achievable — the
non-input tests are unaffected.

Note for any test reading `anchor.pose` from `j/openxr` (tests 4, 6, and any future ones): per
`05-ipc-config.md` §4.3, head/body/device anchor modes report their **configured offset** over
JSON in the normal case, and only switch to the **live world-composed pose** while the monitor is
grabbed (`grabbed: true`) — this applies to all four modes, not just `local`. Assertions on
`anchor.pose` for a leashed/device monitor that isn't grabbed should expect the stored offset, not
a pose that tracks the head/body/grip frame-by-frame.

| # | Test | Steps | Assertions |
|---|---|---|---|
| 1 | `xr_session_up` | Just the gate. | `waitForXrState("focused")` (or `visible`, caveat above); `j/openxr` has non-empty `runtimeName` containing `Monado`; `openxrsessionstate`/`openxractive` events observed if a socket2 listener is attached (optional v1). |
| 2 | `xr_monitor_create_destroy` | `dispatch xrmonitor create XR-t<pid>-1 1280x720` → wait for it in `j/openxr` monitors[] → `dispatch xrmonitor destroy XR-t<pid>-1` → wait gone. | create returns ok; `j/monitors` contains the headless output; `j/openxr` entry has `size_m` ≈ `openxr:default_size`; after destroy both listings drop it. (Socket2 `xrmonitoradded`/`xrmonitorremoved` assertions optional v1.) |
| 3 | `xr_config_declared` | Launched config declares `XR-conf-a/b` (§2.1). Then `keyword`-append is not possible for keywords, so reconciliation is tested via `getFromSocket("/reload")` after swapping the config file variant is out of scope v1 — instead: verify initial declare, then `dispatch xrmonitor create XR-t<pid>-2`, then `/reload`. | Both declared monitors exist with correct anchors (`anchor.mode` `local`/`head` in JSON); after reload both still exist (idempotent reconcile, no destroy/create flicker — same `id`); the runtime-created `XR-t<pid>-2` **survives the reload untouched** (reconcile ignores runtime monitors, `05-ipc-config.md` §2.5). |
| 4 | `xr_anchor_transitions` | Create `XR-t<pid>-3`. `xrmonitor anchor <name> head` → RemoteClient `animate` a 90° head yaw over 2 s → wait 2×`leash_response` → read pose. Then `anchor <name> local`, yaw head back. | After head-yaw: monitor's world pose has re-converged in front of the view (JSON pose within 5 cm/3° of expected leashed target — leash convergence). After `anchor local`: pose frozen (head motion no longer moves it); `anchor.mode` flips in JSON; `xrmonitoranchor` event payload `<name>,local`. |
| 5 | `xr_ray_click_routing` | Create two monitors side by side (`XR-t<pid>-4/5`, local anchors 1 m apart). Aim right controller ray at a known UV of monitor 5 (compute controller pose from the known quad pose, `03/04` math), `setTrigger(0.8)` pulse, then `setTrigger(0.2)`. | Focused monitor becomes 5's output (`j/monitors` `focused: true`); `hyprctl cursorpos` maps to the expected pixel (UV × mode) within a few px; monitor 4 not focused. Button press/release observed via focus/activation of a test client if one is spawned on that output (optional strengthening). |
| 6 | `xr_grab_move` | Create `XR-t<pid>-6`. Aim at it, `setSqueeze(0.8)` (grab), `animate` a 0.5 m controller translation, `setSqueeze(0.2)` (release). | During grab: `j/openxr` shows `grabbed: true` (poll mid-animation); after release: `grabbed: false` and the monitor's `anchor.pose.pos` moved by ≈0.5 m (±5 cm) in the translated direction; `xrmonitorgrab` `<name>,1` then `<name>,0`. |
| 7 | `xr_scroll` | Spawn a scrollable client on an XR monitor (reuse hyprtester's kitty spawn on that output), aim ray at it, `setThumbstick(0, ±0.8)` for N pulses. | Axis events reach the client — assert indirectly: with `openxr:scroll_speed` doubled via `keyword`, the same pulse count produces proportionally more scroll (or v1-minimal: no error + hover monitor stays, plus `j/openxr` hovered flag true; strengthen later with the pointer-scroll test client `hyprtester/clients/pointer-scroll`). |
| 8 | `xr_idle_inhibit` | Needs a small idle client: add `clientNew("idle-notify" PROTOS "ext-idle-notify-v1")` to `hyprtester/CMakeLists.txt` — requests an idle notification with a 1 s timeout, obeying inhibitors, prints `idled`/`resumed` lines. Run it with XR focused; then `keyword openxr:inhibit_idle 0`; then restore. | With `inhibit_idle=1` + state focused: no `idled` within 3 s. With `inhibit_idle=0`: `idled` arrives (idle not inhibited — no real input flowing). Restore → `resumed` on next XR input pulse (activity-for-free path, `05-ipc-config.md` §6.4). |
| 9 | `xr_mirror` | Create `XR-t<pid>-7`, then `keyword monitor HEADLESS-2, preferred, auto, 1, mirror, XR-t<pid>-7` (zero-new-code mirroring via `CMonitor::setMirror`, `02-virtual-monitors.md`). | `j/monitors`: HEADLESS-2 reports `mirrorOf` = the XR monitor; unset (`keyword monitor HEADLESS-2, preferred, auto, 1`) restores it. |
| 10 | `xr_disable_teardown` | `hyprctl openxr disable` → wait `disabled`; then `hyprctl openxr enable` → wait up again. Run with one runtime-created monitor alive and `destroy_monitors_on_stop = 1`. | After disable: state `disabled`; the XR monitor is gone from `j/monitors` (destroy_monitors_on_stop); Hyprland alive and responsive (no crash on teardown — the whole point of the full state machine, `01-session-graphics.md`). After enable: state returns to `focused`/`visible`; declared monitors re-materialize with quads bound. |
| 11 | `xr_runtime_absent` | **Runs when the orchestrator is `unavailable` OR in a dedicated sub-invocation without `XR_RUNTIME_JSON`** (§2.1 step 2): Hyprland launched with `openxr:enabled=1` but no runtime manifest. | `waitForXrState("unavailable")`; `hyprctl openxr` returns `state: unavailable` with empty runtime fields; `hyprctl openxr enable` returns a clean error (not a crash); compositor fully functional (run one trivial non-XR IPC assertion). This is the graceful-degradation contract of `00-overview.md`. |

**As built, four more tests exist beyond the eleven above (WP13 reconciliation) — the implemented
suite is 15 tests, not 11:**

| # | Test | Steps | Assertions |
|---|---|---|---|
| 12 | `xr_ray_hover` (WP7, `ray_live.cpp`) | Bounded live smoke test predating the WP12 input suite: activate the left controller, sweep a small bracket of plausible poses aimed at a freshly-created quad. | Hover registers in `j/openxr` (name-scoped, §5.1-style helper — the WP13 fix in this reconciliation pass, see A.2). SKIPs (not fails) if hover never registers within budget — this test proves the pointer *can* work, it is not a strict gate; see the doc's dual-GPU/Monado-interop appendix below. |
| 13 | `xr_select_hysteresis` (WP12, `input.cpp`) | Doc 04 §4 Schmitt-trigger coverage: drive `trigger_value` across the press (0.7) and release (0.4) thresholds while hovering, using a spawned pointer-scroll test client's button observability (hover alone can't distinguish "hovering" from "clicked"). | Press edge at/above 0.7 registers a button press on the client; release edge at/below 0.4 registers release; jitter between the thresholds does not double-fire. |
| 14 | `xr_two_hand_pointer` (WP12, `input.cpp`) | Doc 04 §3 two-hand ownership: hover monitor A with the left hand, then produce a hover change with the right hand onto monitor B. | Pointer ownership (and `hovered`) transfers to the last-active hand; A stops being reported hovered once B is; transferring back to the left hand on A re-asserts A. |
| 15 | `xr_menu_right_click` (WP12, `input.cpp`) | Doc 04 §1.2/§4: the `menu` action (bound to `a/click` on valve/index, the test profile) maps to a `BTN_RIGHT` edge. | A `menu` press/release edge while hovering produces a right-click on the spawned test client, verified via the same button-observability path as `xr_select_hysteresis`. |

**A 16th test exists for the overlay/ambient-background integration (WP-P5) — the implemented
suite is 16 tests:**

| # | Test | Steps | Assertions |
|---|---|---|---|
| 16 | `xr_overlay_composition` (WP-P5, `overlay.cpp`) | **Opt-in**, gated on `$HYPRTESTER_HYPXRPAPER` (path to a `hypxrpaper` binary; unset/invalid → SKIP, never fail). Wait for the default (exclusive) session to reach focused, then: `/openxr disable` → launch `hypxrpaper` in gradient-panorama mode (no args, no scene asset) as the **primary** OpenXR session (PID-tracked, same `XR_RUNTIME_JSON`/runtime dir as the suite's Monado, `--gpu $HYPRTESTER_XR_GPU` forwarded) → `keyword openxr:overlay 1` → `/openxr enable`. Restore on exit (RAII guard): `disable` → `keyword openxr:overlay 0` → `enable` → kill hypxrpaper by PID. | HypXRland re-reaches `focused` as an `XR_EXTX_overlay` session; `j/openxr` reports `"overlay": true` and still names `Monado`; the declared fixture monitors are bound (`"monitors": [ ... "name": ...]`). `openxr:overlay` is read in `COpenXRManager::start()` (which `/openxr enable` calls directly), so a plain `keyword`-set before `enable` applies it — **no `parseKeyword` special-case is needed** (unlike the `openxr:enabled`/`inhibit_idle` hot-toggles). All phases are bounded (≤20 s) and SKIP on env flake per suite convention. |

There is no dedicated 16-layer-cap integration test (§2's directory-layout note above) and no
unit test for the layer-count-limit policy either — a gap, not an oversight to paper over.

---

## 7. Environment notes: dual-GPU interop (as built, WP13 reconciliation)

Findings from running the `--xr` suite on a dual-GPU dev box (NVIDIA + AMD), recorded here so
they aren't re-discovered from scratch. None of this is Hyprland-side application logic — it's
runtime/driver behavior that shapes how to run and interpret the suite on this class of machine.

- **Monado's null compositor picks a GPU independently of Hyprland.** On a machine with an
  NVIDIA render node (`renderD128`) and an AMD one (`renderD129`), `monado-service`'s null
  compositor selects NVIDIA/Vulkan for its `XRT_COMPOSITOR_NULL` device regardless of which GPU
  Hyprland itself is rendering on.
- **`openxr:gpu` must be pinned to match Monado's GPU on this class of setup.** `01-session-
  graphics.md`'s default ("match Hyprland's primary GPU render node") picks the *wrong* device
  when Hyprland's primary GPU differs from Monado's — the resulting cross-GPU dmabuf import
  crashes inside Monado/Mesa at `xrCreateSwapchain` (the exact interop risk `01-session-
  graphics.md`'s "GPU selection" section's historical-rationale comments describe). `xr-test.conf`
  pins `openxr:gpu = /dev/dri/renderD128` (NVIDIA, matching Monado) specifically for this reason
  — see the comment in that file.
- **Known Monado/Mesa defects, runtime-side, not Hyprland bugs:** even with the GPU pinned
  correctly (no swapchain crash, session reaches FOCUSED, quads render), long-running sessions on
  this environment have been observed to spam `client_egl_insert_fence Failed` and, eventually, a
  `corrupted double-linked list` heap-corruption abort during teardown. This is inside
  Monado/Mesa, not `src/openxr/` — the WP3/WP10 implementation reports gdb-verified valid call
  inputs (owned GBM display, correct format, correct size) at the crash site. **Practical
  consequence for testing: keep individual XR sessions short.** The bounded-time, SKIP-not-FAIL
  posture of `xr_ray_hover`/`xr_grab_move` (§6, tests 12 and 6) and the suite's overall design
  (short-lived per-test sessions rather than one long-running session) already follow this
  advice; don't add tests that hold a session open for extended periods on this class of
  environment.
- **Debugging tip:** the `hyprctl` binary in a `build-debug` tree can be a stale directory
  artifact (pre-existing, unrelated build break) rather than the real CLI. Prefer raw socket IPC
  (`echo -n "j/monitors" | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HIS/.socket.sock`) or the
  system `hyprctl` for manual debugging outside the test harness.

---

## 8. CI posture and local invocation

- **CI**: only the unit tier. `tests/xr/*.cpp` ride the existing `hyprland_gtests` target —
  zero new CI configuration. The integration tier is **not** wired into CI initially
  (omedora L4 lesson: a runtime-and-driver-dependent tier becomes a flaky per-PR gate;
  keep it a local/pre-release gate until it has a stable track record — then consider a
  self-hosted runner).
- **Local invocation** (exact):

```sh
# 1. Build Hyprland + hyprtester with the XR suite compiled in
cmake -B build-debug -DWITH_TESTS=ON -DWITH_XR_TESTS=ON
cmake --build build-debug --target Hyprland hyprtester

# 2. Have Monado available: build the pinned submodule (one-time)
scripts/build-monado.sh
#    ...or have monado-service in PATH, or point at one explicitly:
export HYPRTESTER_MONADO_SERVICE=/path/to/monado-service
# optional, software Vulkan:
export HYPRTESTER_VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
# optional, opt into the overlay/ambient-background test (xr_overlay_composition):
#   point at a hypxrpaper binary (see hypxrpaper's own README to build it). When unset,
#   that single test SKIPs (counts as a pass); everything else is unaffected.
export HYPRTESTER_HYPXRPAPER=/path/to/hypxrpaper

# 3. Run from the hyprtester directory (hyprtester requirement)
cd hyprtester
../build-debug/hyprtester/hyprtester --xr --binary ../build-debug/Hyprland
# subset:
../build-debug/hyprtester/hyprtester --xr --binary ../build-debug/Hyprland xr_session_up xr_grab_move
```

(Adjust the hyprtester binary path to wherever the build places it; `--config` defaults to
`hyprtester/xr-test.conf` under `--xr`.)

---

## 9. Containerized dev/test environment

A rootless-podman Arch+systemd container (`containers/`, driven by
`scripts/xr-container.sh`) runs the whole XR stack in **full session/input/GPU
isolation** — no host DBus, no `/dev/input`, one GPU by construction. It boots
real PID-1 systemd, installs a curated Omarchy desktop, and builds the dev
Hyprland + hyprtester + vendored Monado into a volume (`/build`) so the host tree
is never touched. See [`containers/README.md`](../../containers/README.md) for the
image/build details; this section is the testing-oriented view.

### Topologies

| Invocation | XR runtime | Nesting host | Host mounts | Use |
| --- | --- | --- | --- | --- |
| `test` | vendored Monado **null**, in-container | headless **labwc** (in-container) | **none** (hermetic) | CI-style suite run |
| `session` (windowed) | vendored **windowed** Monado, in-container | host wayland socket (nested window) | wayland socket only | interactive dev, no headset |
| `session --wivrn` | **host WiVRn** runtime | host wayland socket | wayland socket + `/usr/lib/wivrn` + manifest + `wivrn/comp_ipc` | real Quest 3 (default `--gpu split`) |

### The three invocations

```sh
scripts/xr-container.sh test --gpu amd            # hermetic hyprtester --xr, AMD
scripts/xr-container.sh test --gpu nvidia         # …on NVIDIA via CDI
scripts/xr-container.sh session                   # windowed Monado desktop (no headset)
scripts/xr-container.sh session --wivrn           # real headset (default --gpu split)
scripts/xr-container.sh check-gpu --gpu nvidia    # eglinfo/vulkaninfo smoke test
```

GPU selection is by **vendor scan** (`scripts/lib/gpu.sh`), not hardcoded node
names — `--gpu split|amd|nvidia|intel` or an explicit `/dev/dri/renderD*`. The
node is resolved host-side (for `--device`) and again **inside** the container
(for the `openxr:gpu` pin), so a CDI-injected NVIDIA node is verified present
rather than assumed. See [`containers/README.md` § GPU](../../containers/README.md#gpu).

**Split-GPU (`--gpu split`, the dual-GPU `--wivrn` default).** On a machine
where the host compositor and WiVRn's encoder live on **different** GPUs, no
single GPU works: the nested compositor must render on the host-compositor GPU
(`AQ_DRM_DEVICES`) while XR encodes on the other (`openxr:gpu`). `--gpu split`
exposes both GPUs to the container and plumbs the two roles separately —
`--nested-gpu` (default `host` = first non-NVIDIA node) and `--xr-gpu`
(default `nvidia`). This is exactly the native preview's topology (host Hyprland
on AMD, `openxr:gpu` on the NVIDIA node). It degrades to single-GPU on a
one-GPU box.

### Validation matrix (observed on the dual-GPU dev laptop: AMD 890M iGPU + RTX 5070)

| Path | `--gpu amd` | `--gpu nvidia` (CDI) |
| --- | --- | --- |
| `check-gpu` | AMD ICD (radv/radeonsi) | NVIDIA ICD (RTX 5070 EGL + Vulkan) — CDI end-to-end |
| `test` (hermetic suite) | **green** (18/18, ×2) | 1 deterministic fail (`xr_mirror`) + flaky input SKIPs |
| `session --wivrn` | *(single-GPU pin, kept for experiments)* nested backend up → cross-GPU swapchain SEGV (WP3) | *(single-GPU pin)* nested `CBackend::create()` fails on NVIDIA (see below) |
| `session --wivrn` (default **`--gpu split`**) | nested compositor on AMD (`AQ_DRM_DEVICES`) + XR encode on NVIDIA (`openxr:gpu`, CDI) — the working dual-GPU topology (see below) ||

**NVIDIA hermetic finding.** On NVIDIA the nested-into-labwc topology hits an
aquamarine GBM limitation — `GBM: Failed to allocate a GBM buffer: bo null …
format XR24` on the NVIDIA node — so `xr_mirror` (which stands up a `HEADLESS-2`
mirror output) fails deterministically, and the input tests flake (nested
compositor buffer pressure). This is an NVIDIA-driver/aquamarine GBM constraint in
the nested-wlroots-style topology, **not** compositor application logic. **AMD is
the reliable hermetic GPU** (18/18 twice); use `--gpu nvidia` for `check-gpu`
(proving the CDI ICDs) but prefer AMD for the suite on this class of machine.

**`session --wivrn` finding (dual-GPU laptop).** A *single-GPU* `--wivrn` run is
**blocked by topology, not by WP4**: the nested container Hyprland renders its flat
window into the host compositor and so must render on the *host's* GPU (AMD 890M
here) — nesting on NVIDIA fails at `CBackend::create()`. With `--gpu amd` the
nested backend comes up and reaches WiVRn (as in WP3) but SEGVs at the cross-GPU
swapchain. No single GPU satisfies both the nested-window path (host GPU) and
WiVRn's encode path (NVIDIA).

The fix (this WP) is **`--gpu split`**, now the `--wivrn` default: expose both
GPUs and assign them separately — nested compositor (`AQ_DRM_DEVICES`) on the host
GPU (AMD), XR encode (`openxr:gpu`) on the encode GPU (NVIDIA, via CDI). This is
the identical topology the *native* preview already runs (host Hyprland on AMD +
`openxr:gpu=<NVIDIA node>`), so the cross-GPU XR blit is the proven native path,
not a new one. Roles are overridable (`--nested-gpu`/`--xr-gpu`); the plumbing
degrades to single-GPU on a one-GPU machine. A single-GPU `--gpu` pin with
`--wivrn` is retained for experiments but warns. The NVIDIA CDI plumbing is
independently proven by `check-gpu` (full RTX 5070 EGL + Vulkan inside the
container).

### Hermetic vs host suite, and the 4242 story

- **Hermetic (`test`)** is the preferred way to run the suite: it uses its **own
  network namespace** and vendored Monado, touching no host sockets, so it can run
  alongside a live host session safely. This is the container's reason to exist.
- **The host suite** (running `hyprtester --xr` natively, §8) and the container's
  **windowed `session`** both involve a Monado **remote driver on TCP 4242** —
  and there is only **one** 4242 per box. A container that *publishes*
  `127.0.0.1:4242` will collide with a host Monado and poison a concurrent host
  suite run. Therefore windowed `session` **does not publish 4242 by default**;
  opt in with `--publish-remote` (ephemeral free host port, printed in the banner)
  or `--publish-remote=PORT`. Rule of thumb: **serialize** anything that binds
  4242 — a host suite run and a port-publishing container session must not overlap.

---

## Context files to read before implementing

- `docs/openxr/00-overview.md` — lifecycle states (the strings `waitForXrState` matches), build gating
- `docs/openxr/04-input.md` — bindings (valve/index = the tested profile), hysteresis values the input tests exercise
- `docs/openxr/05-ipc-config.md` — the `hyprctl openxr` JSON schema all assertions parse; event names; xr-test.conf keyword syntax
- `hyprtester/src/main.cpp` — `launchHyprland`, settings parsing (where `--xr` goes), `runTests`, `cleanupAndReport`
- `hyprtester/src/shared.hpp` — `TEST_CASE` / `EXPECT*` / `ASSERT*` / `OK` macros, group storage mechanism
- `hyprtester/src/tests/main/tests.hpp` — the group-header pattern to copy for `tests/xr/tests.hpp`
- `hyprtester/src/hyprctlCompat.hpp` — `getFromSocket`, `instances()`
- `hyprtester/CMakeLists.txt` + root `CMakeLists.txt` (~line 675–700) — build wiring, where `WITH_XR_TESTS` lands
- `subprojects/monado/src/xrt/drivers/remote/r_interface.h` @ `c2ddab59dc41366fe520dc4e8abcfea257ecf0b8` (the submodule pin) — the structs to vendor, read it whole before writing `monado_remote_wire.hpp`
- `/home/ajg/code/omedora-4/omedora/testing.md` — the TAP/wait-assert/artifact conventions this suite adopts
