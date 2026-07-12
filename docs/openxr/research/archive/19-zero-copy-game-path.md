# 19 — Zero-Copy Game Path (XR direct scanout + per-monitor GPU affinity)

Status: **research / design only. Nothing here is implemented.** No builds, no live runs beyond the
read-only source reads cited. Author: research pass 2026-07-11 against branch `hypxrland` (worktree
base `cecb3035`). Source-read evidence into *this* tree; PCIe/bandwidth numbers are estimates from
first principles, flagged where they need a live measurement.

The user's workload: a flat game plays **fullscreen on an XR monitor** in-headset. Hybrid laptop —
AMD iGPU (`renderD129`) is Hyprland's render/scanout GPU; NVIDIA dGPU (`renderD128`) runs WiVRn's
compositor + NVENC and is `openxr:gpu`. A PRIME-offloaded game renders on **NVIDIA**, but its frames
make a **NVIDIA→AMD→NVIDIA** round trip before they reach the encoder (§2). This report designs two
stacking mechanisms to kill the round trip: (1) an **XR direct-scanout fast path** that hands the
client's own dmabuf to our blit instead of a composited output buffer, and (2) **per-XR-monitor GPU
affinity** so the buffer is born on NVIDIA.

Cross-refs: report 17 (§4.5 WP-L2 = the dmabuf **modifier attribs** import fix — a hard prerequisite
for *any* NVIDIA import here; WP-L1 = the main-thread GL context save/restore), report 18 (monitor
plugged-state — orthogonal), `docs/openxr/01-session-graphics.md` (blit + EGL context contract),
`docs/openxr/02-*` (headless output lifecycle). Sibling agents are concurrently touching monitor
plugged-state and the wrong-GPU crash; this report is research-only and assumes the wrong-GPU crash
work lands the "any graphics failure fails closed, never SIGSEGV" invariant.

---

## TL;DR

1. **Hyprland already has everything the fast path needs, and our side is nearly free.** Direct
   scanout (`CMonitor::attemptDirectScanout`, `src/output/Monitor.cpp:2092`) commits the *client's*
   buffer to the output via `m_output->state->setBuffer(PBUFFER)` (`:2163`) and returns before any
   compositing (`Renderer.cpp:2044-2054`). Our per-layer presented listener already reads exactly
   that field — `pmon->m_output->state->state().buffer` (`XRMonitorLayer.cpp:35`). **So if DS ran
   for the XR headless output, `takeLatestBuffer()` would already hand the frame thread the game's
   own NVIDIA dmabuf** — the whole existing `m_bufMu` handoff + blit works unchanged, and the blit's
   NVIDIA-into-NVIDIA import is native (WP-L2 modifiers still required). No new cross-thread plumbing.

2. **The eligibility is a perfect match and DS is *off* today.** `isSolitaryBlocked`
   (`Monitor.cpp:1798`) already encodes "fullscreen + opaque + no subsurfaces/popups + no
   floating-over-FS + no overlays/special/lock" — precisely "a flat game owns the monitor." But
   `render:direct_scanout` **defaults to 0/disable** (`ConfigValues.cpp:557`) and is a *global*
   knob that would also change the physical eDP. We need a **per-XR-monitor** opt-in, not the global.

3. **Chrome does NOT force us out of the fast path** (correcting the brief's assumption). Our chrome
   is drawn by *our blit* into the XrSwapchain's transparent **margin** (`XRGraphics.cpp:349-423`),
   not by the compositor. Hovering/grabbing the quad adds no windows, so the game stays solitary and
   DS stays eligible; the blit simply draws content (from the DS'd client buffer) into the inner
   content rect and chrome into the margin, exactly as it does from a composited buffer. The only
   real DS breakers are the standard solitary blockers (a popup, a notification, losing fullscreen).

4. **The one copy that must remain is our blit into the runtime-allocated swapchain image.** OpenXR
   swapchain images are runtime-owned; we `glDrawArrays`/`glBlitFramebuffer` *into* `dstTex`
   (`XRGraphics.cpp:410-420`) — we cannot hand the client dmabuf to the runtime as the swapchain
   image. With the fast path that copy is **on-device NVIDIA→NVIDIA** and feeds NVENC locally: zero
   PCIe crossings for the game path, down from two (~2.6 GB/s aggregate at 2560×1440@90, §5).

5. **GPU affinity (mechanism 2) is the weaker lever because Hyprland has exactly one renderer.**
   There is a single `g_pHyprOpenGL` EGL/GLES context bound to the compositor's primary DRM node
   (AMD here); aquamarine, not a second renderer, handles cross-GPU scanout copies. "Composite this
   XR monitor on NVIDIA" therefore means a *second renderer* (L/XL) — out of scope. The tractable
   affinity variant only moves where the composited buffer is *allocated* (NVIDIA), saving the
   blit-time crossing but keeping a composite-time one: 2 crossings → 1. It is the **baseline for the
   windowed / non-solitary case** that DS cannot cover, not a replacement for DS.

6. **Recommendation:** ship the **XR direct-scanout fast path** (WP-Z2/Z3) as the primary win —
   after WP-L2 (report 17) lands the modifier import — with **auto-fallback to the composited path**
   the instant a solitary blocker appears (WP-Z4). Treat GPU affinity as a **later baseline** (WP-Z6)
   for windowed desktop-in-headset, gated on whether the single copy it saves is worth the
   AMD-allocated-desktop-window cost. Most of Z2-Z4 is testable headless with the vendored Monado
   *null* on a single GPU (eligibility switching, buffer identity, fallback dance); the NVIDIA→NVIDIA
   payoff itself needs the live Quest (§7).

---

## 1. Today's data path (the round trip, code-traced)

The user runs the game fullscreen on `XR-main`, a headless output. Frames are paced by the XR frame
thread (`SCHEDULE_FRAMES` → main thread `mon->scheduleFrame()`, `OpenXRManager.cpp:479-489`), which
drives the normal compositor render for that output.

**Frame N, no fast path (what happens now):**

1. **Composite (crossing 1, NVIDIA→AMD).** `renderMonitor` (`Renderer.cpp:~2000+`) composites the
   XR output's workspace on the **single** GL renderer, which lives on the compositor GPU (**AMD**).
   The game's client texture is a **NVIDIA** PRIME buffer; sampling it into the AMD framebuffer
   reads a full frame across PCIe. Output: the headless output's own swapchain image, GBM-allocated
   on the AMD node (report 17 logged `GBM: Allocated ... XR24 modifier LINEAR` on `renderD129`).
2. **Present.** `m_output->commit()` stores that AMD buffer as `state().buffer`; `presented` fires;
   our listener stashes it (`XRMonitorLayer.cpp:31-45`).
3. **Blit (crossing 2, AMD→NVIDIA).** The frame thread `takeLatestBuffer()`s the AMD buffer and
   `blitBuffer()` imports it into the **NVIDIA** XR EGL display (`XRGraphics.cpp:400`) and renders
   into the NVIDIA XrSwapchain image. Report 17 shows this import *failing* today (43k
   `EGL_BAD_ATTRIBUTE`) for want of modifier attribs — WP-L2. Even once fixed, it is a PCIe crossing.
4. **Encode.** WiVRn/NVENC consumes the NVIDIA swapchain image — local.

Net: **two full-frame PCIe crossings** (NVIDIA→AMD, AMD→NVIDIA) per game frame, plus the composite's
GPU cost on the wrong GPU, plus WP-L2's import fragility. The game was born on NVIDIA and dies on
NVIDIA; it only visits AMD because Hyprland's one renderer lives there.

---

## 2. Mechanism 1 — XR direct scanout (the fullscreen fast path)

### 2.1 How Hyprland's direct scanout works

Entry: `CProtostarRenderer::renderMonitor` (`Renderer.cpp:2042-2057`) — before any compositing, if
`canAttemptDirectScanoutFast()` (`Monitor.cpp:2236`: there is a solitary client / a prior scanout /
DS active) it calls `attemptDirectScanout()`; on success it **returns without compositing** and marks
the mirror FB stale.

`attemptDirectScanout()` (`Monitor.cpp:2092-2220`):

- `isDSBlocked()` (`:1999-2090`) gates it. Reasons: user disable (`render:direct_scanout==0`),
  windowed / non-`CONTENT_TYPE_GAME` under `auto` mode (`:2019-2031`), mirror, screen-record
  (`m_directScanoutBlocked`), software-locked pointer, **no solitary candidate**, no
  texture/buffer, `bufferSize != m_pixelSize || transform != m_transform` (`:2063`), non-dmabuf/shm
  (`:2069-2075`), and color-management/HDR mismatches (`:2077-2087`).
- The candidate itself is `m_solitaryClient` set by `recheckSolitary()` (`:1917-1928`) iff
  `isSolitaryBlocked()` (`:1798-1915`) is clear: `FSMODE_FULLSCREEN`, no active special workspace,
  no notification/error overlay, not session-locked, no DND, workspace alpha==1 and no render
  offset, candidate `opaque()` (`:1863`), candidate size/pos == monitor and not animating (`:1869`),
  no OVERLAY / faded-in TOP layer surfaces, no floating window allowed over fullscreen, no special
  workspace on this monitor, and `getSolitaryResource()` non-null (**single surface, no subsurfaces
  or popups**, `:1911`).
- On pass: save format/buffer/presentation-mode (rollback guard `:2146-2155`),
  `setBuffer(PBUFFER)` where `PBUFFER = PSURFACE->m_current.buffer.m_buffer` (`:2102`,`:2163`), test
  + `m_output->commit()` (`:2168-2191`), and (only if `isMultiGPU()`) attach an explicit in-fence
  (`:2178-2187`). It **locks the client buffer for the backend's use** and releases on page-flip
  (`:2208-2217`).

Leaving DS: `handleDSleave()` (`:2222-2234`) restores the saved DRM format and marks blur dirty.

**This is entirely generic over monitor type** — nothing in the solitary/DS logic is DRM-specific in
its *decision*; only the final `m_output->commit()` differs by backend. That matters next.

### 2.2 Does it run for headless (XR) outputs? — the key question

`m_output` for an XR monitor is an `Aquamarine::CHeadlessOutput` (`OpenXRManager.cpp:1272-1273`
`impl->createOutput` on the headless backend). Its `commit()`/`test()`/`getRenderFormats()` exist
(`/usr/include/aquamarine/backend/Headless.hpp:17-22`) but have no KMS underneath — the aquamarine
`.cpp` is not present in this tree (system package headers only), so the following is **inference to
be validated live (§7)**, high-confidence from the header surface and report-17 behavior:

- Headless `test()`/`commit()` accept whatever buffer is in the output state and simply store it +
  fire the frame/present callbacks — there is no scanout hardware to reject a format/modifier. So
  `attemptDirectScanout()`'s `setBuffer(client) → test() → commit()` should **succeed trivially** and
  fire `presented` carrying the client buffer.
- `isMultiGPU()` (`Monitor.cpp:2240-2275`) compares the output backend's `preferredAllocator()` DRM
  node against the compositor node. The headless backend's allocator is the compositor's primary
  (AMD) → **not** multi-GPU → the explicit-fence branch is skipped. But the *client* buffer is
  NVIDIA. That client→our-read synchronization is a real gap (§2.6, open question Q3).

**If §2.2 holds, mechanism 1 is astonishingly cheap on our side:** flip DS on for the XR output and
`takeLatestBuffer()` starts returning the client's NVIDIA dmabuf; `blitBuffer()` imports it natively
into the NVIDIA XR context (WP-L2 modifiers), and `renderMonitor` stops compositing the output
entirely (early return). Both crossings gone; the composite GPU cost gone too.

### 2.3 Two ways to actually engage it

| | **Z-A: reuse Hyprland DS on the headless output** | **Z-B: XR-scoped client-buffer capture** |
|---|---|---|
| Mechanism | Turn DS on for XR outputs; let `attemptDirectScanout` commit the client buffer; presented listener already forwards it | On the main thread, when the XR output is solitary, grab `PCANDIDATE->getSolitaryResource()->m_current.buffer` ourselves and push it through `m_bufMu`, suppressing the compositor render |
| New code | Tiny: a per-monitor DS enable + let existing paths flow | Moderate: our own eligibility check mirroring `isSolitaryBlocked`, our own buffer-lock/release, our own "skip composite" |
| Saves composite GPU work? | **Yes** — `renderMonitor` early-returns on DS success | Only if we *also* suppress the compositor render (extra work); otherwise we double-render |
| Coupling | To headless `commit()` semantics + a config surface that is global today | Self-contained in `src/openxr/`, no dependence on headless DS |
| Correctness reuse | Inherits Hyprland's battle-tested solitary/blocker set for free | We must re-derive and maintain the blocker set (drift risk) |
| Recommendation | **Preferred.** Least code, reuses the blocker set, kills the composite too | Fallback only if headless `commit()` proves hostile to arbitrary client buffers (§7 test T1) |

Go with **Z-A**. Its only real cost is giving the XR output its **own** DS enable independent of the
global `render:direct_scanout` (which must stay off for the eDP). Options for that toggle:

- New per-`xrmonitor` bool (parsed in `SXRMonitorParams`, `XRMonitorConfig.hpp:70-83`, e.g.
  `direct:on`), stored on the layer, consulted by a small hook. But `isDSBlocked` reads the *global*
  `render:direct_scanout` — the cleanest seam is to have `isDSBlocked` treat an **XR headless output
  with the per-monitor flag set** as `direct_scanout=enable(1)` (a targeted branch keyed on
  `m_output->getBackend()->type()==AQ_BACKEND_HEADLESS` + a `CMonitor` flag the manager sets from the
  layer). This keeps all the *other* blockers intact and leaves the global default untouched.
- Global `openxr:direct_scanout` (default on) as the master gate, ANDed with per-monitor.

### 2.4 Chrome interaction (simpler than feared)

The brief assumed chrome forces compositing. It does not. Chrome lives in the XrSwapchain's
transparent margin and is drawn by **our** blit (`XRGraphics.cpp:349-423`, margin clear + content
rect + chrome pass), regardless of whether the content came from a DS'd client buffer or a composited
output buffer. Hover/grab of the quad adds no windows → the game stays solitary → DS stays engaged.
Concretely:

- **Hidden chrome (common):** `m_chromeAlpha==0`, no chrome draw. Blit imports the DS'd client
  buffer into the content rect. Pure fast path.
- **Visible chrome (hovering/grabbing to move/resize the monitor):** same DS'd client buffer into the
  content rect; chrome drawn over the margin. Still fast path. The animation-only redraw path
  (`OpenXRManager.cpp:768-776`, no-new-buffer chrome tick using the `m_contentTex` snapshot) is moot
  during gameplay because a running game delivers a new buffer every frame.

**The real fallback dance is DS↔composited on the *content source*, driven purely by the solitary
blockers**, and it already has a clean seam: `renderMonitor` calls `handleDSleave()` and falls
through to compositing the instant `attemptDirectScanout()` fails (`Renderer.cpp:2055-2056`). Because
the presented buffer swaps from client→composited transparently, our listener + blit need **zero
changes** to survive the transition — the next `presented` just carries a composited buffer again.
The only flicker risk is a one-frame content stutter at the transition (client buffer at size S vs a
composited buffer); acceptable, and no worse than DS on a physical monitor. WP-Z4 verifies no visible
seam when a popup/notification briefly appears over the game.

### 2.5 UV / margin remap, format, modifiers, cursor

- **Content-rect remap:** already handled. The DS'd client buffer is the full game frame at the
  monitor's pixel mode; `blitBuffer` renders it into the inner content rect via
  `contentX/contentGL/viewport` (`XRGraphics.cpp:336-343,413`). Client buffer size == `m_contentSize`
  == monitor mode, so clicks stay pixel-exact (the UV remap invariant is unchanged). If the game
  renders below native mode, DS's own `bufferSize != m_pixelSize` blocker (`Monitor.cpp:2063`)
  declines it and we composite — correct.
- **Format/modifier:** the client buffer is NVIDIA-allocated (PRIME game) and imported into the
  NVIDIA XR EGL — native vendor match. But NVIDIA's EGL **requires explicit modifier attribs**; our
  import list omits `EGL_DMA_BUF_PLANE*_MODIFIER_LO/HI_EXT` (`XRGraphics.cpp:371-398`). **WP-L2
  (report 17 §4.5) is a hard prerequisite** — without it the fast path imports fail exactly like the
  cross-GPU path does today. This is the single blocking dependency.
- **Cursor:** under DS the compositor draws no cursor into the buffer (on physical outputs a HW
  cursor plane covers this; headless has none). For a fullscreen flat game the cursor is the game's
  own (client-drawn or relative-mode hidden), so this is usually invisible-by-design. But the XR
  synthetic pointer's cursor (ray → absolute pointer → compositor cursor) **would not appear** on a
  DS'd quad. For gameplay that is fine; for a hypothetical fullscreen-but-cursor-driven app it is a
  regression. Flag as a known limitation; the ray/aim overlay (report 14) is the real in-headset
  pointer and is drawn independently.

### 2.6 Synchronization (the subtle one)

On a physical multi-GPU DS, aquamarine inserts an explicit in-fence (`Monitor.cpp:2178-2187`) so KMS
waits for the client's render. For our headless-output-then-blit path, `isMultiGPU()` is false (the
*output allocator* is AMD == compositor), so no fence is attached — yet the buffer we then read is a
**NVIDIA** client buffer whose render may not be complete when `presented` fires. Our blit already
runs inside `CScopedGLContext` and Monado inserts its own release fence, but that governs
*our*→*runtime* sync, not *client*→*our-read*. The client buffer carries an implicit/explicit release
sync object; we must ensure the import waits on it (dmabuf implicit fencing usually covers this on a
single GPU, but cross-vendor implicit fencing is unreliable). **Open question Q3**: does the DS'd
client buffer need an explicit acquire wait before our EGLImage sample, and does aquamarine's
buffer-lock (`:2213`) already serialize it? Validate with a fast-moving game frame for tearing/stale
frames (§7 T4).

---

## 3. Mechanism 2 — per-XR-monitor GPU affinity (the baseline)

### 3.1 Hyprland is single-renderer; aquamarine does the cross-GPU copy

There is exactly one GL renderer (`g_pHyprOpenGL`), bound to one DRM node — the compositor primary
(`g_pCompositor->m_drm.fd`, AMD here). `CProtostarRenderer` computes `m_mgpu = drmDevices > 1`
(`Renderer.cpp:114`) but does **not** spin up a second context; multi-GPU means aquamarine's DRM
backend copies the single renderer's output onto the secondary scanout GPU. `isMultiGPU()`
(`Monitor.cpp:2240`) is per-output allocator-vs-compositor, used to decide fencing, not to pick a
renderer. **Consequence: "composite an XR monitor on NVIDIA" requires a second renderer/EGL context
on the NVIDIA node — a large architectural change (L/XL), out of scope here.**

### 3.2 What affinity *can* do without a second renderer

Move where the XR output's swapchain is **allocated**, not where it renders:

- Headless outputs get their swapchain from the headless backend's `preferredAllocator()` — the
  compositor primary (AMD). If we could allocate the XR output's swapchain on the **NVIDIA** node,
  the composite still runs on AMD (single renderer) and aquamarine copies AMD→NVIDIA into the
  NVIDIA-allocated buffer at present. Then our blit imports a NVIDIA buffer (native) and NVENC is
  local. Crossings: **NVIDIA→AMD (composite sample) + AMD→NVIDIA (present copy) = still 2**, but the
  buffer we hand the runtime is NVIDIA-native, removing the blit's cross-vendor import fragility.
  This is **worse or equal** to today's crossing count for the game path and **strictly worse** than
  DS. Its only value: correctness/robustness for the **windowed / non-solitary** case where DS is
  ineligible and the desktop is a mix of AMD app windows anyway.
- Desktop app windows are AMD-allocated; on a NVIDIA-composited monitor each would need an
  AMD→NVIDIA copy per window. With a single AMD renderer that cost is inverted (the game, on NVIDIA,
  would cross to AMD). So affinity's window economics only favor the NVIDIA side when the *majority*
  of the monitor's content is NVIDIA-born — i.e. exactly the fullscreen-game case that DS already
  captures better. **Affinity does not pay for itself for the game path.**

**Verdict:** affinity is a *baseline* for windowed desktop-in-headset (where DS can never engage),
implemented as "allocate the XR output swapchain on `openxr:gpu`" so the runtime-facing buffer is
NVIDIA-native and the blit import is robust. It is **not** a game-path optimization and must not be
sold as one. Ship it after DS, gated on measuring whether the saved blit-time crossing beats the
NVIDIA-allocation + AMD→NVIDIA present-copy cost for a real desktop workload (§7 T5).

### 3.3 AQ_DRM_DEVICES host implications

The container split already pins the nested compositor to the host-compositor GPU via
`AQ_DRM_DEVICES` (`containers/session/session-launch.sh:110`, resolver in
`scripts/lib/gpu.sh:42`, split plumbing `scripts/xr-container.sh:285-336`) while `openxr:gpu` targets
NVIDIA. On the **host** (fishfood), the same split holds without a container: compositor on AMD,
`openxr:gpu=renderD128`. `AQ_DRM_DEVICES` is a colon-separated list; **aquamarine treats the first
entry as primary** (renderer + `preferredAllocator`). Host-side implications if we want affinity:

- Listing `AMD:NVIDIA` keeps AMD primary (renderer stays on AMD, correct — we do not want to move the
  eDP desktop onto NVIDIA). Adding NVIDIA merely makes it *available* as a secondary node so a
  headless output could request allocation there — but the headless backend's `preferredAllocator`
  is still the primary. So affinity needs an aquamarine-level "allocate this output on node X" hook
  (`getAllocators()` returns a vector, `Headless.hpp:54`), or we allocate the swapchain ourselves and
  hand it in. Non-trivial; flagged as the real cost of Z6.
- **Hazard:** reordering `AQ_DRM_DEVICES` to put NVIDIA first would move the *entire* compositor
  (eDP included) onto NVIDIA — catastrophic for a hybrid laptop (power, the NVIDIA GBM XR24 alloc
  failure noted in memory for the hermetic NVIDIA leg). Any host-side `AQ_DRM_DEVICES` change must
  keep the iGPU first. Document loudly.

---

## 4. What the copies cost, and what stays

Per game frame at **2560×1440, XRGB8888 (4 B/px)** = 14.75 MB. At **90 Hz** = **1.33 GB/s** per
one-directional full-frame copy.

| path | crossings | PCIe traffic (game path) | extra GPU work | notes |
|---|---|---|---|---|
| **today** (no DS, no affinity) | NVIDIA→AMD (composite sample) + AMD→NVIDIA (blit import) | ~**2.66 GB/s** aggregate | composite on the wrong GPU + WP-L2-fragile import | plus ~1 frame of latency per crossing |
| **DS fast path** (Z-A + WP-L2) | none | **0** | composite skipped entirely (DS early-return) | one on-device NVIDIA→NVIDIA blit into the runtime swapchain remains |
| **affinity only** (Z6) | NVIDIA→AMD (composite sample) + AMD→NVIDIA (present copy) | ~**2.66 GB/s** | composite still on AMD | buffer NVIDIA-native → robust import; no crossing *reduction* for the game |

**The copy that always remains:** the blit into the runtime-allocated XrSwapchain image
(`XRGraphics.cpp:410-420`). OpenXR swapchain images are runtime-owned; the KHR_opengl_es_enable
contract requires us to *render into* the acquired image (we cannot substitute the client dmabuf as
the swapchain image). Confirmed by the acquire→blit→release loop (`OpenXRManager.cpp:778-808`). With
the fast path this copy is on-device on NVIDIA and its output feeds NVENC without leaving the GPU —
**the payoff statement: a game frame born on NVIDIA now lives and dies on NVIDIA, one on-device copy,
zero PCIe round trips, and the encoder reads a buffer that never crossed the bus.** Estimated
latency reduction ~2 frames (~22 ms at 90 Hz) plus the eliminated PCIe contention; measure live
(§7 T3).

---

## 5. Decision summary

| question | answer | evidence |
|---|---|---|
| Does DS run for headless outputs today? | **No** — `render:direct_scanout` defaults to 0; needs a per-XR opt-in | `ConfigValues.cpp:557`; `Monitor.cpp:2013` |
| If enabled, would our side "just work"? | **Nearly** — presented listener already forwards `state().buffer` = client buffer; blit unchanged; WP-L2 modifiers required for the NVIDIA import | `XRMonitorLayer.cpp:35`; `Monitor.cpp:2163`; `XRGraphics.cpp:371-400` |
| Does chrome force compositing? | **No** — chrome is our margin blit, DS-compatible; only standard solitary blockers break DS | `XRGraphics.cpp:349-423`; `Monitor.cpp:1798-1915` |
| Can we composite an XR monitor on NVIDIA? | **Not without a second renderer** (L/XL, out of scope) | single `g_pHyprOpenGL`; `Renderer.cpp:114` |
| Does affinity help the game path? | **No** — 2 crossings → still 2; helps only windowed robustness | §3.2 |
| Does one on-device copy remain? | **Yes, unavoidable** — runtime-owned swapchain must be rendered into | `OpenXRManager.cpp:778-808`; `XRGraphics.cpp:410-420` |
| Hard prerequisite? | **WP-L2** (report 17) — modifier attribs, or every NVIDIA import fails | report 17 §4.5 |

**Recommendation:** DS fast path first (biggest, cleanest win, reuses the blocker set), affinity as a
later windowed-case baseline only if measurement justifies it.

---

## 6. Work packages

Sizing: XS ≤ ~½ day, S ≤ ~1 day, M ≤ ~3 days, L ≤ ~1 week.

| WP | dep | what | effort | acceptance |
|---|---|---|---|---|
| **WP-L2** | — (report 17) | **Prerequisite.** dmabuf import passes explicit modifier attribs; NVIDIA imports LINEAR/tiled buffers instead of `EGL_BAD_ATTRIBUTE` | S | live: any NVIDIA import (cross-GPU or DS'd client) succeeds; no per-frame EGL spam |
| **WP-Z1** | — | Confirm headless `commit()`/`test()` accept an arbitrary client dmabuf and fire `presented` with it (test harness or read vendored aquamarine); decide Z-A vs Z-B (§2.3) | S | a headless output committed a foreign dmabuf and re-presented it verbatim (test T1) |
| **WP-Z2** | Z1 | Per-XR-monitor DS enable: a `CMonitor` flag set from the layer + an `isDSBlocked` branch that treats a flagged headless output as `direct_scanout=enable` **without** touching the global; `xrmonitor ... direct:on` parse + `openxr:direct_scanout` master (default on) | M | with a fullscreen opaque game on `XR-main`, `hyprctl` shows the XR output solitary and DS active; `renderMonitor` stops compositing it; presented buffer identity == client buffer |
| **WP-Z3** | Z2, L2 | Blit consumes the DS'd client buffer natively (should be automatic via the presented path); verify content-rect remap, alpha-pin, click pixel-exactness unchanged | S | in-headset the game renders correct, full-rate, from its own NVIDIA buffer; margins transparent; clicks land pixel-exact |
| **WP-Z4** | Z2 | Fallback dance: momentarily break solitary (popup, notification, un-fullscreen) → DS leaves, composite resumes → re-fullscreen → DS re-engages; verify no crash and ≤1-frame seam | S | scripted popup/notification over the game shows no artifact beyond a single frame; `handleDSleave` path exercised |
| **WP-Z5** | Z3 | Synchronization: ensure the client buffer's render completes before our EGLImage read (explicit acquire wait if implicit fencing is insufficient cross-vendor); guard against tearing/stale frames | S–M | fast-motion game frame shows no tearing/stale content in-headset over a sustained run |
| **WP-Z6** | — | GPU affinity baseline: allocate the XR output swapchain on `openxr:gpu` (aquamarine per-output allocator hook or self-allocated swapchain handed in) so the runtime-facing buffer is NVIDIA-native for the **windowed** case; keep AQ_DRM_DEVICES iGPU-first | M–L | a windowed desktop-in-headset monitor presents a NVIDIA-allocated buffer; import robust; measured cost documented vs today |
| **WP-Z7** | Z6 | Measurement + guardrails: instrument crossings/latency; `hyprctl openxr status` per-layer "content-path: direct/composited/affinity"; document the AQ_DRM_DEVICES reorder hazard | S | status distinguishes the three content paths; a before/after latency number recorded on the live box |

**Ordering:** WP-L2 → WP-Z1 → WP-Z2 → (WP-Z3 ∥ WP-Z4) → WP-Z5; then WP-Z6 → WP-Z7 only if T5
measurement justifies affinity. Z2-Z4 is the shippable game-path win; Z6 is optional.

---

## 7. Testing — hyprtester vs live Quest

The hermetic suite runs headless with the vendored Monado **null** on the **reliable AMD** leg (the
NVIDIA hermetic path has the known aquamarine GBM XR24 alloc failure — memory). So the suite has **no
NVIDIA GPU on the reliable path**, i.e. it **cannot** measure the NVIDIA→NVIDIA payoff. What it *can*
cover, single-GPU, is the logic:

| test | what it proves | reliable-leg feasible? |
|---|---|---|
| **T1** (Z1) | headless output re-presents a committed foreign dmabuf verbatim (`state().buffer` identity check via a gtest / a harness output-commit probe) | **Yes** — pure aquamarine/output behavior, no vendor split |
| **T2** (Z2) | with a scripted fullscreen opaque client on an XR monitor, `m_solitaryClient` is set, DS engages, and `takeLatestBuffer()` returns the *client* buffer not a composited one (buffer-pointer assertion) | **Yes** — single GPU; identity is what matters, not the vendor |
| **T3** (payoff) | crossings eliminated / latency reduced | **No** — needs live Quest + the real AMD-compositor/NVIDIA-encode split |
| **T4** (Z4) | fallback dance: popup/notification breaks solitary, DS leaves, re-engages, no crash | **Yes** — logic only |
| **T5** (Z6) | affinity cost/benefit for a windowed workload | **No** — needs the dual-GPU box |

So Z2-Z4's *correctness* (eligibility switching, buffer identity, fallback) is fully hermetic-suite
testable; the *performance* claim and Z5/Z6 are **live-Quest-gated** on the user's exact box
(`gpu=renderD128`, desktop on AMD). Batch the live validation with a running flat game: confirm (a)
in-headset content is correct and full-rate from DS, (b) `hyprctl openxr status` reports content-path
`direct`, (c) a Tracy/timing capture shows the composite skipped and one on-device copy, (d) breaking
fullscreen falls back cleanly. **Never kill any Hyprland**; drive via `hyprctl` against the user's
session read-only and the game's own fullscreen toggle.

---

## 8. Open questions

1. **Q1 (headless commit contract).** Does `CHeadlessOutput::commit()` accept an arbitrary client
   dmabuf (foreign format/modifier) and re-present it, or does it validate against `getRenderFormats`
   and reject? The aquamarine `.cpp` is not in-tree (system headers only). WP-Z1/T1 answers it; if it
   rejects, fall back to Z-B (XR-scoped capture) which never routes through the output commit.
2. **Q2 (per-monitor DS seam).** Is the cleanest opt-in a targeted `isDSBlocked` branch keyed on
   `AQ_BACKEND_HEADLESS` + a `CMonitor` flag, or should the global `render:direct_scanout` gain a
   per-monitor override upstream-generically? The former is self-contained; the latter is more
   honest but touches shared render config other monitors read.
3. **Q3 (cross-vendor client→read sync).** With `isMultiGPU()` false for the headless output but the
   client buffer NVIDIA-born, is implicit dmabuf fencing sufficient before our EGLImage sample, or do
   we need an explicit acquire wait (as physical multi-GPU DS attaches at `Monitor.cpp:2178`)? Tearing
   under fast motion (T4) is the tell. WP-Z5.
4. **Q4 (cursor).** For the fullscreen-game case the missing compositor cursor is fine, but should DS
   be declined when the XR synthetic pointer is *actively* hovering the quad (so the cursor stays
   visible), or is the ray/aim overlay (report 14) always the in-headset pointer and the compositor
   cursor irrelevant on XR quads? UX call.
5. **Q5 (affinity worth it?).** Does WP-Z6's NVIDIA-allocated swapchain actually beat today for a
   *windowed* desktop-in-headset, given the AMD→NVIDIA present-copy it introduces and the AMD app
   windows that dominate that view? If not, drop Z6 and rely on DS for games + accept the current
   path for windowed use.
6. **Q6 (container parity).** The container `--gpu split` leg (AMD nested / NVIDIA XR) is the same
   topology; does it exhibit the round trip too, and can Z2-Z4 be exercised there against the vendored
   Monado to get *some* dual-node coverage short of the Quest? Memory notes NVIDIA-in-container is
   deterministic-broken for GBM XR24, so likely no — but worth a single check.

---

## 9. Files referenced

- `src/output/Monitor.cpp:1798-1915` (`isSolitaryBlocked` — the DS eligibility set), `:1917-1928`
  (`recheckSolitary`), `:1999-2090` (`isDSBlocked` incl. global `render:direct_scanout` read `:2013`,
  content-type gate `:2019-2031`, size/transform `:2063`, dmabuf `:2069-2075`), `:2092-2220`
  (`attemptDirectScanout` — `setBuffer(client)` `:2163`, multi-GPU fence `:2178-2187`, commit `:2191`,
  buffer lock `:2208-2217`), `:2222-2234` (`handleDSleave`), `:2236-2238`
  (`canAttemptDirectScanoutFast`), `:2240-2275` (`isMultiGPU`), `:2664-2698` (`updateSwapchain`,
  `needsACopyFB`)
- `src/render/Renderer.cpp:114` (`m_mgpu` = drmDevices>1, single renderer), `:2042-2057` (DS
  short-circuit before compositing), `:2128-2129` (solitary render path)
- `src/openxr/XRMonitorLayer.cpp:31-45` (presented listener reads `m_output->state->state().buffer`),
  `:76-84` (`takeLatestBuffer`), `:86-96` (`retireBuffer`); `XRMonitorLayer.hpp:28-45` (frame-thread
  zero-refcount rule), `:96-107` (`m_bufMu` handoff fields)
- `src/openxr/XRGraphics.cpp:326-492` (`blitBuffer`: margin clear `:349-359`, dmabuf import attribs
  **missing modifiers** `:371-398`, `s_eglCreateImage` `:400`, content-rect draw `:410-420`, CPU
  fallback `:428-475`, black clear `:477-491`), `:494-505` (`clearTex`, alpha-pin rationale)
- `src/openxr/OpenXRManager.cpp:479-489` (`SCHEDULE_FRAMES` pacing → `mon->scheduleFrame()`),
  `:1269-1290` (`createXRMonitor` → headless `createOutput`), `:740` (`takeLatestBuffer`), `:768-808`
  (chrome auto-hide + acquire→blit→release loop, on-device copy)
- `src/openxr/XRMonitorConfig.hpp:70-83` (`SXRMonitorParams` — where a `direct:` flag would parse)
- `src/config/values/ConfigValues.cpp:557` (`render:direct_scanout` default 0/disable)
- `/usr/include/aquamarine/backend/Headless.hpp:14-37` (`CHeadlessOutput` surface: `commit`/`test`/
  `getRenderFormats`), `:39-58` (`CHeadlessBackend`: `preferredAllocator`/`getAllocators`/`drmFD`)
- `scripts/lib/gpu.sh:42-102` (`resolve_render_node`), `scripts/xr-container.sh:285-336`
  (`resolve_split_gpu` role split), `containers/session/session-launch.sh:104-116` (`AQ_DRM_DEVICES`
  = nested/host-compositor node; `openxr:gpu` = XR/encode node)
- Cross-refs: `docs/openxr/research/17-late-runtime-lifecycle.md` §4.5 (WP-L2 modifier import — hard
  prerequisite), `docs/openxr/research/18-monitor-plugged-state.md` (orthogonal),
  `docs/openxr/research/14-ray-aim-assist.md` (in-headset pointer, cursor Q4)
