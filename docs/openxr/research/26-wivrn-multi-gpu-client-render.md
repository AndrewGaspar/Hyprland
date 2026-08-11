# Research: WiVRn multi-GPU — the OpenXR client renders on a different GPU than the compositor

**Status:** research + **measured**, and **round 1 of the workplan is implemented** (WP-XG-B1, XG1,
XG2, XG3 — see §8.1 for what landed, what deviated and what is still unproven). The rest of the
workplan is not. The architectural findings are
code-read against WiVRn 26.6.2 at `~/code/wivrn-26.6.2` (branch `hypxr-patches-26.6.2`, read-only —
another session owns that tree) and its vendored Monado `1b526bb3a0ff326ecd05af4c2c541407f53c6d4b`
at `build-server-26.6.2/_deps/monado-src`. The **cross-vendor buffer-sharing findings in §4 are not
literature — they are the output of a purpose-written Vulkan probe run on this box on 2026-08-10**,
committed at `docs/openxr/research/poc/26-cross-gpu-dmabuf/`. HypXRland worktree base `e66328fd0`.

The ask, verbatim:

> "that's how I'd like mixed HypXRland and gaming to work in the future anyway, so willing to pay
> some amount of dev cost"

with the explicit steer that the interesting boundary is **render**, not just encode.

The trigger: "The Big Walk" under xrizer panics on `assert_eq!(pd, info.physical_device)` — xrizer
asks WiVRn for the suggested `VkPhysicalDevice`, gets the **AMD 890M** (because
`wivrn.service.d/override.conf` pins `VK_DRIVER_FILES=radeon_icd.json`), while DXVK renders the game
on the **NVIDIA RTX 5070**.

House style follows `research/24` and `research/25`: ground truth first with `file:line`, honest
sizing, a recommendation split into the cheap thing and the general thing, then a WP ladder.

**Path roots used throughout:**

- **`W`** = `/home/ajg/code/wivrn-26.6.2`
- **`M`** = `/home/ajg/code/wivrn-26.6.2/build-server-26.6.2/_deps/monado-src`
- unprefixed paths are the HypXRland tree.

---

## 0. Executive summary

**Direct cross-vendor dma-buf image sharing WORKS on this box today, and the direction WiVRn
actually needs is the permissive one.** Monado's server allocates swapchain images and the client
imports them, so the required flow is **AMD exports → NVIDIA imports**. Measured: pixel-exact at
2064×2208 for all four tested swapchain formats including 10-bit, with no padding and no tricks.
The reverse direction (NVIDIA exports → AMD imports) is the fragile one, and it fails for a reason
worth knowing — see below. **The modifier intersection between NVIDIA 610.43.03 and RADV is exactly
one entry: `DRM_FORMAT_MOD_LINEAR`.** No tiled format is shareable in either direction, which
re-confirms the hard-won `force_linear` lesson from tasks #31/#135 on new hardware and a new driver.

**Cross-vendor `sync_fd` semaphores also work, and genuinely order GPU work** — the waiter was held
for the full 591 ms of the signaller's job, both directions. `OPAQUE_FD` semaphores fail both
directions. This matters more than it sounds: Monado's per-frame client→compositor sync tries an
**`OPAQUE_FD` timeline semaphore first**, and when that import fails it **aborts client-compositor
creation** rather than degrading to the `sync_fd` fence path that would have worked. That is a
one-line-ish gate, not an architecture problem.

**The Vulkan spec agrees with the measurements, normatively** (§6.1): `OPAQUE_FD` memory *and*
`OPAQUE_FD` semaphores are required to match `deviceUUID`+`driverUUID`, while `DMA_BUF` memory and
`SYNC_FD` semaphores carry **"No restriction"**. Monado's swapchain interchange uses exactly the two
handle types that are spec-illegal across devices. That reframes the work from "make cross-GPU work"
to **"stop doing something the spec forbids"**. And the OpenXR spec turns out *not* to require a
runtime to reject a foreign `physicalDevice` at `xrCreateSession` — the match is an
application-side Valid Usage, and Vulkan deliberately lacks the "runtime **must** return
`XR_ERROR_GRAPHICS_DEVICE_INVALID`" sentence that OpenGL and D3D both carry (§6.2). **Monado's
rejection is a choice, not a requirement.**

**We would be finishing a six-year-old upstream commit, not inventing a direction.**
`XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX` was added by Christoph Haag in 2020 with a commit message
naming the intended mechanism — *"if compositor and client run on different GPUs, the swapchains use
linear tiling instead of optimal tiling"* — **and that code has never existed in Monado** (§6.3).
Upstream shipped the policy seam without the mechanism, and its own multi-GPU page documents our
exact failure (`vkAllocateMemory: VK_ERROR_INVALID_EXTERNAL_HANDLE`) and prescribes our exact fix.
Nobody upstream has built it: WiVRn closed the request *"not planned"* in Jan 2026 and Valve declined
too — **but both judged it against a GPU→host→GPU copy model that §4 shows is unnecessary** (§6.4–6.5).

**The dev cost is real but bounded, and it is almost entirely in Monado, not WiVRn.** WiVRn's own
collapse of the two device UUIDs (`W/server/compositor/compositor.cpp:797-798`) is a two-line
change. The actual work is four patches to the vendored Monado, extending the existing
`W/patches/monado/` series at the next free number, **0009**. Sized at **11 agent-tasks** for the
full split (§8), of which 8 are code and 1 needs the headset and the user.

**The cost per frame is not an *extra* copy — it is the same unavoidable copy, made remote.**
Report 19 established that the blit into the runtime-owned OpenXR swapchain image can never be
eliminated ("The copy that always remains"). Cross-GPU only changes that blit's *destination* from
local VRAM to a buffer the compositor GPU owns. Measured at 2064×2208 per eye, in the recommended
topology (compositor + encode on AMD, game on NVIDIA): the NVIDIA client's write into the shared
image costs **1.313 ms/eye** and the AMD compositor's read of it costs **0.498 ms/eye**. But the
compositor's read happens in *every* topology, so **the marginal cost of going cross-GPU is a single
term: +1.22 ms/eye = 2.44 ms both eyes, ~22% of an 11.1 ms frame** (§4.6a). Forcing the compositor's
swapchains LINEAR costs the AMD compositor only **+0.039 ms/eye (+9%)**.

> **These figures were re-measured on 2026-08-10 with the box idle and exclusive, and they replace
> an earlier set that was wrong by up to 21×.** Two defects: the benchmark serialized every copy
> behind a pipeline barrier, and — more insidiously — a failed cross-vendor `OPAQUE_FD` semaphore
> import had **poisoned the NVIDIA device against VRAM image allocations**, silently pushing every
> NVIDIA allocation into system memory (§4.8). Both run logs are committed so the delta is
> auditable. The lesson generalises beyond this report: an allocator with a sysmem fallback turns
> that driver bug into a silent 20× performance loss with no error anywhere.

**Can the overhead be optimised away when the game is exclusive? Partly — and the part that can be
is already shipped** (§10). WiVRn 26.6.2 *already* skips the layer squash whenever a session
submits exactly one projection layer (`W/server/compositor/compositor.cpp:326-345`), automatically,
per frame, with no configuration. But the squash was never the cost. The cost is the PCIe write,
and it cannot be removed per-frame because *which GPU owns the swapchain* is fixed for the session.
The two tiers that would remove it are both architecturally blocked: **nvenc cannot run on the dGPU
while the compositor is on the iGPU** (it imports into CUDA with `OPAQUE_FD`, which requires
matching device UUIDs — `W/server/encoder/video_encoder_nvenc.cpp:389-411`), and full dGPU
residency needs the Android-only client-allocates path *plus* an NVIDIA→AMD import that fails on
both pitch and residency. **Good news for policy: because the shipped bypass is a pure per-frame
`if` with no setup or teardown, no hysteresis is needed at all** — a notification costs exactly one
frame of squash, and debouncing would only make it worse.

**Recommendation: Shape A (full split), but do Shape B first as a same-week unblock.** Shape B —
move the WiVRn compositor to NVIDIA and offload encode to the AMD iGPU via the documented vaapi
`device` option — is a config change, not code, and it makes the game work today. It costs the
AMD-only evaluation and it re-engages the cross-GPU desktop-buffer path that HypXRland already
solved in the #31/#135 era. **It also has a blocker worth knowing before you try it:** WiVRn's VAAPI
capability probe ignores the configured `device` and probes the *compositor's* GPU
(`W/server/encoder/encoder_settings.cpp:136-146`), so with an NVIDIA compositor it will likely
report vaapi unsupported. That is a genuine WiVRn bug and a 1-task fix (§7).

---

## 1. What already exists (verified, not remembered)

### 1.1 The hardware and what the live stack pins today

| Node | PCI | GPU | driver |
|---|---|---|---|
| `renderD128` / `card1` | `0000:c1:00.0` | **NVIDIA RTX 5070 Laptop (GB206M), 8 GB** | proprietary 610.43.03 |
| `renderD129` / `card2` | `0000:c2:00.0` | **AMD Radeon 890M iGPU (Strix)** | RADV (Mesa) |

Render-node numbering is inverted from the usual guess and nothing may assume it (research/25 §3.1).

Live pins today — **the whole chain is same-GPU, AMD-only**:

- `~/.config/hypr/hyprland-xr.conf`: `env = AQ_DRM_DEVICES,/dev/dri/card2` (keeps aquamarine off the
  dGPU entirely) and `gpu = /dev/dri/renderD129` for `openxr:gpu`, with the comment
  *"AMD-ONLY EVALUATION (Strix Point no-dGPU candidate)"*.
- `~/.config/systemd/user/wivrn.service.d/override.conf`:
  `Environment=VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json` plus
  `LIBVA_DRIVER_NAME=radeonsi`. Its own comment: *"AMD-only evaluation (as-if the NVIDIA GPU doesn't
  exist): restrict the server's Vulkan compositor to the RADV ICD so it can't pick the RTX 5070."*
- `~/.config/wivrn/config.json`: `{"encoder":{"encoder":"vaapi","codec":"h265","device":"/dev/dri/renderD129"}, ...,
  "openvr-compat-path":"/home/ajg/.local/share/xrizer"}`.

So the vaapi `device` key **is already set, but to the same GPU the compositor runs on** — the
cross-device VAAPI import path exists in the code and is **not currently exercised** by this config.

`openxr:force_linear` is unset (= `auto`), and `auto` correctly resolves to *no force* here because
`shouldForceLinear(AUTO, gpusKnown=true, sameGpu=true) == false`
(`src/openxr/XRMonitorConfig.cpp:533-549`). **The HypXRland cross-GPU code paths are dormant on the
live box today.**

### 1.2 Our own prior art, and what it already proved on this hardware

The #31/#135-era work (`e42d59616`, `3bcac889a`/`b93279dde`, `4460d2c8a`/`349da50e4`,
`6c71ae70d`/`f12b946a0`, `0023623e4`, `ea09ae3f3`/`67e00251f`) is directly load-bearing here. The
lessons, condensed, with the ones this report re-tests marked:

1. **A cross-GPU dmabuf import can hard-crash the graphics driver, uncatchably.** `e42d59616`:
   radeonsi `driUnbindContext` SIGSEGV when an OpenXR runtime imports cross-GPU dmabufs into an EGL
   context on another vendor's GPU. Took the desktop down twice. This is *why* HypXRland fails
   closed on a wrong `openxr:gpu` rather than trying. **Still the governing safety fact.**
2. **NVIDIA's EGL rejects modifier-less dmabuf imports, even for LINEAR** (`3bcac889a`, 43,210
   `EGL_BAD_ATTRIBUTE` in one 23-minute session). Explicit per-plane modifier attribs are mandatory;
   `DRM_FORMAT_MOD_INVALID` must *not* be emitted as an explicit attrib.
3. **Modifiers were not enough — foreign vendor tiling is rejected outright, so LINEAR must be
   forced** (`4460d2c8a`; this *corrected* research/17's LINEAR claim). **§4 re-confirms this at the
   Vulkan layer on new silicon: the NVIDIA↔RADV modifier intersection is `{LINEAR}` and nothing
   else.**
4. **NVIDIA's allocator cannot hand back `MOD_LINEAR` for a scanout buffer** and returns
   `BLOCK_LINEAR_2D` anyway (`ea09ae3f3`). **§4 shows this does *not* apply to non-scanout images:
   NVIDIA allocated and exported a true LINEAR image happily.** The constraint is specific to
   scanout.
5. **Render node vs card node**: decide cross-GPU with `DRM::sameGpu()` / `drmDevicesEqual`, never
   by comparing major:minor — *"the numeric interface is precisely what invited the bug"*
   (`ea09ae3f3`).
6. Report 19's unavoidable-copy finding (`archive/19-zero-copy-game-path.md` §4): the blit into the
   runtime-owned swapchain image cannot be elided, because OpenXR swapchain images are runtime-owned
   and we must render *into* the acquired image. **This is the reason §0 can say the cross-GPU cost
   is a relocated copy, not a new one.**
7. Report 19 §2.6 flagged **cross-vendor implicit fencing as unreliable** and left it as open
   question Q3, unanswered since 2026-07-11. **§4.5 answers it: explicit `sync_fd` works and orders
   correctly; do not rely on implicit fencing.**

---

## 2. Q1 — The Monado IPC swapchain path, precisely

### 2.1 Who allocates: the **server**, always, on Linux

`ipc_compositor_swapchain_create` branches on an optional client-side allocator —
`M/src/xrt/ipc/client/ipc_client_compositor.c:411-428`:

```c
struct xrt_image_native_allocator *xina = icc->xina;
if (xina == NULL) { xret = swapchain_server_create(icc, info, out_xsc); }
else              { xret = swapchain_allocator_create(icc, xina, info, out_xsc); }
```

`xina` is **non-NULL only on Android** — `M/src/xrt/ipc/client/ipc_client_instance.c:100-106`:

```c
	struct xrt_image_native_allocator *xina = NULL;
#ifdef XRT_GRAPHICS_BUFFER_HANDLE_IS_AHARDWAREBUFFER
	// On Android, we allocate images natively on the client side.
	xina = android_ahardwarebuffer_allocator_create();
#endif
```

So on Linux it is always `swapchain_server_create` → `ipc_call_swapchain_create` → server
`M/src/xrt/ipc/server/ipc_server_handler_swapchain.c:106` `xrt_comp_create_swapchain(ics->xc, ...)`
→ `multi_compositor` pass-through (`M/src/xrt/compositor/multi/comp_multi_compositor.c:437-446`) →
`comp_base` (`M/src/xrt/compositor/util/comp_base.c:46-60`) → `comp_swapchain_create_init`
(`M/src/xrt/compositor/util/comp_swapchain.c:494-551`) → **`vk_ic_allocate(vk, ...)` at
`comp_swapchain.c:519`**, on the server's single `vk_bundle`.

**This is the single most important structural fact in the report.** It means the buffer is born on
the *compositor's* GPU and the *client* is the importer — so for "compositor on AMD, game on
NVIDIA", the required direction is **AMD exports → NVIDIA imports**, which §4 measures as the
permissive direction. Had it been the other way round, this report would recommend against the whole
idea.

A client-allocates path exists and is fully wired (`swapchain_allocator_create` →
`ipc_call_swapchain_import` → `xrt_comp_import_swapchain` → `comp_swapchain_import_init`
`comp_swapchain.c:553-593` → `vk_ic_from_natives`) — it is simply Android-gated. It is a second
lever if the export direction ever needs to flip.

### 2.2 Which way the FDs travel: server → client, once per swapchain

Protocol: `M/src/xrt/ipc/shared/proto/50-swapchain.json:13-35` declares `swapchain_create` with
`"out_handles"` and `swapchain_import` with `"in_handles"`. Transport is plain `SCM_RIGHTS` on the
AF_UNIX socket — `M/src/xrt/ipc/shared/ipc_message_channel_unix.c:206` (`cmsg->cmsg_type =
SCM_RIGHTS`), send at `:211`, receive `memcpy(out_handles, (int *)CMSG_DATA(cmsg), fds_size)` at
`:173`. Limits `XRT_MAX_IPC_HANDLES 16`, `XRT_MAX_SWAPCHAIN_IMAGES 8`.

**The fd transfer and the client-side import happen exactly once, at swapchain creation — never per
frame.** The compositor then samples those same images in place every frame (§2.6). So a residency
or layout mismatch is a *per-frame correctness* problem even though the import is one-shot; there is
no per-frame blit that would launder it.

### 2.3 How the client imports — **this is blocker #1**

`client_vk_swapchain_create` (`M/src/xrt/compositor/client/comp_vk_client.c:652`) calls
`vk_create_image_from_native` at `:700`. That function
(`M/src/xrt/auxiliary/vk/vk_helpers.c:1078-1300`) does, on Linux:

- **Handle type** `:1115` → `vk_csci_get_image_external_handle_type`
  (`M/src/xrt/auxiliary/vk/vk_compositor_flags.c:214-226`) → a hard-coded
  **`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR`** at `:217`.
  **`VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT` appears nowhere in the Monado tree.**
- **Tiling** `:1172` → **`.tiling = VK_IMAGE_TILING_OPTIMAL`**, with no
  `VkImageDrmFormatModifierListCreateInfoEXT` and no
  `VkImageDrmFormatModifierExplicitCreateInfoEXT` anywhere.
- **Memory import** `:1189-1196`:

```c
	VkImportMemoryFdInfoKHR import_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT_KHR,
	    .fd = image_native->handle,
	};

	// TODO memoryTypeBits from VkMemoryFdPropertiesKHR
```

That TODO at `vk_helpers.c:1196` is precisely the missing piece. The memory type is instead taken
from the *locally created image's* requirements (`vk_alloc_and_bind_image_memory`,
`vk_helpers.c:807-833`), which is only valid same-device. **`vkGetMemoryFdPropertiesKHR` appears
nowhere in `M/src` except that comment.**

The export side matches: `M/src/xrt/auxiliary/vk/vk_image_allocator.c:62-63` returns `OPAQUE_FD`,
and `:256` creates the image `VK_IMAGE_TILING_OPTIMAL`.

`OPAQUE_FD` + `OPTIMAL` is **not cross-device-portable by specification** — it is importable only
where `deviceUUID`/`driverUUID` match. Three further landmines in the same function:

1. The `importable` guard at `:1117-1123` (`vk_csci_get_image_external_support`) asks the *local*
   device about the format and never inspects the fd's provenance, so **it passes, yielding
   undefined behaviour rather than a clean error**.
2. The memory-type bug above.
3. `:1253-1259` aborts with `VK_ERROR_OUT_OF_DEVICE_MEMORY` if `requirements.size >
   image_native->size` — two different GPUs routinely disagree on size. There is already an escape
   hatch, `XRT_DEBUG_VK_IGNORE_MEMORY_SIZE_MISMATCH` (`:1256`), which is a useful bring-up tool.

Both `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` are **detected**
(`has_EXT_*` flags generated by `vk_generate_inc_files.py:327,330`) and **never read anywhere in
`M/src`**. WiVRn does enable both, but explicitly *"For FFMPEG"*
(`W/server/utils/wivrn_vk_bundle.cpp:254-260`) — i.e. the two extensions this work needs are already
on the device, just unused by the swapchain path.

A comment worth not trusting: `M/src/xrt/include/xrt/xrt_compositor.h:2171-2178` claims these natives
are *"DMABUF file descriptors on Linux"*. Under Mesa the fd genuinely is a dma-buf, but Vulkan is
told `OPAQUE_FD`, which licenses the driver to assume same-device layout and skip all modifier
negotiation.

### 2.4 Where `client_vk_deviceUUID` actually flows

Struct — `M/src/xrt/include/xrt/xrt_compositor.h:2386-2390`:

```c
	//! The vk device as used by the compositor, never changes.
	xrt_uuid_t compositor_vk_deviceUUID;

	//! The vk device suggested for Vulkan clients, never changes.
	xrt_uuid_t client_vk_deviceUUID;
```

Server → client: `ipc_handle_system_compositor_get_info` copies the whole struct verbatim per client
(`M/src/xrt/ipc/server/ipc_server_handler.c:403-411`, `*out_info = ics->server->xsysc->info;`). The
client fetches it **once, at `xrGetSystem` time** (`ipc_client_compositor.c:1032` from
`ipc_client_instance.c:222`) — before any graphics binding exists — and caches it.

Consumed by `oxr_vk_get_physical_device` (`M/src/xrt/state_trackers/oxr/oxr_vulkan.c:566-666`),
matching UUIDs at `:638`. **If no match** — `:647-650`:

```c
	if (gpu_index == -1) {
		oxr_warn(log, "Did not find runtime suggested GPU, fall back to GPU 0\n\tuuid: %s", suggested_uuid_str);
		gpu_index = 0;
	}
```

Then two **hard** identity checks reject a client that picked differently:

- `xrCreateSession`: `M/src/xrt/state_trackers/oxr/oxr_session.c:1285-1292` → `XR_ERROR_VALIDATION_FAILURE`,
  *"XrGraphicsBindingVulkanKHR::physicalDevice %p must match device %p specified by %s"*.
  (`XrGraphicsBindingVulkan2KHR` is a typedef alias, so `enable2` hits the same code.)
- `xrCreateVulkanDeviceKHR`: `M/src/xrt/state_trackers/oxr/oxr_api_system.c:448-452` →
  `XR_ERROR_HANDLE_INVALID`.

**The nasty interaction:** both checks compare against `suggested_vulkan_physical_device`, which is
the *fallback* `phys[0]` when the UUID didn't match. So a client whose `VkInstance` doesn't enumerate
the compositor's GPU is silently pinned to its own device 0 with only a warning, and then fails much
later inside `vkAllocateMemory`. That is the real-world cross-GPU failure signature and it is a
miserable one to debug. Any patch here should make the mismatch **loud**, not just permitted.

### 2.5 `XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX` — the seam already exists, and does nothing useful

Both declared adjacently, `M/src/xrt/compositor/main/comp_settings.c:30-31`:

```c
DEBUG_GET_ONCE_NUM_OPTION(force_gpu_index,        "XRT_COMPOSITOR_FORCE_GPU_INDEX",        -1)
DEBUG_GET_ONCE_NUM_OPTION(force_client_gpu_index, "XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX", -1)
```

- `FORCE_GPU_INDEX` selects the `VkPhysicalDevice` the **compositor process itself** uses
  (`comp_compositor.c:742` → `comp_vulkan.c:128`).
- `FORCE_CLIENT_GPU_INDEX` drives **only the advertised UUID** (`comp_compositor.c:743` →
  `comp_vulkan.c:131,151-167` → `comp_compositor.c:1191`). It has **zero** effect on any allocation,
  import, or sync code.

Default coupling at `M/src/xrt/compositor/util/comp_vulkan.c:146-148`:

```c
	// By default suggest GPU used by compositor to clients
	if (vk_res->client_gpu_index < 0) { vk_res->client_gpu_index = vk_res->selected_gpu_index; }
```

**Verdict: upstream models the *concept* of a separate client GPU end to end — two env vars, two
settings fields, two UUID fields, two `comp_vulkan` fields, and doc comments sanctioning it — but
the *mechanism* to make it work (dma-buf + modifiers + fd-derived memory types) was never built.**
Setting the env var today relocates the failure from "wrong device selected" to "`vkAllocateMemory`
fails on import". WiVRn never calls `comp_vulkan_init_bundle` at all (it uses `vk_init_from_given`,
`W/server/compositor/compositor.cpp:714-728`), so `FORCE_CLIENT_GPU_INDEX` is **dead code in
WiVRn** — the only GPU knob that reaches WiVRn is `XRT_COMPOSITOR_FORCE_GPU_INDEX`
(`W/server/utils/wivrn_vk_bundle.cpp:30`), which selects the server's device.

### 2.6 The per-frame sync path — mostly already cross-device-clean

Client side, `client_vk_compositor_layer_commit` (`M/src/xrt/compositor/client/comp_vk_client.c:613`),
four fallbacks **in this order** (`:629-640`):

1. `submit_handle` (`:136-147`) — app-supplied sync handle.
2. **`submit_semaphore` (`:149-201`) — an `OPAQUE_FD` *timeline semaphore*.** The **server** creates
   it and exports a native handle; the client imports it via
   `vk_create_timeline_semaphore_from_native` (`:112`). Handle type
   `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT` (`M/src/xrt/auxiliary/vk/vk_sync_objects.c:31,
   48-67`).
3. `submit_fence` (`:203-237`) — `vk_create_and_submit_fence_native` exporting
   **`VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT`** (`vk_sync_objects.c:112`).
4. `submit_fallback` (`:240-255`) — `vkQueueWaitIdle` then commit with an invalid handle.

Server side the wait is **on the CPU**, on the multi-compositor's dedicated wait thread:
`wait_fence` (`comp_multi_compositor.c:130-153`, `vkWaitForFences`, 100 ms timeout with a
*"Waiting on client fence timed out > 100ms!"* warning) and `wait_semaphore` (`:156-179`). WiVRn's
own `layer_commit` receives `XRT_GRAPHICS_SYNC_HANDLE_INVALID` unconditionally
(`M/src/xrt/compositor/multi/comp_multi_system.c:600`) and discards it
(`W/server/compositor/compositor.cpp:276-278`).

**Cross-GPU verdict:**

- Path 3 (`sync_fd`) is a kernel `dma_fence` — device-agnostic and portable. §4.5 proves it works
  and orders correctly between these two drivers.
- **Path 2 is a hard blocker and is tried first.** Worse, the gate is client-only
  (`comp_vk_client.c:891-893` checks `vk_can_import_and_export_timeline_semaphore` on the *client's*
  bundle), and `setup_semaphore` failure does `goto err_pool`, **aborting client-compositor creation
  outright**. A cross-GPU client therefore fails at session creation rather than degrading to path 3.
- Because the server's wait is already host-side, **no cross-device GPU semaphore chain is needed at
  all.** This is the least-bad area of the whole problem.

### 2.7 Does the multi-client layer copy? **No.**

`multi_compositor` does zero rendering and zero copying: `transfer_layers_locked`
(`M/src/xrt/compositor/multi/comp_multi_system.c:263`) re-issues the layer calls on the native
compositor with the **same `xrt_swapchain` pointers** the client got (e.g. `:73`
`xrt_comp_layer_projection(xc, xdev, layer->xscs, data)`). WiVRn then samples those images directly
at composite time (`W/server/compositor/compositor.cpp:85-88`,
`W/server/compositor/layer_squasher.cpp:141-149`).

---

## 3. Q2 — What breaks in WiVRn when the UUIDs diverge (patch-point inventory)

The anchor, verified — `W/server/compositor/compositor.cpp:797-798`:

```cpp
	std::ranges::copy(dev_id.deviceUUID, res.compositor_vk_deviceUUID.data);
	std::ranges::copy(dev_id.deviceUUID, res.client_vk_deviceUUID.data);
```

`dev_id` comes from `:781`. Consumed at `W/server/driver/wivrn_session.cpp:297-298`.

| # | Area | Patch point | What breaks / what is needed |
|---|---|---|---|
| 1 | **Device pick** | `W/server/utils/wivrn_vk_bundle.cpp:192-215`; env `XRT_COMPOSITOR_FORCE_GPU_INDEX` at `:30` | No config-file GPU key exists; selection is by ICD filtering + a discrete>integrated heuristic (`:77-94`). A client-GPU concept must be added; `FORCE_CLIENT_GPU_INDEX` is dead here. |
| 2 | **The UUID collapse** | `W/server/compositor/compositor.cpp:797-798` | Two lines. Needs a config key and a UUID→device resolution for the client side. |
| 3 | **Server allocation** | `M/.../vk_image_allocator.c:63` (`OPAQUE_FD`), `:256` (`VK_IMAGE_TILING_OPTIMAL`), export chained at `:343-347` | Must become `DMA_BUF` + `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` with an explicit LINEAR modifier when cross-GPU. |
| 4 | **Client import** | `M/.../vk_helpers.c:1115` (handle type), `:1172` (tiling), `:1189-1196` (import + the TODO) | Must become `DMA_BUF` + explicit modifier + `vkGetMemoryFdPropertiesKHR`-derived memory type. |
| 5 | **oxr device gate** | `M/.../oxr_vulkan.c:638`, `:647-650`; `M/.../oxr_session.c:1285-1292`; `M/.../oxr_api_system.c:448-452` | Two pointer-identity rejections must permit a divergent client device; the silent `phys[0]` fallback must become loud. |
| 6 | **Format advertisement** | `W/server/compositor/compositor.cpp:739-744`; `M/.../comp_vulkan.c:382-428`; `M/.../vk_compositor_flags.c:344+` (queries `optimalTilingFeatures` and `OPAQUE_FD` exportability on the **server** device only) | The advertised list must become the **intersection** of both devices' capabilities, evaluated for `DRM_FORMAT_MODIFIER` tiling + `DMA_BUF`. |
| 7 | **No modifier in the API** | `struct xrt_swapchain_create_info`, `M/src/xrt/include/xrt/xrt_compositor.h:894-912` | Has **no modifier field**. Hard prerequisite for any dmabuf scheme. |
| 8 | **Sync** | `M/.../comp_vk_client.c:149-201` (timeline first), `:891-893` (client-only gate), `vk_sync_objects.c:31,48-67` | Force the `SYNC_FD` fence path when cross-GPU. See §2.6. |
| 9 | **Queue-family ownership** | `W/server/compositor/layer_squasher.cpp:443-445` (declares `eShaderReadOnlyOptimal`, **no acquire barrier**) vs `M/.../comp_vk_client.c:748-757` (releases to `VK_QUEUE_FAMILY_EXTERNAL`, **never matched**) | Asymmetric today; harmless same-device, **not** correct for a foreign device. Needs `VK_QUEUE_FAMILY_FOREIGN_EXT` on both halves. `VK_QUEUE_FAMILY_FOREIGN_EXT` appears **nowhere** in either tree. |
| 10 | **VMA singleton** | `W/common/vk/vk_allocator.h:25` (`class vk_allocator : public singleton<vk_allocator>`), assert at `W/common/utils/singleton.h:32-36` | A **second `VkDevice` in the server process trips the assert**. Fine for our design (the second device lives in the *client* process) but forecloses any "server opens both GPUs" variant. |
| 11 | **Memory types** | `W/server/utils/wivrn_vk_bundle.cpp:399-413` | Resolved against the one device throughout. |

### 3.1 The prior art hiding in the encoder — reuse it

**WiVRn already performs a correct, explicit-modifier dma-buf → `VkImage` import today**, in the
VAAPI encoder: `W/server/encoder/ffmpeg/video_encoder_va.cpp:363-467`. It has exactly the pieces the
swapchain path lacks:

- `vk::ExternalMemoryImageCreateInfo{.handleTypes = eDmaBufEXT}` (`:390-392`)
- `vk::ImageDrmFormatModifierExplicitCreateInfoEXT{...}` with per-plane layouts (`:393-397`)
- **`vk.device.getMemoryFdPropertiesKHR(vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT, object.fd)`
  at `:410`** — the very query `vk_helpers.c:1196` leaves as a TODO
- `vk::ImportMemoryFdInfoKHR{.handleType = eDmaBufEXT, .fd = dup(object.fd)}` (`:412-424`)
- a fourcc→`VkFormat` table (`:125-155`) and the tiling decision at `:385`

**Direction note:** in that path **VAAPI allocates and Vulkan imports** — so with encode on the AMD
iGPU and the compositor on NVIDIA, it is the *AMD-exports → NVIDIA-imports* direction, i.e. the
permissive one §4 measured. That is good news for Shape B.

**And a genuine bug worth reporting upstream:** the VAAPI capability probes construct
`video_encoder_va` with an `encoder_settings{}` that **omits `.device`** —
`W/server/encoder/encoder_settings.cpp:136-146` (`prober::check_vaapi`) and `:362-372` (the 10-bit
probe). So VAAPI support is always probed against the **compositor's** GPU, never the configured
offload device. With an NVIDIA compositor and `device: renderD129`, WiVRn will very likely decide
vaapi is unsupported and refuse the configuration that would have worked.

### 3.2 The patch series to extend

`W/patches/monado/` holds 8 patches applied by `W/patches/apply.sh` (`git am`), globbed with
`CONFIGURE_DEPENDS` at `W/CMakeLists.txt:269-279`. Convention: `NNNN-<subject-slug>.patch`, plain
`git format-patch` mbox, slugs truncated to a fixed width. `git am` ignores the `N/8` counter, so
appending works without renumbering.

**Precedent already in the user's own tree:** the series has previously been extended with
`0009-b-space_overseer-...` (`4842ae22`) and `0010-b-space_overseer-...` (`329bd1d3`), both since
reverted (`d5cb0296`, `1d09cdac`). **So 0009 is the next free number** and the commit-message style
(`patches/monado: <one-line>`) is established.

Notably, **no existing patch touches device selection, swapchain allocation, or graphics sync** —
this work is all new surface. But `0007-don-t-verify-GL-stuff.patch` (which removes
`glxFBConfig`/`visualid` rejections from `oxr_verify.c`) is **direct precedent for relaxing a
graphics-binding validation through this series** — exactly the shape of change item 5 above needs.

Builds must respect `-DGIT_DESC=v26.6.2`.

---

## 4. Q3 — Does cross-vendor dma-buf sharing work on this box TODAY? **Measured, not assumed**

Source: `docs/openxr/research/poc/26-cross-gpu-dmabuf/xgpu_dmabuf.cpp` (+ `CMakeLists.txt`), ~1200
lines, no dependencies beyond the Vulkan loader. It creates its own `VkInstance` and two `VkDevice`s,
touches no compositor, no Wayland surface, and takes no DRM master. Run 2026-08-10 on the live box;
the dGPU was already `runtime_status: active` before the run, so nothing was woken that was asleep.

Build and run:

```
cmake -S docs/openxr/research/poc/26-cross-gpu-dmabuf -B <build> && cmake --build <build> -j8
<build>/xgpu_dmabuf [--width W] [--height H] [--iters N] [--format 0..3]
```

Defaults are 2064×2208 — the WiVRn/Quest 3 per-eye shape.

### 4.1 The two devices, as Vulkan sees them

```
  NVIDIA GeForce RTX 5070 Laptop GPU             vendor=0x10de driver=NVIDIA
      deviceUUID=0fec34e97f02f760a848bc113180d8ec driverUUID=08d21a137e9852738a860d4c7ef8f6ea
      renderD128 card1  tsPeriod=1.0ns  modifier=1 dmabuf=1 foreign=1 semfd=1 fencefd=1
      memory heaps:
        heap[0]   8151 MiB flags=0x1 (VRAM)
        heap[1]  23523 MiB flags=0x0 (sysmem)
  AMD Radeon 890M Graphics (RADV STRIX1)         vendor=0x1002 driver=radv
      deviceUUID=00000000c20000000000000000000000 driverUUID=414d442d4d4553412d44525600000000
      renderD129 card2  tsPeriod=10.0ns  modifier=1 dmabuf=1 foreign=1 semfd=1 fencefd=1
      memory heaps:
        heap[0]   5398 MiB flags=0x0 (sysmem)
        heap[1]  10796 MiB flags=0x1 (VRAM)
```

Both drivers advertise `VK_EXT_image_drm_format_modifier`, `VK_EXT_external_memory_dma_buf`,
`VK_EXT_queue_family_foreign`, `VK_KHR_external_semaphore_fd` and `VK_KHR_external_fence_fd`. **The
capability surface is complete on both sides — nothing is missing at the driver level.**

### 4.2 The modifier lists, verbatim (`R8G8B8A8_UNORM`, usage `TRANSFER_SRC|DST|SAMPLED|COLOR_ATT`)

```
  NVIDIA GeForce RTX 5070 Laptop GPU             R8G8B8A8_UNORM : 7 modifiers
      DRM_FORMAT_MOD_LINEAR                                      planes=1 exp=1 imp=1 ded=0 max=32768x32768
      NVIDIA_BLOCK_LINEAR_2D(h=0 gob) [0x0300000000606010]       planes=1 exp=1 imp=1 ded=1 max=32768x32768
      NVIDIA_BLOCK_LINEAR_2D(h=1 gob) [0x0300000000606011]       planes=1 exp=1 imp=1 ded=1 max=32768x32768
      NVIDIA_BLOCK_LINEAR_2D(h=2 gob) [0x0300000000606012]       planes=1 exp=1 imp=1 ded=1 max=32768x32768
      NVIDIA_BLOCK_LINEAR_2D(h=3 gob) [0x0300000000606013]       planes=1 exp=1 imp=1 ded=1 max=32768x32768
      NVIDIA_BLOCK_LINEAR_2D(h=4 gob) [0x0300000000606014]       planes=1 exp=1 imp=1 ded=1 max=32768x32768
      NVIDIA_BLOCK_LINEAR_2D(h=5 gob) [0x0300000000606015]       planes=1 exp=1 imp=1 ded=1 max=32768x32768
  AMD Radeon 890M Graphics (RADV STRIX1)         R8G8B8A8_UNORM : 8 modifiers
      DRM_FORMAT_MOD_LINEAR                                      planes=1 exp=1 imp=1 ded=0 max=16384x16384
      AMD [0x0200000000000a04]                                   planes=1 exp=1 imp=1 ded=0 max=16384x16384
      AMD [0x0200000010401604]                                   planes=1 exp=1 imp=1 ded=0 max=16384x16384
      AMD [0x0200000010401b04]                                   planes=1 exp=1 imp=1 ded=0 max=16384x16384
      AMD [0x0200000010437b04]                                   planes=3 exp=1 imp=1 ded=0 max=16384x16384
      AMD [0x0200000010467b04]                                   planes=3 exp=1 imp=1 ded=0 max=2560x2560
      AMD [0x020000001046bb04]                                   planes=2 exp=1 imp=1 ded=0 max=2560x2560
      AMD [0x02000000104abb04]                                   planes=2 exp=1 imp=1 ded=0 max=2560x2560
```

> **The intersection is `{DRM_FORMAT_MOD_LINEAR}` and nothing else** — for all four formats tested
> (`R8G8B8A8_UNORM`, `R8G8B8A8_SRGB`, `B8G8R8A8_UNORM`, `A2B10G10R10_UNORM_PACK32`).

Note `0x02000000104abb04` in AMD's list: **the same modifier that black-screened XR monitors in the
`4460d2c8a` era.** The finding is unchanged on new silicon and a new driver generation. Also note
NVIDIA's LINEAR entry has `ded=0` — unlike its block-linear entries it does not require a dedicated
allocation, and **it exports fine**, which refutes the generalised form of the `ea09ae3f3` lesson:
NVIDIA's refusal to hand back `MOD_LINEAR` applies to *scanout* buffers, not to ordinary images.

### 4.3 The share test — **AMD → NVIDIA works, unconditionally**

```
  --- R8G8B8A8_UNORM: AMD Radeon 890M --> NVIDIA GeForce RTX 5070 ---
      modifier intersection (src-exportable AND dst-importable): 1
        DRM_FORMAT_MOD_LINEAR
      TRY modifier DRM_FORMAT_MOD_LINEAR ...
        export ok: mod=DRM_FORMAT_MOD_LINEAR size=17.79 MiB src-mem=type0 heap1 DEVICE_LOCAL (VRAM heap) fd=32
        plane[0] offset=0 rowPitch=8448 size=18653184
        import ok: dst-mem=type0 heap1  (sysmem heap)
        result: OK  (all 4557312 px match)
```

Pixel-exact, at the native 2064×2208, **no padding required**, and identically OK for all four
formats including the 10-bit `A2B10G10R10_UNORM_PACK32`. The test writes a deterministic per-pixel
pattern on the exporter, transfers ownership via `VK_QUEUE_FAMILY_FOREIGN_EXT`, reads back on the
importer and compares all 4,557,312 pixels.

**This is the direction Monado needs** (server allocates → client imports, §2.1). It works today
with no driver changes, no kernel changes, and no tricks.

### 4.4 The reverse direction fails — and the reason is a row-pitch alignment mismatch

```
  --- R8G8B8A8_UNORM: NVIDIA --> AMD ---
        export ok: mod=DRM_FORMAT_MOD_LINEAR size=17.38 MiB src-mem=type0 heap1 (sysmem heap) fd=32
        plane[0] offset=0 rowPitch=8256 size=18229248
  vkCreateImage -> VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT
```

The probe sweeps widths to prove the cause:

```
  width  NV pitch  AMD pitch  NV->AMD import
  1832   7328      7424       REJECTED
  1920   7680      7680       OK   (pitches match)
  2016   8064      8192       REJECTED
  2048   8192      8192       OK   (pitches match)
  2064   8256      8448       REJECTED
  2080   8320      8448       REJECTED
  2112   8448      8448       OK   (pitches match)
  2144   8576      8704       REJECTED
  2160   8640      8704       REJECTED
  2560   10240     10240      OK   (pitches match)
```

**NVIDIA hands back a *tight* linear pitch (`width × bpp`); RADV requires the linear stride to be
256-byte aligned.** The import succeeds exactly when the two agree — i.e. when `width × 4` is
already a multiple of 256, i.e. **width a multiple of 64 px at 4 bpp**. NVIDIA is permissive about
*receiving* an over-aligned pitch, which is why the AMD→NVIDIA direction never trips.

The workaround is measured and works:

```
  padded-width workaround for 2064x2208:
    AMD wants rowPitch=8448 for width 2064  ->  padded width = 2112 (+48 px, +2.3% memory)
    padded NVIDIA->AMD share: OK (pixel-exact)
```

Allocate at the padded width and treat the left `W` columns as the content. **+2.3% memory, zero
extra copies.** This matters only if a future design ever flips the export direction (§2.1's
Android-style client-allocates lever) — the recommended shape does not need it.

### 4.5 Sync — `sync_fd` works cross-vendor **and genuinely orders**; `OPAQUE_FD` does not

Capability probe, both devices advertise everything. The interesting part is what actually functions:

```
  cross-vendor semaphore handoff NVIDIA -> AMD:
      SYNC_FD    IMPORT OK, cross-device wait -> VK_SUCCESS
      OPAQUE_FD  IMPORT INTO DST FAILED (VK_ERROR_UNKNOWN)
  cross-vendor semaphore handoff AMD -> NVIDIA:
      SYNC_FD    IMPORT OK, cross-device wait -> VK_SUCCESS
      OPAQUE_FD  IMPORT INTO DST FAILED (VK_ERROR_INITIALIZATION_FAILED)
```

A capability probe proves nothing about ordering, so the PoC also runs a real ordering test: submit
a deliberately long job (400 full-frame copies) on the signaller, export its `sync_fd` **after**
submit, import on the waiter, and time when the waiter's fence retires.

```
  ordering test NVIDIA -> AMD (sync_fd):
      src job (400 full-frame copies) took 591.3 ms; dst wait returned at 591.2 ms (VK_SUCCESS)
      import cost 0.03 ms; ORDERING HONOURED (waiter held for the signaller's work)
  ordering test AMD -> NVIDIA (sync_fd):
      src job (400 full-frame copies) took 179.3 ms; dst wait returned at 179.2 ms (VK_SUCCESS)
      import cost 0.06 ms; ORDERING HONOURED (waiter held for the signaller's work)
```

**This answers report 19's open question Q3, unanswered since 2026-07-11.** Explicit `sync_fd` is
reliable across these two vendors, both directions, and costs ~0.05 ms to hand over. Do **not** rely
on implicit fencing; do use explicit `sync_fd`.

It also pins down blocker #8 precisely: Monado's preferred path (`OPAQUE_FD` timeline semaphore) is
exactly the one that does **not** work cross-vendor, and its fallback (`SYNC_FD` fence) is exactly
the one that does — but the fallback is unreachable because the timeline failure aborts
client-compositor creation (§2.6).

### 4.6 Cost — **re-measured 2026-08-10 on an idle, exclusive box**

> **The first version of this section was wrong, and wrong by more than an order of magnitude on
> the NVIDIA side.** Two independent defects, both now fixed; both run outputs are committed so the
> delta is auditable (`sample-run-2026-08-10.txt` = flawed, `sample-run-2026-08-10-remeasured.txt`
> = clean).
>
> 1. **Barrier serialization.** The benchmark placed a full pipeline barrier between iterations,
>    so it measured per-copy latency including a pipeline drain rather than sustained throughput.
> 2. **Silent sysmem fallback (§4.8).** Every NVIDIA allocation was landing in system memory
>    because a failed cross-vendor `OPAQUE_FD` semaphore import — run *earlier in the same
>    process* — had poisoned the device against VRAM image allocations.
>
> Fixed: no inter-copy barriers, GPU-side timestamp queries, a discarded warm-up pass and best-of-3
> timed passes, and the sync probes moved to run **last**. NVIDIA's local copy went from a reported
> 1.614 ms to **0.076 ms — a 21× correction.** AMD's numbers barely moved (0.488 → 0.475 ms),
> because RADV was never poisoned and the iGPU is genuinely bandwidth-bound.

At 2064×2208 RGBA8 (17.4 MiB/eye), 90 Hz = 11.1 ms frame budget:

| measurement | per eye | vs old figure |
|---|---|---|
| local `OPTIMAL→OPTIMAL` copy, NVIDIA (VRAM) | **0.076 ms** (479 GB/s) | was 1.614 ms |
| local `OPTIMAL→OPTIMAL` copy, AMD (VRAM) | **0.475 ms** (77 GB/s) | was 0.488 ms |
| compositor reads its own **OPTIMAL** swapchain image, AMD | **0.438 ms** | was 0.444 ms |
| compositor reads its own **LINEAR** swapchain image, AMD | **0.477 ms** | was 0.489 ms |
| compositor reads own OPTIMAL, NVIDIA | 0.071 ms | was 1.599 ms |
| compositor reads own LINEAR, NVIDIA | 0.110 ms | was 2.298 ms |

**Forcing LINEAR costs the AMD compositor +0.039 ms/eye (+9%)** — the conclusion is unchanged and
if anything slightly better than the first pass claimed.

**The recommended topology (server/compositor = AMD, client/game = NVIDIA):**

| stage | per eye | both eyes | % of frame |
|---|---|---|---|
| NVIDIA client writes into the AMD-allocated shared image | **1.313 ms** | 2.6 ms | 24% |
| AMD compositor reads the shared image | **0.498 ms** | 1.0 ms | 9% |
| | | **3.6 ms** | **33%** |

**But 3.6 ms is not the marginal cost of going cross-GPU, and quoting it as such (as §0 originally
did) overstates the bill.** The compositor reads the swapchain image in *every* topology — that
0.498 ms/eye is work it does today, same-GPU, and it is within noise of its own local LINEAR read
(0.477 ms). The only line that is genuinely *caused* by the split is the client's write, and §4.6a
decomposes it.

### 4.6a Decomposition — where the client's 1.313 ms actually goes

Three writes on the same GPU, same size, same format, differing only in destination:

| NVIDIA writes a full eye into… | per eye | memory |
|---|---|---|
| (a) its own `OPTIMAL` image | 0.071 ms | VRAM |
| (b) its own `LINEAR` image | **0.091 ms** | VRAM |
| (c) its own **exportable** `LINEAR` image | 0.078 ms | VRAM |
| (d) the **AMD-allocated** shared `LINEAR` image | **1.313 ms** | AMD-side |

- **(b)−(a) = +0.020 ms/eye** — the cost of LINEAR tiling alone. Negligible.
- **(c)−(b) ≈ 0** — **making a buffer exportable costs nothing.** Contrary to the first pass's
  hypothesis, NVIDIA happily places an exportable dma-buf in VRAM; the earlier "exportable buffers
  land in sysmem" reading was an artifact of §4.8's poisoning.
- **(d)−(b) = +1.22 ms/eye = 2.44 ms both eyes.** **This is the entire cross-GPU bill**: the PCIe
  write into memory the other GPU owns. Everything else is noise.

> **The marginal cost of Shape A is ~2.4 ms of an 11.1 ms frame (22%), not 3.9 ms** — and it is
> one single, irreducible term: a PCIe write of 17.4 MiB per eye per frame. This number is the
> spine of the exclusive-mode analysis in §10.

### 4.7 Where the shared buffer physically lives

**Corrected in the re-measure.** The first pass reported that "neither driver ever placed a
shareable dma-buf in dGPU VRAM". That was an artifact of §4.8. On a healthy device:

- **AMD exports** → the allocation is in AMD's `DEVICE_LOCAL` heap (carved system RAM on an iGPU),
  and NVIDIA imports it as a sysmem-heap allocation. This is the recommended direction, it works,
  and the NVIDIA client's writes to it cross PCIe — the 1.22 ms/eye of §4.6a.
- **NVIDIA exports** → the allocation **is** placed in NVIDIA VRAM (`type1 heap0 DEVICE_LOCAL`).
  Exportability does not force it to system memory.

That second correction has a sharp consequence, and it is the one place the re-measure made the
picture *worse* rather than better — see §4.8.

### 4.8 Two driver behaviours found by the re-measure, both worth knowing

**(1) A failed cross-vendor `OPAQUE_FD` semaphore import poisons the NVIDIA device against VRAM
image allocations.** After `vkImportSemaphoreFdKHR(OPAQUE_FD)` returns `VK_ERROR_UNKNOWN`, every
subsequent `vkAllocateMemory` for an **image** in a `DEVICE_LOCAL` VRAM type returns
`VK_ERROR_OUT_OF_DEVICE_MEMORY` — on an idle 8 GB card with nothing else running. **Bare
(non-image) allocations in the same memory type keep succeeding**, which is what makes it so easy
to miss: a naive "is VRAM available?" probe says yes. An allocator with a sysmem fallback (like the
first version of this PoC, like VMA, like most engines) silently relocates everything to system
memory and reports success. The PoC now demonstrates this deterministically: with `--no-sync` there
are **zero** allocation failures; with the sync probes enabled there are failures, and *only after*
the probe runs. This is a plausible upstream NVIDIA bug report, and it is a live hazard for
WP-XG3 — Monado tries `OPAQUE_FD` semaphores **first** on every client (§2.6), so a cross-GPU
client would trip this on every session before falling back.

**(2) NVIDIA→AMD import now fails at *submission*, not just at image creation.** With NVIDIA
exporting from VRAM, the padded-width workaround of §4.4 gets further than before — RADV accepts
the explicit-modifier image *and* the memory import (binding it to a `HOST_VISIBLE|HOST_COHERENT`
sysmem type) — and then dies on the first `vkQueueSubmit` with:

```
radv/amdgpu: Not enough memory for command submission.
VK_ERROR_DEVICE_LOST
```

**So §4.4's "padded NVIDIA→AMD share: OK (pixel-exact)" result only held because NVIDIA had been
poisoned into system memory by (1).** When the exported buffer genuinely lives in dGPU VRAM, RADV
cannot make it resident — consistent with there being no PCI P2PDMA path between these two devices.
The reverse direction is therefore usable *only* if the exporter can be made to place the buffer in
system memory, which NVIDIA's Vulkan offers no direct way to request. I did not root-cause the RADV
side further; the failure is reproducible and the recommended direction is unaffected. The PoC now
gates the reverse-direction tests behind `--reverse` because the `DEVICE_LOST` takes the whole
process with it.

**Net effect on the report's conclusions:** the recommended direction (AMD exports → NVIDIA
imports) is unaffected and still pixel-exact for all four formats. The *fallback* direction is
weaker than first reported, which matters only for the "client allocates" variant discussed in
§10.4.

---

## 5. Q4 — The multi-client heterogeneous wrinkle

The target future shape is **a HypXRland desktop client on AMD (same-device, zero extra cost) and a
game client on NVIDIA, simultaneously**. Three sub-questions.

### 5.1 Is `client_vk_deviceUUID` structurally one-per-system? Yes today; no by necessity.

It is one value per system compositor: one `xrt_system_compositor_info` embedded at
`M/src/xrt/include/xrt/xrt_compositor.h:2363-2400` with `//! ... never changes` on the UUID fields,
one server-side instance (`M/src/xrt/ipc/server/ipc_server.h:387`, created once at
`ipc_server_process.c:698`), assigned once at `comp_multi_system.c:859`.

**But nothing in the IPC design prevents per-client values.** The handler already has per-client
state and returns a full struct copy per client:

```c
ipc_handle_system_compositor_get_info(volatile struct ipc_client_state *ics, struct xrt_system_compositor_info *out_info)
{ *out_info = ics->server->xsysc->info; }        // ipc_server_handler.c:403-411
```

Each client has its own socket, its own `ipc_client_state`, its own shared-memory segment, its own
thread, and its own `multi_compositor`. `IPC_MAX_CLIENTS` is 32, `MULTI_MAX_CLIENTS` 64.
**Returning a different UUID per client is a one-line change on the wire.**

What *does* get in the way is ordering and information, not structure:

1. **The client fetches the info at `xrGetSystem`**, before any graphics binding exists, and caches
   it with no refresh. At that moment the server knows nothing about the client's GPU or even its
   graphics API. `struct xrt_session_info` (`xrt_compositor.h:948-954`) carries no GPU field, and it
   is sent later anyway. A per-client suggestion needs either a new IPC call or a new field plus a
   client-side re-fetch — **and some way for the client to state a preference, which the OpenXR API
   does not provide.**
2. **One allocator.** Images still come from the single server `vk_bundle`. Diverging the UUID alone
   just relocates the failure (§2.5).

**Practical answer for the near term:** the suggestion is a *policy* the server chooses, and the
server has one useful signal available at `xrGetSystem` time — the client's process identity
(pid over the socket). A per-client policy is implementable but is not needed for v1: **if the
swapchain images become LINEAR + dma-buf (WP-XG1/XG2), they are importable by *both* GPUs**, so a
single global suggestion still works for the heterogeneous case. The AMD desktop client simply
imports a buffer that happens to be on its own device (near-free), and the NVIDIA game client
imports the same kind of buffer across PCIe. **The heterogeneity is handled by making the buffer
universally importable, not by making the suggestion per-client.** That is a much smaller change and
it is the design this report recommends.

The residual cost: the AMD desktop client's swapchains also become LINEAR, costing it the +10%
measured in §4.6 — 0.039 ms/eye. Acceptable. If it ever isn't, per-client policy is the escape
hatch, and it is not foreclosed.

### 5.2 What may a runtime legally return, and is xrizer's assert its own invention?

**The assert is xrizer's, not the spec's** — but xrizer is not being unreasonable, because the
*runtime* is what enforces the match. Two distinct things:

- The runtime **tells** the app which device to use, via `xrGetVulkanGraphicsDeviceKHR` /
  `xrGetVulkanGraphicsDevice2KHR`.
- The runtime then **rejects** a session whose `XrGraphicsBindingVulkanKHR::physicalDevice` differs —
  in Monado, `oxr_session.c:1285-1292` with `XR_ERROR_VALIDATION_FAILURE`, and
  `oxr_api_system.c:448-452` for `xrCreateVulkanDeviceKHR` with `XR_ERROR_HANDLE_INVALID`.

So an app that ignores the suggestion gets a hard error from the runtime anyway. xrizer asserting
early converts a confusing late failure into an obvious early one. **Removing the assert in xrizer
alone would not help** — Monado would reject the session. The fix has to be on the runtime side
(item 5 in §3), which is what the workplan does.

**§6.2 confirms this from the spec text:** the `physicalDevice` match is a Khronos **Valid Usage**
item — an *application* obligation whose violation is UB and which the runtime is *not required to
detect*. Vulkan deliberately lacks the "runtime **must** return `XR_ERROR_GRAPHICS_DEVICE_INVALID`"
sentence that OpenGL and D3D both carry. **A runtime may legally accept a foreign
`physicalDevice`.** Monado's rejection is a choice, and xrizer's own source comment
(*"Monado seems to (incorrectly) give validation errors unless we call this"*) shows its author
thought so too.

### 5.3 Does anything else break with two clients on two GPUs?

The composite path samples each client's images directly (§2.7), so a mixed set is fine provided
every image is importable by the compositor device — which LINEAR + dma-buf guarantees. The one real
gap is the queue-family ownership asymmetry (§3 item 9): the client releases to
`VK_QUEUE_FAMILY_EXTERNAL` and the compositor never acquires. Same-device that is harmless; with a
foreign device it is incorrect and must become `VK_QUEUE_FAMILY_FOREIGN_EXT` on both halves.
`VK_QUEUE_FAMILY_FOREIGN_EXT` currently appears **nowhere** in either tree. The PoC uses it
throughout and it behaves correctly on both drivers.

---

## 6. Q5 — Upstream state of the art

**Nobody upstream has shipped this, and two of the three relevant projects have explicitly declined.**
But the spec is on our side in a way that matters, and Monado's own documentation prescribes exactly
the fix this report recommends.

### 6.1 The Vulkan spec says, normatively, what §4 measured

This is the single most satisfying result of the research pass: the empirical PoC findings and the
Vulkan specification's normative compatibility table agree **exactly**. From `Vulkan-Docs` `main`,
`chapters/capabilities.adoc` ("Some external memory handle types can only be shared within the same
underlying physical device and/or the same driver version"):

| Handle type | `driverUUID` | `deviceUUID` |
|---|---|---|
| `EXTERNAL_MEMORY_..._OPAQUE_FD_BIT` | **Must match** | **Must match** |
| `EXTERNAL_MEMORY_..._DMA_BUF_BIT_EXT` | **No restriction** | **No restriction** |
| `EXTERNAL_SEMAPHORE_..._OPAQUE_FD_BIT` | **Must match** | **Must match** |
| `EXTERNAL_SEMAPHORE_..._SYNC_FD_BIT` | **No restriction** | **No restriction** |
| `EXTERNAL_FENCE_..._OPAQUE_FD_BIT` | Must match | Must match |
| `EXTERNAL_FENCE_..._SYNC_FD_BIT` | **No restriction** | **No restriction** |

`VkImportMemoryFdInfoKHR`'s man page is literally titled *"**Import memory created on the same
physical device** from a file descriptor"*, with `VUID-VkImportMemoryFdInfoKHR-fd-00668` requiring
same-device provenance — and `handleType` restricted to `OPAQUE_FD` **or** `DMA_BUF_BIT_EXT`.
Ownership transfer across the boundary requires `VK_QUEUE_FAMILY_FOREIGN_EXT`, **not**
`VK_QUEUE_FAMILY_EXTERNAL_KHR` (which itself demands same physical device and driver version) —
which is precisely §3 item 9's bug.

> **So Monado's swapchain interchange is not merely *untested* cross-device; it is
> spec-illegal cross-device, on both the memory and the semaphore side.** That reframes WP-XG1–XG3
> from "make it work" to "stop doing something the spec forbids". It also explains the exact failures
> the PoC saw: `OPAQUE_FD` semaphore import returned `VK_ERROR_UNKNOWN` / `VK_ERROR_INITIALIZATION_FAILED`,
> while `DMA_BUF` memory and `SYNC_FD` semaphores worked first try.

⚠️ Trap for whoever writes the patches: the auto-generated `docs.vulkan.org` refpage for
`VkExternalSemaphoreHandleTypeFlagBits` claims `OPAQUE_FD` has "No restriction". **That is wrong** —
the spec source says "Must match". Cite the spec source, not the refpage.

### 6.2 What the OpenXR spec actually requires of the runtime — less than everyone assumes

Read from OpenXR **1.1.62**. `XR_KHR_vulkan_enable` §12.23 is advisory: the device
*"**should** be passed to `xrCreateSession`"*. `XR_KHR_vulkan_enable2` §12.24.2 obliges only the
*runtime* to *report* a compatible device.

The `physicalDevice` **must** match line on `XrGraphicsBindingVulkanKHR` /
`XrGraphicsBindingVulkan2KHR` is a **Valid Usage** item — in Khronos grammar an *application*
obligation whose violation is undefined behaviour and which **the runtime is not required to
detect**. There is no "otherwise `xrCreateSession` must return …" clause for Vulkan.

**The contrast with OpenGL and D3D proves the omission is deliberate.**
`XrGraphicsBindingOpenGLWin32KHR` says: *"If the GPU used by the runtime does not match the GPU on
which the OpenGL context of the application was created, `xrCreateSession` **must** return
`XR_ERROR_GRAPHICS_DEVICE_INVALID`."* D3D11/D3D12 carry equivalent language. **Vulkan has no such
sentence.**

The one place the spec *does* bind the runtime is `xrCreateVulkanDeviceKHR`: if
`vulkanPhysicalDevice` doesn't match, the runtime **must** return `XR_ERROR_HANDLE_INVALID`. Note
that attaches to *device creation*, not to `xrCreateSession` — a distinction commonly misquoted.

> **Conclusion for §5.2: a runtime may legally accept a foreign `physicalDevice` at `xrCreateSession`.**
> Monado's rejection at `oxr_session.c:1285-1292` is a **choice**, not a spec requirement — a
> runtime-side enforcement of an application-side Valid Usage. That is exactly one `if`, and it is
> what WP-XG4 relaxes. (The app is still in UB territory by the letter of the spec; the honest
> framing is that we are choosing to define that behaviour in our runtime.)

**[NF]** No spec language anywhere about a runtime returning different devices per instance or
session. The contract is per-`(instance, systemId)` and simply silent on variance — so §5.1's
per-client suggestion is not prohibited, merely unmodelled.

### 6.3 Monado: the seam was added in 2020, with a mitigation that was never built

`XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX` came from **Christoph Haag, 2020-08-07**, commit
[`e48c748a5`](https://gitlab.freedesktop.org/monado/monado/-/commit/e48c748a57ea8426814e2bce369d314964a87c34),
[MR !472](https://gitlab.freedesktop.org/monado/monado/-/merge_requests/472). The commit message
states the rationale:

> The reason this is both done on the service side is that **if compositor and client run on
> different GPUs, the swapchains use linear tiling instead of optimal tiling.**

**That code does not exist in Monado today, and may never have.** Checked against current `main`
(`f037264d2`, 2026-08-07): `VK_IMAGE_TILING_LINEAR` appears exactly once in `src/`, in a readback
helper; `vk_image_allocator.c` and `vk_create_image_from_native` both hardcode
`VK_IMAGE_TILING_OPTIMAL` unconditionally; `VK_EXT_image_drm_format_modifier` appears only as an
error-string enum in `vk_print.c`. **Monado implements DRM format modifiers nowhere.**

> **This is the report's most useful upstream finding.** Upstream shipped the *policy* seam
> (two env vars, two UUIDs) together with a stated *mechanism* (linear tiling for the cross-GPU
> case) that was never implemented. WP-XG1/XG2 are not inventing a new direction — **they are
> finishing a six-year-old commit**, using the modifier mechanism that did not exist in 2020.

Upstream's own docs are candid about the consequence:

- [getting-started.html](https://monado.freedesktop.org/getting-started.html): *"`XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX` … **Expect breakage when choosing a different GPU.**"*
- [multi-gpu.html](https://monado.freedesktop.org/multi-gpu.html) (created by Haag the same day) documents **our exact failure**: *"Vulkan client on intel GPU: `vkAllocateMemory: VK_ERROR_INVALID_EXTERNAL_HANDLE`"*, and prescribes the fix: *"The solution should simply be to either **attempt to provide vkFormat LINEAR between GPUs in your own software implementation** or to ensure all applications are operating on the same GPU."*

**[NF] There is no open Monado issue for cross-GPU *client* rendering.** Everything in the tracker is
compositor/display-side: #287 (pick the GPU the HMD is plugged into, open), #204 (DRM lease, closed),
MR !2820 (DRM-lease device selection, **closed unmerged** 2026-07-15), MR !2834 (Wayland direct
backend device selection, open). The only real multi-GPU *implementation* attempt is
[MR !2174](https://gitlab.freedesktop.org/monado/monado/-/merge_requests/2174) (CAVE support), which
uses `VK_KHR_device_group` — **same-vendor only**, and has sat unmerged since 2024-03-14.

*Sourcing caveat: freedesktop GitLab's notes API needs auth and the web UI is behind anti-bot, so
issue/MR descriptions are verbatim but comment threads were not readable — including why !2820 was
closed.*

### 6.4 WiVRn: declined once, and the capability was lost in a rewrite

[WiVRn#203 "Encode on alternate GPU (NVENC)"](https://github.com/WiVRn/WiVRn/issues/203), opened
2024-12-07, **closed "not planned" 2026-01-05** by maintainer `xytovl`: *"Closing as not planned,
**this would add complexity with little benefit**."* Earlier in the thread he scoped it as
`VK_KHR_device_group` or *"a second logical device, and in the `present_image` method do a GPU to
host then host to GPU copy."*

**A user reported our exact failure in that thread and it did not change the outcome** —
`Bruno5430`, 2025-09-30: forcing WiVRn onto the iGPU with `MESA_VK_DEVICE_SELECT` enables vaapi, but
then *"the game has to use the same iGPU… Otherwise the runtime (**both xrizer and OpenComposite**)
crashes with `The VkPhysicalDevice that the OpenVR app used is different from the one that the OpenXR
runtime used!`"*

**A capability regression worth knowing:** WiVRn commit
[`22a819b8c`](https://github.com/WiVRn/WiVRn/commit/22a819b8c) (2026-03-29, "Implement main
compositor") replaced Monado's `comp_main` with WiVRn's own compositor. `comp_settings` now has zero
hits in WiVRn. **On WiVRn ≤ 26.2.x, `XRT_COMPOSITOR_FORCE_CLIENT_GPU_INDEX` would have been live;
from v26.6 it is dead.** So §2.5's "dead code in WiVRn" is a recent loss, not an eternal state —
which slightly strengthens the case that WP-XG5 is restoration rather than novelty.

Also confirmed: the vaapi `device` key is the **only** GPU/render-node key in WiVRn's entire config
schema, and WiVRn's default device heuristic is `Discrete(4) > Integrated(3)` — **so on this laptop
WiVRn would pick the NVIDIA dGPU by default**; the AMD-only result comes entirely from the
`VK_DRIVER_FILES` pin. (Minor upstream bug spotted in passing: the `XRT_COMPOSITOR_FORCE_GPU_INDEX`
bound check is `index > phys_devices.size()`, should be `>=`.)

### 6.5 xrizer, OpenComposite, Proton, SteamVR — three strategies, none of them copying

**xrizer's assert is at `src/openxr_data.rs:471-478`** (not in `vulkan.rs`), in `SessionData::new`:

```rust
// Monado seems to (incorrectly) give validation errors unless we call this.
let pd = unsafe { instance.vulkan_graphics_device(system_id, info.instance) }.unwrap();
assert_eq!(pd, info.physical_device);
```

Note the comment: xrizer calls `xrGetVulkanGraphicsDeviceKHR` **purely to silence Monado's
validation**, then panics if it disagrees. It otherwise uses the *game's* device throughout
(`graphics_backends/vulkan.rs:509`, straight from the OpenVR `VRVulkanTextureData_t`). The
runtime-suggested device is used only for the throwaway pre-Submit session.

The only upstream response to date is [PR #297](https://github.com/Supreeeme/xrizer/pull/297)
(opened 2026-02-05, **still unmerged**), which replaces the bare assert with a **better panic
message** naming both devices. **[NF]** xrizer has no `DRI_PRIME`/`VK_DRIVER_FILES` override
anywhere.

**OpenComposite** aborts identically (`DrvOpenXR/XrBackend.cpp:265-273`) and its message names the
only known workaround: *"…except for on multi-gpu, in which case `DRI_PRIME=1` should fix things on
Linux."* `DRI_PRIME` occurs exactly once in that repo — in that string.

**Proton is the one project that does something different** (`wineopenxr/openxr.c`): for native
Vulkan apps it passes the app's devices through **unchecked**; for D3D11 it warns and then
**overrides** `our_vk_binding.physicalDevice = wine_instance->vk_phys_dev`. Its real mechanism is
*steering*, not copying — it reads the runtime device's `vendorID`/`deviceID` and reports the
matching DXGI adapter LUID so the D3D app naturally initialises on the runtime's GPU.

**SteamVR/Valve declined too.** `SteamVR-for-Linux#869` (open), contributor Packetdancer,
2026-04-28: *"To allow them to work on separate GPUs would mean the texture would need to be copied
Steam GPU -> CPU -> SteamVR GPU every time it updated… The performance hit involved would be…
'Not Great'."* Reproduced on both AMD-iGPU+NVIDIA-dGPU and Intel-iGPU+AMD-dGPU — **cross-device, not
merely cross-vendor**. SteamVR's own runtime matches by `deviceUUID` and hard-fails
(*"IVRSystem::GetOutputDevice: failed to find VkPhysicalDevice matching deviceUUID"*).

Worth noting Valve's stated cost model assumes a **GPU→CPU→GPU** round trip. §4 shows that is not
necessary here: a dma-buf import is a direct device read, no host bounce.

### 6.6 The received wisdom, corrected by measurement

| Folklore | Status | Evidence |
|---|---|---|
| "NVIDIA can't import foreign dma-bufs" | **Stale since driver 525.** | NVIDIA's `cubanismo`, 2024-10-22: *"we've supported importing 'foreign' dma-bufs for several releases now via EGL and Vulkan"* ([open-gpu-kernel-modules#243](https://github.com/NVIDIA/open-gpu-kernel-modules/discussions/243)) |
| "Cross-vendor semaphore sharing doesn't work" | **True for `OPAQUE_FD`, false for `SYNC_FD`.** | NVIDIA added `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT` in **545.23.06** (2023-10-17), gated on `nvidia-drm modeset=1`; broadly usable from 555.58; needs kernel ≥ 6.8. We run 610.43.03. **§4.5 confirms it works and orders.** |
| "Only LINEAR is shareable NVIDIA↔RADV" | **True, and independently confirmed.** | KWin [MR !4177](https://invent.kde.org/plasma/kwin/-/merge_requests/4177): *"to import a buffer from a non-NVidia GPU to a NVidia GPU, **only the linear modifier is valid**."* Mesa's `ac_get_supported_modifiers()` emits only `AMD_FMT_MOD` + LINEAR. Matches §4.2 exactly. |
| "NVIDIA ignores the explicit stride on LINEAR dma-buf import" | **Not reproduced here — we tested it.** | [forum 364360](https://forums.developer.nvidia.com/t/egl-import-via-egl-ext-image-dma-buf-import-modifiers-ignores-explicit-stride-causes-image-distortion-in-virtio-gpu-venus/364360) reports this for **EGL/virtio-gpu Venus**. §4.3 imported an AMD buffer with `rowPitch=8448` at width 2064 (tight would be 8256) into NVIDIA's **Vulkan** and got pixel-exact results. Treat as EGL/Venus-specific. Re-confirmed in the 2026-08-10 re-measure: still pixel-exact with NVIDIA exporting from VRAM. |
| "The shared buffer lives in dGPU VRAM" | **False.** | §4.7 — neither driver ever placed a shareable dma-buf in VRAM. Xaver Hugl, 2026-07-31: *"the driver will not share the buffers on the GPU with the compositor, but actually create a copy in system memory."* |

**Everyone who solved a version of this converged on LINEAR + a copy.** `linux-dmabuf-v1` is
normative about it (*"the client must force the buffer to have a linear layout"* when allocating on
a non-main device without explicit modifiers); wlroots 0.14.1 shipped *"backend/drm: force linear
layout for multi-GPU buffers"*; KWin went CPU-copy (2021) → EGL import + a `glFinish` costing up to
**3 ms** (2023) → compositor-side copies via linux-dmabuf v6 (2025, taking Cyberpunk on an eGPU from
27 → 50 fps). **[NF] No project anywhere shares a *tiled or compressed* image cross-vendor.**

For a latency sanity check, NVIDIA's own [`nvpro-samples/xr_multi_gpu`](https://github.com/nvpro-samples/xr_multi_gpu)
measures device→device image transfer at **0.7–2.2 ms over PCIe 5.0** (same-vendor, Windows,
SLI-gated). Our re-measured 1.313 ms/eye sits inside that band — mild independent corroboration
that the cross-device transfer figure is the right order of magnitude.

---

## 7. Q6 — The shapes, ranked

Sizing below is in **agent-tasks**, consistent with report 25.

### Shape B — compositor + client on NVIDIA, encode offloaded to AMD **(do this first)**

**0 code tasks in the happy path; 1 task if the probe bug bites (it probably will).**

Move the WiVRn compositor to the dGPU and keep encode on the iGPU's VCN via the documented vaapi
`device` option (`W/docs/configuration.md:81-84`, consumed at
`W/server/encoder/ffmpeg/video_encoder_va.cpp:115-123`). Concretely: drop the
`VK_DRIVER_FILES=radeon_icd.json` pin from the wivrn user override, set
`openxr:gpu = /dev/dri/renderD128`, keep `"device":"/dev/dri/renderD129"` in `config.json`.

- **What it buys:** the game works, because DXVK's NVIDIA device and the compositor's device are the
  same. No Monado patches at all.
- **What it costs:** the AMD-only evaluation ends. The old cross-GPU *desktop* path re-engages —
  HypXRland's XR EGL context moves to NVIDIA and must import AMD-allocated desktop buffers, i.e.
  `force_linear` auto-detection fires again. **That path is already built and shipped** (`3bcac889a`,
  `4460d2c8a`, `6c71ae70d`, `0023623e4`, `ea09ae3f3`), so this is a revert to a previously
  known-good topology, not new risk.
- **The "dGPU always awake" cost is smaller than it looks:** research/25 §3.4 found the live
  Hyprland already holds 11 fds on `/dev/nvidia0` with `runtime_status: active` and 12.46 W draw,
  *despite* the `AQ_DRM_DEVICES` pin. The dGPU is not sleeping today.
- **The blocker to expect:** `prober::check_vaapi` omits `.device`
  (`W/server/encoder/encoder_settings.cpp:136-146`, and `:362-372` for 10-bit), so vaapi is probed
  on the NVIDIA compositor GPU and will likely be declared unsupported. **Fix = pass `.device`
  through to the probes. 1 task, and it is a genuine upstream bug worth a PR.** **The code half is
  DONE 2026-08-10** — see WP-XG-B1 in §8; the live config flip is still owed.
- **Risk:** low. **Reversible:** entirely, by restoring two config files.

### Shape A — full split: client on NVIDIA, compositor + encode on AMD **(the recommendation)**

**11 agent-tasks (§8).** Import boundary is AMD-exports → NVIDIA-imports, the permissive direction
(§4.3). LINEAR swapchains, `sync_fd` sync, **~2.4 ms/frame marginal** of an 11.1 ms budget (§4.6a).

- **What it buys:** exactly the user's stated future — HypXRland desktop and encode stay on the
  efficient iGPU where they already work, and only the game's rendering is on the dGPU. The dGPU can
  be idle when no game is running. It also fixes the heterogeneous multi-client case (§5.1) as a
  side effect, because the buffers become universally importable.
- **What it costs:** ~2.4 ms/frame marginal, +9% on the AMD compositor's
  swapchain reads, and four carried Monado patches that must be rebased at each WiVRn bump.
- **Risk:** medium. The two real unknowns are (i) whether NVIDIA's Vulkan is as happy *rendering
  into* a LINEAR imported image as it was *copying* into one in the PoC, and (ii) rebase burden.
- **Upstreamability:** good. Every patch fixes something upstream Monado already wants — the
  `vk_helpers.c:1196` TODO is theirs, and `FORCE_CLIENT_GPU_INDEX` is a feature they shipped without
  a mechanism.

### Shape C — copy-path variant of A

**Moot, and worth saying so explicitly.** C was scoped as the fallback if direct import failed. It
does not fail. Moreover, per report 19 §4, the blit into the runtime-owned swapchain image is
**already unavoidable** — so what would have been "the copy path" is simply Shape A: the client's
mandatory blit now targets a remote buffer. There is no zero-copy alternative to compare against and
no additional relay blit to pay for. **The marginal cost of going cross-GPU is (remote write −
local write), not a whole extra copy.**

The one place a genuine extra copy would appear is if the pitch-alignment problem (§4.4) had to be
solved by staging rather than padding — it doesn't, padding is free.

### Shape D — what upstream suggests

**Upstream suggests Shape A, in writing, and has since 2020.** Monado's own multi-GPU page
prescribes *"attempt to provide vkFormat LINEAR between GPUs in your own software implementation"*
(§6.3), and the `FORCE_CLIENT_GPU_INDEX` commit message names linear tiling as the intended
mitigation for exactly this case. There is no alternative upstream design to evaluate: the only
other multi-GPU implementation in the ecosystem is Monado MR !2174's `VK_KHR_device_group`, which is
**same-vendor only** (NVIDIA SLI Mosaic) and cannot express an NVIDIA↔AMD pair.

The two shapes upstream *rejected* are both worse than Shape A and worth naming so they are not
re-proposed:

- **`VK_KHR_device_group`** (WiVRn#203's first suggestion, Monado MR !2174) — same-vendor only.
  Not applicable.
- **GPU → host → GPU copy** (WiVRn#203's second suggestion; Valve's stated model in
  SteamVR-for-Linux#869) — a full host round trip per eye per frame. **§4 shows this is unnecessary:
  a dma-buf import is a direct device read with no host bounce.** Both maintainers' "not worth the
  complexity" verdicts were reached against this more expensive model, which is a meaningful reason
  their conclusion need not be ours.

**0 tasks — this is Shape A under a different name.**

### Ranking

1. **Shape B now** — same-week unblock for The Big Walk, config only, fully reversible.
2. **Shape A next** — the durable answer, and the one the user actually asked for.
3. Shape C — not applicable.

These compose: B is not wasted work, because it validates the game/xrizer/DXVK stack end to end on
this box while A is being built, and it re-exercises the shipped `force_linear` desktop path.

---

## 8. Workplan

Sized in agent-tasks. **A** = required for Shape A. **B** = the Shape B unblock. Patches land in
`W/patches/monado/` starting at **0009** (§3.2), and all builds pass `-DGIT_DESC=v26.6.2`.

| WP | Scope | Tasks | Variant |
|---|---|---|---|
| **WP-XG-B1** | **Shape B unblock.** Pass `.device` through `prober::check_vaapi` and the 10-bit probe (`W/server/encoder/encoder_settings.cpp:136-146`, `:362-372`) so vaapi capability is probed on the *configured* encode device, not the compositor's. Then flip the live config (drop the `VK_DRIVER_FILES` pin, `openxr:gpu = renderD128`, keep `device: renderD129`) and confirm the game runs. **Headset-in-the-loop; needs the user.** Upstream this as a WiVRn PR. **STATUS: code DONE 2026-08-10** (`wivrn-xg` `c2da849d`). Both probes now take the configured device: `prober::check_vaapi` gained a `device` parameter fed from `config.device`, and the 10-bit probe passes `encoder.device`, which the `encoder_settings` it is validating already carries. The probe cache is keyed by `(codec, device)` rather than by codec alone, because the three encoder slots may name different devices. **Still owed: the live half** — flipping the config and confirming the game runs is headset-in-the-loop and was not attempted. | 1 | **B** |
| ~~WP-XG0~~ | ~~Re-measure properly.~~ **DONE 2026-08-10** — folded into the research pass. Benchmark reworked to GPU timestamp queries with pipelined submission and a discarded warm-up; sync probes moved last after discovering the §4.8 poisoning; re-run on an idle exclusive box. Results in §4.6/§4.6a, both run logs committed. **The one piece not covered and still open: a `VK_IMAGE_USAGE_COLOR_ATTACHMENT` render-into-imported-LINEAR case** (the PoC copies into it rather than rendering into it) — folded into WP-XG9. | — | — |
| **WP-XG1** | **Monado patch 0009 — dma-buf export with explicit modifiers.** `xrt_swapchain_create_info` gains a modifier/cross-GPU field (`xrt_compositor.h:894-912`). `vk_image_allocator.c` `:63`/`:256` become `DMA_BUF` + `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` with a LINEAR modifier list when the flag is set, keeping the existing `OPAQUE_FD`/`OPTIMAL` path byte-identical when it is not. Plumb `vkGetImageDrmFormatModifierPropertiesEXT` + per-plane `vkGetImageSubresourceLayout` so the layouts can travel. **STATUS: DONE 2026-08-10** — `patches/monado/0009-vk-allocate-swapchain-images-as-dma-bufs-with-an-exp.patch` in `wivrn-xg` (`cc91de4a`). Field is `xrt_swapchain_create_info::cross_device`; it reaches the server for free because the whole struct is already an `in` argument of the `swapchain_create` IPC call. The modifier list is exactly `{DRM_FORMAT_MOD_LINEAR}` (§4.2), and the queried modifier plus per-plane layout land on `struct vk_image` and are copied into `xrt_image_native` by `comp_swapchain_create_init`. **Two deviations, both forced:** (i) `vk_init_from_given` had to gain a `dma_buf_modifier_enabled` argument — a bundle built that way cannot read back which extensions were enabled, so `has_EXT_external_memory_dma_buf`/`has_EXT_image_drm_format_modifier` are false on **both** the client bundle and WiVRn's server bundle (`W/server/compositor/compositor.cpp:714-728`), and the feature could never have engaged; (ii) a mutable-format cross-device image is seeded with its own format so it satisfies `VUID-VkImageCreateInfo-tiling-02353`. | 2 | A |
| **WP-XG2** | **Monado patch 0010 — client import via dma-buf.** `vk_create_image_from_native` (`vk_helpers.c:1115,1172,1189-1196`): `DMA_BUF` handle type, explicit-modifier image create, and **memory type from `vkGetMemoryFdPropertiesKHR`** — closing the tree's own TODO. Model it on the working import at `W/server/encoder/ffmpeg/video_encoder_va.cpp:363-467`. Carry the plane layouts over IPC (new fields in the swapchain-create reply). Also relax the `requirements.size` abort at `:1253-1259` for the cross-device case. **STATUS: DONE 2026-08-10** — `patches/monado/0010-vk-import-swapchain-images-as-dma-bufs-with-the-memo.patch` (`wivrn-xg` `cc91de4a`, amended in `fe4d4bc5`). The tree's own TODO at `vk_helpers.c:1196` is closed. Layouts travel in a new `struct ipc_arg_swapchain_layout` on the `swapchain_create` **reply** — one per swapchain, not per image, matching how the single `size` and `use_dedicated_allocation` in that same reply already collapse, with an added assert that the images agree; the IPC generator needed no change because a struct in `out` is an established shape. The size abort becomes a debug line for the cross-device case only. **Three additions the row did not anticipate:** (i) importability is checked against the modifier with `VkPhysicalDeviceImageDrmFormatModifierInfoEXT`, because the existing helper asks about `OPTIMAL` tiling — a layout the fd does not have; (ii) `vkGetMemoryFdPropertiesKHR` had to be added to `vk_generate_inc_files.py`, it was not loaded; (iii) `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` were added to the **optional** device extensions the runtime enables for `XR_KHR_vulkan_enable2` — deliberately *not* to the required `xrt_gfx_vk_device_extensions` string, which is handed to applications verbatim, where a name some driver lacks would fail `vkCreateDevice` for a client that never wanted cross-GPU swapchains (a review caught this; the first draft had it in both). | 2 | A |
| **WP-XG3** | **Monado patch 0011 — force the `SYNC_FD` fence path when cross-GPU.** Suppress the `OPAQUE_FD` timeline-semaphore negotiation (`comp_vk_client.c:149-201`, gate at `:891-893`) so `submit_fence` (`:203-237`) is chosen, and make `setup_semaphore` failure **degrade** instead of `goto err_pool`. §4.5 says the fence path is correct cross-vendor; this WP is what makes it reachable. **STATUS: DONE 2026-08-10** — `patches/monado/0011-c-client-never-negotiate-an-OPAQUE_FD-timeline-semap.patch` (`wivrn-xg` `cc91de4a`). Per §4.8 the suppression is by **device identity established first**, not try-and-fall-back: `client_vk_compositor_create` compares its own `VkPhysicalDeviceIDProperties::deviceUUID` against the compositor's (now passed down from `xrt_system_compositor_info`) and skips `setup_semaphore` outright. A cross-device client that also cannot export `sync_fd` fences now gets a warning that sync will fall back to `vkQueueWaitIdle`. **One deliberate change that is not gated on cross-device:** `setup_semaphore` failure no longer does `goto err_pool`. Failing a whole session over the loss of one of four sync paths was wrong for a same-device client too, and the function leaves `c->sync.xcsem` NULL on failure so `submit_semaphore` declines and `submit_fence` takes over. | 1 | A |
| **WP-XG4** | **Monado patch 0012 — let the client legally use a different device.** Relax the two identity checks (`oxr_session.c:1285-1292`, `oxr_api_system.c:448-452`) when cross-GPU mode is on, and make the silent `phys[0]` fallback at `oxr_vulkan.c:647-650` **loud** (it currently converts a config error into an inscrutable `vkAllocateMemory` failure). Precedent for this shape of change: existing patch `0007-don-t-verify-GL-stuff.patch`. **STATUS: DONE 2026-08-10** — `patches/monado/0012-st-oxr-let-a-client-legally-render-on-a-different-de.patch` (`wivrn-xg2` `e16cdf98`). Both checks now consult one predicate, `oxr_vk_suggests_cross_device_client(sys)`, which is true only when the compositor reports **both** UUIDs and they differ — the same "not reported means not cross-device" rule round 1 had to add to the client gate. The fallback is louder in both directions: it now names every device the client's `VkInstance` enumerated (name + UUID), and in cross-GPU mode it is an **`XR_ERROR_RUNTIME_FAILURE` rather than a fallback**, because silently rendering on the wrong GPU is the exact outcome the configuration exists to prevent — the message points at `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`, the usual cause. **One deviation from the row:** relaxing turned out to be safe rather than merely permitted, and that is worth stating — nothing downstream needs the two devices to be equal, because `determine_cross_device` measures the device the application *actually* bound. An app that ignores the suggestion and picks the compositor's own device simply gets the same-device path. | 1 | A |
| **WP-XG5** | **WiVRn — split the two UUIDs.** `W/server/compositor/compositor.cpp:797-798` stops copying one UUID into both; add a config key (e.g. `"client-gpu"`) resolved to a `VkPhysicalDevice`/UUID, defaulting to the compositor's device so existing setups are bit-identical. Wire it to the WP-XG1 swapchain flag. **STATUS: DONE 2026-08-10** (`wivrn-xg2` `a52ebc49`). The key is `"client-gpu"`, top-level, kebab-case like every other WiVRn key, `std::optional<std::string>`, `null` spelled out as "use the compositor's". Grammar and resolution live in the new `server/utils/cross_gpu.cpp`; the resolved `VkPhysicalDevice`, both UUIDs and the derived `cross_device` bool live on `wivrn::vk_bundle`, resolved once at bundle construction, so `sys_info()` publishes two independently-correct UUIDs on **every** path including the default one. **`XRT_COMPOSITOR_FORCE_CROSS_DEVICE` is deleted** — not in a new patch but by amending 0009, since it was always labelled "temporary, for XG5 to delete" and a series that adds then removes its own debug knob is noise to carry. An unresolvable `client-gpu` **throws at startup** with every candidate device listed (name, UUID, render node, primary node). | 1 | A |
| **WP-XG6** | **Format-list intersection.** `W/server/compositor/compositor.cpp:739-744` / `M/.../comp_vulkan.c:382-428` / `vk_compositor_flags.c:344+` currently evaluate formats against the server GPU with `OPTIMAL` tiling and `OPAQUE_FD`. Advertise the intersection of both devices, evaluated for `DRM_FORMAT_MODIFIER` tiling + `DMA_BUF`. ~~§4.2 says all four common formats survive, so this should narrow nothing today — it is correctness insurance.~~ **That premise was wrong; see the STATUS.** **STATUS: DONE 2026-08-10**, in two halves. Server half (`wivrn-xg2` `a52ebc49`): `compositor::narrow_formats_to_cross_device` re-evaluates every format Monado advertised against **both** physical devices with `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`, `DRM_FORMAT_MOD_LINEAR` and `DMA_BUF` export-on-server/import-on-client, and drops what does not survive; it runs only when the two devices differ, so the same-device list is untouched. Client half, the array-size term (`patches/monado/0013-...`): `client_vk_swapchain_create` runs the *existing* import check — patch 0010's `vk_check_dma_buf_import_support`, made public rather than duplicated — against its own device **before** the IPC round trip, and returns `XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED` → `XR_ERROR_FEATURE_UNSUPPORTED` from the `xrCreateSwapchain` the application called. **The design choice the hazard demanded: refuse loudly, never degrade.** Falling back single-device is not available — the allocation shape is chosen once for a whole swapchain by the device that does *not* have the limit, and an opaque same-device image is unimportable on the client for every layer count including one. **The row's "this should narrow nothing" did not survive measurement: it narrows 8 formats to 6** (§8.2). | 1 | A |
| **WP-XG7** | **Queue-family ownership across the boundary.** Add the missing `VK_QUEUE_FAMILY_FOREIGN_EXT` acquire in `W/server/compositor/layer_squasher.cpp:443-445` and match the client's release (`comp_vk_client.c:748-757`, currently `VK_QUEUE_FAMILY_EXTERNAL` and unmatched). Neither tree uses `FOREIGN_EXT` today; the PoC does and it works on both drivers. **STATUS: DONE 2026-08-10** — client release in `patches/monado/0014-...`, server acquire in `wivrn-xg2` `b29e1eda`. The client's `dstQueueFamilyIndex` becomes `FOREIGN_EXT` when and only when `cross_device`; `layer_squasher::do_layers` gains a per-frame pre-pass that acquires, from `FOREIGN_EXT`, every image the frame's layers reference whose swapchain has `vkic.info.cross_device` set (deduplicated, since one image can back several layers). **The layout-matching problem and its answer:** a queue-family transfer that also changes layout requires both halves to name the same `oldLayout`/`newLayout`, and the server does not know the client's `barrier_optimal_layout` — except that it does, because that layout is a pure function of the swapchain format, so the server recomputes it with the same `vk_csci_get_barrier_optimal_layout` the client used. `VK_EXT_queue_family_foreign` is added to WiVRn's optional device extensions and to Monado's `enable2` optional list; **its absence on the compositor is logged as an error, not skipped quietly**, because the client releases to `FOREIGN` regardless and an unmatched release is corruption rather than a lost optimisation. | 1 | A |
| **WP-XG8** | **Live bring-up.** xrizer + The Big Walk with the game on NVIDIA and the compositor on AMD; confirm no `assert_eq!(pd, info.physical_device)`, measure real frame times against a same-GPU baseline, and check the HypXRland desktop client still composites correctly in the same session (the §5.1 heterogeneous case). **Headset-in-the-loop; needs the user.** **STATUS: ATTEMPT 1 FAILED 2026-08-10, and the cause is fixed** (`wivrn-xg` `xg-round-3`, patch `0015`). The server stack behaved — format narrowing matched the harness at runtime, the compositor stayed on AMD, encode worked, hypxrhud streamed — but the HypXRland desktop client refused every session in a connect/disconnect loop, because its wrong-GPU guard asks `xrGetVulkanGraphicsDevice2KHR` which GPU the runtime uses and was handed the **client** suggestion. The row's own item, "check the desktop client still composites", was the one that failed, and it failed on policy rather than on any of the cross-GPU mechanism. The device suggestion is now answered per graphics API. **The live half is still owed and now has a two-sided pass condition: the desktop client comes up *and* xrizer still gets NVIDIA.** See §8.3. | 1 | A |
| **WP-XG9** | **Exclusive-mode verification (§10).** Three cheap things, one task: (a) confirm WiVRn's shipped one-projection-layer fast path (`compositor.cpp:326-345`) actually engages for a real xrizer/DXVK title — it may submit a HUD quad and never hit `layer_count == 1`; (b) decide whether the four things that path silently drops (colour scale/bias, chroma key, FOV-union viewport shrink, source-smaller-than-stream edge smear — §10.2) matter in practice, and log a warning if a layer needing them takes the fast path; (c) close out the last WP-XG0 gap by benchmarking *rendering into* an imported LINEAR image (`COLOR_ATTACHMENT`), not just copying into it. **No new mechanism — this is verification of behaviour that already ships.** | 1 | A |

**Shape B (the unblock): WP-XG-B1 — 1 agent-task**, and it may be zero code if the probe happens to
succeed anyway.
**Shape A (the full split): WP-XG1, XG2, XG3, XG4, XG5, XG6, XG7, XG8, XG9 — 11 agent-tasks**
(XG0 is done; XG9 replaces it in the count).

**Ordering:** XG-B1 → XG1 → XG2 → XG3 → (XG4 ∥ XG5) → XG6 → XG7 → XG8 → XG9. XG1+XG2 are the
irreducible core; if they land and a same-GPU session is still healthy, the rest is mechanical.
XG9 can run any time after XG8, or independently against a same-GPU session today.

**Not an agent task:** deciding whether to run the dGPU at all (§9 Q2), and any change to the live
`wivrn.service` — the user does that.

### 8.1 Round 1 (XG-B1 + XG1 + XG2 + XG3) — what it proved, and what it did not

Landed 2026-08-10 as carried patches, fork-first, in a clone of the live tree at `wivrn-xg`, branch
`xg-round-1`: `c2da849d` (XG-B1), `cc91de4a` (the three Monado patches plus the one WiVRn-side line
they require), `fe4d4bc5` (the harness), `9845b932` (formatting), `a9fbbb2a` (fixes from a review of the series —
see the end of this section). Nothing pushed, and the live tree at `W` was not touched.

**The series applies and builds the way the existing eight patches do.** A from-scratch configure
fetches Monado at the pinned rev and `patches/apply.sh` `git am`s all eleven cleanly;
`wivrn-server` then builds with `-DGIT_DESC=v26.6.2`. (`WIVRN_WERROR` is off, matching the user's
own build directory — the preset's `-Werror` trips on a pre-existing GCC 16 `-Wpedantic` complaint
in `common/utils/wrap_lambda.h`, unrelated to any of this.)

**Validation is by a harness that links the patched Monado and calls it**, not by a re-implementation
alongside it: `tools/xg-cross-gpu-harness/` drives `vk_ic_allocate`, `vk_ic_get_handles`,
`vk_create_image_from_native`, `vk_create_and_submit_fence_native` and
`vk_create_fence_sync_from_native` out of the built `libaux_vk.a`. The only glue it reproduces is
the eight-line copy of modifier and plane layout from `struct vk_image` into `struct
xrt_image_native`, which in the product lives inside `comp_swapchain_create_init`. At 2064×2208,
three images, RGBA8, all three configurations are **pixel-exact over all 54,687,744 bytes**:

| configuration | result |
|---|---|
| `cross_device` off, one device (the untouched legacy path) | OK, opaque, 19.12 MiB |
| `cross_device` on, both roles on the AMD iGPU (what the temporary env lever produces) | OK, modifier `0x0`, 17.79 MiB |
| `cross_device` on, AMD allocates → NVIDIA imports (the real topology) | OK, modifier `0x0`, 17.79 MiB |

The allocated image comes back with `rowPitch = 8448` and `size = 17.79 MiB`, which are exactly the
figures §4.3's standalone probe measured — the patched product path and the research PoC agree
digit for digit.

**The cost re-measures through the product code to §4.6a's number.** With enough iterations to ramp
the dGPU's clocks (300; at 20 the NVIDIA baseline reads 3.4 ms and is meaningless — a harness
caveat worth remembering), the client's blit into an image of its own costs **0.071 ms** and into
the AMD-allocated shared image **1.286 ms**, so the marginal cost of the shared destination is
**+1.214 ms/eye** against §4.6a's +1.22 ms/eye. Independent reproduction, different code path.

**§4.8's poison is real on today's driver and patch 0011's shape is vindicated.** Run in its own
process, last, the harness's `--probe-opaque-fd-semaphore` mode does the import the patch exists to
avoid: the AMD export succeeds, the NVIDIA import returns `VK_ERROR_INITIALIZATION_FAILED`, and the
very next plain `DEVICE_LOCAL` image allocation on the idle 8 GB card returns
`VK_ERROR_OUT_OF_DEVICE_MEMORY`. This is why the suppression checks device identity first instead
of trying and falling back.

**A hard limitation found by the harness, not anticipated by the workplan: multiview swapchains
cannot cross this boundary.** RADV allocates a two-layer LINEAR modifier image happily and imports
it on itself, but NVIDIA's proprietary driver reports `maxArrayLayers = 1` for
`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`, so `array_size = 2` fails at import. Patch 0010 now
checks `maxArrayLayers` and says so by name rather than letting `vkCreateImage` return a bare
`VK_ERROR_FORMAT_NOT_SUPPORTED`. Consequences: an OpenXR application that uses a multiview
swapchain cannot run cross-GPU at all on this pair; **WP-XG6's format negotiation should grow an
array-size term**, and XG8 should note that OpenVR-shaped clients (xrizer submits per-eye textures,
so `array_size = 1`) are the ones expected to work. Related and unresolved: RADV reports
`arrayPitch = 0` for that two-layer image, so even the layout it hands out would not describe the
layer stride.

**The temporary activation lever, for WP-XG5 to delete:** `XRT_COMPOSITOR_FORCE_CROSS_DEVICE=1`, a
`DEBUG_GET_ONCE_BOOL_OPTION` read in `comp_vk_client.c`, forces `client_vk_compositor::cross_device`
true regardless of the UUIDs. Everything downstream — the swapchain flag, the dma-buf allocation, the
explicit-modifier import, the semaphore suppression — hangs off that one bool, which is otherwise
computed by comparing the client's `deviceUUID` against `xrt_system_compositor_info::compositor_vk_deviceUUID`.
Once XG5 splits those two UUIDs the comparison starts answering true on its own and the env var
should be removed.

**What XG8 must still prove** (none of it was attempted, and none of it is inferable from the above).
**Round 2 closed items 3, 4 and 5 of this list; §8.2 carries the current version of it:**

1. **That a real client's `VkDevice` has the two extensions enabled.** The runtime enables
   `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier` itself under `enable2`,
   and asks for neither under `enable1` — see the review note below. Under xrizer the game's device
   is created by **DXVK**, which consults neither list, so whether it happens to enable them is
   unknown and outside this runtime's control. If it does not, swapchain creation fails loudly with
   a message naming both. **This is the single most likely thing to go wrong, and if it does, the
   fix is in DXVK's device creation, not here.**
2. **That NVIDIA is as happy *rendering into* an imported LINEAR image as copying into one.** The
   harness copies (`vkCmdCopyImage`); a game renders (`COLOR_ATTACHMENT`). This is the same gap
   WP-XG0 left open and WP-XG9 (c) still owns.
3. **That the whole session works**, which needs XG4 (the `oxr` identity checks still reject a
   divergent `physicalDevice`) and XG5 (nothing yet hands out a different client UUID). Until both
   land, the only way to reach any of this code is the env lever, same-device.
4. **The queue-family ownership transfer in the product.** The harness does the
   `VK_QUEUE_FAMILY_FOREIGN_EXT` acquire/release correctly; WiVRn's compositor still does not
   (WP-XG7), and `comp_vk_client.c` still releases to `VK_QUEUE_FAMILY_EXTERNAL`.
5. **Format-list correctness.** WP-XG6 is untouched: formats are still advertised from the server
   device with `OPTIMAL` tiling and `OPAQUE_FD`, so a format that survives that check but not the
   modifier check would fail late.
6. **Anything about the encoder or the live config.** XG-B1's code change is unexercised at runtime;
   whether vaapi now probes the iGPU correctly with an NVIDIA compositor is untested.

**The series was reviewed for what it changes with cross-device mode OFF, and the honest answer is
"three things, now two".** The claim to beat was "flag off ⇒ behaviour unchanged"; it did not survive
contact, and the findings are worth recording because two of them are the kind that would only have
surfaced in front of the user.

- **A build break nothing here compiles.** `xrt_gfx_vk_provider_create` gained two parameters and has
  **two** callers — `oxr_session_gfx_vk.c` and `tests/tests_comp_client_vulkan.cpp`. WiVRn does not
  build Monado's tests, so every build in this round passed while that target could not compile.
  Fixed and verified both ways: the unfixed file fails with *"cannot convert `VkDevice` to
  `const xrt_uuid_t *`"*, the fixed one builds and the test passes. **Lesson for the remaining
  rounds: `-DBUILD_TESTING=ON` on a standalone Monado configure is the only thing here that compiles
  Monado's own callers.**
- **The required-extension list was a real risk and is reverted.** Adding the two extensions to
  `xrt_gfx_vk_device_extensions` — handed to applications verbatim as "enable these" — would fail
  `vkCreateDevice` for any client on a driver lacking either, with cross-GPU off. They now go only
  in the `enable2` optional list. This costs nothing that was ever going to work: DXVK creates the
  game's device and consults neither list.
- **`setup_semaphore` failing on `XRT_ERROR_IPC_FAILURE` is fatal again.** Degrading *every*
  failure, as the row asked, would also swallow a dead compositor socket and turn a clean
  `xrCreateSession` failure into an obscure one several calls later. Every other failure still
  degrades to the fence path.
- **A landmine in the gate itself.** `compositor_vk_deviceUUID` had **no readers at all** before this
  series — only writers and explicit `(void)` no-ops — so any compositor that left it zero would have
  silently moved *every* client onto the cross-device paths. It is now treated as "not reported".
  WiVRn fills it in (`W/server/compositor/compositor.cpp:800`), so this was latent, not live.
- Smaller: the import no longer trusts `vk_bundle`'s `has_EXT_*` flags alone (a bundle built by
  `vk_init_from_given` is *told* what to believe and cannot verify it — `vkGetDeviceProcAddr`
  returning NULL for an unenabled extension's entry point can), the IPC reply's layout struct is
  zero-initialised, and the plane copy is bounded like the two IPC loops already were.

**What remains deliberately changed with the flag off, and should be understood as such:** every
`XR_KHR_vulkan_enable2` client now gets those two extensions enabled on its device when the physical
device supports them, and a `setup_semaphore` failure that is *not* an IPC failure no longer kills
the session. Both are judged improvements; neither is "unchanged".

### 8.2 Round 2 (XG4 + XG5 + XG6 + XG7) — the trigger is real, and the format list was not insurance

Landed 2026-08-10 on branch `xg-round-2`, cut from `xg-round-1` at `a9fbbb2a`: `e16cdf98` (the three
new Monado patches plus the deletion of the temporary lever), `a52ebc49` (client-GPU selection and
the format intersection), `b29e1eda` (the queue-family acquire), `7cea6954` (the harness). Nothing
pushed; `~/code/wivrn-xg` and `~/code/wivrn-26.6.2` were not touched.

Patch series, now fourteen: `0012-st-oxr-let-a-client-legally-render-on-a-different-de.patch`
(`27e5653b`), `0013-c-client-refuse-a-cross-device-swapchain-the-client-.patch` (`d46fd7b8`),
`0014-c-client-release-a-cross-device-swapchain-image-to-V.patch` (`71c8736d`), and `0009` re-cut as
`e3361712` with `XRT_COMPOSITOR_FORCE_CROSS_DEVICE` removed.

**A from-scratch configure `git am`s all fourteen and builds.** `wivrn-server` links with
`-DGIT_DESC=v26.6.2` (`--version` prints `WiVRn version 26.6.2`) and `WIVRN_WERROR=OFF`, the only
warning being the pre-existing GCC 16 `-Wpedantic` complaint in `wrap_lambda.h`. **And the hazard-2
lesson was applied:** a standalone Monado configured `-DBUILD_TESTING=ON` builds and passes
**23/23** `ctest`, including `tests_comp_client_vulkan` — which is excluded from the `all` target, so
it has to be named explicitly or ctest reports it "Not Run" rather than failing. That target and
`st_oxr` are what compile this round's `oxr` and client-compositor changes outside WiVRn.

**The one measured result that contradicts the workplan: XG6 narrows the format list.** The row said
"§4.2 says all four common formats survive, so this should narrow nothing today — it is correctness
insurance". §4.2 measured four formats; Monado advertises nineteen. Evaluated properly, the AMD
compositor → NVIDIA client list is **six formats where the same-device list is eight**:

| format | same-device | cross-device | who declines |
|---|---|---|---|
| `R16G16B16A16_UNORM` | KEPT | **dropped** | the client, cannot import |
| `R16G16B16A16_SFLOAT` | KEPT | KEPT | — |
| `R8G8B8A8_SRGB` | KEPT | KEPT | — |
| `B8G8R8A8_SRGB` | KEPT | KEPT | — |
| `R8G8B8A8_UNORM` | KEPT | KEPT | — |
| `B8G8R8A8_UNORM` | KEPT | KEPT | — |
| `R5G6B5_UNORM_PACK16` | KEPT | KEPT | — |
| `R32_SFLOAT` | KEPT | **dropped** | the client, cannot import |
| the six depth/stencil formats | dropped | dropped | the compositor, cannot export |

So the insurance paid out immediately: two formats would have been advertised and then failed inside
`vkCreateImage` in a client that had every reason to trust the list. The four formats §4.2 measured
all survive, as promised — note that `A2B10G10R10_UNORM_PACK32`, one of those four, **is not in
Monado's `VK_CSCI_FORMATS` list at all**, so it is never advertised by either path and the
10-bit-swapchain question is Monado's, not this work's.

**The depth row is the other new limitation, and it is not a cross-vendor one.** RADV declines to
*export* any depth or stencil format as a LINEAR explicit-modifier dma-buf, so no depth format can
back a cross-device swapchain. Same-device sessions are unaffected (they still use `OPTIMAL` +
`OPAQUE_FD`, where depth is fine); a cross-GPU session simply will not advertise a depth format, and
an application wanting `XR_KHR_composition_layer_depth` gets none. See §9 Q6.

**The harness now drives the new pieces, and links the server's own code to do it.** The client-GPU
resolver and the format-agreement query live in `server/utils/cross_gpu.cpp` — plain C over
`VkPhysicalDevice` handles and a `PFN_vkGetInstanceProcAddr`, needing no `VkDevice` and no
compositor — and that file is compiled straight into `tools/xg-cross-gpu-harness`. Results:

- **`--client-gpu`**: every accepted spelling resolves to the device it names, on both GPUs — lower-case
  UUID (`0fec34e97f02f760a848bc113180d8ec`), dashed upper-case UUID, `/dev/dri/renderD128`, and bare
  `renderD128`. Unset resolves to the compositor's own device. `renderD999` is refused with the full
  candidate listing. The UUIDs it prints match §4.1 digit for digit.
- **`--formats`**: the table above, printed for both configurations, with the four §4.2 formats asserted.
- **`--import-gate`**: on the client device, `array_size = 1` is accepted and `array_size = 2` is refused
  with `VK_ERROR_FEATURE_NOT_PRESENT` and the message naming `maxArrayLayers`, *before* the compositor
  allocates anything. This is the same function `client_vk_swapchain_create` now calls.
- **The three round-1 swapchain configurations still pass, pixel-exact over all 54,687,744 bytes**, with
  the legacy path still opaque/no-modifier at 19.12 MiB and the cross-device path still modifier `0x0`
  at 17.79 MiB, `rowPitch = 8448`. The marginal cost of the shared destination re-measures at
  **+1.192 ms/eye** against round 1's +1.214 and §4.6a's +1.22. No regression from any of this round.

**The default path is unchanged, and this time the claim is narrow enough to be true.** Every new
behaviour is gated: `narrow_formats_to_cross_device` behind `vk.cross_device`, the acquire pre-pass
behind `vkic.info.cross_device` per swapchain, the `FOREIGN_EXT` release behind `c->cross_device`,
the import gate behind `xinfo.cross_device`, both `oxr` relaxations behind
`oxr_vk_suggests_cross_device_client`. With no `client-gpu` key the resolver returns the compositor's
own device (asserted by the harness), the two UUIDs are equal by construction, `cross_device` is
false, and the legacy allocator path is what the harness exercises and finds byte-identical.
**What does change with the flag off, and should be understood as such:** `VK_EXT_queue_family_foreign`
is now enabled on the compositor's device and on `enable2` clients' devices — enabled but unused —
and the unmatched-GPU warning in `oxr_vk_get_physical_device` is considerably more verbose.

> **`wivrn-server` was deliberately not run.** A live `wivrn-server` from `~/code/wivrn-26.6.2` was
> serving the headset throughout this work. Starting a second one risks the D-Bus name and, worse,
> `active_runtime.cpp` rewriting `~/.config/openxr/1/active_runtime.json` out from under the live
> session. Only `--version` was invoked. Everything else was proved through the harness, which takes
> no DRM master, opens no compositor and touches no session.

**Two new hazards for XG8.**

1. **`VK_EXT_queue_family_foreign` cannot be verified on a client device the application created.**
   The extension defines no entry points, so the `vkGetDeviceProcAddr`-returns-NULL trick round 1
   used for the two dma-buf extensions does not apply — there is nothing to ask for. The runtime adds
   it to the `enable2` optional list, but under xrizer the device is DXVK's and consults neither list.
   The client releases to `FOREIGN_EXT` whenever it is cross-device, because a release the compositor
   cannot match is worse than a technically-unenabled constant, and every driver accepts the value.
   **This joins the same XG8 bucket as `VK_EXT_external_memory_dma_buf` and
   `VK_EXT_image_drm_format_modifier`: three extensions DXVK must happen to enable.**
2. **The ownership transfer is one-directional and unbalanced by design.** The client releases every
   frame; the compositor acquires only for images the frame's layers actually reference. A released
   image that no layer uses is never acquired, and the client's next acquire is the pre-existing
   `UNDEFINED` → optimal barrier with `VK_QUEUE_FAMILY_IGNORED` — a discard, which is what a client
   re-rendering the whole image wants and what the PoC and harness both validated. It is not a
   symmetric protocol and validation layers do not track ownership across devices, so nothing will
   complain either way; it is recorded here so that a future corruption report starts in the right
   place.

**The precise XG8 checklist, superseding §8.1's list.** Items 3, 4 and 5 of that list are now done
(the identity checks, the ownership transfer, the format list). What remains:

1. **Three extensions on DXVK's device.** `VK_EXT_external_memory_dma_buf`,
   `VK_EXT_image_drm_format_modifier`, `VK_EXT_queue_family_foreign`. The first two fail loudly by
   name at swapchain creation; the third fails silently, because there is no way to detect it. **Still
   the single most likely thing to go wrong, and the fix would be in DXVK, not here.**
2. **`COLOR_ATTACHMENT` into an imported LINEAR image on NVIDIA.** Unchanged from round 1: the harness
   copies, a game renders. This is WP-XG9 (c).
3. **A session end to end.** Set `client-gpu` to the dGPU's render node, drop the `VK_DRIVER_FILES`
   pin so the server's `VkInstance` can enumerate it (**the resolver throws at startup if it cannot** —
   which is the intended, legible failure, but it is a config change that must accompany the key),
   confirm no `assert_eq!(pd, info.physical_device)` from xrizer, and measure frame times against a
   same-GPU baseline.
4. **That a real title asks for a swapchain the narrowed list still contains.** Six formats remain;
   xrizer/DXVK titles overwhelmingly want `R8G8B8A8_SRGB` or `B8G8R8A8_SRGB`, which do. A title
   wanting a depth layer will not get one.
5. **That the HypXRland desktop client still composites in the same session** — the §5.1
   heterogeneous case, now genuinely reachable: it stays on AMD, computes `cross_device = false`
   against the compositor's UUID, and should take the untouched path while the game does not.
6. **The encoder and the live config** — XG-B1's runtime half, unchanged and still unexercised.

### 8.3 Round 3 (XG8 attempt 1) — the suggestion was handed to a client that cannot use it

**The stack worked. The desktop did not, and the reason was one line of policy.** With
`client-gpu = /dev/dri/renderD128` live for the first time on 2026-08-10, the Quest connected and
everything §8.1/§8.2 built behaved: the format narrowing ran at runtime and matched the harness digit
for digit (depth formats "rejected by the compositor's GPU, which cannot export it"), the compositor
stayed on AMD, vaapi encode worked, and hypxrhud — an EGL/GL ES client — connected and streamed.
**Hyprland went into a connect/disconnect loop instead**, which the server's journal records as
`client_connected` → `describe_client` → `client_disconnected` cycles from one pid — **71 of them
between 18:19:08 and 18:42:47**, every ~2 s at first and every ~32 s once its retry backoff settled,
and still going as this was written. The same journal shows the earlier same-GPU session that day
(13:47) with Hyprland connecting **once** and staying, alongside `steam`, `wineopenxr test instance`
and `Big Walk` — so the loop is specific to the cross-GPU configuration, not to the day's build.

**What refused, and why it was wrong.** `COpenXRManager::start()` carries a wrong-GPU guard
(`src/openxr/OpenXRManager.cpp:623-682`, added after coredumps 8986/39318) which asks *"which GPU
does the runtime composite on"* through `XR_KHR_vulkan_enable2` — `xrGetVulkanGraphicsDevice2KHR`,
`src/openxr/XRGpuProbe.cpp:41` — and refuses to start XR when the answer differs from the DRM node
`openxr:gpu` selected. That query answers with `client_vk_deviceUUID`. **Until XG5 those two were the
same value by definition; after XG5 they are not, and the guard was reading the wrong one.** It
compared the dGPU against its own AMD EGL context, concluded the runtime had moved underneath it, and
did exactly what it was written to do. Hyprland enables `XR_MNDX_egl_enable` + `XR_KHR_opengl_es_enable`
to bind with and `XR_KHR_vulkan_enable2` *only* to run that probe (`src/openxr/XRSession.cpp:82`, and
the comment at `:97`), so it is the exact case where the two answers must differ.

**Item 1 — how the GL/EGL client path picks its internal Vulkan device: it has none.** This was the
question the round was gated on, and the answer removes a whole branch of the workplan.
`oxr_session_populate_egl` (`oxr_session_gfx_egl.c:63-72`) calls `xrt_gfx_provider_create_gl_egl`
(`comp_egl_client.c:654-789`), which builds a `client_gl_compositor` that wraps the
`xrt_compositor_native` **directly** — `comp_gl_client.c:622`, `c->xcn = xcn;`. There is no
`client_vk_compositor` underneath it. Grepping every GL/EGL client file for `VkInstance|VkDevice|VkPhysicalDevice|vkCreate|vkEnumerate|vk_bundle` returns **zero matches**; the only Vulkan-shaped text
is two integer format tables (`comp_gl_client.c:63-108`). The device such a client renders on is
whatever its own `EGLContext` was already created on — the context arrives fully formed in
`XrGraphicsBindingEGLMNDX::context` and is only made current (`comp_egl_client.c:711`). Consequences:

- **hypxrhud worked by design, not because NVIDIA was masked in its environment.** No EGL client
  consults the suggestion on the binding path.
- **Hyprland would not have crashed had its guard not fired.** Its EGL context is on the compositor's
  own GPU, which is where the images come from; the guard was a false positive, full stop.
- **Item 3 is moot as written.** There is no GL-client Vulkan device to re-derive from the EGL
  context, so the defence-in-depth patch that item asked for has nothing to patch. See "what was
  deliberately not built" below for the guard that *would* be the real version of it.
- The GL client imports through `glImportMemoryFdEXT(memory, size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, …)`
  (`comp_gl_memobj_swapchain.c:79-82`, preferred) or `eglCreateImageKHR(… EGL_LINUX_DMA_BUF_EXT)`
  (`comp_gl_eglimage_swapchain.c:274-291`, fallback chosen at `comp_egl_client.c:508-555`). **The
  preferred path is an opaque fd, importable on the exporting device and nowhere else** — which is
  why "the compositor's device" is not a preference for these clients but the only possible answer.
  It also never sets `xrt_swapchain_create_info::cross_device` (the only writer in the tree is
  `comp_vk_client.c:678`), so the server allocates it ordinary `OPAQUE_FD`/`OPTIMAL` images and the
  entire XG1/XG2 machinery stays out of its way.

**Where the per-API logic landed, and why not in the server.** In `st/oxr`, as
`patches/monado/0015-…`. The workplan assumed the WiVRn compositor could answer per client, because
`ipc_handle_instance_describe_client` logs each client's extensions — **it can't, and the log is what
proves it.** The struct it prints is `struct xrt_application_info` (`xrt_instance.h:72-87`): twelve
booleans for hand tracking, eye gaze, body and face tracking, and **no graphics-binding bit at all**
(the journal lines from the incident are exactly those twelve). `ipc_handle_system_compositor_get_info`
does have the per-client state in scope (`ipc_server_handler.c:402-411`, and describe_client provably
precedes it — `ipc_client_connection.c:413-414` runs at `xrCreateInstance`, `get_system_info` at
`xrGetSystem`), so the *ordering* supports a per-client answer; the *data* does not exist. Answering
there would mean inventing new wire fields and then varying a struct every other reader treats as a
constant of the system. In `st/oxr` the extension set is already authoritative and in scope, needs no
IPC, needs no WiVRn change, and fixes in-process Monado targets for free. **WiVRn itself is unchanged
this round.**

**What the patch does.** One pure decision — `oxr_select_vk_client_device_uuid(extensions, info)` and
`oxr_select_gl_client_device_uuid(info)` in `oxr_system.c` — consulted by the two entry points that
answer "which device":

| asked by | before | after |
|---|---|---|
| `xrGetVulkanGraphicsDevice(2)KHR`, Vulkan-only client | `client_vk_deviceUUID` | unchanged |
| `xrGetVulkanGraphicsDevice(2)KHR`, client that also enabled EGL/GL/GLES | `client_vk_deviceUUID` | **`compositor_vk_deviceUUID`** |
| `xrGetSystemEGLDeviceMND` (`XR_MND_query_egl_device`) | `client_vk_deviceUUID` | **`compositor_vk_deviceUUID`** |
| either, on a single-device runtime | one UUID | the same UUID, both branches |

The second row is the incident. **The third row was a second, independent instance of the same bug,
found while reading rather than by the failure**: `oxr_egl.c:88` matched EGL devices against
`client_vk_deviceUUID` under a comment and an error message that both said "the compositor's device".
Any EGL client that used that extension to choose its device would have been pointed at the dGPU and
then failed its first `glImportMemoryFdEXT`. Hyprland does not use it; something else would have.

Two deliberate details. **The decision is taken from the enabled extension set, not from the graphics
binding**, because both entry points are called *before* `xrCreateSession` when no binding exists —
and an instance enabling both kinds counts as GL, because the binding is what has to work and the
Vulkan use is a probe. **A compositor that reports no device of its own is not second-guessed**: with
`compositor_vk_deviceUUID` unset the answer stays the suggestion, since handing a GL client a zeroed
UUID would match no device at all. `oxr_vk_suggests_cross_device_client` now measures divergence
against *what this client is told* rather than what the compositor published, so the identity-check
relaxations XG4 added stay strict for a GL client, which has nothing to diverge.

**The pure-Vulkan answer is unchanged byte for byte, and that is asserted, not asserted-to.** xrizer's
`assert_eq!(pd, info.physical_device)` still sees the `client-gpu` device.

**Validation.**

- **All fifteen patches `git am` cleanly from a from-scratch configure** at the pinned Monado rev, and
  `wivrn-server` builds with `-DGIT_DESC=v26.6.2` / `WIVRN_WERROR=OFF` — `--version` prints
  `WiVRn version 26.6.2`, zero errors and, this round, zero warnings.
- **Standalone Monado `-DBUILD_TESTING=ON`: 24/24 `ctest`**, including `tests_comp_client_vulkan` and
  `st_oxr`'s `tests_input_transform`, both of which must be named explicitly since they are excluded
  from `all`. The new `tests_oxr_client_device` (18 assertions) drives the selection directly:
  Vulkan-only → client GPU; EGL-only → compositor; EGL **and** vulkan_enable2 → compositor; each of
  the three GL extensions alone; **one device → the same answer whichever kind of client asks**; and
  the unset-compositor-UUID fallback. That last-but-one case is the "default path is bit-identical"
  claim, now a test rather than a reading.
- **No harness regression.** `--client-gpu` resolves all four spellings on both GPUs and refuses
  `renderD999` with the full listing; `--formats` still narrows nineteen advertised formats to the
  same **six**, with the same four §4.2 formats surviving. Neither touches the changed code, so this
  is a build check rather than a proof — stated as such.
- **Caveat on `all`:** a standalone Monado `--target all` does *not* link, and did not before this
  round either: WiVRn's patch 0008 moves `u_git_tag` into WiVRn's build, so `monado-ctl` and
  `libmonado.so` fail with `undefined reference to u_git_tag`. §8.1's lesson ("`-DBUILD_TESTING=ON` is
  the only thing that compiles Monado's own callers") holds, but it has to be run as
  *build the test targets, then ctest* — not `all`.

**What was deliberately not built, and should be considered later.** An EGL client on the wrong GPU
today gets an opaque fd from another device handed to `glImportMemoryFdEXT` — undefined behaviour, and
plausibly the very radeonsi crash Hyprland's guard exists to prevent. The runtime *could* refuse that
loudly: query the bound `EGLContext`'s device (`EGL_EXT_device_query` → `EGL_DEVICE_UUID_EXT`, which
`oxr_egl.c` already trusts to equal the Vulkan `deviceUUID`), compare against
`compositor_vk_deviceUUID`, and fail `xrCreateSession` — which would make Hyprland's own guard
redundant for every EGL client, not just this one. It is not in this round on purpose: **tonight's
failure was a safety guard firing wrongly, and the wrong response to that is to add a second guard
that can fire wrongly** on a session the user is about to trust. It wants its own round, with the
desktop client already known-good.

**Two things about the live box, recorded because they are not inferable later.** The `wivrn-server`
still running as of this writing is the **`wivrn-xg` round-2 build**, and its compositor child holds
fds on *both* render nodes (11 on AMD `renderD129`, 2 on NVIDIA `renderD128` plus the `nvidia*`
nodes) — i.e. it is still the cross-GPU configuration, with a headset still connected, **and Hyprland
is still retrying and being refused every ~32 s**. Meanwhile `~/.config/wivrn/config.json` **no longer
contains the `client-gpu` key**: the config was reverted but the service has not been restarted, so
the running process is the only remaining copy of the failing configuration. Everything in this round
was proved through builds, `ctest` and the harness; no `wivrn-server` was started, nothing was
restarted, and the GPUs were **not** exclusive — no timing was measured and none is claimed.

**What XG8 attempt 2 must verify.** §8.2's checklist stands; these are added, and the first two are
the pass/fail of this round:

1. **The desktop client comes up.** With the same `client-gpu`, Hyprland's probe must now report the
   *AMD* device, log `XR GPU verified against runtime`, and start a session — not
   `XR is refusing to start`. Its log will also carry the new runtime line naming both UUIDs and
   saying why the answer was overridden; if that line is absent, the running server is not the
   patched one.
2. **xrizer still gets NVIDIA.** No `assert_eq!(pd, info.physical_device)`. The two together are the
   whole point: two clients, two answers, one session.
3. That hypxrhud still connects — it should be untouched, and now for a reason rather than by luck.

---

## 9. Open questions for the user

1. **Upstream-first or fork-first, given that upstream has twice said no?** §6.4 and §6.5 show WiVRn
   closed the request *"not planned, complexity with little benefit"* and Valve declined too — but
   **both reached that verdict against a GPU→host→GPU copy model that §4 shows is unnecessary.** A
   PR carrying the measured dma-buf numbers might land where the 2024 request didn't. That is a
   judgement call about your appetite for upstream advocacy, not a technical one. (See also Q4.)
2. **Do you want Shape B applied now, before Shape A is built?** It unblocks The Big Walk this week
   at the cost of ending the AMD-only evaluation and moving the compositor to the dGPU. Given
   research/25's finding that the dGPU never actually sleeps today, the power argument for AMD-only
   is already weaker than when it was adopted — but that is your call, not mine, and it interacts
   with the Strix-Point-no-dGPU candidacy question.
3. **~~Is ~3.9 ms/frame acceptable?~~ ANSWERED — the number is ~2.4 ms, and you accepted it.**
   The re-measure (§4.6) cut it, and §10 establishes that it cannot be reduced further by any
   exclusive-mode trick: tier 1 is already shipped and free, tiers 2 and 3 are architecturally
   blocked. The residual question is narrower: **if 2.4 ms ever proves too much, the only lever left
   is Shape B** (compositor on the dGPU), which removes the transfer by construction. Do you want
   that treated as the fallback plan, or is 2.4 ms simply fine?
4. **How much carried-patch burden are you willing to take?** Shape A adds four Monado patches to
   `W/patches/monado/`, rebased at every WiVRn bump. Upstreaming them (they fix Monado's own TODO
   and complete a feature it half-shipped) would remove that burden but on upstream's timeline, not
   yours. Do you want the patches written upstream-first (cleaner, slower) or fork-first (faster,
   carried)?
5. **Should the AMD desktop client also pay the LINEAR cost** (+0.039 ms/eye) so one global
   suggestion serves both clients (§5.1), or do you want per-client device policy built from the
   start? I recommend the former — it is much smaller and the escape hatch stays open.
   **Partly answered by what XG5 shipped, and not in the direction the question assumed.** `client-gpu`
   is one global suggestion, as recommended — but the LINEAR cost is *not* global, because the
   decision is made per client from the device it actually bound. The AMD desktop client compares its
   own UUID against the compositor's, finds them equal, and takes the untouched same-device path;
   only the client that really is elsewhere pays. The +0.039 ms/eye the question worried about is
   therefore not charged to anyone. **What is still global is the advertised format list** — narrowed
   for every client once the two devices differ (§8.2) — which is the remaining reason to want
   per-client policy, and a much weaker one. Confirm you are happy with that before XG8.
6. **Is losing depth swapchains cross-GPU acceptable?** New, from §8.2: RADV will not export any
   depth or stencil format as a LINEAR explicit-modifier dma-buf, so a cross-GPU session advertises
   no depth format and `XR_KHR_composition_layer_depth` is unavailable to *every* client in that
   session, including the same-device desktop one. Same-GPU sessions are untouched. I do not know of
   an xrizer title that submits depth layers, and WiVRn's squasher treats depth as an optional
   refinement rather than a requirement, so I expect this to cost nothing in practice — but it is a
   capability the session silently stops advertising and you should know before it surprises you.

---

## 10. Addendum — can the overhead be optimised out when the game is exclusive?

The user's question, verbatim:

> "Is it possible to seamlessly optimize out this overhead when the NVidia app is exclusive
> (don't need to composite with anything else)?"

**One-line answer: partly — and the part that is possible is already implemented and already fires
automatically.** WiVRn 26.6.2 *already* has a one-projection-layer fast path that skips the layer
squash. But the squash was never the expensive part. The expensive part is the **PCIe write into
the compositor-owned buffer (§4.6a: 2.44 ms of the ~2.4 ms total)**, and that cannot be removed by
any per-frame decision, because *which GPU owns the swapchain* is fixed for the life of the session.
Removing it means moving the compositor to the dGPU — i.e. Shape B — which is a whole-session
property, not something that can flip when a notification appears.

### 10.1 Decomposing the per-frame cost by stage

Using the re-measured §4.6/§4.6a numbers, per frame, both eyes, against an 11.1 ms budget:

| stage | both eyes | removable when exclusive? |
|---|---|---|
| **PCIe write** — game blits into the AMD-owned swapchain image | **2.44 ms** | **No** (see §10.4) |
| LINEAR-vs-OPTIMAL tiling penalty on that write | 0.04 ms | No, and negligible |
| **Layer squash** — 2 compute dispatches + a full-eye RGBA intermediate | *(not measured)* | **Yes — already skipped, automatically** |
| **Foveation + RGB→BT.709 4:2:0 convert** — 1 compute dispatch | *(not measured)* | **No — mandatory, see §10.3** |
| Compositor reads the swapchain image | 1.0 ms | No — but not a cross-GPU cost; it happens in every topology |
| Encoder input handoff — `vkCmdCopyImage` into the VA-imported dmabuf | *(not measured)* | No — AMD-side, already in WiVRn's existing budget |

**Caveat, stated plainly:** the squash, foveation and encoder-handoff rows are *not* measured by the
PoC — it benchmarks image copies, not WiVRn's compute shaders. Their absolute costs would need
instrumenting inside a running WiVRn. What the PoC *does* establish is the one row that dominates
and the one row the user asked about.

### 10.2 Composite bypass — **already shipped**, and gated on exactly the right predicate

`W/server/compositor/compositor.cpp:326-345`:

```cpp
// Check if we can pass a layer directly to foveation
if (layer_accum.layer_count == 1 and
    (layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION or
     layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION_DEPTH))
```

…with the `else` branch commented `// no fast-path, squash layers` (`:348`). In the fast path the
client's swapchain image views go straight to the foveation pass (`:335-338`); in the slow path they
go via `squasher.get_views()` (`:360`). The squash itself is `wivrn::layer_squasher::do_layers`
(`W/server/compositor/layer_squasher.cpp:388-642`), one compute dispatch **per view** (`:633-634`)
running Monado's `layer.comp`, writing a single 2-array-layer RGBA intermediate allocated once at
`layer_squasher.cpp:344-365`.

So "exactly one full-view projection layer" already means **2 compute dispatches and one full-eye
RGBA intermediate skipped per frame**, with no configuration and no work from us. The predicate is a
pure `layer_count == 1` plus a type check — no FOV-coverage test, no opacity test.

**What the existing fast path silently drops** (product risk; read from the two branches, not
observed at runtime):

- **Colour scale/bias** — `layer_squasher.cpp:554-557` → `layer.comp:498`. Identity by default; a
  silent visual difference for apps setting `XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE`.
- **Chroma key** — `layer_squasher.cpp:696-708` → `layer.comp:118-153`. No-op unless enabled.
- **FOV-union viewport shrink** — `layer_squasher.cpp:542-577`. The squash intersects layer FOV with
  HMD FOV and shrinks the encoded viewport; the fast path forgoes that resolution win.
- **Source-smaller-than-stream edge smear** — the squash clamps source ≥ encoder extent via
  `min_size` (`:575-576`); the fast path has no clamp, and `fill_ubo`'s tail
  `std::ranges::fill(ubo, ubo[0])` (`W/server/compositor/foveation.cpp:470-471`) repeats the final
  source column rather than rescaling. Reasoned from code, not measured.

### 10.3 Why full bypass (encoder eats the client's image) is impossible

The blocker is **not** dmabuf import and **not** encoder-side image binding — both are flexible
enough. `video_encoder::present_image` takes the image as a **per-frame argument**
(`W/server/encoder/video_encoder.h:127-129`, called at `compositor.cpp:504-512`), and the Vulkan
Video backend already re-points per frame via a `VkImage`-keyed view cache
(`W/server/encoder/video_encoder_vulkan.cpp:778-785`).

The blocker is that **foveated resampling and RGB→BT.709 4:2:0 conversion are fused into one
mandatory compute pass**, `W/server/compositor/shaders/foveation.comp:46-55,118`:

```glsl
const mat3 color_space = mat3(
/* Y */ 0.2126,  0.7152,  0.0722,
/* Cb*/-0.1146, -0.3854,  0.5,
/* Cr*/ 0.5   , -0.4542, -0.0458);
...
colour.xyz = rgb_to_ycbcr(from_linear_to_srgb(colour.rgb));
```

…and **every encoder backend addresses `ePlane0`/`ePlane1` of a 2-plane YCbCr image** (vaapi
`video_encoder_va.cpp:515-555`, nvenc `video_encoder_nvenc.cpp:446-476`, Vulkan Video
`video_encoder_vulkan.cpp:688-728`, x264 `video_encoder_x264.cpp:242,258`). A client's RGBA
swapchain image has no plane aspects. There is no RGB input path anywhere.

**Good news hiding in this:** foveation is *not* lost by the existing bypass — it is a separate pass
that consumes whatever views it is given (`compositor.cpp:429-438`), so the fast path keeps full
foveation *and* gets correct cropping free, because the sub-rect and Y-flip are baked into its
integer coordinate table on the CPU (`foveation.cpp:520-562`). Foveation also self-disables to a 1:1
mapping when there is nothing to shrink (`foveation.cpp:351-360`).

Removing the colour conversion would mean relocating it into the encoder — for vaapi, a
VA-API/ffmpeg RGB→NV12 filter stage. **That trades one compute pass for another and saves nothing.**

### 10.4 The encoder hop — seamless in principle, architecturally blocked in Shape A

**The protocol would allow it.** The stream descriptor is five fields
(`W/common/wivrn_packets.h:693-704`): `width`, `height`, `codec[3]`, `frame_rate`, `refresh_rate`.
Encoder *implementation*, bitrate and bit depth are **not** in it.
`send_video_stream_description` (`W/server/compositor/compositor.cpp:660-672`) derives it only from
image extent, `settings[0].fps` and `settings[*].codec` — so swapping vaapi↔nvenc at matched
codec/resolution/fps yields a **byte-identical descriptor**, and the client early-returns without
touching the decoder (`W/client/scenes/stream.cpp:1152-1161`,
`if (video_stream_description == description) return;`).

Parameter sets are in-band by construction (MediaCodec is configured with no `csd-0`/`csd-1`,
`W/client/decoder/android/android_decoder.cpp:137-172`), and mid-stream IDR + resync is a routine
self-healing path that already fires on every packet loss (`W/server/encoder/idr_handler.cpp:49-55`)
and every bitrate change (`W/server/encoder/ffmpeg/video_encoder_ffmpeg.cpp:71-82`). The client never
blacks out during it, because `latest_frames` is a 3-deep rolling buffer only ever swapped, never
cleared (`stream.cpp:566,582-590`). **Cost of a swap ≈ 1 network RTT of reprojected-only frames —
1-2 frames at 90 Hz.** There is even an existing mid-session descriptor re-send:
`compositor::resume()` (`compositor.cpp:829-834`), fired on every doff/don.

**But it cannot be reached in Shape A.** nvenc feeds CUDA by exporting a device-local `VkBuffer`
with **`vk::ExternalMemoryHandleTypeFlagBitsKHR::eOpaqueFd`** and importing via
`cuImportExternalMemory` (`W/server/encoder/video_encoder_nvenc.cpp:389-411`). Per §6.1, `OPAQUE_FD`
requires matching `deviceUUID`/`driverUUID` — so **nvenc only works when the compositor's Vulkan
device *is* the NVIDIA GPU.** With the compositor on AMD, nvenc is unreachable **by construction,
not merely unimplemented**. VAAPI is the only cross-GPU-capable backend, and it is already in use.

Two further constraints if revisited: a resident shadow encoder set costs roughly **130-165 MB**
(vaapi preallocates `initial_pool_size = 10` full-size surfaces per encoder,
`video_encoder_va.cpp:88`) and would hold a **VCN session** open while idle — the exact resource
`hypxrva` exists to arbitrate. And **bit depth must be held constant**: it is absent from the
descriptor, so a 10→8-bit swap would change the H.265 profile mid-stream with no protocol signal,
the one case the sources cannot promise is seamless.

### 10.5 Hysteresis — the happy answer is that none is needed

The concern was that exclusivity flips whenever a notification or HUD overlay appears, and our own
compositor deliberately avoids riding transient overlays (the `coversOutput` gate,
`src/openxr/XRStereoPair.hpp:43-49`, whose comment notes that a gate keyed on the wrong thing
produces "the compositor broke" failure modes).

**That concern does not apply to tier 1, because the transition is free.** The fast path is a
per-frame `if` on `layer_count` with no setup, no teardown, no reallocation and no state: the
foveation pass consumes either the client's views or the squasher's, and nothing else differs. A HUD
quad appearing costs exactly one frame of squash; it disappearing returns to the fast path the next
frame. **There is nothing to debounce, and adding hysteresis would make it worse** by keeping the
squash alive for frames that no longer need it.

Hysteresis *would* be needed for anything switching encoders or buffer residency (tiers 2-3), where
a transition costs 1-2 frames of reprojection plus encoder re-init. Suggested policy **if those ever
become reachable**: ~2 s of continuous exclusivity before switching in, ~5 s before switching back,
so a notification cannot induce a switch. But since tier 2 and tier 3 are both blocked, **this policy
has nothing to govern today** and should not be built speculatively.

### 10.6 Ranking the tiers

| tier | what it does | recovers | status |
|---|---|---|---|
| **1 — composite bypass** | skip the squash when one projection layer | 2 dispatches + 1 RGBA intermediate/frame | ✅ **already shipped and automatic** — zero work |
| **2 — encoder hop** (vaapi/AMD ↔ nvenc/NVIDIA) | keep pixels on the dGPU through encode | most of the 2.44 ms | ❌ **blocked**: nvenc's `OPAQUE_FD` CUDA import pins it to the compositor's GPU (§10.4) |
| **3 — full dGPU residency** (swapchain allocated on NVIDIA) | eliminates the PCIe write | the whole 2.44 ms | ❌ **blocked**: needs the client-allocates path (Android-only, §2.1) *and* NVIDIA→AMD import, which fails on both pitch (§4.4) and residency (§4.8) |

**Recommendation: option (c) from the user's framing — "the estimate was pessimistic".** The
re-measure already did most of the optimising: the marginal cross-GPU cost is **~2.4 ms, not
3.9 ms**, tier 1 is already free and already on, and the remaining 2.4 ms is a single irreducible
PCIe write no per-frame exclusivity decision can remove. **If that 2.4 ms ever proves unacceptable,
the answer is not a clever exclusive-mode path — it is Shape B** (compositor on the dGPU), which
removes the transfer by construction and is a config change.

**One genuinely cheap follow-up falls out**, the only new work this addendum proposes: verify the
shipped fast path actually engages for a real xrizer/DXVK game (it may submit a HUD quad and never
hit `layer_count == 1`), and decide whether the four things it silently drops (§10.2) matter. That
is **WP-XG9**.
