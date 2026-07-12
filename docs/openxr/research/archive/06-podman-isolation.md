# 06 — Running the HypXRland dev compositor inside a Podman container (GPU passthrough + session isolation)

Research report. **No implementation, no live runs.** Written to let the user decide whether to
green-light an implementation agent. Host context: Arch Linux (Omarchy), rootless podman preferred,
user `ajg` uid 1000, dual GPU — `renderD128` = NVIDIA RTX 5070 Max-Q, `renderD129` = AMD Strix iGPU,
kernel 7.0.10-arch1-1.

---

## TL;DR

- **Yes — a container is the right tool, and it solves the input problem cleanly:** a containerized
  compositor with **no `/dev/input` mounted physically cannot grab host devices**, so the TTY-conflict
  goes away by construction. XR controller input arrives over the OpenXR IPC socket, not evdev.
- **Two recommended topologies:** (a) *interactive dev + headset* = container runs the nested Hyprland as
  a **Wayland client of the host** and points `XR_RUNTIME_JSON` at the **host's** wivrn-server socket
  (`$XDG_RUNTIME_DIR/wivrn/comp_ipc`); (b) *hermetic `hyprtester --xr`* = container bundles
  vendored `monado-service` (null compositor) + Hyprland + harness, GPU injected via CDI.
- **GPU:** NVIDIA via `nvidia-container-toolkit` CDI (`--device nvidia.com/gpu=all`); AMD via a plain
  `--device /dev/dri/renderD129 --group-add keep-groups`. On *this* box render nodes are `0666`, so no
  group juggling is even needed for the render node itself.
- **Effort M**, main risks are NVIDIA driver-version drift (host↔image) and the headless-backend-needs-a-seat
  quirk (forces the nested-Wayland fallback, which re-introduces a host dependency).

---

## Topology comparison

| # | Topology | Where XR runtime lives | Host deps mounted in | Input isolation | Desktop visible? | Effort | Verdict |
|---|---|---|---|---|---|---|---|
| **a** | Nested Hyprland = Wayland **client** of host; XR → host WiVRn | host wivrn-server | host `$WAYLAND_DISPLAY` socket + `wivrn/comp_ipc` socket + GPU | **Full** (no `/dev/input`; kbd/mouse arrive via host wayland only when the nested window is focused) | Yes (a floating window on host) | **M** | ✅ Best for interactive dev with headset |
| **a′** | Same, but XR → vendored **Monado windowed** (no headset) | container Monado | host `$WAYLAND_DISPLAY` + GPU | Full | Yes (nested + Monado window) | M | ✅ Containerized `preview-xr.sh` (no headset) |
| **b** | Compositor **headless-only** + XR streams to Quest via WiVRn | host **or** container WiVRn | GPU (+ WiVRn socket if host-side; +host network if container-side) | Full | No (headset only) | M–L | ✅ Best for hermetic-ish headset runs |
| **b-test** | Headless compositor + vendored Monado **null** + harness, all in container | container Monado | GPU only (+ host `$WAYLAND_DISPLAY` for the nested fallback) | Full | No | **M–L** | ✅ Best for `hyprtester --xr` |
| **c** | Container owns a VT / DRM lease / dedicated seat | any | `/dev/input`, `/dev/dri/card*`, DRM-master, logind seat, `--privileged` | **None** (re-introduces the exact host-input fight) | Yes (real TTY) | **XL** | ❌ Reject for podman — not a supported workflow |

---

## Per-question findings

### 1. GPU access from a container

**AMD / Mesa (renderD129) — trivial.** Pass the render node and keep the user's supplementary groups;
put Mesa in the image; the kernel `amdgpu` driver stays on the host. Rootless recipe is
`--device /dev/dri/renderD129 --group-add keep-groups`
([Podman GPU docs](https://podman-desktop.io/docs/podman/gpu),
[oneuptime AMD](https://oneuptime.com/blog/post/2026-03-18-run-amd-gpu-containers-podman/view)).
`--group-add keep-groups` (crun-only) preserves the host `render`/`video` gids inside the userns so the
node's group perms still resolve. **On this box it isn't even required for the render node**: `ls -l /dev/dri`
shows `renderD128`/`renderD129` are `crw-rw-rw-` (mode 0666, world-rw), only `card1/card2` are `0660 root:video`.
Mesa userspace shipped in the image against a host `amdgpu` kernel is version-tolerant (the UAPI is stable);
this is the low-risk GPU path.

**NVIDIA (renderD128) — use CDI, don't hand-roll device nodes.** Install `nvidia-container-toolkit`, generate a
CDI spec (`nvidia-ctk cdi generate`, auto-refreshed by the `nvidia-cdi-refresh` systemd unit since toolkit
v1.18), then `podman run --device nvidia.com/gpu=all`. CDI injects: the device nodes (`/dev/nvidia0`,
`/dev/nvidiactl`, `/dev/nvidia-uvm`, `/dev/nvidia-caps/*`), the **host userspace driver libraries**
(`libcuda.so.<ver>`, `libnvidia-ml.so.1`, `libGLX_nvidia`, `libEGL_nvidia`, `libnvidia-glvkspirv`, …),
firmware, and the **EGL/Vulkan ICD manifests** (`/usr/share/glvnd/egl_vendor.d/10_nvidia.json`,
`/usr/share/vulkan/icd.d/nvidia_icd.json`) plus a `ldconfig` hook
([NVIDIA CDI support](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/cdi-support.html),
[DeepWiki CDI](https://deepwiki.com/NVIDIA/nvidia-container-toolkit/3.2-container-device-interface-(cdi))).
Because our XR path is **EGL + Vulkan graphics, not CUDA**, the spec must include the graphics/display
capabilities — with the classic runtime this was `NVIDIA_DRIVER_CAPABILITIES=graphics,display,compute,utility`;
CDI bakes the graphics libs into the generated spec, but **verify the ICDs actually landed** (a CUDA-only spec
will `eglInitialize`/`vkCreateInstance`-fail).

> **Pitfall — driver-version lockstep.** The userspace driver is *bind-mounted from the host*, so the image
> must **not** ship its own NVIDIA userspace driver (it would shadow the host's and mismatch the host kernel
> module). The toolkit requires `libnvidia-container*` to exactly match the `nvidia-container-toolkit*` package
> version, and a background host driver bump (e.g. 580.65→580.95) can leave a stale CDI spec that fails to
> regenerate
> ([CDI-generate mismatch #82](https://github.com/NVIDIA/nvidia-container-toolkit/issues/82),
> [toolkit troubleshooting](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/troubleshooting.html)).
> Practical rule: keep the image driver-agnostic and **re-run `nvidia-ctk cdi generate` after every host driver
> update** (or rely on `nvidia-cdi-refresh`). The RTX 5070 needs a very recent driver; make sure the toolkit in
> use is new enough to enumerate a Blackwell node.

**EGL + GBM headless (our render path).** `src/openxr/` renders with EGL on a **GBM device opened from a render
node** — no KMS, no DRM-master, no seat needed for *rendering*. So a bare `--device /dev/dri/renderD12x` (AMD)
or CDI-injected nvidia node (NVIDIA) is sufficient for the compositor's XR blits; this is the same headless
EGL/GBM contract the compositor already satisfies today under nested launch.

**Single-GPU containers dodge the cross-GPU crash.** MEMORY (`07-testing.md §7`) documents that Monado's null
compositor picks NVIDIA independently, and a cross-GPU dmabuf import crashes at `xrCreateSwapchain`. A container
is a **GPU-visibility control**: expose *only* `renderD128` (NVIDIA) via CDI **and** pin `openxr:gpu` to it, or
expose *only* `renderD129` (AMD) and let Monado pick AMD — either way the whole session is single-GPU and the
documented cross-GPU crash cannot occur. This is a genuine reliability win of the container approach.

### 2. Display/session topologies

**(a) Nested Hyprland as a Wayland client of the host.** Mount the host Wayland socket read-only and set
`WAYLAND_DISPLAY`; `XDG_RUNTIME_DIR` must be a dir the container-user (uid 1000 via `keep-id`) owns, because the
socket is owner-only
([x11docker wiki](https://github.com/mviereck/x11docker/wiki/How-to-provide-Wayland-socket-to-docker-container),
[oneuptime Wayland-in-podman](https://oneuptime.com/blog/post/2026-03-18-run-wayland-applications-podman-containers/view)).
*Isolation retained:* filesystem, process tree, package/dep set, and — crucially — **input**: the nested
compositor sees no evdev devices, so keyboard/mouse reach it **only** through the host compositor's normal
window-focus routing (i.e. when its window is focused on the host). *Isolation lost:* it depends on the host
compositor being up, and shares the host GPU. **This is exactly the isolation the user wanted** — the TTY
approach failed because two real compositors fought over `/dev/input`; here only the host owns input and hands
it in through Wayland. This is the containerized form of today's `preview-xr.sh` (`--wivrn` or windowed-Monado).

**(b) Headless-only + WiVRn stream to the Quest 3.** `HYPRLAND_HEADLESS_ONLY=1`, no desktop window, monitors
composite straight into the XR quad layers presented to the headset. Two sub-choices for where `wivrn-server`
lives:

- **wivrn-server on the host (recommended).** It's already installed and set up on this box (native WiVRn,
  server v26.6.1, `/usr/bin/wivrn-server`, manifest `/usr/share/openxr/1/openxr_wivrn.json` = active runtime).
  Leave it on the host so it owns the network path to the Quest, avahi, and pairing. The containerized compositor
  becomes a **plain OpenXR client** of it: mount the host socket `$XDG_RUNTIME_DIR/wivrn/comp_ipc` into the
  container's runtime dir, mount the WiVRn runtime `.so` + manifest, and set
  `XR_RUNTIME_JSON=/…/openxr_wivrn.json`
  ([WiVRn socket path, LVRA wiki](https://wiki.vronlinux.org/docs/fossvr/wivrn/);
  the Steam-in-pressure-vessel recipe mounts exactly this socket:
  `PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/wivrn/comp_ipc` —
  [WiVRn issue #872](https://github.com/WiVRn/WiVRn/issues/872)). **No headset ports cross the container
  boundary** — only a unix socket does. Cleanest isolation for the compositor, headset plumbing stays native.
- **wivrn-server in the container.** Then the container needs the network to the headset:
  **9757/TCP+UDP** (WiVRn stream) and **5353/UDP** (avahi/mDNS discovery)
  ([WiVRn wiki, ports](https://wiki.vronlinux.org/docs/fossvr/wivrn/)). mDNS across a podman NAT bridge is
  painful (avahi wants L2), so this sub-choice effectively needs `--network=host`, which erodes network isolation
  and can collide with a host wivrn-server already bound to 9757. **Only do this if the host has no WiVRn** —
  here it does, so prefer the host-server sub-choice above.

**(c) Dedicated VT / DRM lease / full seat.** Not sensible under podman. It would require `--privileged`,
`/dev/input/*`, `/dev/dri/card*` with DRM-master, and a logind seat inside the container — which re-introduces the
exact `/dev/input` contention the user is trying to escape, and podman has no seat/DRM-lease brokering. If a real
dedicated seat is ever wanted, `systemd-nspawn --machine` with its own seat is the more honest tool, but it's
heavyweight and still fights the host over hardware. **Reject.**

### 3. OpenXR runtime plumbing in-container

- **Loader → runtime.** The OpenXR loader reads `XR_RUNTIME_JSON` (or `…/openxr/1/active_runtime.json`); the
  manifest names a `.so` the app dlopens in-process, which then connects to the compositor's unix IPC socket.
  Both our runtimes are in-process client libs: **Monado** connects to `$XDG_RUNTIME_DIR/monado_comp_ipc`
  (hard-coded in the orchestrator's readiness poll, `MonadoOrchestrator.cpp:94`), **WiVRn** to
  `$XDG_RUNTIME_DIR/wivrn/comp_ipc`. So "plumb the runtime in" = **mount the manifest, the runtime `.so`, and the
  IPC socket, then set `XR_RUNTIME_JSON`.** For a *hermetic* test the vendored `monado-service` runs *inside* the
  container and the socket never crosses the boundary at all — set `XR_RUNTIME_JSON` to
  `<monado-build>/openxr_monado-dev.json` exactly as the harness does today.
- **Socket path must line up.** Both runtimes derive the socket from `XDG_RUNTIME_DIR`. If a host socket is
  bind-mounted, the container's `XDG_RUNTIME_DIR` must be arranged so the runtime looks in the right place —
  simplest is to keep `XDG_RUNTIME_DIR=/run/user/1000` inside the container and bind-mount just the specific
  sockets (not the whole dir) to their canonical names.
- **uid / namespace tolerance.** These are **pathname unix sockets** (not abstract), so they're plain filesystem
  objects; access is governed by the socket's owner/mode. Under `XDG_RUNTIME_DIR` that's `0700`/owner-1000, so the
  container process must be uid 1000. Rootless podman with **`--userns=keep-id`** maps host-1000 → container-1000
  (instead of the default container-root → host-1000), which is exactly what's needed for the owner check to pass
  ([Red Hat: rootless userns modes](https://www.redhat.com/en/blog/rootless-podman-user-namespace-modes),
  [Podman keep-id](https://oneuptime.com/blog/post/2026-03-17-use-userns-keep-id-option-podman/view)). The Monado
  IPC protocol itself carries no uid check beyond the socket perms, so keep-id is sufficient; there is no abstract-
  namespace or peer-cred gate to trip.

### 4. Input isolation (the original motivation)

Confirmed for topologies (a) and (b): **do not mount `/dev/input`.** libinput then enumerates zero devices, and the
containerized Hyprland cannot open/grab any host keyboard, mouse, or tablet — the failure mode from the separate-TTY
attempt (host compositor still holding `/dev/input`, or both fighting for it) is structurally impossible. Sources of
input in each mode:

- **(a) nested-Wayland:** keyboard/pointer arrive as Wayland events from the host compositor, only while the nested
  window has host focus — the host stays in control, which is the desired behavior.
- **(b) headless + XR:** the *only* input is XR controllers/hands, delivered by the OpenXR runtime over the IPC
  socket (`xrSyncActions`), never via evdev. (For the null-compositor test path, "controller input" is the scripted
  remote-driver stream on TCP 4242, again not evdev.)

If you ever *want* one physical keyboard inside the container (e.g. headless dev without a nested window), you'd mount
exactly one `/dev/input/eventN` — but that's an opt-in, and even then Hyprland/libinput grabbing it would pull it away
from the host, so prefer the nested-Wayland window for text entry.

### 5. Audio, dbus, portals

Brief — none are needed for headless XR, mount only what you want:

- **Audio:** mount `$XDG_RUNTIME_DIR/pipewire-0` (+ `pipewire-0.lock`, and `pulse/native` for the Pulse shim) and,
  if the app uses it, the session-bus socket. WiVRn streams audio to the headset separately; for compositor XR
  mirroring, audio is usually irrelevant.
- **D-Bus:** the session bus `$XDG_RUNTIME_DIR/bus` — needed if the compositor talks to logind/portals; a headless XR
  compositor mostly doesn't. Omit for max isolation; things like idle-inhibit via logind will simply no-op.
- **Portals:** `xdg-desktop-portal` needs the session bus + the portal running; screencast/file-chooser break without
  it. Not required for the XR mirror path. Skip unless a specific dev workflow needs it.

### 6. Image strategy

- **Base:** `archlinux:latest` (Omarchy/host is Arch — same libc/toolchain family), install Hyprland's build deps
  (aquamarine, hyprutils, hyprlang, hyprgraphics, wayland, wayland-protocols, mesa, vulkan-icd-loader, libinput,
  pixman, cairo, pango, libdrm, egl-wayland, plus `openxr` for `HAVE_OPENXR`, and `git cmake ninja gcc` for building).
- **Repo/build:** bind-mount the repo (`-v /home/ajg/code/Hyprland:/src`) and keep `build-debug/` on the host, OR use
  a container-local build dir. **Running the *existing* host-built `build-debug/Hyprland` inside the container works
  only if the container's package versions match the host's** — Arch is rolling, so an `archlinux:latest` pulled a
  week later can drift (glibc, mesa, hyprutils/aquamarine ABIs are the usual suspects) and the binary will fail to
  load or crash. Two safe options: (1) **build inside the container** against its own libs (most robust, ccache makes
  it cheap); (2) **pin the image** to a dated Arch snapshot (e.g. an archive mirror) matching the host and accept the
  reused binary — brittle, needs re-pinning on every host update. Recommend option (1) for the test container and for
  first-time setup; the incremental cost is small.
- **ccache:** mount a shared cache `-v ~/.cache/ccache:/root/.cache/ccache` (or a named volume) and set
  `CMAKE_CXX_COMPILER_LAUNCHER=ccache` so host and container builds share objects.
- **Vendored deps:** `subprojects/monado` (+ eigen, vulkan-headers) are already vendored and build via
  `scripts/build-monado.sh` / `cmake --target monado` — they build fine inside the container too (GCC 16 needs the
  `-include cstdint` shim the script already handles).

### 7. Concrete recommendation

**(i) Interactive dev with the Quest 3 → topology (a) nested-Wayland client + host WiVRn runtime.** This is a
containerized `preview-xr.sh --wivrn`: nested Hyprland shows a flat window on the host (for keyboard/mouse), streams
the XR quads to the headset via the host's wivrn-server, and cannot touch host input. Sketch:

```sh
podman run --rm -it \
  --userns=keep-id \                                  # map host uid 1000 -> container 1000, so it owns the
                                                      #   XDG_RUNTIME_DIR sockets it mounts (Wayland + WiVRn)
  --device nvidia.com/gpu=all \                       # CDI: inject NVIDIA nodes + userspace driver + EGL/Vulkan ICDs
                                                      #   (RTX 5070 = renderD128; matches WiVRn's encode GPU)
  --security-opt label=disable \                      # let the container read the bind-mounted host sockets (SELinux)
  -e XDG_RUNTIME_DIR=/run/user/1000 \                 # keep the canonical path so both runtimes find their sockets
  -e WAYLAND_DISPLAY=wayland-1 \                      # host session's display (nested-client backend)
  -e XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_wivrn.json \  # point the OpenXR loader at host WiVRn
  -e HYPRLAND_BIN=/src/build-debug/Hyprland \
  -v /run/user/1000/wayland-1:/run/user/1000/wayland-1 \        # host compositor socket (input arrives ONLY here)
  -v /run/user/1000/wayland-1.lock:/run/user/1000/wayland-1.lock \
  -v /run/user/1000/wivrn:/run/user/1000/wivrn \                # host wivrn-server IPC dir (comp_ipc socket)
  -v /usr/share/openxr/1/openxr_wivrn.json:/usr/share/openxr/1/openxr_wivrn.json:ro \
  -v /usr/lib/libopenxr_wivrn.so:/usr/lib/libopenxr_wivrn.so:ro \   # the runtime .so the manifest names (verify name)
  -v /home/ajg/code/Hyprland:/src \                    # repo (build in-image or reuse if versions match, §6)
  hypxrland-dev:latest \
  /src/scripts/preview-xr.sh --wivrn
# NOTE: no /dev/input mount anywhere -> host keeps sole ownership of keyboard/mouse.
# Precondition: wivrn-server running on the host with the headset connected (same as native --wivrn today).
```

**(ii) Hermetic `hyprtester --xr` → topology (b-test): Monado null + Hyprland + harness all in-container.** Sketch:

```sh
podman run --rm \
  --userns=keep-id \
  --device nvidia.com/gpu=all \                       # or, to force single-GPU AMD: --device /dev/dri/renderD129
  --security-opt label=disable \
  -e XDG_RUNTIME_DIR=/run/user/1000 \                 # harness makes its own isolated /tmp/hyprtester-xr-<pid>, but
                                                      #   the nested fallback needs a real runtime dir (§ below)
  -e HYPRTESTER_XR_GPU=/dev/dri/renderD128 \          # pin openxr:gpu to Monado's GPU (avoid cross-GPU crash, §1)
  -e WAYLAND_DISPLAY=wayland-1 \                       # ONLY needed for the headless-backend fallback (see risk)
  -v /run/user/1000/wayland-1:/run/user/1000/wayland-1 \       # ditto — remove if the headless backend works in-container
  -v /run/user/1000/wayland-1.lock:/run/user/1000/wayland-1.lock \
  -v /home/ajg/code/Hyprland:/src \
  -v hypxrland-ccache:/root/.cache/ccache \
  hypxrland-dev:latest \
  bash -lc 'cd /src && cmake --build build-debug --target Hyprland hyprtester monado &&
            cd hyprtester && ../build-debug/hyprtester/hyprtester --xr --binary ../build-debug/Hyprland'
```

Monado, Hyprland, and the harness are all inside; only the GPU (and, for now, the host Wayland socket for the
fallback) cross the boundary. Expose only `renderD128` to make the whole run single-GPU and sidestep the documented
`xrCreateSwapchain` cross-GPU crash entirely.

**Overall effort: M.** A `scripts/xr-container.sh` wrapper + a `Containerfile` (Arch base + deps) is the whole
deliverable; the compositor code needs no changes. Budget more if the headless-backend-needs-a-seat issue (below)
forces work.

### Risks

1. **Headless backend needs a seat → nested-Wayland fallback re-adds a host dep.** MEMORY (`06-testing.md §2.2`,
   WP10 notes) records that `HYPRLAND_HEADLESS_ONLY=1` fails to bring up Aquamarine in a seatless sandbox and the
   harness falls back to nesting inside the host's Wayland session (symlinking `$WAYLAND_DISPLAY`). A container is
   likewise seatless (no logind seat), so the *hermetic* test container (ii) probably still needs the host Wayland
   socket mounted for that fallback — partial, not total, hermeticity. Verify whether Aquamarine's headless backend
   comes up in-container with just a GPU and no seat; if not, the mount stays.
2. **NVIDIA driver drift** (host↔image lockstep, §1) — image must stay driver-agnostic; regenerate CDI after host
   driver bumps; confirm the toolkit enumerates the Blackwell (5070) node.
3. **Reusing the host `build-debug` binary** across a rolling-Arch version gap (§6) — prefer building in-container.
4. **WiVRn runtime `.so` path/name** in the mount above is a guess — confirm the exact filename the
   `openxr_wivrn.json` manifest references before wiring the mount (topology b, host-server sub-choice).
5. **Only one wivrn-server per box** (binds 9757); a container running its own server collides with the host's —
   another reason to prefer the host-server sub-choice for headset runs.
6. **SELinux/label** — `--security-opt label=disable` (or `:z`/`:Z` on the binds) is needed for the container to read
   host-owned sockets; on Arch without SELinux enforcing this is usually a no-op but keep it for portability.

### Open questions (for the implementation agent to resolve empirically)

- Does Aquamarine's **headless backend start in-container with a GPU but no logind seat**, or is the host-Wayland
  nested fallback mandatory? (Determines how hermetic (ii) can be.)
- Exact **filename of the WiVRn OpenXR runtime `.so`** named by `/usr/share/openxr/1/openxr_wivrn.json`, and whether
  it dlopens any *other* host libs that must also be mounted.
- Does the current **`nvidia-container-toolkit` on this box enumerate the RTX 5070** and inject working EGL+Vulkan
  ICDs (not a CUDA-only spec)? A `vkinfo`/`eglinfo` inside the container is the smoke test.
- Is **`renderD129`-only (AMD) fast enough** for WiVRn encode / preview, to offer a fully single-GPU AMD container as
  the simplest reliable option (no CDI, no driver lockstep)?
- Confirm the **Monado null compositor picks the GPU that's actually visible** in the container (if only `renderD128`
  is exposed, it must pick NVIDIA) — expected, but verify against the cross-GPU-crash history.
- **Future work — vendor keywords in `openxr:gpu`.** The container tooling resolves `--gpu amd|nvidia|intel` by a
  vendor scan of `/sys/class/drm` (`scripts/lib/gpu.sh`); the compositor's `openxr:gpu` config still takes only a
  literal `/dev/dri/renderD*` path. Teaching `openxr:gpu` to accept the same vendor keywords (resolved with
  `drmGetDevices2` at config-parse time) would let a hyprland.conf express portable GPU selection directly, instead of
  the wrapper computing a box-specific node and injecting it. Not implemented in WP4.

---

## Sources

- NVIDIA CDI: [Support for Container Device Interface](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/cdi-support.html) · [CDI DeepWiki](https://deepwiki.com/NVIDIA/nvidia-container-toolkit/3.2-container-device-interface-(cdi)) · [Troubleshooting](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/latest/troubleshooting.html) · [CDI generate/version mismatch #82](https://github.com/NVIDIA/nvidia-container-toolkit/issues/82)
- Podman GPU / rootless: [Podman Desktop GPU](https://podman-desktop.io/docs/podman/gpu) · [oneuptime GPU passthrough](https://oneuptime.com/blog/post/2026-03-18-use-gpu-passthrough-podman/view) · [oneuptime AMD GPU](https://oneuptime.com/blog/post/2026-03-18-run-amd-gpu-containers-podman/view)
- Podman userns/keep-id: [Red Hat rootless userns modes](https://www.redhat.com/en/blog/rootless-podman-user-namespace-modes) · [oneuptime keep-id](https://oneuptime.com/blog/post/2026-03-17-use-userns-keep-id-option-podman/view)
- Wayland-in-container: [x11docker wiki](https://github.com/mviereck/x11docker/wiki/How-to-provide-Wayland-socket-to-docker-container) · [oneuptime Wayland in Podman](https://oneuptime.com/blog/post/2026-03-18-run-wayland-applications-podman-containers/view)
- WiVRn: [LVRA WiVRn wiki](https://wiki.vronlinux.org/docs/fossvr/wivrn/) · [WiVRn repo](https://github.com/WiVRn/WiVRn) · [comp_ipc socket mount, issue #872](https://github.com/WiVRn/WiVRn/issues/872) · [Arch LXC WiVRn/Monado gist](https://gist.github.com/BRUrban/185a1c8748be952fe2404fd35e535bfe)
- Local: `scripts/preview-xr.sh`, `hyprtester/src/xr/MonadoOrchestrator.cpp` (socket `monado_comp_ipc`, TCP 4242), `docs/openxr/06-testing.md` (§2.2 nested fallback, §7 dual-GPU), project MEMORY.
