# Machine Setup — bringing the whole HypXRland stack up on a new box

The compositor is one of eight moving parts. A HypXRland desk you can actually work in is:

| # | Component | What it is | Where it lives |
|---|---|---|---|
| 1 | **compositor** | this repo's `hypxrland` branch, built and launched as its own wayland session | `~/code/hypxrland` (a git *worktree*) |
| 2 | **WiVRn server** | the streaming OpenXR runtime the headset talks to, locally patched | `~/code/wivrn` + worktree `~/code/wivrn-<tag>` |
| 3 | **hypxrvoice** | voice control daemon (wake word → intent → `hyprctl`) | `~/code/hypxrvoice` |
| 4 | **hypxrhud** | the shared in-headset HUD daemon + battery gauges | `~/code/hypxrhud` |
| 5 | **hypxrva** | libva shim that frees the VCN for WiVRn's encoder while donned | `~/code/hypxrva` |
| 6 | **hypxrpaper** | ambient-background OpenXR client (overlay mode, overlay test) | `~/code/hypxrpaper` |
| 7 | **dotfiles** | `~/.config/hypr` on this machine's *device branch* | `AndrewGaspar/omarchy-hyprland-config` |
| 8 | **session env** | `~/.config/uwsm/env` — the PATH and libva wiring everything else assumes | `~/.config/uwsm/env` |

plus one optional ninth: the **XREAL** Monado flavor (`docs/openxr/07-xreal.md`), only for the Air 2
Ultra glasses rig.

`scripts/hypxr-setup.sh` installs, updates and — most usefully — **verifies** all of them:

```sh
scripts/hypxr-setup.sh --check          # audit everything, change nothing
scripts/hypxr-setup.sh --check wivrn    # audit one component
scripts/hypxr-setup.sh all              # install/update everything except monado
scripts/hypxr-setup.sh compositor voice # just these
scripts/hypxr-setup.sh monado           # the optional XREAL runtime, opt-in only
```

It never runs `sudo` and never resets a repo with uncommitted changes. Root steps are **printed**
for you at the end, and re-checked on the next run.

`scripts/fishfood.sh` is unchanged and still the right tool for the everyday "pull and rebuild the
compositor" loop. `hypxr-setup.sh` is the whole-stack bootstrap: it is the one that knows about the
other seven components, and it is the one to use when a checkout has fallen far behind.

---

## 1. Per-machine decision table

Everything below is auto-detected. Override any of it in `~/.config/hypxr/setup.env` (a plain
shell file, sourced if present) when a guess is wrong; the resolved values are printed under
**Machine profile** at the top of every run.

| Variable | What decides it | framework (Arch/Omarchy) | thinkpad-x1 (Fedora/omedora) |
|---|---|---|---|
| `HYPXR_DISTRO` | `/etc/os-release` | `arch` | `fedora` |
| GPU topology | render nodes + their kernel drivers | **hybrid**: `renderD128`=nvidia, `renderD129`=amdgpu | **single**: `renderD128`=i915/xe |
| `HYPXR_GPU_NODE` → `openxr:gpu` | the node the XR runtime's compositor renders on | `/dev/dri/renderD129` | `/dev/dri/renderD128` |
| `AQ_DRM_DEVICES` (in `hyprland-xr.conf`) | keeps aquamarine off the dGPU | `/dev/dri/card2` — **load-bearing** | not needed (harmless if set to the only card) |
| `HYPXR_VK_ICD` | Vulkan ICD to pin services to | `radeon_icd.json` | `intel_icd.json` — pin unnecessary, single ICD |
| `HYPXR_VAAPI_DRIVER` | kernel driver → VA driver | `radeonsi` | **`iHD`** |
| `HYPXR_LIBVA_DRIVERS_PATH` | `pkg-config --variable=driverdir libva` | **`/usr/lib/dri`** | **`/usr/lib64/dri`** |
| `HYPXR_WIVRN_ENCODER` | GPU media engine | `vaapi` + `device: /dev/dri/renderD129` | `vaapi` + `device: /dev/dri/renderD128` |
| hypxrhud `[hud] gpu` | must match the XR runtime's GPU | `/dev/dri/renderD129` | omit (auto-scan cannot be wrong) |
| hypxrhud `no-nvidia.conf` drop-ins | only meaningful with an NVIDIA card | **required** | **omit** |
| dotfiles device branch | one per machine, rebased on `master` | `framework` | `thinkpad-x1` |
| `HYPXR_WIVRN_TAG` | the upstream release the patches sit on | `26.6.2` | same |
| `HYPXR_HUD_PREFIX` | where hypxrhud installs | `/usr/local` (one sudo step) | `/usr/local`, or `$HOME/.local` to avoid root |
| XREAL / monado | only for the Air 2 Ultra glasses | in use | skip |

### The GPU policy is a *profile*, not a checklist

On the **hybrid** machine, a pile of masks exists for one reason: stopping gratuitous consumers from
opening `/dev/nvidia*` and holding the dGPU out of D3cold (≈5-7 W idle). Those are `AQ_DRM_DEVICES`,
the Chrome PATH shadow + `.desktop` override, the two hypxrhud `no-nvidia.conf` drop-ins, and the
`VK_DRIVER_FILES` line in the WiVRn drop-in.

On a **single-GPU Intel** machine **none of that applies**. There is one card, one ICD, one render
node. Set `openxr:gpu` to the only render node and delete the rest. `hypxr-setup.sh --check` makes
this distinction itself: it reports the masks as *required* on a hybrid box and as *unnecessary* on a
single-GPU box.

### The `/usr/lib/dri` vs `/usr/lib64/dri` trap

`~/.config/uwsm/env` points `LIBVA_DRIVERS_PATH` at hypxrva's shim-only directory. The WiVRn
service drop-in has to point it **back at the system directory**, because that is where the real
encoder driver lives — and that path is `/usr/lib/dri` on Arch and `/usr/lib64/dri` on Fedora.
Copying the drop-in verbatim between the two machines silently kills hardware encode. The setup
script resolves the directory from `pkg-config --variable=driverdir libva` and checks the drop-in
against it.

hypxrva's shim itself is fine either way: it searches both (`src/shim/resolve.c` carries
`/usr/lib/dri`, `/usr/lib64/dri`, and the multiarch variants), and it discovers the real driver from
the DRM fd's kernel driver name — `amdgpu → radeonsi`, `i915`/`xe` → `iHD`/`i965`, `nvidia → nvidia`.
So Intel needs no hypxrva configuration at all.

---

## 2. Fresh machine, start to finish

### 2.1 Packages

```sh
scripts/hypxr-setup.sh --check deps
```

prints exactly what is missing and the command for your distro.

#### The pre-flight probe

The first Fedora bring-up (ThinkPad X1 / fc44, 2026-08-03) cost **eight staggered failures**, six of
which were nothing but a missing `.pc` file discovered one `cmake` configure at a time: run a
component, read one error, install one package, run the next component, hit the next missing module.
CMake stops at the *first* unmet dependency, so a fresh box surfaces them strictly one per attempt.

The `deps` component therefore probes **every** pkg-config module the whole stack's CMakeLists ask
for — compositor, the shared XR client trio, WiVRn (including the Monado it `FetchContent`s), voice,
hud and va — in one pass, before any component runs, and emits **one** consolidated install command
for exactly what is absent. On a box that already has everything it collapses to a single line:

```
✓ pre-flight: all 61 build dependencies present (compositor, xr, wivrn, voice, hud, va)
```

The table lives in `DEP_PROBES` in `scripts/hypxr-setup.sh`; it is the only place that knows the
module → package mapping, and the place to extend when a component gains a dependency. Modules that
ship no `.pc` (Boost, Eigen3, glslang, `hyprwayland-scanner`, `gdbus-codegen`) are probed by header
or by program instead.

Two mappings in it are traps, both learned the hard way:

| Module | Fedora | Why it bites |
|---|---|---|
| `libeis-1.0` | **`libeis-devel`** | `libei-devel` is the obvious guess and is **wrong** — it ships only the *client* `libei-1.0.pc`. Installing it and re-running fails identically, which reads like the probe is broken. |
| `x264` | **`x264-devel`** | RPM Fusion **free only**; Fedora proper does not carry it at all, so `dnf install x264-devel` fails until the repo is enabled. Arch has it in the main repos, which is why the reference box never saw this. |

#### ffmpeg on Fedora: two answers, and the obvious one is the worse one

Fedora's free ffmpeg is **split per library** (`libavcodec-free-devel`, `libavutil-free-devel`, …).
Both routes below satisfy the *build*; only one of them gives you a working *stream*.

**(a) Recommended — RPM Fusion's full ffmpeg.** WiVRn's VAAPI path needs the `h264_vaapi` /
`hevc_vaapi` encoders **at runtime**, and `ffmpeg-free` is built without them. `x264-devel` exists
only here. And the freeworld `intel-media-driver` you need for Intel ENCODE entrypoints comes from
the same repo. One repo enable solves all three:

```sh
sudo dnf install -y \
  "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm"
sudo dnf swap ffmpeg-free ffmpeg --allowerasing
sudo dnf install -y ffmpeg-devel
```

**(b) Minimal — headers only, from the free repo:**

```sh
sudo dnf install -y libavcodec-free-devel libavutil-free-devel \
                    libavfilter-free-devel libswscale-free-devel
```

with the caveat that this satisfies the build and hardware encode can still be **dead at runtime**.
Either way, verify before donning:

```sh
vainfo | grep -i enc      # hw ENCODE entrypoints must be listed
```

**Arch / Omarchy.** No `-devel` split, so the runtime packages carry the headers:

```sh
sudo pacman -S --needed base-devel git cmake ninja ccache openxr vulkan-headers \
                        vulkan-icd-loader libva-utils inotify-tools jq
sudo pacman -S --needed hyprland      # pulls aquamarine, hyprlang, hyprutils, hyprcursor,
                                      # hyprgraphics, hyprwayland-scanner, glaze, ...
sudo pacman -S --needed wivrn-server  # the base user unit + the packaged OpenXR manifest
```

**Fedora / omedora.** omedora already enables COPR `agaspar/omedora-4`, which ships the whole
`hypr*` stack as RPMs with a real `hyprland.spec` — so `dnf builddep` resolves the compositor's
dependency set in one command:

```sh
sudo dnf copr enable -y agaspar/omedora-4     # already on if the box runs omedora
sudo dnf builddep -y hyprland
sudo dnf install -y git cmake ninja-build ccache gcc-c++ openxr-devel vulkan-headers \
                    vulkan-loader-devel libva-devel libva-utils inotify-tools jq
```

`dnf builddep hyprland` does **not** cover the six non-compositor components, and on fc44 it missed
`libeis-devel` even for the compositor. The pre-flight probe above is the authority, not the spec.

> **VERIFIED on Fedora 44** (thinkpad-x1, 2026-08-03) — these names are correct, installed cleanly,
> and satisfied the build they were needed for: `boost-devel` `pipewire-devel` `jansson-devel`
> `libva-devel` `libsndfile-devel` `libeis-devel`.

> **STILL UNVERIFIED** — check these on the Fedora machine:
> - The exact name of the OpenXR loader devel package. That box already had a loader, so
>   `openxr-devel` above has never actually been installed. Verify:
>   `dnf provides '*/pkgconfig/openxr.pc'`, then confirm with `pkg-config --modversion openxr`.
> - **WiVRn is not packaged for Fedora at all.** There is no `wivrn-server` RPM, so there is no base
>   `wivrn.service` for the drop-in to sit on and no `/usr/share/openxr/1/openxr_wivrn.json`. The
>   `wivrn` component builds the server from source anyway; on Fedora you additionally install the
>   unit yourself (§2.4). Its build dependencies are listed in the WiVRn tree at `docs/building.md` —
>   `dnf builddep` has no spec to work from.
> - **Intel hardware encode.** Stock Fedora's `intel-media-driver` may ship without the H.264/H.265
>   *encode* entrypoints. Verify: `vainfo | grep -iE 'EncSlice|EncSliceLP'`. If they are absent,
>   install RPM Fusion's freeworld `intel-media-driver` build — WiVRn cannot stream without one.
>   This is the same repo that option (a) above already turns on.
> - Vulkan ICD manifests are at `/usr/share/vulkan/icd.d` on both distros (nothing in omedora
>   changes this). The Intel manifest is `intel_icd.json`.

### 2.2 Dotfiles and the device branch

`~/.config/hypr` **is** the live config — clone it in place, then give the machine its own branch:

```sh
git clone https://github.com/AndrewGaspar/omarchy-hyprland-config ~/.config/hypr
git -C ~/.config/hypr checkout -b <device-branch> origin/master
```

Device branches are always **rebased** on `master`, never merged (see `AGENTS.md` in that repo).
`thinkpad-x1` already exists; if the work laptop is that machine, check it out and rebase it onto
current `master` in a **temporary worktree** — rebasing in place flashes intermediate commits
through the running compositor. Then `git reset --hard` the live tree to the result and
`hyprctl reload`.

Then the per-machine display-toggle installer (idempotent, `--check`-able):

```sh
~/.config/hypr/scripts/setup-xr-display.sh
```

It symlinks `~/.local/bin/omarchy-hw-external-monitors`, `~/.local/bin/hypxr-monitor-internal` and
`~/.config/environment.d/10-local-bin.conf` back into the repo. Log out and back in afterwards.

### 2.3 Session environment

`~/.config/uwsm/env` needs two additions (uwsm *rebuilds* PATH here, so an `environment.d` drop-in
alone is not enough — it gets buried):

```sh
# ~/.local/bin must precede omarchy's bin so the XR-aware
# omarchy-hw-external-monitors shadow wins for every session process.
export PATH="$HOME/.local/bin:$PATH"

# hypxrva: dynamic VA-API decode gating.
export LIBVA_DRIVER_NAME=hypxr
export LIBVA_DRIVERS_PATH=$HOME/.local/lib/hypxrva
```

Both take effect at the next login.

### 2.4 The components

```sh
scripts/hypxr-setup.sh all
```

runs, in dependency order: `deps dotfiles env compositor wivrn voice hud va paper`. What each does
that you cannot see from the outside:

**compositor.** Creates `~/code/hypxrland` as a *worktree* of this repo on branch `fishfood`,
initializes the submodules (the vendored `wayland-protocols` and `hyprutils` are required — the
system versions have broken this build before), configures with ccache + Ninja, and **fails loudly
if the OpenXR loader is missing** rather than silently producing a vanilla Hyprland. Then it writes
`~/.local/share/hypxrland/launch.sh`:

```sh
#!/bin/sh
export PATH="$HOME/code/hypxrland/build/bin:$PATH"
export HYPXRLAND_SESSION=1
exec uwsm start -e -D Hyprland -- $HOME/code/hypxrland/build/Hyprland \
     --config $HOME/.config/hypr/hyprland-xr.conf
```

The `PATH` export is what makes the session's `hyprctl` the one with the `openxr` command. The
launcher indirection means the installed `.desktop` never has to change again.

It also runs `git fetch --tags origin` first. `CMakeLists.txt` stamps the version banner from
`git describe --tags`, and on a fork clone with no tags that prints

```
fatal: No names found, cannot describe anything
```

in the middle of the configure. It is harmless — but it reads exactly like a build failure, and cost
a real "is this broken?" detour on the Fedora bring-up. The message comes from CMake, not from the
script, so fetching the tags is the fix at the right layer.

**The one root step for the session** (printed by the script):

```sh
sudo install -m644 ~/.local/share/hypxrland/hypxrland.desktop \
     /usr/share/wayland-sessions/hypxrland.desktop
```

`DesktopNames` stays `Hyprland`, so portals, theming and app behavior are identical to the stock
session; only the greeter entry's `Name` differs. On Arch the reference box calls it **"Omarchy XR"**
and SDDM lists it next to "Hyprland". On Fedora, omedora *packages* its own session entry
(`hyprland-omedora` owns `/usr/share/wayland-sessions/omedora.desktop`) — this hand-installed file
sits alongside it and **GDM** lists both.

**wivrn.** Clones the fork, adds the worktree `~/code/wivrn-<tag>` on branch
`hypxr-patches-<tag>`, and builds `build-server` with:

```
-DGIT_DESC=v26.6.2
```

**`GIT_DESC` is load-bearing.** The headset client refuses to pair or stream when the server's
version string does not match the installed APK's build. A build without it reports a git-describe
string from the fork's history and the headset rejects it.

It then writes `~/.config/systemd/user/wivrn.service.d/override.conf`:

```ini
[Service]
ExecStart=
ExecStart=%h/code/wivrn-26.6.2/build-server/server/wivrn-server
# hybrid boxes only: keep the server's Vulkan compositor off the dGPU
Environment=VK_DRIVER_FILES=/usr/share/vulkan/icd.d/radeon_icd.json
# hypxrva bypass — WiVRn's OWN vaapi ENCODE must hit the real driver directly...
Environment=LIBVA_DRIVER_NAME=radeonsi
# ...and be pointed back at the SYSTEM dri dir (Arch /usr/lib/dri, Fedora /usr/lib64/dri).
Environment=LIBVA_DRIVERS_PATH=/usr/lib/dri
```

Revert cleanly with `systemctl --user revert wivrn.service`.

`~/.config/wivrn/config.json` is per-machine (encoder + device):

```json
{"encoder": {"encoder": "vaapi", "codec": "h265", "device": "/dev/dri/renderD129"},
 "bit-depth": 8, "tcp-only": true, "inhibit": "worn"}
```

Finally it points `~/.config/openxr/1/active_runtime.json` at the *patched build's*
`openxr_wivrn-dev.json`, so the loaded runtime library matches the running server. Without this the
loader picks the packaged `/usr/share/openxr/1/openxr_wivrn.json`, which points at `/usr/bin`'s
unpatched server.

**On Fedora**, where no package provides the base unit, install one first:

```sh
install -Dm644 ~/code/wivrn-26.6.2/server/dist/wivrn.service.in \
        ~/.config/systemd/user/wivrn.service
systemctl --user daemon-reload
```

The drop-in overrides `ExecStart` anyway, so a minimal unit is sufficient. Also open the streaming
port in `firewalld` (the Arch package ships `/usr/lib/firewalld/services/wivrn.xml`; on Fedora you
add the equivalent by hand or run `tcp-only` on a trusted zone).

**voice.** Submodules (`whisper.cpp`, `llama.cpp`), then `scripts/fetch-models.sh` — the models are
not in git and the daemon cannot transcribe without `ggml-base.en.bin`. Seeds
`~/.config/hypxrvoice/config.toml` from the example with `asr.model` filled in, installs
`hypxrvoiced.service` bound to `graphical-session.target` (so a relog re-resolves the compositor
instance instead of `hyprctl`ing a dead one), and enables it.

**hud.** Builds, then installs. `CMAKE_INSTALL_PREFIX=/usr/local` is what the reference box uses,
which makes `sudo cmake --install build` the one root step — it places the binaries, both user
units, and the D-Bus activation file (`io.github.andrewgaspar.hypxrhud.service`, which is what makes
the daemon start on the first `CreatePanel` instead of at login). To avoid root entirely,
reconfigure with `-DCMAKE_INSTALL_PREFIX=$HOME/.local` and the units land in
`~/.local/lib/systemd/user`. The battery gauge only shows a headset percentage against the
**patched** wivrn-server (it publishes the `Battery` property).

**va.** `./install.sh` — user-local by construction, it refuses any prefix under `/usr`. It runs
`ctest --output-on-failure` as part of the install; when that fails, the setup script prints the
captured output rather than swallowing it, so you see the failing test instead of a bare
`Errors while running CTest`. To get an install anyway while a test is being fixed:
`~/code/hypxrva/install.sh --skip-tests`. The shim
directory `~/.local/lib/hypxrva/` must contain **nothing but the shim**, because
`LIBVA_DRIVERS_PATH` points at it. The watcher is started by the compositor config, not systemd:

```
exec-once = ~/.local/bin/hypxrva-watcher
```

**paper.** A plain CMake build. It is a *primary OpenXR session*, not a service — `openxr:overlay`
and `scripts/preview-xr.sh --env` launch it. `$HYPXRPAPER_BIN` overrides discovery.

### 2.5 Optional: XREAL

Only for the Air 2 Ultra glasses rig. Skip it on a Quest/WiVRn-only machine.

```sh
git -C ~/code/hypxrland submodule sync --recursive     # see the note below — do NOT skip this
git -C ~/code/hypxrland submodule update --init subprojects/monado
scripts/hypxr-setup.sh monado
```

> **`sync` before `update`, always.** A submodule's remote URL is frozen into `.git/config` at first
> init, so a later `.gitmodules` URL change — monado moved to the `AndrewGaspar` fork — never reaches
> an existing checkout on its own. The next fetch then dies with
> `fatal: remote error: upload-pack: not our ref <sha>`, which looks like a corrupt submodule and is
> not. This exact failure happened on the Fedora bring-up.

> **Never `setcap` `monado-service`.** A file capability puts the process into secure-execution
> mode, and the dynamic loader then *drops* the environment — silently discarding `VK_DRIVER_FILES`
> and `__GLX_VENDOR_LIBRARY_NAME`. The runtime picks the wrong GPU and crashes at swapchain time.
> If an old `/etc/sudoers.d/10-xreal-setcap` rule exists, delete it; to strip a capability without
> rebuilding, `sudo setcap -r <monado-service>`. See `docs/openxr/07-xreal.md` §1.4b.

---

## 3. Updating an existing install

This is the common case, not the rare one, and it has one sharp edge:

> **The `hypxrland` branch gets force-pushed on every upstream rebase** (most recently onto
> v0.56.1; before that v0.56.0), and so has the
> WiVRn patch branch on every retag. `git pull` / `merge --ff-only` **cannot** cross a history
> rewrite. The documented update is fetch + hard reset.

`scripts/hypxr-setup.sh <component>` does exactly that, and it does the second half that people
forget:

1. Refuse if the checkout has uncommitted changes — stop and report, never reset over your work.
2. `git fetch origin --prune`
3. `git reset --hard origin/<branch>`
4. `git submodule sync --recursive && git submodule update --init` — **sync before update**: a
   submodule's remote URL is frozen into `.git/config` at first init, not read from `.gitmodules`
   each time, so a URL change (monado moving to the `AndrewGaspar` fork) otherwise fails with
   `fatal: remote error: upload-pack: not our ref <sha>` on the next fetch. Seen for real on the
   Fedora bring-up. If you are updating a compositor checkout **by hand** rather than through this
   script, the two `submodule` lines are part of the update, not an optional extra:

   ```sh
   git -C ~/code/hypxrland fetch origin --prune
   git -C ~/code/hypxrland reset --hard origin/hypxrland
   git -C ~/code/hypxrland submodule sync --recursive     # BEFORE update, every time
   git -C ~/code/hypxrland submodule update --init
   rm -rf ~/code/hypxrland/build                          # see 5
   ```
5. **`rm -rf` the build directory whenever HEAD actually moved.** This is a rule from repeated
   pain: a stale CMake/ninja cache survives a history swap and then fails with missing-rule errors
   or soname mismatches after a system library bump — failures that look like source bugs and are
   not. ccache makes the full rebuild cheap; a stale cache costs an afternoon.

The same treatment applies to the dotfiles device branch, which is *rebased* (and therefore
force-pushed) whenever `master` moves.

For the everyday case — the compositor is only a few commits behind and nothing was rewritten —
`scripts/fishfood.sh update` is still faster and still correct.

---

## 4. Verification checklist

Run these in a live HypXRland session with the headset on.

```sh
# 0. Everything the setup script knows how to check.
scripts/hypxr-setup.sh --check              # expect: 0 failures

# 1. The compositor sees the runtime and the monitors.
hyprctl openxr status -j | jq
#    → runtime name, session state "live" once donned, XR-main in .monitors[]

# 2. Every service is up.
systemctl --user status wivrn hypxrhud hypxrhud-battery hypxrvoiced
#    wivrn is *started by the session* (exec-once), so `disabled / active` is correct.
#    The other three are enabled and bound to graphical-session.target.

# 3. The streaming path.
journalctl --user -u wivrn -b --no-pager | tail -40
wivrnctl status                             # headset connected?
wivrnctl pair                               # first time only: prints a PIN

# 4. Hardware encode really is hardware.
vainfo | grep -iE 'EncSlice|EncSliceLP'     # entrypoints must exist
hypxrva-vaprobe | tail -1                   # which real driver a decode context lands on

# 5. The HUD and the voice daemon.
busctl --user tree io.github.andrewgaspar.hypxrhud
hypxrvoicectl ptt toggle                    # or SUPER+ALT+V
```

In-headset smoke test: don the headset → `XR-main` plugs in as a monitor → `SUPER SHIFT G` grabs it
by gaze → `SUPER, Home` recenters → `SUPER ALT N` mints a second monitor → doff → the XR monitors
unplug and the laptop panel comes back (the display autopilot rides hypxrva's flag file).

---

## 5. What stays manual on a new machine

`hypxr-setup.sh` deliberately does not do these:

1. **Package installation.** Probed exhaustively and printed per distro as one command — but never
   executed. Enabling RPM Fusion and the remaining Fedora-side unknown in §2.1 (the OpenXR loader
   package name) still need a human with that box.
2. **`sudo install` of the wayland-session `.desktop`** into `/usr/share/wayland-sessions/`.
3. **`sudo cmake --install` for hypxrhud** into `/usr/local` — avoidable by choosing a
   `$HOME/.local` prefix instead.
4. **Creating the dotfiles device branch** and rebasing it onto `master`. Which machine is which is
   a judgement call, and the rebase must happen in a temporary worktree, not the live config.
5. **Headset-side software.** The patched WiVRn client APK is sideloaded onto the headset and stays
   there; it is not installed from the desktop. It must match the server's `GIT_DESC` version.
6. **Pairing.** `wivrnctl pair` prints a PIN you type in the headset. Once done,
   `~/.config/wivrn/known_keys.json` remembers it.
7. **Choosing the encoder.** `vaapi` vs `nvenc` and the render node are per-machine facts the script
   reports on but will not silently change under you.
8. **Firewall.** On Fedora there is no `wivrn-server` package to drop the `firewalld` service file.
9. **Logging out and back in** after the first `~/.config/uwsm/env` and `environment.d` changes.

---

## 6. Related

- `scripts/hypxr-setup.sh` — the script this page documents.
- `scripts/fishfood.sh` — the compositor-only update loop (`setup` / `update` / `gen-session`).
- `~/.config/hypr/scripts/setup-xr-display.sh` — the per-machine display-toggle installer.
- `~/.config/hypr/AGENTS.md` — the dotfiles branching strategy.
- `docs/openxr/05-configuration.md` — every `openxr:*` variable, `xrmonitor`, `xrrule`.
- `docs/openxr/06-testing.md` — the containerized test runner (`scripts/xr-container.sh`).
- `docs/openxr/07-xreal.md` — the optional XREAL rig, including the setcap warning.
