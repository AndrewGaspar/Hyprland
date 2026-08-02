# 06 — Testing

This page is a developer's guide to how the OpenXR extension is tested today. There are two
tiers, and one containerized runner that ties them together:

1. **Unit tier** — GoogleTest cases over the extension's pure functions (parsing, anchor math,
   ray/quad intersection, blend-mode selection, chrome/grab classifiers, and more). Compiled
   unconditionally into the existing `hyprland_gtests` target; needs no OpenXR runtime and runs
   on every test build.
2. **Integration tier** — the `hyprtester --xr` suite, which brings up a real (headless) Monado
   session, drives scripted head/controller input over Monado's remote driver, and asserts
   against the live compositor via `hyprctl openxr` JSON and socket2 events. Gated behind a
   CMake option and a local Monado build; skips cleanly when no Monado is present.

The **containerized hermetic suite** (`scripts/xr-container.sh test`) runs the integration tier
inside a rootless-podman container with its own GPU, network namespace, and vendored Monado. It
is the preferred, reproducible way to run the suite, because it touches no host sockets and can
run alongside a live desktop session.

Behavior under test is specified in the sibling docs: the session/graphics lifecycle (00, 01),
virtual monitors (02), anchoring math (03), input (04), and the IPC/config surface — the
`hyprctl openxr` JSON schema and event names every integration assertion parses — in the
configuration reference (05).

---

## 1. Unit tier — GoogleTest

The unit tests live in `tests/xr/*.cpp` and are picked up automatically by the root
`CMakeLists.txt` (`file(GLOB_RECURSE TESTFILES "tests/*.cpp")`), compiled into the
`hyprland_gtests` executable and linked against `hyprland_lib` + `GTest::gtest_main`, then
registered with `gtest_discover_tests`. They exercise code that is compiled **unconditionally**
— pure functions with no `HAVE_OPENXR` guard and no OpenXR headers — so they build and run even
on a tree configured without an OpenXR SDK. They are part of the normal `-DWITH_TESTS=ON` test
build and carry no extra dependencies.

Each file follows the standard gtest style used elsewhere in `tests/` (`TEST(Group, Case) { … }`,
including the header under test by its `src/`-relative path). Coverage as it stands:

| File | Area under test |
|---|---|
| `tests/xr/parser.cpp` | `parseXRMonitorLine` grammar: the documented examples, defaults, all anchor modes, adaptive-radius validation, and malformed-input errors. |
| `tests/xr/anchor_math.cpp` | `src/openxr` anchor math + `XRMath.hpp`: critically-damped spring convergence and large-dt stability, yaw-frame extraction (incl. near-vertical gaze), positional/angular deadzones, grab offset round-trip, resize clamping, reference-space change, and re-center/recenter behavior. |
| `tests/xr/ray_intersect.cpp` | ray–quad intersection: hit UV in [0,1]², misses (behind/parallel/out of bounds), nearest-t across overlapping quads, rotated quads, and slack expansion. |
| `tests/xr/blendmode.cpp` | `OpenXR::pickBlendMode`: `auto`/explicit `opaque`/`alpha`/`additive`, runtime-preferred selection, and unsupported-mode fallback. |
| `tests/xr/chrome_hit.cpp` | the chrome region classifier: body vs. bar vs. corners vs. dead margin, content-rect insets, pose-offset placement, and UV remap that keeps clicks pixel-exact. |
| `tests/xr/chrome_fade.cpp` | the pure chrome fade-in/out advance (hide-delay hold, active-beats-hide, zero-dt/zero-fade edge cases). |
| `tests/xr/grab_gating.cpp` | `grabActionForRegion` gating: bar always moves, corners always resize, body moves only per the grab-anywhere / hand-body flags, margins never grab. |
| `tests/xr/grab_ring.cpp` | the release pose-latch ring buffer and the release-velocity rewind/reject heuristic (calm vs. flick trajectories, ratio-based outlier detection). |
| `tests/xr/hand_grab.cpp` | hand-grab mode parsing and semantics: pinch vs. grasp space selection, per-profile input-kind mapping, and grab-value-by-mode. |
| `tests/xr/one_euro.cpp` | the 1-euro filter used for the grab carry: passthrough first sample, jitter reduction, dt robustness, per-axis/quaternion pose filtering against a reference series. |
| `tests/xr/adaptive.cpp` | adaptive (dock↔follow) anchoring: geofence dwell, walk-away undock/redock, ease endpoints, height handling, and serialize/parse round-trip. |
| `tests/xr/plugged.cpp` | the monitor plug/unplug policy state machine: follow-session vs. presence gating, doff/donn transitions, defer-first-plug, and reprobe backoff. |
| `tests/xr/force_linear.cpp` | the `force_linear` swapchain policy: `on`/`off`/`auto`, and auto's cross-GPU detection. |
| `tests/xr/dmabuf_attribs.cpp` | dmabuf attribute emission for imported swapchain images: size/format/plane emission, multi-plane and modifier handling. |
| `tests/xr/event_queue.cpp` | the lock-free frame→main event queue: lossless ordered bursts, fill-to-capacity, empty-pop, and reset. |

These are the compile-time and pure-logic guarantees. Anything that needs a live session — real
device poses, swapchain import, focus routing, teardown — belongs to the integration tier.

---

## 2. Integration tier — `hyprtester --xr`

### 2.1 What it is and how it is gated

The integration suite is a group of `hyprtester` test cases (group name `xr`) that launch a
dedicated Hyprland instance against a live headless Monado runtime and assert over IPC. Its
sources live in two directories, both entirely wrapped in `#ifdef WITH_XR_TESTS`:

- `hyprtester/src/xr/` — infrastructure: `MonadoOrchestrator`, `RemoteClient`, the vendored
  Monado wire header, and `xr_helpers`.
- `hyprtester/src/tests/xr/` — the test cases themselves, one concern per file, plus the group
  header `tests.hpp`.

`hyprtester` GLOBs all of `src/*.cpp` into one binary, so the tier is gated by a **compile
definition**, not by excluding files. The root `CMakeLists.txt` declares the option inside the
`if(BUILD_TESTING OR WITH_TESTS)` block:

```cmake
option(WITH_XR_TESTS "Build the OpenXR integration test suite into hyprtester (needs a local Monado for running)" OFF)
```

and `hyprtester/CMakeLists.txt` turns it into `target_compile_definitions(hyprtester PRIVATE
WITH_XR_TESTS)` plus a `HYPRTESTER_SOURCE_ROOT` define (the repo root, used to find the vendored
Monado build and the test configs). It also defines a convenience `monado` custom target that
runs `scripts/build-monado.sh` — deliberately *not* part of `ALL`, since Monado is a multi-minute
external build needed only to *run* the suite.

The group header `tests.hpp` defines `TEST_GROUP_NAME "xr"` and stores cases in `xrTestCases`.
Cases use `TEST_CASE(name)` from `hyprtester/src/shared.hpp` (registering into both the global
`testCases` map and `xrTestCases`) and the shared assertion macros (`EXPECT`, `EXPECT_OK`,
`EXPECT_CONTAINS`, the `ASSERT*` variants, `OK`). Each case begins with the
`XR_SKIP_IF_UNAVAILABLE()` macro, which skips (counting as a pass) when the runtime is absent or
the wire ABI has drifted (§4).

### 2.2 How the suite runs its own Hyprland + Monado

`XR_RUNTIME_JSON` must be in Hyprland's environment *at launch*, and Monado must be up *before*
Hyprland starts its session — so the XR suite is a separate `hyprtester` invocation, selected by
the `--xr` flag. Under `--xr`, `main.cpp`'s `runXrSuite()`:

1. Defaults the config to `hyprtester/xr-test.conf` (instead of `test.lua`) and runs only
   `xrTestCases`.
2. Constructs a single `CMonadoOrchestrator` and starts it (§3). If Monado is unavailable, the
   suite still launches Hyprland — **without** `XR_RUNTIME_JSON` — so `xr_runtime_absent` can
   assert the graceful-unavailable path; all other cases skip.
3. Launches Hyprland with `XR_RUNTIME_JSON=<manifest>` and the shared isolated
   `XDG_RUNTIME_DIR` (§3).

**Launch-mode fallback.** The stock `HYPRLAND_HEADLESS_ONLY=1` launch cannot bring up
Aquamarine's headless backend in a seatless sandbox (rootless podman, some CI). `runXrSuite()`
handles this: it tries the headless launch first, and on failure symlinks the host's Wayland
socket into the isolated run directory and relaunches **nested** inside the host's Wayland
session (a few retries). Under the nested fallback Aquamarine names the base output `WAYLAND-1`
rather than the `HEADLESS-1` from `xr-test.conf`'s `monitor =` line, so tests never hard-code the
base monitor's name; every XR monitor a test creates is backend-independent.

`xr-test.conf` is a **classic hyprlang** config (the `xrmonitor` keyword is registered in the
legacy config manager — see the configuration reference). It enables OpenXR, sets a headless base
monitor, and declares two static XR monitors (`XR-conf-a`, `XR-conf-b`) used as reconciliation
fixtures. It intentionally carries no machine-specific GPU pin; per-box settings are merged in at
runtime (§3).

---

## 3. MonadoOrchestrator

`CMonadoOrchestrator` owns the `monado-service` process for the whole `--xr` run (started once,
torn down after reporting). The Monado source of truth is the `subprojects/monado` git submodule,
pinned to the same commit as the vendored wire header (§4). Build it once with
`scripts/build-monado.sh` (or `cmake --build build-debug --target monado`); the service binary
lands at `subprojects/monado/build/src/xrt/targets/service/monado-service`. Header-only build
deps are vendored too — `subprojects/eigen` and `subprojects/vulkan-headers` — leaving only the
Vulkan ICD loader as a system dependency.

**Binary resolution** (`resolveBinary`): `$HYPRTESTER_MONADO_SERVICE` if set → the vendored
submodule build under `HYPRTESTER_SOURCE_ROOT` → `monado-service` on `PATH`. The env override is
**authoritative**: if `$HYPRTESTER_MONADO_SERVICE` is set but doesn't resolve to a usable binary,
resolution fails outright rather than falling through — which is also the supported way to force
the no-Monado/SKIP leg (point the var at a nonexistent path). If nothing resolves, the
orchestrator reports unavailable and the suite skips.

**Launch environment** (via `CProcess::addEnv`):

| Env | Value | Purpose |
|---|---|---|
| `XRT_COMPOSITOR_NULL` | `true` | headless null compositor — no HMD, no window (still needs a Vulkan device) |
| `P_OVERRIDE_ACTIVE_CONFIG` | `remote` | select the remote driver: devices scripted over TCP instead of real hardware |
| `XRT_NO_STDIN` | `1` | don't block on the service's interactive stdin |
| `XDG_RUNTIME_DIR` | isolated run dir | where the Monado IPC socket lands; **must equal** Hyprland-under-test's `XDG_RUNTIME_DIR` |
| `VK_DRIVER_FILES` | *(optional)* forwarded from `$HYPRTESTER_VK_DRIVER_FILES` | force software Vulkan (lavapipe) when the real GPU misbehaves |

The run directory is isolated-but-shared: a fresh per-run dir set as `XDG_RUNTIME_DIR` for both
`monado-service` and the launched Hyprland (shared so the OpenXR client finds Monado's socket at
`$XDG_RUNTIME_DIR/monado_comp_ipc`; isolated so a developer's real session is never touched and
concurrent runs can't collide). The service's stdout/stderr are captured to a log for artifact
dumps.

**Readiness** (`pollReadiness`, 100 ms poll, 10 s timeout, both conditions): a `connect()` to the
`monado_comp_ipc` unix socket succeeds, and a TCP `connect()` to `127.0.0.1:4242` — the remote
driver's scripting port — is accepted (that fd is kept for `RemoteClient`). The manifest handed
to Hyprland as `XR_RUNTIME_JSON` is Monado's build-tree manifest (`openxr_monado-dev.json` next
to the built binary, or the installed `share/openxr/1/openxr_monado.json` for an installed
binary).

**Per-box GPU pin.** Machine-specific launch knobs stay out of the tracked config: the harness
generates a wrapper config that sources `xr-test.conf`, then an optional untracked
`hyprtester/xr-test-local.conf`, then `openxr:gpu = $HYPRTESTER_XR_GPU` when that env var is set.
This matters on dual-GPU boxes — see §8.

**Teardown**: `SIGTERM` → wait up to 3 s → `SIGKILL` + reap. The run directory is removed unless a
test failed (then it is kept, with its path printed, for inspection). Teardown runs even when
tests fail.

---

## 4. RemoteClient and the vendored wire header

Scripted device input reaches Monado through its remote driver's TCP wire protocol, defined in
`subprojects/monado/src/xrt/drivers/remote/r_interface.h`. The suite does **not** link against
Monado (that would drag its headers/libs into every `hyprtester` build). Instead the POD wire
structs are vendored into `hyprtester/src/xr/monado_remote_wire.hpp`, pinned to Monado commit
`c2ddab59dc41366fe520dc4e8abcfea257ecf0b8` — the **same** commit the `subprojects/monado`
submodule is pinned to. Re-pinning is a deliberate maintenance task: bump the commit, re-copy the
structs, re-verify the asserts. This header is the only sanctioned vendoring of Monado in the
tree, lives entirely under `hyprtester/`, and never ships in the compositor.

The header is self-contained (the handful of `xrt_defines.h` PODs the wire structs embed are
inlined) and carries the **compile-time half of ABI-drift protection**: `static_assert`s on the
magic header value and on the exact struct sizes (`r_remote_controller_data == 120`,
`r_head_data == 128`, `r_remote_data == 376`). If a re-pin changes the layout, the build fails
loudly.

`RemoteClient` takes over the accepted TCP socket and performs the **runtime half** of ABI
protection: on connect it validates the wire header value and full-struct framing, and on any
mismatch marks the **whole suite SKIP, not FAIL** — a newer locally-installed Monado may
legitimately have evolved the wire struct past the pin, which is a maintenance task, not a
Hyprland regression. Thereafter usage is read-modify-write of one complete device snapshot per
tick (there are no deltas): setters mutate the current template; a pulse stamps the header and
writes the struct.

With `P_OVERRIDE_ACTIVE_CONFIG=remote` the devices enumerate as **Valve Index controllers**
(`valve/index_controller` interaction profile) plus an HMD — which is why the input document's
binding table exercises the valve/index profile. `RemoteClient` exposes head/controller pose
setters, trigger/squeeze/thumbstick analogs, hand-curl (emulated hand tracking), and an
`animate` helper that interpolates and pulses at ~60 Hz so leash-spring and grab-motion tests see
continuous motion.

---

## 5. xr_helpers — waiting, asserting, artifacts, naming

`hyprtester/src/xr/xr_helpers.{hpp,cpp}` provide the conventions the cases rely on:

- **Wait/assert split.** Never assert on asynchronous state directly; wait for it first.
  `waitForJson(cmd, pred, …)` polls `getFromSocket()` (hyprtester's IPC channel) until a
  predicate holds or a timeout expires; `waitForXrState(state, …)` is the specialization over the
  `hyprctl openxr` status JSON. State assertions go over IPC (`j/openxr`, `j/monitors`), not
  pixels; numeric pose checks parse the JSON and compare with generous tolerances.
- **SKIP handling.** `shouldSkip()` reports the skip reason (`monado-service not found`, or the
  wire ABI-mismatch reason); `logSkip()` emits both a human line and a TAP-style
  `ok - <name> # SKIP <reason>` line so external harnesses can grep it. A SKIP counts as a pass.
  The `XR_SKIP_IF_UNAVAILABLE()` macro at the top of each case wires this in.
- **Artifact capture on failure.** `dumpXrArtifacts(testName)` (called by each case via an RAII
  guard that checks `this->failed`) writes to `hyprtester/artifacts/<run-id>/<testName>/`:
  `monitors.json`, `openxr.json`, a tail of the Monado log, and a tail of the Hyprland log. A
  green run writes nothing.
- **Unique per-run names.** Monitors created by a test at runtime use a per-pid, per-counter name
  (`monitorName(n)`), so re-runs and concurrent developers never collide on "name already taken".
  The two config-declared fixtures (`XR-conf-a/b`) are exempt because the config is static and the
  instance is launched fresh per run.

---

## 6. The integration cases

The suite is the `xr` group; all cases skip cleanly when Monado is unavailable, and (except
`xr_runtime_absent`) gate on the session reaching FOCUSED (or VISIBLE where scripted input isn't
required). The cases, by file:

**Session and graceful degradation** (`session.cpp`)
- `xr_session_up` — the session reaches focused/visible against Monado's null compositor + remote
  driver and reports `Monado` as the runtime.
- `xr_runtime_absent` — Hyprland launched with `openxr:enabled=1` but no `XR_RUNTIME_JSON` reports
  `state: unavailable`, `enable` returns a clean error (not a crash), and the compositor stays
  fully functional.
- `xr_gpu_mismatch_fails_closed` — forcing `openxr:gpu` at a render node that is *not* the one the
  runtime composites on makes `enable` fail closed (report the runtime unavailable) instead of
  taking the compositor down on a cross-GPU import. Guarded: it skips unless the runtime-GPU probe
  succeeded and a second distinct render node exists to force.

**Virtual monitors** (`monitors.cpp`)
- `xr_monitor_create_destroy` — create/destroy a runtime XR monitor via the dispatcher; assert it
  appears/disappears in `j/openxr` and `j/monitors` with the expected size.
- `xr_monitor_create_mode` — the mode passed to `openxr create` is the mode the output actually
  RUNS (asserted from `j/monitors`, not the requested mode echoed back by `j/openxr`), and it
  survives a config reload — a reparse clears the rule manager, and reconciliation only reinstalls
  rules for config-*declared* monitors, so a runtime-created one used to snap back to 1920x1080.
- `xr_force_linear_realloc` — toggling the `force_linear` swapchain policy reallocates the
  monitor's swapchains.
- `xr_config_declared` — the two declared fixtures exist with correct anchors; a config reload
  reconciles idempotently (declared monitors keep their ids, a runtime-created monitor survives
  untouched).
- `xr_mirror` — a normal headless monitor set to `mirror` an XR monitor reports `mirrorOf` and
  restores on unset (zero-new-code mirroring through the standard monitor path).

**Anchoring** (`anchors.cpp`, `adaptive.cpp`)
- `xr_anchor_transitions` — switch a monitor's anchor mode and drive head yaw; the leashed pose
  re-converges in front of the view, then freezes when switched back to `local`; the anchor event
  fires.
- `xr_adaptive_geofence` — the adaptive dock↔follow behavior: walking the head out of the geofence
  undocks and follows, returning re-docks.

**Monitor plug lifecycle** (`plugged.cpp`)
- `xr_plugged_follow_session` — the monitor plug state follows the session per policy.
- `xr_plugged_create_while_sessionless` — creating an XR monitor with no live session behaves per
  policy.
- `xr_plugged_survives_monitor_refresh` — a monitor refresh doesn't spuriously unplug.

**Input** (`input.cpp`, `ray_live.cpp`)
- `xr_ray_click_routing` — aim a controller ray at a known UV of one of two side-by-side monitors,
  press/release the trigger; focus and cursor position land on the expected output and pixel.
- `xr_select_hysteresis` — the trigger Schmitt threshold (press vs. release) registers exactly one
  button edge and doesn't double-fire on jitter, verified via a spawned test client.
- `xr_two_hand_pointer` — pointer ownership transfers to the last-active hand and back.
- `xr_scroll` — thumbstick scroll reaches a scrollable client on an XR monitor.
- `xr_menu_right_click` — the `menu` action maps to a right-click edge, verified on a test client.
- `xr_ray_hover` — a bounded live smoke test that a swept controller ray registers hover on a
  fresh quad; skips (does not fail) if hover never registers within budget.

**Grab: move, region gating, and release latch** (`grab_live.cpp`, `grab_region.cpp`,
`grab_latch.cpp`)
- `xr_grab_move` — squeeze-grab a monitor, translate the controller, release; the monitor's anchor
  pose moves by the translated amount and the grab events fire.
- `xr_grab_gating_body_vs_bar` — grabbing the chrome bar always moves; grabbing the body follows
  the grab-anywhere gating.
- `xr_grab_body_default_regression` — the default body-grab disposition holds.
- `xr_grab_corner_resize` — grabbing a corner resizes from that corner.
- `xr_grab_release_latch` — the release pose-latch lands the monitor at its pre-jerk pose (no
  fist-open lurch).
- `xr_grab_calm_release_noregress` — a calm release is not rewound.
- `xr_grab_fast_flick_noregress` — a deliberate fast flick is not falsely rewound as an outlier.

**Idle, teardown, overlay** (`idle.cpp`, `teardown.cpp`, `overlay.cpp`)
- `xr_idle_inhibit` — with `inhibit_idle` on and a focused session, an idle client is not idled;
  turning it off lets idle fire; restoring resumes on the next XR input. Uses the **legacy boolean**
  spellings on purpose, so it doubles as the migration regression test (`1` → `focused`, `0` → `off`).
- `xr_idle_inhibit_modes` — the `off | focused | present` mode surface (research/20 phase 2):
  `idleInhibitMode` in status resolves each spelling (including legacy `1`/`true` → `focused` and an
  unrecognized value → the `present` default), and `present` on the harness' presence-less runtime
  falls back to the `focused` predicate end-to-end (holds the bit while focused, releases on `off`).
  **Coverage limit:** the null/remote driver cannot script don/doff, so every presence-*supported*
  row (worn-but-not-focused inhibits, doff releases despite WiVRn's sticky presence, absent before
  the first event) is gtest-only — `tests/xr/idle_inhibit.cpp`.
- `xr_disable_teardown` — `hyprctl openxr disable`/`enable` round-trips through the full state
  machine without crashing and restores the declared monitors.
- `xr_overlay_composition` — **opt-in**, gated on `$HYPRTESTER_HYPXRPAPER` (a path to a
  `hypxrpaper` binary; unset/invalid → skip). Launches `hypxrpaper` as the primary OpenXR session
  and re-enters Hyprland as an `XR_EXTX_overlay` session; asserts `overlay: true` in `j/openxr`,
  that Monado is still the runtime, and that the declared fixtures are bound. It restores state on
  exit (disable → clear overlay → enable → kill the primary).

---

## 7. Running the suite locally

```sh
# 1. Build Hyprland + hyprtester with the XR suite compiled in.
cmake -B build-debug -DWITH_TESTS=ON -DWITH_XR_TESTS=ON
cmake --build build-debug --target Hyprland hyprtester

# 2. Have Monado available — build the pinned submodule once…
scripts/build-monado.sh
#    …or point at one explicitly / rely on PATH:
export HYPRTESTER_MONADO_SERVICE=/path/to/monado-service
# optional: force software Vulkan
export HYPRTESTER_VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
# optional: opt into xr_overlay_composition (else it skips)
export HYPRTESTER_HYPXRPAPER=/path/to/hypxrpaper

# 3. Run from the hyprtester directory.
cd hyprtester
../build-debug/hyprtester/hyprtester --xr --binary ../build-debug/Hyprland
# a subset:
../build-debug/hyprtester/hyprtester --xr --binary ../build-debug/Hyprland xr_session_up xr_grab_move
```

The unit tier rides the existing `hyprland_gtests` target and needs none of the above — it runs
on every ordinary test build. The integration tier is not wired into CI as a per-PR gate: it
depends on a live runtime and a real GPU, so it stays a local / containerized gate.

Only **one** `monado-service` runs per box — its remote driver binds TCP 4242 — so anything that
binds 4242 (a native host suite run, or a container session publishing the port) must be
serialized. This is the main reason to prefer the hermetic container runner below.

---

## 8. Containerized suite and dev sessions

`scripts/xr-container.sh` drives a rootless-podman Arch + systemd container that boots real PID-1
systemd, installs a curated Omarchy desktop, and builds Hyprland + hyprtester + the vendored
Monado into a volume so the host tree is never touched. See
[`containers/README.md`](../../containers/README.md) for image/build details; this is the
testing-oriented view.

### 8.1 Hermetic test run (`test`) — the preferred runner

```sh
scripts/xr-container.sh test --gpu amd                  # full hyprtester --xr suite
scripts/xr-container.sh test --gpu amd xr_session_up    # a subset (name filter)
scripts/xr-container.sh test --gpu nvidia               # …on NVIDIA via CDI
scripts/xr-container.sh test --gpu amd --keep           # leave the container up to debug
```

`test` boots `:session` with **no host wayland/X11/wivrn mounts** — only the `/src` overlay, the
build/ccache volumes, and one GPU device. It uses its **own network namespace** and the vendored
Monado null compositor, so it touches no host sockets and can run safely alongside a live host
session. A headless **labwc** is the nesting host: `runXrSuite` tries the stock headless launch
(which cannot work in seatless rootless podman) and then nests into labwc — the only light
compositor advertising both protocols Aquamarine's nested backend requires (`xdg_wm_base >= v6`
and `zwp_linux_dmabuf_v1`). The real exit code comes from a sentinel file (`machinectl shell`
always exits 0); on failure the run log and every preserved `/tmp/hyprtester-xr-*` dir are copied
to `containers/artifacts/<timestamp>/`.

GPU selection is by **vendor scan** (`scripts/lib/gpu.sh`), not hardcoded node names —
`--gpu amd|nvidia|intel|split` or an explicit `/dev/dri/renderD*`. The node is resolved host-side
(for `--device`) and again **inside** the container (to pin `openxr:gpu`), so a CDI-injected
NVIDIA node is verified present rather than assumed. `--gpu amd` is the default and the reliable
hermetic GPU; on NVIDIA the nested-into-labwc topology hits an Aquamarine GBM allocation limit
(`XR24` on the NVIDIA node), which is a driver/GBM constraint, not compositor logic — use
`--gpu nvidia` mainly to prove the CDI ICDs via `check-gpu`.

### 8.2 Interactive sessions (`session`)

```sh
scripts/xr-container.sh session                   # windowed Monado desktop, no headset
scripts/xr-container.sh session --wivrn           # real Quest via host WiVRn (default --gpu split)
scripts/xr-container.sh session --env forest      # + hypxrpaper ambient background (overlay)
scripts/xr-container.sh session --passthrough     # openxr:blend_mode = alpha
```

`session` nests a full Omarchy desktop as a window on the host, with the dev Hyprland's XR
extension enabled and input isolated (no `/dev/input`). Its topologies:

| Invocation | XR runtime | Nesting host | Host mounts |
|---|---|---|---|
| `session` (windowed) | vendored **windowed** Monado, in-container | host Wayland socket | Wayland socket only |
| `session --wivrn` | **host WiVRn** runtime | host Wayland socket | Wayland socket + WiVRn libs/manifest/socket |

Windowed `session` does **not** publish Monado's remote port by default (a fixed 4242 publish
once poisoned a concurrent host suite run); opt in with `--publish-remote` for an ephemeral host
port. `session --wivrn` targets a real headset and defaults to `--gpu split`: on a dual-GPU box
the nested compositor must render on the host-compositor GPU (`AQ_DRM_DEVICES`) while WiVRn
encodes on the other (`openxr:gpu`), so both GPUs are exposed and assigned separately
(`--nested-gpu` / `--xr-gpu`); it degrades to a single node on a one-GPU box.

### 8.3 Dual-GPU interop note

Runtime/driver behavior specific to dual-GPU boxes, recorded so it isn't rediscovered — none of
it is compositor application logic:

- Monado's null compositor picks a GPU **independently** of Hyprland. If `openxr:gpu` doesn't
  match the render node Monado composites on, the cross-GPU dmabuf import crashes inside
  Monado/Mesa at `xrCreateSwapchain` — hence the per-box `openxr:gpu` pin (§3) and the
  `xr_gpu_mismatch_fails_closed` guard (§6). Whatever runs the OpenXR runtime/encoder dictates the
  XR GPU.
- Keep individual XR sessions short. Long-running sessions on some driver stacks have shown
  Monado/Mesa-side fence-insert spam and teardown heap-corruption aborts inside Monado, not
  `src/openxr`. The suite's short-lived per-test sessions and the SKIP-not-FAIL posture of the
  live smoke tests already follow this.

---

## 9. Preview and fishfood scripts

Beyond the automated suite, several scripts stand the extension up interactively:

- **`scripts/preview-xr.sh`** — a no-headset desktop preview: launches `monado-service` in
  **windowed** mode with the remote driver (a "Monado" window shows the rendered XR space) and a
  nested dev Hyprland (`build-debug`) with the XR extension enabled, then lets you drive the fake
  head/controllers with `monado-gui remote`. `--wivrn` uses the system WiVRn runtime and a real
  headset instead; `--passthrough` sets `openxr:blend_mode = alpha`; `--env pano|forest|<path>`
  launches `hypxrpaper` as an ambient background (Hyprland then composites as an overlay);
  `--conf <file>` swaps the base nested config (e.g. the Omarchy-mirror config from the next
  script). The nested Hyprland runs under its own private DBus session bus, and the script kills
  only the PIDs/process group it spawned.
- **`scripts/gen-omarchy-xr-conf.sh`** — generates a nested-safe config under
  `~/.config/hypr/xr-nested/` that mirrors the user's Omarchy desktop (keybinds, look-and-feel,
  theme) but skips anything that mutates global session state or launches daemons, plus a
  `uwsm-app` shim so keybind-launched apps land in the nested session. Feed the generated
  `nested.conf` to `preview-xr.sh --conf`.
- **`scripts/fishfood.sh`** — installs the extension as a real desktop session alongside the
  distro Hyprland: a sibling git worktree built RelWithDebInfo, launched by a generated
  wayland-session `.desktop` entry pointing at the built binary with the user's XR front-end
  config. `setup` / `update` / `gen-session` subcommands; builds serialize through the shared
  build mutex and the session file install is left for the user to run with root.

---

## 10. Vendored dependencies

The test/runtime dependency chain is vendored as git submodules so the suite pins exactly what it
validates against:

- `subprojects/monado` — pinned to the same commit as the vendored wire header (§4); built by
  `scripts/build-monado.sh` into `subprojects/monado/build` (or a `MONADO_BUILD`-redirected tree
  for read-only source mounts).
- `subprojects/eigen`, `subprojects/vulkan-headers` — header-only Monado build deps.
- `subprojects/hypxrpaper` — the ambient-background client used by the optional overlay test and
  the `--env` preview/session modes.

Re-pinning Monado is a coordinated maintenance step: bump both the submodule and the wire
header's pin together, re-copy the structs, and re-verify the size `static_assert`s. If an
installed Monado has drifted past the pin, the suite skips rather than fails.

Design research behind the anchoring, input, grab, and composition behaviors under test lives
under `docs/openxr/research/`.
