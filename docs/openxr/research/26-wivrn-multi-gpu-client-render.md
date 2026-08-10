# Research: WiVRn multi-GPU — the OpenXR client renders on a different GPU than the compositor

**Status:** research + **measured**. No product code is implemented. The architectural findings are
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
image costs **1.43 ms/eye** and the AMD compositor's read of it costs **0.49 ms/eye** — about
**3.9 ms of an 11.1 ms 90 Hz frame for both eyes**. Forcing the compositor's swapchains LINEAR costs
the AMD compositor only **+0.045 ms/eye (+10%)**, which is the single most encouraging number in
this report. Caveats on the NVIDIA-side absolute numbers in §4.6 — they look clock-limited and the
ratios, not the absolutes, are what should be trusted.

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

### 4.6 Cost — and an honest caveat about the absolute numbers

At 2064×2208 RGBA8 (17.4 MiB/eye), 90 Hz = 11.1 ms frame budget:

| measurement | per eye | both eyes | % of frame |
|---|---|---|---|
| local `OPTIMAL→OPTIMAL` copy on NVIDIA (baseline) | 1.614 ms | — | — |
| local `OPTIMAL→OPTIMAL` copy on AMD (baseline) | 0.488 ms | — | — |
| **compositor reads its own OPTIMAL swapchain image, AMD** | 0.444 ms | 0.9 ms | 8% |
| **compositor reads its own LINEAR swapchain image, AMD** | **0.489 ms** | 1.0 ms | 9% |
| compositor reads own OPTIMAL, NVIDIA | 1.599 ms | 3.2 ms | 29% |
| compositor reads own LINEAR, NVIDIA | 2.298 ms | 4.6 ms | 41% |

**The recommended topology (server/compositor = AMD, client/game = NVIDIA):**

| stage | per eye | both eyes | % of frame |
|---|---|---|---|
| **NVIDIA client writes into the AMD-allocated shared image** | 1.431 ms | 2.9 ms | 26% |
| **AMD compositor reads the shared image** | 0.492 ms | 1.0 ms | 9% |
| | | **3.9 ms** | **35%** |

The reverse topology (server = NVIDIA, client = AMD) is worse where it hurts — the compositor's
read is the per-frame-critical one:

| stage | per eye | both eyes | % of frame |
|---|---|---|---|
| AMD client writes into the NVIDIA-allocated shared image | 0.542 ms | 1.1 ms | 10% |
| NVIDIA compositor reads the shared image | 2.329 ms | 4.7 ms | 42% |

**Two things to take from this table:**

1. **Forcing LINEAR costs the AMD compositor almost nothing — +0.045 ms/eye, +10%.** This is the
   number that makes Shape A viable, and it is a pleasant surprise given how much pain LINEAR caused
   in the `4460d2c8a` era (that pain was import *failure*, not throughput).
2. **The recommended topology is also the cheaper one**, which happily aligns with the user's
   existing AMD-only pin and with keeping the latency-critical composite on the GPU that owns the
   buffer.

> **Caveat — do not over-trust the NVIDIA absolute numbers.** The benchmark inserts a full pipeline
> barrier between iterations, so it measures *serialized per-copy latency including pipeline drain*,
> not streaming bandwidth. The NVIDIA local copy measuring 22.6 GB/s effective on a card capable of
> several hundred is the tell; the dGPU may also have been clock- or power-limited, and another
> session was using it during the run. Two consequences: (a) the NVIDIA-side absolutes are a
> conservative **upper bound**, and (b) the anomaly where the remote write (1.431 ms) came out
> *faster* than the local copy (1.614 ms) is a measurement artifact, not a real result. The AMD-side
> ratios (0.444 vs 0.489) are internally consistent and are the ones to trust. **WP-XG0 should
> re-measure with timestamp queries and a pipelined submission before anyone sizes a frame budget on
> these figures.**

### 4.7 Where the shared buffer physically lives

Worth recording, because it drives the PCIe story: when **AMD** exports, the allocation lands in
AMD's `DEVICE_LOCAL` heap — which on an iGPU is carved system RAM — and NVIDIA imports it as a
sysmem-heap allocation. When **NVIDIA** exports, the allocation lands in NVIDIA's **sysmem** heap
(`heap[1]`, no `DEVICE_LOCAL`), not VRAM. **Neither driver ever placed a shareable dma-buf in dGPU
VRAM**, which is expected: cross-vendor P2P over PCIe would require PCI P2PDMA and is not in play
here. So in every working configuration the shared surface is in system memory, and the dGPU's
access to it is a PCIe transfer. That is the true source of the 1.43 ms/eye write cost.

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
measured in §4.6 — 0.045 ms/eye. Acceptable. If it ever isn't, per-client policy is the escape
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

See §6 for the spec-language confirmation from the upstream research pass.

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

*(Pending the upstream research pass; see §9 Q1. The structural findings above do not depend on it.)*

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
  through to the probes. 1 task, and it is a genuine upstream bug worth a PR.**
- **Risk:** low. **Reversible:** entirely, by restoring two config files.

### Shape A — full split: client on NVIDIA, compositor + encode on AMD **(the recommendation)**

**11 agent-tasks (§8).** Import boundary is AMD-exports → NVIDIA-imports, the permissive direction
(§4.3). LINEAR swapchains, `sync_fd` sync, ~3.9 ms/frame of an 11.1 ms budget.

- **What it buys:** exactly the user's stated future — HypXRland desktop and encode stay on the
  efficient iGPU where they already work, and only the game's rendering is on the dGPU. The dGPU can
  be idle when no game is running. It also fixes the heterogeneous multi-client case (§5.1) as a
  side effect, because the buffers become universally importable.
- **What it costs:** ~3.9 ms/frame (to be re-measured, §4.6 caveat), +10% on the AMD compositor's
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

### Shape D — anything upstream suggests

*(Pending §6.)*

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
| **WP-XG-B1** | **Shape B unblock.** Pass `.device` through `prober::check_vaapi` and the 10-bit probe (`W/server/encoder/encoder_settings.cpp:136-146`, `:362-372`) so vaapi capability is probed on the *configured* encode device, not the compositor's. Then flip the live config (drop the `VK_DRIVER_FILES` pin, `openxr:gpu = renderD128`, keep `device: renderD129`) and confirm the game runs. **Headset-in-the-loop; needs the user.** Upstream this as a WiVRn PR. | 1 | **B** |
| **WP-XG0** | **Re-measure properly, and prove the no-regression baseline.** Rework the PoC benchmark to use timestamp queries and pipelined submission (kill the per-iteration barrier, §4.6 caveat) and re-run; add a `VK_IMAGE_USAGE_COLOR_ATTACHMENT` *render-into-imported-LINEAR* case, which the current PoC does not cover and which is unknown (i) in §7. Separately: build WiVRn 26.6.2 unmodified and confirm a same-GPU session is healthy, as the regression baseline every later WP is judged against. | 1 | A |
| **WP-XG1** | **Monado patch 0009 — dma-buf export with explicit modifiers.** `xrt_swapchain_create_info` gains a modifier/cross-GPU field (`xrt_compositor.h:894-912`). `vk_image_allocator.c` `:63`/`:256` become `DMA_BUF` + `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` with a LINEAR modifier list when the flag is set, keeping the existing `OPAQUE_FD`/`OPTIMAL` path byte-identical when it is not. Plumb `vkGetImageDrmFormatModifierPropertiesEXT` + per-plane `vkGetImageSubresourceLayout` so the layouts can travel. | 2 | A |
| **WP-XG2** | **Monado patch 0010 — client import via dma-buf.** `vk_create_image_from_native` (`vk_helpers.c:1115,1172,1189-1196`): `DMA_BUF` handle type, explicit-modifier image create, and **memory type from `vkGetMemoryFdPropertiesKHR`** — closing the tree's own TODO. Model it on the working import at `W/server/encoder/ffmpeg/video_encoder_va.cpp:363-467`. Carry the plane layouts over IPC (new fields in the swapchain-create reply). Also relax the `requirements.size` abort at `:1253-1259` for the cross-device case. | 2 | A |
| **WP-XG3** | **Monado patch 0011 — force the `SYNC_FD` fence path when cross-GPU.** Suppress the `OPAQUE_FD` timeline-semaphore negotiation (`comp_vk_client.c:149-201`, gate at `:891-893`) so `submit_fence` (`:203-237`) is chosen, and make `setup_semaphore` failure **degrade** instead of `goto err_pool`. §4.5 says the fence path is correct cross-vendor; this WP is what makes it reachable. | 1 | A |
| **WP-XG4** | **Monado patch 0012 — let the client legally use a different device.** Relax the two identity checks (`oxr_session.c:1285-1292`, `oxr_api_system.c:448-452`) when cross-GPU mode is on, and make the silent `phys[0]` fallback at `oxr_vulkan.c:647-650` **loud** (it currently converts a config error into an inscrutable `vkAllocateMemory` failure). Precedent for this shape of change: existing patch `0007-don-t-verify-GL-stuff.patch`. | 1 | A |
| **WP-XG5** | **WiVRn — split the two UUIDs.** `W/server/compositor/compositor.cpp:797-798` stops copying one UUID into both; add a config key (e.g. `"client-gpu"`) resolved to a `VkPhysicalDevice`/UUID, defaulting to the compositor's device so existing setups are bit-identical. Wire it to the WP-XG1 swapchain flag. | 1 | A |
| **WP-XG6** | **Format-list intersection.** `W/server/compositor/compositor.cpp:739-744` / `M/.../comp_vulkan.c:382-428` / `vk_compositor_flags.c:344+` currently evaluate formats against the server GPU with `OPTIMAL` tiling and `OPAQUE_FD`. Advertise the intersection of both devices, evaluated for `DRM_FORMAT_MODIFIER` tiling + `DMA_BUF`. §4.2 says all four common formats survive, so this should narrow nothing today — it is correctness insurance. | 1 | A |
| **WP-XG7** | **Queue-family ownership across the boundary.** Add the missing `VK_QUEUE_FAMILY_FOREIGN_EXT` acquire in `W/server/compositor/layer_squasher.cpp:443-445` and match the client's release (`comp_vk_client.c:748-757`, currently `VK_QUEUE_FAMILY_EXTERNAL` and unmatched). Neither tree uses `FOREIGN_EXT` today; the PoC does and it works on both drivers. | 1 | A |
| **WP-XG8** | **Live bring-up.** xrizer + The Big Walk with the game on NVIDIA and the compositor on AMD; confirm no `assert_eq!(pd, info.physical_device)`, measure real frame times against WP-XG0's baseline, and check the HypXRland desktop client still composites correctly in the same session (the §5.1 heterogeneous case). **Headset-in-the-loop; needs the user.** | 1 | A |

**Shape B (the unblock): WP-XG-B1 — 1 agent-task**, and it may be zero code if the probe happens to
succeed anyway.
**Shape A (the full split): WP-XG0, XG1, XG2, XG3, XG4, XG5, XG6, XG7, XG8 — 11 agent-tasks.**

**Ordering:** XG-B1 → XG0 → XG1 → XG2 → XG3 → (XG4 ∥ XG5) → XG6 → XG7 → XG8. XG1+XG2 are the
irreducible core; if they land and a same-GPU session still passes XG0's baseline, the rest is
mechanical.

**Not an agent task:** deciding whether to run the dGPU at all (§9 Q2), and any change to the live
`wivrn.service` — the user does that.

---

## 9. Open questions for the user

1. **Upstream reconnaissance is still outstanding.** The web research pass covering Monado GitLab
   (existing MRs/issues on client GPU selection), WiVRn GitHub (maintainer position on multi-GPU /
   PRIME), the exact OpenXR spec language on `xrGetVulkanGraphicsDevice2KHR`, and the xrizer assert's
   permalink had not returned when this report was written. **§6 and Shape D are placeholders.** If
   an upstream MR already does WP-XG1/XG2, the workplan could shrink by several tasks — worth
   waiting for before starting XG1. Do you want that filled in as an erratum, or should the report
   be held until it lands?
2. **Do you want Shape B applied now, before Shape A is built?** It unblocks The Big Walk this week
   at the cost of ending the AMD-only evaluation and moving the compositor to the dGPU. Given
   research/25's finding that the dGPU never actually sleeps today, the power argument for AMD-only
   is already weaker than when it was adopted — but that is your call, not mine, and it interacts
   with the Strix-Point-no-dGPU candidacy question.
3. **Is ~3.9 ms/frame of an 11.1 ms budget acceptable** for the cross-GPU game path, before
   optimisation? That is the honest current estimate (with §4.6's caveat that the NVIDIA half is a
   conservative upper bound). If your bar is "no measurable difference from a native dGPU session",
   Shape A may not clear it and Shape B is the answer permanently.
4. **How much carried-patch burden are you willing to take?** Shape A adds four Monado patches to
   `W/patches/monado/`, rebased at every WiVRn bump. Upstreaming them (they fix Monado's own TODO
   and complete a feature it half-shipped) would remove that burden but on upstream's timeline, not
   yours. Do you want the patches written upstream-first (cleaner, slower) or fork-first (faster,
   carried)?
5. **Should the AMD desktop client also pay the LINEAR cost** (+0.045 ms/eye) so one global
   suggestion serves both clients (§5.1), or do you want per-client device policy built from the
   start? I recommend the former — it is much smaller and the escape hatch stays open.
