# HypXRland container environment

Rootless-podman Arch container that boots real PID-1 systemd, installs a curated
Omarchy v3.8.2 desktop, and builds the dev Hyprland + hyprtester + vendored
Monado inside — full session/input/GPU isolation for XR dev and hermetic tests.

Driver: [`scripts/xr-container.sh`](../scripts/xr-container.sh). WP1 ships
`build`, `shell`, and `check-gpu`; `session` (WP3) and `test` (WP2) are stubs.

## Images (built by `xr-container.sh build`)

| Tag | Contents |
| --- | --- |
| `hypxrland-ctr:base` | archlinux + systemd + Hyprland/monado build deps + labwc + an Omarchy v3.8.2 clone (`Containerfile.base`) |
| `hypxrland-ctr:pkgs` | `:base` + curated Omarchy desktop packages (`omarchy-install-ctr.sh packages`) |
| `hypxrland-ctr:session` | `:pkgs` + config/theme/seed tail (`omarchy-install-ctr.sh config`) |

The desktop is installed via **boot-install-commit** (not a `podman build` RUN):
a `podman build` has no PID-1 systemd, so the packages/config run inside a live
boot entered with `machinectl shell dev@.host` and are committed on top.

## Quick start

```sh
scripts/xr-container.sh build              # build all three images
scripts/xr-container.sh build --check-gpu  # + smoke-test GPU (eglinfo/vulkaninfo)
scripts/xr-container.sh shell              # dev logind shell (GPU: --gpu amd|nvidia)
#   inside the shell:
bash /src/containers/build-in-ctr.sh       # build Hyprland + hyprtester + monado into /build
```

Iterate on the desktop config without a full repackage:
`scripts/xr-container.sh build --config` (re-runs only the config tail on `:pkgs`).

## Layout

- `/src` — the repo, **overlay-mounted** (`:O`). Hyprland's CMake generates
  `version.h`/shaders/protocols in-source at configure time, so it can't be a
  strict `:ro` mount; the overlay upper is ephemeral, so the host tree is never
  touched. Build output goes to the `/build` volume, never here.
- `/build` — `hypxrland-build` volume. Hyprland + hyprtester + `monado/` (Monado
  redirected here via `MONADO_BUILD`/`EIGEN_BUILD`).
- `~/.cache/ccache` — `hypxrland-ccache` volume.
- `/var/cache/pacman/pkg` — `hypxrland-pacman` volume (warm package cache).

## GPU

Single-GPU by construction (dodges the cross-GPU `xrCreateSwapchain` crash).
`--gpu amd` → `--device /dev/dri/renderD129` (no CDI). `--gpu nvidia` →
`--device nvidia.com/gpu=all`, which needs the host CDI spec:

```sh
sudo pacman -S nvidia-container-toolkit
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml   # re-run after driver updates
```

The image ships **no NVIDIA userspace driver** — CDI injects the host libraries.

## Safety

The host compositor is also named `Hyprland`. This tooling never kills by
process name; every container is uniquely named per-invocation and removed by
tracked name only.
