# 17 — Late/Absent-Runtime Session Lifecycle (fishfood login incidents)

Status: **research / root-cause analysis only. Nothing here is implemented.** No live runs beyond
read-only observation (`hyprctl openxr status`, log/crash-report reads) of the user's live fishfood
session. Author: research pass 2026-07-11 against branch `hypxrland` (worktree base `452d6b02`),
source-read + live-log evidence.

The incident (user's first fishfood session, logged in at SDDM with the runtime absent):

1. `openxr { enabled = 1 }` at login while **wivrn-server was not running and the headset was not
   donned** — XR init presumably failed/degraded.
2. Toggling `openxr:enabled` off/on via `hyprctl keyword` caused **"weird rendering problems on the
   main monitor"** (the physical eDP desktop).
3. After starting WiVRn and enabling again, the XR monitor in-headset was **"just a black screen"**.

Scope: the session/runtime lifecycle and the two rendering bugs. The sibling report 18 owns how XR
monitors' *plugged* state should track session connectedness — referenced, not designed here. The
`openxr:gpu` wrong-GPU **crash** robustness (coredumps 8986/39318, `xrCreateSwapchain` →
`driUnbindContext` SEGV) is being handled by a separate dedicated investigation and is out of scope
here beyond one cross-reference in §4.4.

Cross-refs: `src/openxr/OpenXRManager.{hpp,cpp}` (state machine, `start()`/`stop()`/`abortStart()`,
`onConfigReload()`), `src/openxr/XRSession.cpp` (`createInstance`/`getSystem`/`pollEvents` loss
handling), `src/openxr/XRGraphics.{hpp,cpp}` (EGL display selection, `CScopedGLContext`, blit
pipeline), `src/config/legacy/ConfigManager.cpp:1125-1153` (keyword special-cases),
`src/debug/log/Logger.cpp` (the log gate that ate all the evidence), `docs/openxr/00-overview.md`
(state machine contract), `docs/openxr/01-session-graphics.md` (EGL context ownership + teardown
order), `docs/openxr/research/18-*` (sibling: monitor plugged-state).

---

## TL;DR

1. **We flew blind, by default.** `debug:disable_logs` defaults to **true**
   (`src/config/values/ConfigValues.cpp:624`) and `~/.config/hypr/hyprland-xr.conf` does not
   override it, so `CLogger::log()` drops every `[OPENXR]` line before it reaches the file
   (`src/debug/log/Logger.cpp:15-16`, `:47-48`). The live session's 7 MB `hyprland.log` contains
   **zero** `[OPENXR]` lines while `hyprctl openxr status` simultaneously reports a FOCUSED WiVRn
   session. Only aquamarine lines survive — they ride a raw `CLoggerConnection` on the underlying
   hyprutils logger (`src/Compositor.cpp:304`) that bypasses the gate. First WP is logging hygiene;
   every other diagnosis below had to be reconstructed from EGL error spam, crash reports, and code.

2. **Black screen (bug b) is root-caused with a smoking gun**: the desktop renders and allocates on
   the **AMD** iGPU (`renderD129`) while `openxr:gpu = /dev/dri/renderD128` puts the XR EGL context
   on the **NVIDIA** dGPU (the GPU WiVRn needs). Every per-frame blit tries to import an
   AMD-allocated dmabuf into the NVIDIA EGL display and fails — the session log holds **43,210**
   `eglCreateImageKHR ... EGL_BAD_ATTRIBUTE` errors from a ~23-minute session (§4.1). The blit's
   fallback chain then paints the quad **opaque black** (`XRGraphics.cpp:477-491`): session FOCUSED,
   monitors composited, content black. A concrete contributing bug: our import attribs **never pass
   the dmabuf modifier** (`XRGraphics.cpp:371-398`) — NVIDIA's EGL rejects implicit-modifier
   imports. (Correction 2026-07-12: the buffers were assumed LINEAR here, but the live fishfood
   buffers are actually AMD-tiled `0x0200…` — see §4.1 box — so passing modifiers was necessary but
   not sufficient; the buffers also had to be forced LINEAR via `openxr:force_linear`.)

3. **Host-monitor corruption on toggle (bug a)** has no surviving logs (gated off + instance dir
   rotated), but code analysis yields one high-confidence mechanism: main-thread XR GL bursts run
   inside `CScopedGLContext`, which **swaps the main thread's current EGL context to the XR context
   and exits to `EGL_NO_CONTEXT` without restoring** (`XRGraphics.cpp:43-51`). Hyprland's renderer
   re-binds lazily (`makeEGLCurrent()` guard, `src/render/OpenGL.cpp:709-715`), so the *unbind* is
   survivable — but any Hyprland GL helper that runs *while the XR context is current* (texture/FBO
   dtors triggered by the monitor teardown inside `stop()`) **steals the binding mid-burst**, after
   which the rest of the XR burst's `glDelete*`/`glGen*` calls execute against *Hyprland's* context
   with XR object IDs — deleting the desktop renderer's textures/framebuffers. §5 has the full
   candidate table.

4. **The lifecycle is a dead end by design.** `XR_STATE_UNAVAILABLE` explicitly does no auto-retry
   (`OpenXRManager.hpp:30-31`), and `onConfigReload()` only calls `start()` from `DISABLED`
   (`OpenXRManager.cpp:1566`) — so after a failed login-time start, `hyprctl keyword openxr:enabled 1`
   is a **silent no-op** (the user must toggle 0-then-1, which works only because `stop()` from
   UNAVAILABLE happens to land in DISABLED). Session loss (headset off, server death) also parks in
   UNAVAILABLE forever. §6 designs a dormant-with-reprobe state machine (event-loop timer, backoff,
   `xrGetSystem` polling for "runtime up, headset not donned") plus optional
   `systemctl --user start wivrn` autostart. With those, `openxr { enabled = 1 }` at login becomes
   safe and self-healing.

5. One prior crash class is **already fixed** upstream of this worktree but was part of the user's
   first-session experience: crash reports 18:25–19:03 on the `6ced4f9f` build die at **startup**
   in `start()` → `publishAdaptiveStringTuning()` dereferencing a `CConfigValue<char const*>`
   (§3.2) — fixed by `e2b2e20f` ("never read string config values on the frame thread"). The
   fishfood binary the user first ran predated the fix; the crash was *deterministic at login*.

---

## 1. Environment and evidence sources

### 1.1 The fishfood setup

- Binary: `~/code/hypxrland/build/Hyprland` (RelWithDebInfo), config
  `~/.config/hypr/hyprland-xr.conf`:

  ```
  openxr {
      enabled = 1
      gpu = /dev/dri/renderD128
      blend_mode = alpha
  }
  xrmonitor = XR-main, 2560x1440@90, anchor:local pos:0,1.5,-1.5 yaw:0 adaptive:on roam:body, size:1.6
  ```

- GPUs (hybrid laptop, from `/sys/class/drm/*/device/vendor`):

  | node | vendor | driver | role |
  |---|---|---|---|
  | `renderD128` / `card1` | `0x10de` NVIDIA GB206M (RTX 5070 Max-Q) | `nvidia` | WiVRn's GPU; `openxr:gpu` target |
  | `renderD129` / `card2` | `0x1002` AMD Strix (Radeon 880M/890M) | `amdgpu` | **Hyprland's render/scanout GPU** |

  Log-confirmed the compositor is on AMD:

  ```
  DEBUG from aquamarine ]: drm: Starting backend for /dev/dri/card2, with driver amdgpu
  ...
  DEBUG from aquamarine ]: zwp_linux_dmabuf_v1: Got node /dev/dri/renderD129
  DEBUG from aquamarine ]: reopenDRMNode: opening node /dev/dri/renderD129
  DEBUG from aquamarine ]: Created a GBM allocator with drm fd 18
  ```

- Runtime: WiVRn v26.6.1 via `~/.config/openxr/1/active_runtime.json` →
  `/usr/lib/wivrn/libopenxr_wivrn.so`. Confirmed live mid-investigation:

  ```
  $ hyprctl openxr status        # read-only, user's live session
  state: focused
  runtime: WiVRn 'v26.6.1'
  system: Meta Quest 3 on WiVRn
  blend mode: alpha
  ...
  monitor XR-main (ID 0): 2560x1440@90.00 size 1.60m anchor local pos [-0.51, 1.06, -1.63] ...
  ```

  Note what status does **not** show: that the quad's content pipeline is failing every frame
  (§4.1). "state: focused + black screen" is exactly the observability gap WP-L6 closes.

### 1.2 The logging blackout (why there is no `[OPENXR]` evidence)

The session log (`$XDG_RUNTIME_DIR/hypr/<sig>/hyprland.log`, 70,962 lines / 7 MB at read time)
contains **zero** lines matching `openxr` in any case, despite a live FOCUSED session:

```
$ grep -c 'OPENXR' hyprland.log   → 0
$ grep -ic 'openxr' hyprland.log  → 0
$ grep -ic 'wivrn' hyprland.log   → 0
```

Mechanism, verified in source:

- Every XR log call goes through `Log::logger->log(...)` (`src/openxr/` uses no other sink), which
  early-returns when `m_logsEnabled == false` (`src/debug/log/Logger.cpp:15-16`).
- `m_logsEnabled = !*PDISABLELOGS` (`Logger.cpp:48`), and `debug:disable_logs` **defaults to
  true** (`src/config/values/ConfigValues.cpp:624`). The startup banner even warns about it:

  ```
  DEBUG ]: !!!!HEY YOU, YES YOU!!!!: further logs to stdout / logfile are disabled by default.
  ```

- Aquamarine lines survive because the backend logs through a raw
  `Hyprutils::CLI::CLoggerConnection` attached to the *underlying* hyprutils logger
  (`src/Compositor.cpp:304`), skipping `CLogger::log()`'s gate entirely. That asymmetry is why the
  only trace of the XR blit failure is 43k *aquamarine-prefixed* EGL errors (§4.1) — the matching
  `[OPENXR] eglCreateImageKHR failed (0x...), falling back to CPU path` WARN (`XRGraphics.cpp:425`)
  was swallowed.

Consequence for this report: the first session's evidence (login-time failure, the toggle, the
first black screen) is **gone** — its instance dir was rotated away and its `[OPENXR]` lines were
never written. Everything below is reconstructed from the current session's log, crash reports, and
code. WP-L6 makes sure this never happens again (fishfood config gets `debug:disable_logs = false`;
XR WARN/ERR paths get rate-limited so that's cheap).

---

## 2. What `openxr:enabled = 1` does at STAGE_LATE with the runtime absent (code trace)

`CCompositor::initManagers(STAGE_LATE)` constructs the manager and calls `init()`
(`src/Compositor.cpp:772-775`).

`init()` (`OpenXRManager.cpp:64-84`), in order:

1. Registers the `hyprctl openxr` IPC surface.
2. Listens on `config.reloaded` + `config.props_refreshed` → `onConfigReload()`.
3. **`reconcileDeclaredMonitors()`** — materializes `xrmonitor = XR-main, 2560x1440@90, ...` as a
   real headless output *immediately*, session or not (doc 02 lazy binding;
   `createXRMonitor` works in every manager state, `OpenXRManager.hpp:82-86`). So at login the
   user's desktop **always** has a phantom 2560x1440 monitor — workspaces can land on it, focus can
   wander into it. (Sibling report 18's plugged-state design addresses whether it should be
   "connected" while sessionless; not designed here.)
4. Reads `openxr:enabled` → `start()`.

`start()` (`OpenXRManager.cpp:168-298`) with no reachable runtime:

- `setState(XR_STATE_STARTING)` → `publishAdaptiveStringTuning()` (§3.2's old crash site) →
  `m_session->createInstance()` (`XRSession.cpp:53-116`):
  `xrEnumerateInstanceExtensionProperties` + required-extension check
  (`XR_MNDX_egl_enable` / `XR_KHR_opengl_es_enable`) + `xrCreateInstance` via `XR_CHK` (`:45`).
  With wivrn-server down, the loader still finds the manifest and loads
  `libopenxr_wivrn.so`, but the WiVRn/Monado IPC client cannot connect — extension enumeration or
  `xrCreateInstance` fails → `createInstance()` returns false.
- → `[OPENXR] no runtime / required extensions unavailable, state -> unavailable`
  (`OpenXRManager.cpp:199` — swallowed by the log gate, §1.2) → `abortStart()`.

`abortStart()` (`OpenXRManager.cpp:300-321`) at *this* failure point touches **no GL**: `m_graphics`
exists but `initEGL` never ran (`m_eglDisplay == EGL_NO_DISPLAY`, `m_xrContext == EGL_NO_CONTEXT`),
so `destroyGL()`/`destroyEGL()` are no-ops. Lands in `XR_STATE_UNAVAILABLE`.

Key ordering fact for §5: in `start()`, EGL work (`initEGL`, step 3, `:233`) happens **only after**
`createInstance` (step 1) *and* `getSystem` (step 2) succeed. A truly-absent runtime means the
toggle path never touches EGL. Conversely, once the runtime answers (server up), a *later* failure
— `createSession` failing because **no headset is connected** (WiVRn requires a connected client;
`scripts/preview-xr.sh` documents this precondition) — aborts *after* `initEGL` created the XR
context, and `abortStart()` → `destroyGL()` then runs main-thread GL context swaps (§5.2).

### 2.1 The UNAVAILABLE dead end

The state is *defined* as terminal:

```
XR_STATE_UNAVAILABLE,     // start attempted: no runtime / xrCreateInstance or system lookup failed,
                          // or instance loss. NO auto-retry polling — user re-enables explicitly.
```
(`OpenXRManager.hpp:30-31`)

And the explicit re-enable is broken in the most natural spelling. `onConfigReload()`
(`OpenXRManager.cpp:1563-1571`):

```cpp
if (enabled && m_state == XR_STATE_DISABLED)
    start(); // start() reconciles declared monitors itself
else if (!enabled && m_state != XR_STATE_DISABLED)
    stop();
```

- `hyprctl keyword openxr:enabled 1` while UNAVAILABLE (value already 1, or even freshly set): state
  is not DISABLED → **no start(). Silent no-op.** `start()` itself would accept it — its guard
  admits UNAVAILABLE (`:170`) — the reload path just never asks.
- The 0-then-1 dance works by accident: `enabled 0` → `stop()` from UNAVAILABLE → `lost = m_session
  && m_session->m_instanceLost` = false (session already reset) → lands DISABLED (`:376`) → `enabled
  1` → start.
- `hyprctl openxr enable` works (calls `start()` directly, `XRIpc.cpp:117-118`).

So the user's observed step 2 ("toggled off/on") was the *required* incantation, not a redundant
one. WP-L7 is the one-line fix; WP-L3 makes the whole question moot with auto-reprobe.

---

## 3. Crash-report evidence from the fishfood sessions (same day)

### 3.1 Report inventory

`~/.cache/hyprland/` holds seven reports from 2026-07-11 between 18:25 and 19:03 — all from
fishfood builds (`Tag: v0.55.0-341-g6ced4f9f`, i.e. **before** `e2b2e20f`), i.e. from the user's
first login attempts.

### 3.2 The deterministic login crash (fixed, but it shaped the first session)

`hyprlandCrashReport64278.txt` (18:50, SIGSEGV, build `6ced4f9f`):

```
#4  CConfigValue<char const*>::ptr() const           src/config/ConfigValue.hpp:51
#5  CConfigValue<char const*>::operator*() const     src/config/ConfigValue.hpp:55
#6  COpenXRManager::publishAdaptiveStringTuning()    src/openxr/OpenXRManager.cpp:1628
#7  COpenXRManager::start()                          src/openxr/OpenXRManager.cpp:181
#8  COpenXRManager::init()                           src/openxr/OpenXRManager.cpp:83
#9  CCompositor::initManagers(eManagersInitStage)    src/Compositor.cpp:777
```

This is the known "cursed case": under the legacy manager, STRING values populate `m_hlangp` and
leave `m_p` null; the generic `CConfigValue<const char*>::ptr()` dereferences null. **On that build,
`openxr { enabled = 1 }` crashed the compositor at every login** — before the runtime question even
arose. Fixed by `e2b2e20f` (`publishAdaptiveStringTuning` now uses `CConfigValue<std::string>`,
`OpenXRManager.cpp:1626-1631`); the current worktree carries the fix. Recorded here because (a) it
was part of the user's "XR init at login failed" experience, and (b) it is the cautionary tale for
WP-L3: *the login path with enabled=1 had never actually been exercised before fishfooding* — the
suite always starts sessions against a live Monado.

The later same-day SIGABRT coredumps (PIDs 8986 / 39318, ~20:16-20:18) are the wrong-GPU
`xrCreateSwapchain` → Mesa `driUnbindContext` crash — **out of scope here**, owned by the dedicated
cross-GPU investigation (§4.4).

---

## 4. Bug (b): black XR quad after a late enable — root cause

### 4.1 The evidence

Current session (started 19:45 with WiVRn up, `gpu = renderD128`): FOCUSED, monitors composited,
and the log accumulates this at content-frame rate, starting at line 783 (i.e. immediately once
frames flow) and reaching **43,210 occurrences by 20:08**:

```
ERR from aquamarine ]: [EGL] Command eglCreateImageKHR errored out with EGL_BAD_ATTRIBUTE (0x12292):
EGL_BAD_ATTRIBUTE error: In eglCreateImageKHR: requested buffer attributes are not supported
```

(The "from aquamarine" prefix is misleading: the EGL debug callback is **process-global**, so
XR-frame-thread EGL failures are reported through aquamarine's handler — and it is the only handler
whose output survives the log gate, §1.2. The XR blit's own WARN at `XRGraphics.cpp:425` was
swallowed, and it would have fired per-frame anyway — see WP-L6 rate-limiting.)

Buffer provenance, same log: the headless XR output's swapchain is allocated by aquamarine's GBM
allocator **on the AMD node** with LINEAR modifier:

```
DEBUG from aquamarine ]: reopenDRMNode: opening node /dev/dri/renderD129
DEBUG from aquamarine ]: Created a GBM allocator with drm fd 18
DEBUG from aquamarine ]: GBM: Allocated a new buffer with size [Vector2D: x: 1920, y: 1080] and format XR24 with modifier 0x0 : LINEAR
```

> **Correction (2026-07-12, after B1/WP-L2 shipped).** This LINEAR observation was NOT
> representative of the failing fishfood session. Once modifiers were actually passed (commit
> b93279dd), the live log showed the headless XR output's buffers carry **AMD-tiled** modifier
> `0x0200000104abb04` (vendor 0x02, DCC, 2 planes) — the compositor renders on the AMD iGPU and
> aquamarine picks its native tiling, not LINEAR. NVIDIA's EGL correctly rejects the foreign
> vendor tiling with `EGL_BAD_ATTRIBUTE`. So B1 alone is insufficient on this box and B2 (below) is
> **required**, not a no-op guard. Fixed by `openxr:force_linear` (auto): when the XR EGL node ≠ the
> buffer allocator node, the XR output's aquamarine swapchain is reconfigured with
> `SSwapchainOptions::multigpu` (forces `DRM_FORMAT_MOD_LINEAR`), which NVIDIA imports fine.
>
> **Correction 2 (2026-07-12, Defect A).** The first cut of the fix (349da50e) set the multigpu flag
> and called `swapchain->reconfigure()`, but the buffers **still** came back AMD-tiled — the live log
> showed the exact same `0x2000000104abb04` modifier reaching the import. Root cause: aquamarine's
> `CSwapchain::reconfigure()` (v0.12.0) only honors `multigpu` on its **fullReconfigure** path, which
> runs only when the FORMAT or SIZE changes. A bare `multigpu` flip with identical format/size/length
> is swallowed by the no-op early-out (or handled by the `resize` path, which re-acquires buffers
> WITHOUT passing `.multigpu`) — so no LINEAR re-allocation ever happened. We can't patch the system
> lib, so `CMonitorState::updateSwapchain()` now forces the fullReconfigure path on a multigpu change:
> it first clears the swapchain (empty-size reconfigure drops the buffers and the remembered size) so
> the follow-up reconfigure can no longer match the no-op/resize predicates and must re-acquire every
> buffer with the new modifier policy. `applyCrossGpuLinear` then logs the **negotiated** modifier of
> the re-allocated buffers (peek + rollback) so a live run confirms LINEAR (`0x0`) in one line, and
> damages the monitor so the fresh buffer is presented promptly (`content:` flips black→dmabuf on the
> next import). Upstream aquamarine bug worth filing: `reconfigure()`'s early-out ignores `multigpu`.

### 4.2 The failing import and the black fallback

`CXRGraphics::blitBuffer()` (`XRGraphics.cpp:326-492`), per presented buffer:

1. **DMA-BUF path** (`:361-426`): builds an attrib list — `EGL_WIDTH/HEIGHT`,
   `EGL_LINUX_DRM_FOURCC_EXT`, per-plane `FD/OFFSET/PITCH` (`:371-398`) — and calls
   `s_eglCreateImage(m_eglDisplay /* NVIDIA */, ..., attribs)` (`:400`). Two independent reasons it
   fails here:
   - **Cross-GPU**: the fd references AMD VRAM/GTT pages; NVIDIA's EGL can only accept it as an
     imported dmabuf in formats/layouts it supports, and rejects the rest as `EGL_BAD_ATTRIBUTE`.
   - **Missing modifier attribs (concrete bug, fix WP-L2)**: the list never includes
     `EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT/HI_EXT` even though aquamarine's dmabuf attrs carry the
     modifier. Implicit-modifier import is a Mesa courtesy; **NVIDIA's EGL requires explicit
     modifiers** and fails exactly with `EGL_BAD_ATTRIBUTE` otherwise — *even for LINEAR buffers*.
     On AMD-only boxes we never noticed because Mesa accepts implicit imports from its own driver.
     (Correction, see §4.1 box: the live fishfood buffers are NOT LINEAR but AMD-tiled `0x0200…`;
     passing modifiers is still necessary but not sufficient cross-GPU — hence B2/`openxr:force_linear`.)
2. **CPU data-pointer fallback** (`:428-475`): requires `BUFFER_CAPABILITY_DATAPTR`; GBM-allocated
   buffers don't have it → skipped.
3. **Clear fallback** (`:477-491`): scissors to the content rect and clears **opaque black**
   (`glClearColor(0,0,0,1)`), deliberately opaque so an empty quad doesn't punch a passthrough hole
   under `blend_mode = alpha` (`clearTex` comment `:500-502`).

Net effect: session FOCUSED, quad composited at the right pose with the right size — showing a
solid black rectangle, refreshed at full rate, with 30+ EGL errors/second as the only witness.
This precisely matches the user's report ("the XR monitor in-headset was just a black screen") and
is *unrelated to the late enable per se* — it would look identical on a login with WiVRn already
running. The "late enable" merely reordered when the user first saw it.

### 4.3 Why `first blit landed` never fires

`OpenXRManager.cpp:801` logs `first blit landed for XR monitor ...` after the first successful
blit and drives `m_hasContent`. The clear fallback still *submits* the layer (black is content as
far as the compositing path is concerned), so from the runtime's perspective everything is healthy.
There is no counter, no status field, and no event that distinguishes "importing fine" from
"black-clearing 90 times a second". WP-L6 adds a per-layer content-path state
(`ok | cpu-fallback | black-fallback`) to `hyprctl openxr status` and an event on transitions.

### 4.4 Scope boundary: the other direction of the same misconfiguration

Setting `openxr:gpu` to the *compositor's* GPU instead (`renderD129`, AMD) makes the import work
but crashes inside `xrCreateSwapchain` → WiVRn client EGL → Mesa `driUnbindContext` (coredump
39318). That wrong-GPU/cross-GPU **crash** robustness track (validate-at-start, fail-closed instead
of SIGSEGV) is being handled by a separate dedicated agent and is intentionally not designed in
this report. The lifecycle design below assumes its outcome: `start()` failures of any graphics
flavor must land in UNAVAILABLE, never crash.

### 4.5 Fix directions for the black screen (WP-L2)

| option | idea | verdict |
|---|---|---|
| **B1. Pass explicit modifiers** | Append `EGL_DMA_BUF_PLANE0_MODIFIER_LO/HI_EXT` (and per-plane) from aquamarine's dmabuf attrs when the modifier is not INVALID | **Do first.** Small, correct on all vendors, and is the difference between "NVIDIA rejects everything" and "NVIDIA imports LINEAR". Keep the no-modifier attempt as fallback for drivers without `EXT_image_dma_buf_import_modifiers`. |
| B2. Force LINEAR allocation for XR outputs when `openxr:gpu` ≠ compositor node | Headless output swapchain hints; LINEAR is the only layout both vendors agree on | **SHIPPED (`openxr:force_linear`, 2026-07-12).** NOT a no-op guard: the live buffers are AMD-tiled, not LINEAR, so B1 alone leaves a black quad. `applyCrossGpuLinear` sets `CMonitor::m_forceLinearSwapchain` → `SSwapchainOptions::multigpu` when the XR EGL node differs from the buffer allocator node; `auto`/`on`/`off`. |
| B3. CPU round-trip fallback via `gbm_bo_map` | When import fails and the buffer is LINEAR, map the dmabuf and drive the existing CPU-staging path (`:428`) | Correctness backstop (a slow desktop beats a black one), bounded cost at 2560x1440@90 is real (~1.3 GB/s memcpy) — gate behind a log-once WARN and consider frame-skipping. |
| B4. Log-once + status surface | First import failure per layer logs once with the fourcc/modifier/EGL error; per-frame repeats silenced; status shows the content-path state | Part of WP-L6; makes B1-B3 diagnosable in the field. |

Acceptance for the user's exact box: `gpu = renderD128` (NVIDIA, matching WiVRn) with the desktop
on AMD must show live desktop content in-headset, or — if the import is truly impossible — visibly
degrade (status + notification), never a silent black quad.

---

## 5. Bug (a): host-monitor rendering corruption on `openxr:enabled` toggle

No log evidence survives (§1.2), so this section is a ranked mechanism analysis with the code
pinned down for each candidate. Reproduction + bisection is part of the WP.

### 5.1 The context-ownership contract, and where the main thread breaks it

`CScopedGLContext` (`XRGraphics.cpp:43-51`):

```cpp
CXRGraphics::CScopedGLContext::CScopedGLContext(CXRGraphics& gfx) : m_gfx(gfx) {
    eglMakeCurrent(m_gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_gfx.m_xrContext);
}
CXRGraphics::CScopedGLContext::~CScopedGLContext() {
    ...
    eglMakeCurrent(m_gfx.m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}
```

It neither saves nor restores the previously-current context. On the **frame thread** that is
correct (the thread owns nothing else). But it also runs on the **main thread**, which normally has
*Hyprland's renderer context* current, in exactly the enable/disable paths the user toggled:

- enable: `initBlitGL()` (`XRGraphics.cpp:232-234`, inside `start()` step 6);
- disable: `stop()`'s per-layer GL teardown (`OpenXRManager.cpp:345-349`), then `destroyGL()`
  (`XRGraphics.cpp:653-…`, which does its own bare `eglMakeCurrent(m_xrContext)` at `:655` and
  unbinds after);
- failed enable with a reachable runtime: `abortStart()` → `destroyGL()` (§2 — *this is the
  headset-not-donned case*, so it plausibly fired during the user's session even "before WiVRn",
  if wivrn-server was partially up, and definitely fires on enable-attempts while the headset is
  off).

Hyprland's renderer tolerates the *unbind* fine: every renderer entry point calls
`makeEGLCurrent()` which re-binds iff `eglGetCurrentContext() != m_eglContext`
(`src/render/OpenGL.cpp:709-715`, callers all over `src/render/GLRenderer.cpp`).

The dangerous interleave is the reverse — Hyprland GL running **while the XR context is current**:

1. Main thread enters an XR GL burst; XR context becomes current.
2. Something inside the burst's dynamic extent runs Hyprland-side GL. The concrete path in
   `stop()`: after the scoped block, `teardownLayers()` (`OpenXRManager.cpp:1491-1526`) destroys
   headless outputs (`destroyOutputDeferred`) → monitor-removed events → workspace evacuation,
   decoration/damage updates, `GLTexture`/`GLFramebuffer` dtors — all of which call
   `makeEGLCurrent()` and *rebind Hyprland's context*. That specific sequence is *outside* the
   scoped block today (the brace at `:349` closes before `destroyGL()` at `:351`), **but**
   `destroyGL()` then re-binds the XR context bare (`XRGraphics.cpp:655`) and any listener-driven
   GL that fires between/inside these windows executes against whichever context lost the race.
3. GL object names are context-local (no share group between the XR context and Hyprland's).
   `glDeleteTextures`/`glDeleteFramebuffers`/`glGen*` issued in the wrong context operate on **the
   other owner's live objects with the same IDs** — deleted desktop textures, recycled FBO names,
   stale bindings. Symptom: "weird rendering problems on the main monitor" that persist until the
   affected surfaces re-upload — exactly a toggle-shaped corruption.

### 5.2 Candidate table

| # | mechanism | trigger window | severity/likelihood | fix |
|---|---|---|---|---|
| A1 | Hyprland GL steals the binding inside/between main-thread XR GL windows; remaining XR GL runs against the renderer context (wrong-context `glDelete*`/`glGen*`) | `stop()` teardown, `abortStart()` → `destroyGL()`, `initBlitGL()` on enable | **High.** Only candidate that explains *pixel-level corruption*; fires on every toggle with a reachable runtime (incl. headset-absent aborts) | WP-L1: save/restore in `CScopedGLContext` (main thread), assert-current at burst end, explicit `g_pHyprOpenGL->makeEGLCurrent()` after every main-thread XR GL section |
| A2 | Monitor-topology thrash: toggle destroys + recreates the declared 2560x1440 headless output (`teardownLayers()` honoring `openxr:destroy_monitors_on_stop`, default **true**, `ConfigValues.cpp:814`; re-created by `reconcileDeclaredMonitors()` on the next enable) → workspace evacuation/re-layout on eDP each way | every toggle, *even with no runtime at all* (declared monitors exist while DISABLED, §2) | Medium. Explains flicker/misplaced windows/ghost workspace churn, not texture garbage. If the user's "weird rendering" was layout-shaped, this is it — and it is the only candidate that fires with a truly absent runtime | Report 18's plugged-state work plus WP-L3's "don't churn monitors on failed starts"; consider `destroy_monitors_on_stop` semantics for *declared* monitors (keep the output, mark unplugged) |
| A3 | `destroyEGL()` terminating a display shared with the renderer | none — guarded: shared-display fallback sets `m_ownsDisplay = false` and skips terminate (`XRGraphics.hpp:94`, fallback at `XRGraphics.cpp:140-148`) | Ruled out for this box (owned NVIDIA display); keep the guard | — |
| A4 | EGL error-callback log flood (30+/s) causing hitching perceived as "rendering problems" | whenever bug (b) is active | Low as *the* explanation; real as an aggravator (7 MB log in 23 min) | WP-L6 rate-limiting |

### 5.3 The fix shape (WP-L1)

- `CScopedGLContext` gains save/restore: capture `eglGetCurrentDisplay/Context/Surface(EGL_DRAW|EGL_READ)`
  in the ctor, restore in the dtor. Frame-thread behavior is unchanged (prior = nothing). Doc 01's
  "context must never be current across runtime-interop XR calls" is unaffected — the scope
  boundaries stay exactly where they are.
- `destroyGL()`/`destroyLayerGL()`'s bare `eglMakeCurrent` calls (`XRGraphics.cpp:637`, `:655`)
  move onto the same RAII guard.
- Belt-and-braces: at the end of every *main-thread* XR GL section, assert
  `eglGetCurrentContext() == m_xrContext` before the restore and log if the binding was stolen
  (that log is the reproducer detector), then call `g_pHyprOpenGL->makeEGLCurrent()` explicitly.
- Acceptance: 20× `openxr:enabled` 0/1 keyword loop under desktop load in three regimes — runtime
  absent, runtime up + headset off (abortStart path), runtime up + headset on — with zero visual
  artifacts on the physical monitor and zero "binding stolen" logs; plus the existing hermetic
  suite green.

---

## 6. Lifecycle design: dormant, self-healing, late-runtime-aware

### 6.1 WiVRn runtime facts that shape the design

- **Manifest is always present** (`~/.config/openxr/1/active_runtime.json` →
  `/usr/lib/wivrn/libopenxr_wivrn.so`), so the loader always *loads* WiVRn; absence manifests as
  IPC-connect failure inside `xrEnumerateInstanceExtensionProperties`/`xrCreateInstance`, i.e.
  `createInstance()` false → UNAVAILABLE. Cheap to retry.
- **Server up, headset not connected**: instance creation succeeds; the failure moves to
  `xrGetSystem` (`XR_ERROR_FORM_FACTOR_UNAVAILABLE` — the OpenXR-spec'd "poll me" result) or to
  `createSession`. Both currently land UNAVAILABLE via `abortStart()` — indistinguishable from
  "no runtime" in state, logs (gated), and `hyprctl openxr status`.
- **Headset disconnect mid-session**: WiVRn ends the session — our `pollEvents()` already handles
  the spectrum (`XRSession.cpp:279-367`): `STOPPING` → `xrEndSession`, stay resident, runtime may
  re-`READY` (headset re-dons within the same server life reconnect cleanly today);
  `EXITING` → teardown to DISABLED (user intent); `LOSS_PENDING` / hard
  `XR_ERROR_INSTANCE_LOST`/`SESSION_LOST` from a dead server → teardown to UNAVAILABLE
  (`OpenXRManager.cpp:328`, `:376`, teardown handoff `:588-592`, `:624-629`). The *handling* is
  right; the *destination* (terminal UNAVAILABLE) is the gap.
- **systemd**: the package ships exactly one user unit, `/usr/lib/systemd/user/wivrn.service`
  (simple `ExecStart=/usr/bin/wivrn-server`, `WantedBy=default.target`, currently disabled+inactive
  on this box). **No socket unit** — no socket activation upstream; if we want on-demand start, we
  start the service ourselves.

### 6.2 Proposed state machine amendment

Keep the enum (doc 00 forbids renumbering); change UNAVAILABLE's *behavior* from terminal to
dormant-with-reprobe:

```
DISABLED ──enabled=1──▶ STARTING ──ok──▶ RUNNING_*
   ▲                      │fail
   │user disable          ▼
   └─────────────── UNAVAILABLE ◀──── session/instance loss (stop(), lost=true)
                        │  ▲
                 reprobe │  │ probe failed (backoff)
                 timer   ▼  │
                     [probe: createInstance→getSystem]
                        │success
                        ▼
                     STARTING (full start())
```

- **Probe** = phase 1: `xrCreateInstance` (+ extension check). Fail → destroy, re-arm timer with
  backoff (2s → 4s → 8s → … cap 30s; reset on any success or on user toggle). Phase 2: with the
  probe instance alive, `xrGetSystem` — `XR_ERROR_FORM_FACTOR_UNAVAILABLE` means "runtime up,
  headset not donned": keep the instance, poll `xrGetSystem` at a fixed gentle cadence (2s; this is
  the spec-intended pattern) *without* backoff growth, and surface the distinct sub-state.
  Success → destroy the probe instance and run the normal `start()` (start owns its own instance;
  reusing the probe instance across `initEGL` reshuffles `start()`'s carefully-ordered failure
  paths for no gain).
- **Timer**: the codebase-standard event-loop timer (`src/managers/eventLoop/EventLoopTimer.hpp`,
  `g_pEventLoopManager->addTimer(...)`, re-arm inside the callback via `self->updateTimeout(ms)`,
  `Time::steadyNow()`), owned by the manager, created lazily on first entry to UNAVAILABLE,
  removed on `start()` success, `stop()` to DISABLED, and manager destruction. Main-thread only —
  zero interaction with the frame thread or the refcount rules.
- **Status surface**: `hyprctl openxr status` gains `waiting: runtime` / `waiting: headset` +
  next-probe-in; `openxrsessionstate` IPC event already fires on state edges
  (`OpenXRManager.cpp:152-153`) — sub-state lands as a new `openxrwaiting` event so bars can show
  "put on your headset".
- **EXITING stays terminal** (user deliberately quit XR from the runtime UI → DISABLED, no
  reprobe) — the `m_instanceLost` discrimination at `stop()` (`:328`) already encodes this.
- **Config**: `openxr:reprobe` (bool, default **on** — the whole point of fishfood login),
  `openxr:reprobe_interval_ms` (base, default 2000). Numeric → no parseKeyword special-case needed;
  the timer reads them on each re-arm (main thread).

Why not "gate enable on headset connected"? Because with reprobe the gate is emergent: enable while
undonned simply parks in `waiting: headset` and completes the moment the headset connects —
strictly better UX than refusing. The only *hard* gate worth keeping is crash-shaped (wrong-GPU
validation), which is the sibling investigation's remit.

### 6.3 Starting wivrn-server on demand

| approach | pros | cons |
|---|---|---|
| **`systemctl --user start wivrn.service`** (via `Systemd::spawn`-style exec or sd-bus) | unit exists upstream; journald logs; sandboxing (`ProtectSystem=strict` etc.) already curated; restarts/lifecycle owned by systemd; idempotent (`start` on active unit is a no-op) | assumes systemd user session (fine: fishfood runs under `wayland-wm@Hyprland.service`); unit is currently *disabled* — `start` works anyway, but we should not `enable` it ourselves |
| direct spawn of `/usr/bin/wivrn-server` | no systemd dependency | orphan lifecycle ours; loses sandboxing + journal; conflicts if the unit is also used; double-start races |
| socket activation | zero-config UX | upstream ships no socket unit; wivrn's control socket is not designed for it; we'd be maintaining a distro-specific unit |

**Recommendation**: `openxr:autostart_runtime = off|wivrn` (string, default **off** — starting
servers is a policy decision the user opts into; fishfood config sets it). On entering UNAVAILABLE
with the *runtime-absent* probe result (not headset-absent), issue one `systemctl --user start
wivrn.service` per dormancy episode (not per probe tick), then let the reprobe timer pick the
server up. Failure of the systemctl itself just logs — the timer keeps probing regardless. Config
reads happen on the main thread in the timer callback; it's a string option, so it uses
`CConfigValue<std::string>` and is read *only* on the main thread (per the `e2b2e20f` rule).

### 6.4 Interaction with the toggle special-case and reconcile

- `ConfigManager.cpp:1135-1136` already routes bare `keyword openxr:enabled` to
  `onConfigReload()`; WP-L7's condition fix (`m_state == XR_STATE_DISABLED` →
  `m_state == XR_STATE_DISABLED || m_state == XR_STATE_UNAVAILABLE`) makes the keyword path able to
  kick a dormant manager, and the reprobe timer makes it unnecessary.
- `reconcileDeclaredMonitors()` runs from `init()` and every reload; under reprobe it must **not**
  churn outputs on each failed probe (probes don't call `start()`, so this holds by construction —
  only a successful probe reaches `start()` → reconcile).

---

## 7. Work packages

Sizing: S ≤ ~1 day, M ≤ ~3 days. "Bug" = fixes one of the two observed incidents; "life" =
lifecycle feature.

| WP | kind | what | effort | acceptance |
|---|---|---|---|---|
| **WP-L1** | bug (a) | `CScopedGLContext` save/restore of the prior EGL binding; RAII-ify the bare `eglMakeCurrent`s in `destroyGL`/`destroyLayerGL` (`XRGraphics.cpp:637,:655`); assert-current + "binding stolen" WARN at main-thread burst ends; explicit `g_pHyprOpenGL->makeEGLCurrent()` after main-thread XR GL | S | 20× enable/disable keyword loop in all three regimes (runtime absent / up+undonned / up+donned) with no physical-monitor artifacts and no stolen-binding WARNs; hermetic suite green |
| **WP-L2** | bug (b) | dmabuf import: pass explicit `EGL_DMA_BUF_PLANE*_MODIFIER_LO/HI_EXT` from aquamarine attrs (fallback to modifier-less attempt); log-once per layer with fourcc/modifier/error; optional `gbm_bo_map` LINEAR CPU backstop behind a WARN | S (+M for the backstop) | on the user's box (`gpu=renderD128`, desktop on AMD): live desktop content in-headset, or an explicit degraded status — never a silent black quad; zero per-frame EGL error spam in the log |
| **WP-L3** | life | dormant UNAVAILABLE: event-loop reprobe timer with backoff, two-phase probe (instance → `xrGetSystem`), distinct `waiting: runtime` / `waiting: headset` sub-states + `openxrwaiting` event, `openxr:reprobe{,_interval_ms}` | M | login with `enabled=1`, wivrn-server down, headset off → desktop unaffected; start wivrn-server + don headset (no hyprctl at all) → session reaches VISIBLE/FOCUSED unaided; probes provably stop when DISABLED |
| **WP-L4** | life | session-loss → dormant → auto-reconnect: route the existing `LOSS_PENDING`/`INSTANCE_LOST` teardown (already lands UNAVAILABLE, `OpenXRManager.cpp:376`) into WP-L3's reprobe; keep EXITING → DISABLED terminal | S (on top of L3) | kill wivrn-server mid-session → clean teardown (no crash, monitors per `destroy_monitors_on_stop`/report-18 policy) → restart server + reconnect headset → session returns unaided; `xr_session_loss` hyprtester case using the vendored Monado (kill monado-service) |
| **WP-L5** | life | `openxr:autostart_runtime = off|wivrn`: once per dormancy episode, `systemctl --user start wivrn.service` on a runtime-absent probe result (§6.3) | S | with autostart on + service inactive: login with enabled=1 starts the server exactly once; systemctl failure degrades to plain reprobe; default off changes nothing |
| **WP-L6** | bug hygiene / observability | fishfood `hyprland-xr.conf` sets `debug:disable_logs = false`; rate-limit all per-frame XR WARN/ERR paths (log-once + counter); per-layer content-path state (`ok/cpu/black`) in `hyprctl openxr status` + transition event; consider routing XR WARN/ERR past the gate the way aquamarine's connection does | S | a rerun of the black-screen scenario yields ≤ a handful of log lines that name the failing import *and* a status line that says the quad is black-falling-back; log growth bounded |
| **WP-L7** | bug (a-adjacent) | `onConfigReload()` start-condition accepts UNAVAILABLE (`OpenXRManager.cpp:1566`) so `keyword openxr:enabled 1` retries instead of silently no-oping | XS | from UNAVAILABLE, a single `hyprctl keyword openxr:enabled 1` (value unchanged) triggers a start attempt; covered by a hyprtester case |

Ordering: **L6 first** (evidence for everything else), then L1+L2 (the two user-visible bugs, both
S), then L3 → L4 → L5 (each builds on the previous), L7 rides along with any of them. The wrong-GPU
crash WPs live in the sibling investigation and should merge before L3's "any start() failure lands
UNAVAILABLE, never crashes" invariant is advertised.

---

## 8. Open questions

1. **Bug (a) ground truth**: was the corruption pixel-garbage (A1) or layout/flicker-shaped (A2)?
   One sentence from the user disambiguates; WP-L1's stolen-binding WARN then confirms A1
   mechanically on the next toggle.
2. **Did the container's validated `--gpu split` session actually show content?** Memory records
   "validated FOCUSED … user-confirmed connecting" for the same AMD-compositor/NVIDIA-XR split that
   black-screens here. If nobody looked at the quad's content, the container leg has the same bug;
   if content was visible, something differs (allocator node? modifiers?) and is worth diffing
   before WP-L2 lands.
3. **`destroy_monitors_on_stop` vs declared monitors**: on toggle-off the declared `XR-main` output
   is destroyed and recreated on enable (§5.2/A2). Should *declared* monitors instead persist
   unplugged across stop/start? That is report 18's call — flagging the dependency.
4. **Probe cost on WiVRn**: phase-2 `xrGetSystem` polling holds a WiVRn client IPC connection open
   from the compositor indefinitely. Verify an idle instance doesn't pin server resources or spam
   the WiVRn dashboard; if it does, fall back to full instance-per-probe at the phase-1 cadence.
5. **Should XR ERR-level logs bypass `debug:disable_logs`?** Aquamarine's do (by accident of
   architecture). A deliberate carve-out for `Log::ERR` from subsystems would have made all three
   incidents self-evident — but it changes a long-standing upstream default; maybe fishfood-config
   only.

---

## 9. Files referenced

- `src/openxr/OpenXRManager.hpp:26-37` (state enum + no-retry contract), `:159-164` (onConfigReload contract)
- `src/openxr/OpenXRManager.cpp:64-84` (`init()`), `:168-298` (`start()` order), `:300-321`
  (`abortStart()`), `:323-377` (`stop()`, final-state decision `:328`/`:376`), `:379-386`
  (`mapSessionState`), `:588-592` + `:624-629` (frame→main teardown handoff), `:801` (first-blit
  log), `:1491-1526` (`teardownLayers`), `:1557-1589` (`onConfigReload`, start-gate `:1566`),
  `:1621-1634` (`publishAdaptiveStringTuning`, the fixed crash site)
- `src/openxr/XRSession.cpp:53-116` (`createInstance`), `:119-155` (`getSystem`), `:279-367`
  (`pollEvents`: READY/STOPPING/EXITING/LOSS_PENDING + `XR_ERROR_INSTANCE_LOST` sweep)
- `src/openxr/XRGraphics.cpp:43-51` (`CScopedGLContext`), `:66-152` (display selection: follow-
  primary default `:76-81`, fail-loud override `:133-138`, shared-display fallback `:140-148`),
  `:326-492` (`blitBuffer`: attribs `:371-398`, import `:400`, gated WARN `:425`, CPU `:428-475`,
  black clear `:477-491`), `:637`/`:655` (bare `eglMakeCurrent` in teardown)
- `src/render/OpenGL.cpp:709-715` (`makeEGLCurrent` guard); `src/render/GLRenderer.cpp` (callers)
- `src/config/legacy/ConfigManager.cpp:1125-1153` (keyword special-cases incl. `openxr:enabled`)
- `src/config/values/ConfigValues.cpp:624` (`debug:disable_logs` default), `:814`
  (`destroy_monitors_on_stop` default)
- `src/debug/log/Logger.cpp:14-25` (gate), `:41-51` (`recheckCfg`); `src/Compositor.cpp:304`
  (aquamarine's gate-bypassing connection)
- `src/managers/eventLoop/EventLoopTimer.hpp` (reprobe timer primitive)
- `/usr/lib/systemd/user/wivrn.service` (quoted §6.1); `~/.config/openxr/1/active_runtime.json`
- Live evidence: `$XDG_RUNTIME_DIR/hypr/e2b2e20f…_1783820753_915503046/hyprland.log` (43,210×
  `EGL_BAD_ATTRIBUTE`, amdgpu/renderD129 allocator lines, zero `[OPENXR]` lines);
  `~/.cache/hyprland/hyprlandCrashReport64278.txt` (login-time `publishAdaptiveStringTuning` SEGV,
  build `6ced4f9f`); `hyprctl openxr status` (FOCUSED WiVRn session concurrent with the black quad)
- Sibling: `docs/openxr/research/18-*` (XR monitor plugged-state tracking — referenced §2/§7/Q3)
