#!/usr/bin/env bash
# HypXRland container tooling — build a hermetic Arch+systemd+Omarchy image and
# get an interactive dev shell inside it (rootless podman). WP1 of the podman
# containerization plan: base image + in-container build + build/shell.
#
# Subcommands:
#   build [--pkgs|--config|--rebuild] [--check-gpu]
#       Two-stage boot-install-commit that produces three images:
#         hypxrland-ctr:base     archlinux + systemd + Hyprland/monado build deps
#                                + labwc + an Omarchy v3.8.2 tree (Containerfile.base)
#         hypxrland-ctr:pkgs     :base after the curated Omarchy desktop package
#                                install (containers/omarchy-install-ctr.sh packages)
#         hypxrland-ctr:session  :pkgs after the config/seed tail (…config)
#       --pkgs      stop after committing :pkgs (no config tail).
#       --config    re-run ONLY the config tail on the existing :pkgs image (fast
#                   iteration) and recommit :session.
#       --rebuild   force a clean rebuild of :base (and everything above it).
#       --check-gpu after building, smoke-test GPU access (eglinfo/vulkaninfo).
#
#   shell [--gpu amd|nvidia|intel|/dev/dri/renderD*]
#       Boot :session and drop into a real logind shell as `dev` (machinectl
#       shell). Repo is overlay-mounted at /src (host tree untouched), build
#       tree in the hypxrland-build volume at /build, ccache in hypxrland-ccache.
#       Build inside with:  bash /src/containers/build-in-ctr.sh
#       Container is removed on shell exit.
#
#   check-gpu [--gpu amd|nvidia|intel|/dev/dri/renderD*]
#       Standalone GPU smoke test (same as build --check-gpu).
#
#   test [--full] [--gpu amd|nvidia|intel|/dev/dri/renderD*] [--build] [--keep]
#        [TEST_NAMES...]
#       Hermetic in-container `hyprtester` (WP2). Boots :session with NO host
#       wayland/X/wivrn mounts, brings up a headless labwc as the nesting
#       host inside, runs the suite nested into it, and reports the real
#       exit code (sentinel file — machinectl shell always exits 0). Everything
#       (Hyprland + vendored monado null-compositor) runs inside the container.
#       (default) the `--xr` suite (containers/test/run-xr-tests.sh).
#       --full    the FULL non-XR suite instead (containers/test/run-full-tests.sh):
#                 tests/{main,clients,misc}. This suite cannot run on the host at
#                 all — the host has no kitty (108 of the cases spawn one) and a
#                 non-`--xr` hyprtester there can select the developer's LIVE
#                 compositor. In here there is no live session to hit.
#       --build   force an in-container build (build-in-ctr.sh) before testing;
#                 otherwise auto-builds only if /build has no binaries.
#       --keep    leave the container running afterwards (for debugging).
#       TEST_NAMES  optional subset of test names to run (else the whole
#                 group), passed straight through to hyprtester.
#       On failure, artifacts (run log + preserved /tmp/hyprtester-xr-* dirs
#       with hyprland + monado logs) are copied to containers/artifacts/<ts>/.
#
#   session [--wivrn] [--conf FILE] [--gpu split|amd|nvidia|intel|/dev/dri/renderD*]
#           [--nested-gpu SPEC] [--xr-gpu SPEC] [--env pano|forest|<path>]
#           [--passthrough] [--publish-remote[=PORT]] [--no-audio]
#       Boot :session and launch a full Omarchy desktop as a NESTED window on the
#       host, with the dev Hyprland's XR extension enabled. waybar/mako/walker/
#       portals autostart on the container's OWN private buses (no shim; verify
#       with `busctl --user list` inside). Input is isolated (no /dev/input).
#         (default, no headset)  a vendored windowed Monado runs in-container; its
#                                XR view appears as a second window via host XWayland.
#         --wivrn                talk to the host WiVRn runtime for a REAL headset
#                                (host must run wivrn-server with the headset up;
#                                this container never starts wivrn-server).
#         --conf FILE            source FILE (bind-mounted ro) as the base config
#                                instead of the image's ~/.config/hypr/hyprland.conf.
#         --passthrough          openxr:blend_mode = alpha (composite over passthrough).
#         --env pano|forest|<path>
#                                Ambient background: launch hypxrpaper as the PRIMARY
#                                OpenXR session (gradient sky / bundled 'forest-clearing'
#                                3D scene / a panorama or scene <path> reachable inside
#                                the container) and run HypXRland as an XR_EXTX_overlay on
#                                top (forces openxr:overlay = 1). Mirrors preview-xr.sh
#                                --env. Needs hypxrpaper built into /build (build-in-ctr.sh).
#         --gpu SPEC             GPU selection (default: split with --wivrn, else amd).
#                                SPEC = split | amd|nvidia|intel | /dev/dri/renderD*.
#             split              expose BOTH GPUs and assign roles separately: the
#                                nested compositor renders on the host-compositor GPU
#                                (AQ_DRM_DEVICES) and XR encodes on a different GPU
#                                (openxr:gpu). This is the ONLY reliable --wivrn mode
#                                on a dual-GPU box; degrades to single-GPU when the
#                                machine has only one GPU. A single-GPU --gpu pin with
#                                --wivrn is kept for experiments (warns; known-broken).
#         --nested-gpu SPEC      (split) override the host-compositor/AQ node
#                                (default: 'host' = first non-NVIDIA present node).
#         --xr-gpu SPEC          (split) override the XR/encode node (default: nvidia).
#                                Passing either --nested-gpu/--xr-gpu implies --gpu split.
#         --publish-remote[=PORT] publish the in-container Monado remote-driver port
#                                (container 4242) to the host on an EPHEMERAL free
#                                port (or PORT). OFF by default — a fixed 4242 publish
#                                once poisoned a concurrent host suite run.
#         --no-audio             do NOT share the host PipeWire/PulseAudio sockets.
#                                By default the host audio daemon is shared read-only
#                                (pipewire-0 + pulse/native) so in-container apps
#                                (chromium, etc.) play/capture through it; no audio
#                                daemon runs in the container. --no-audio omits the
#                                mounts (apps degrade to silent). Absent host audio is
#                                auto-skipped with a notice regardless.
#       Runs interactively (attached logs); teardown on exit = `podman rm -f` its
#       container, which cleanly kills the whole session tree.
#
#   exec <cmd…>
#       Run <cmd…> inside the RUNNING session container as `dev` in its logind
#       session (e.g. `xr-container.sh exec hyprctl openxr status`). Convenience
#       wrapper around `machinectl shell dev@.host … bash -lc`.
#
#
# SAFETY: this box's HOST compositor is also named "Hyprland". This script NEVER
# kills by process name. Container teardown is `podman rm -f <tracked-name>`
# only; every container name is unique per-invocation ($$) and tracked.
#
# GPU: render nodes are resolved at run time by a vendor scan of /sys/class/drm
# (scripts/lib/gpu.sh) — no node names are hardcoded. `--gpu amd|nvidia|intel`
# picks the first node of that vendor; `--gpu /dev/dri/renderDNNN` forces one;
# $HYPXRLAND_{AMD,NVIDIA,INTEL}_NODE override the scan. NVIDIA additionally needs
# the host CDI spec (nvidia-container-toolkit + `nvidia-ctk cdi generate`); if it's
# absent the script prints the setup commands (and, for check-gpu, falls back to
# AMD). AMD/Intel are plain --device <node>; NVIDIA is --device nvidia.com/gpu=all.

set -euo pipefail

# --- constants ---------------------------------------------------------------
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Shared GPU render-node resolver (vendor scan + env overrides + explicit path).
# shellcheck source=lib/gpu.sh
source "$REPO/scripts/lib/gpu.sh"
IMG_BASE="hypxrland-ctr:base"
IMG_PKGS="hypxrland-ctr:pkgs"
IMG_SESSION="hypxrland-ctr:session"
VOL_PACMAN="hypxrland-pacman"
VOL_BUILD="hypxrland-build"
VOL_CCACHE="hypxrland-ccache"
CONTAINERFILE="$REPO/containers/Containerfile.base"
INSTALL_SCRIPT="$REPO/containers/omarchy-install-ctr.sh"
INSTALL_IN_CTR="/tmp/omarchy-install-ctr.sh"
CCACHE_DIR_IN_CTR="/home/dev/.cache/ccache"
BUILD_IN_CTR_SCRIPT="$REPO/containers/build-in-ctr.sh"
RUN_XR_TESTS_SCRIPT="$REPO/containers/test/run-xr-tests.sh"
RUN_FULL_TESTS_SCRIPT="$REPO/containers/test/run-full-tests.sh"
ARTIFACTS_DIR="$REPO/containers/artifacts"

# GPU render nodes are resolved at run time by resolve_render_node (scripts/lib/
# gpu.sh) — a vendor scan of /sys/class/drm, overridable per-vendor via
# $HYPXRLAND_{AMD,NVIDIA,INTEL}_NODE or an explicit `--gpu /dev/dri/renderDNNN`.
# No node names are hardcoded here (they differ box to box).
CDI_NVIDIA_SPEC_YAML="/etc/cdi/nvidia.yaml"
CDI_NVIDIA_SPEC_JSON="/etc/cdi/nvidia.json"

# session mounts: host XR bits land under this container-side dir (a plain
# top-level mount point, NOT under systemd-managed /run, so nothing shadows it).
HOST_MNT="/hypxrland-host"
WIVRN_MANIFEST="${WIVRN_RUNTIME_JSON:-/usr/share/openxr/1/openxr_wivrn.json}"
WIVRN_LIB_DIR="/usr/lib/wivrn"

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m==> WARN: %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[1;31m==> ERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# --- shared helpers ----------------------------------------------------------

ensure_volumes() {
    local v
    for v in "$@"; do
        podman volume exists "$v" >/dev/null 2>&1 || podman volume create "$v" >/dev/null
    done
}

# wait_for_systemd <container> — block until systemd settles + the dev user
# manager is active.
wait_for_systemd() {
    local ctr="$1" i state
    log "Waiting for systemd to reach running/degraded in $ctr"
    for i in $(seq 1 60); do
        state=$(podman exec "$ctr" systemctl is-system-running 2>/dev/null || true)
        case "$state" in
            running|degraded) echo "   systemd: $state (after ${i}s)"; break ;;
        esac
        [[ $i -eq 60 ]] && { podman logs "$ctr" 2>&1 | tail -30; die "systemd never settled (last: ${state:-none})"; }
        sleep 1
    done
    for i in $(seq 1 30); do
        [[ "$(podman exec "$ctr" systemctl is-active user@1000.service 2>/dev/null || true)" == "active" ]] && {
            echo "   user@1000.service: active"; return 0; }
        [[ $i -eq 30 ]] && die "user@1000.service never became active"
        sleep 1
    done
}

# run_stage <container> <stage> — run omarchy-install-ctr.sh as dev in a logind
# session; read the REAL exit code back from the sentinel (machinectl shell
# always exits 0). Fails loudly with a log tail.
run_stage() {
    local ctr="$1" stage="$2" rc
    log "Running omarchy install stage '$stage' as dev (logind session)"
    podman exec "$ctr" rm -f /tmp/omarchy-install.exit >/dev/null 2>&1 || true
    podman exec "$ctr" machinectl shell dev@.host /usr/bin/bash -lc \
        "bash '$INSTALL_IN_CTR' '$stage'" || true
    rc=$(podman exec "$ctr" cat /tmp/omarchy-install.exit 2>/dev/null || echo missing)
    if [[ $rc != 0 ]]; then
        warn "install stage '$stage' failed (rc=$rc) — last 60 log lines:"
        podman exec "$ctr" tail -60 /var/log/omarchy-install.log 2>/dev/null \
            | sed -E 's/\x1b\[[0-9;?]*[mGKhlABCD]//g' || true
        die "omarchy install stage '$stage' failed"
    fi
    log "install stage '$stage' succeeded"
}

commit_systemd() {
    local ctr="$1" img="$2"
    log "Committing $ctr -> $img"
    podman commit \
        --change 'CMD ["/sbin/init"]' \
        --change 'STOPSIGNAL SIGRTMIN+3' \
        "$ctr" "$img" >/dev/null
}

# --- GPU device selection ----------------------------------------------------
nvidia_cdi_available() {
    [[ -f $CDI_NVIDIA_SPEC_YAML || -f $CDI_NVIDIA_SPEC_JSON ]]
}

print_nvidia_cdi_setup() {
    cat >&2 <<EOF
   NVIDIA CDI spec not found ($CDI_NVIDIA_SPEC_YAML). To enable the NVIDIA path,
   on the HOST run (needs sudo — this script will not do it for you):
       sudo pacman -S nvidia-container-toolkit
       sudo nvidia-ctk cdi generate --output=$CDI_NVIDIA_SPEC_YAML
   then re-run with --gpu nvidia. (Regenerate after every host driver update.)
EOF
}

# check_nvidia_cdi_freshness — belt-and-braces UX: if the CDI spec's baked driver
# version disagrees with the loaded kernel module, print the one-line regen hint.
# Non-fatal (a pacman hook regenerates on driver updates); silent when they match.
check_nvidia_cdi_freshness() {
    local spec="$CDI_NVIDIA_SPEC_YAML"
    [[ -f $spec ]] || spec="$CDI_NVIDIA_SPEC_JSON"
    [[ -f $spec ]] || return 0
    local loaded spec_ver
    loaded=$(cat /sys/module/nvidia/version 2>/dev/null || true)
    spec_ver=$(grep -oE 'host-driver-version=[0-9][0-9.]*' "$spec" 2>/dev/null | head -1 | cut -d= -f2)
    [[ -n $loaded && -n $spec_ver && $loaded != "$spec_ver" ]] || return 0
    warn "NVIDIA CDI spec driver version ($spec_ver) != loaded module ($loaded) — spec is stale."
    warn "Regenerate:  sudo nvidia-ctk cdi generate --output=$CDI_NVIDIA_SPEC_YAML"
}

# gpu_devargs <gpu> [--allow-amd-fallback] — set REPLY_DEVARGS (podman --device
# array) and REPLY_GPU (the effective gpu keyword, possibly changed by fallback)
# for a `--gpu` spec (amd|nvidia|intel|/dev/dri/renderD*). NVIDIA goes through
# CDI (nvidia.com/gpu=all); everything else resolves to a host render node via
# resolve_render_node. Dies with setup guidance on an unusable request unless
# --allow-amd-fallback, in which case an unavailable NVIDIA CDI degrades to AMD.
gpu_devargs() {
    local gpu="$1" allow_fallback=0
    [[ "${2:-}" == --allow-amd-fallback ]] && allow_fallback=1
    REPLY_DEVARGS=()
    REPLY_GPU="$gpu"
    case "${gpu,,}" in
        nvidia)
            if nvidia_cdi_available; then
                check_nvidia_cdi_freshness
                REPLY_DEVARGS=(--device nvidia.com/gpu=all)
            elif [[ $allow_fallback -eq 1 ]]; then
                warn "requested --gpu nvidia but CDI is unavailable on this host."
                print_nvidia_cdi_setup
                local n; n=$(resolve_render_node amd) || die "no NVIDIA CDI and no AMD node to fall back to"
                warn "falling back to AMD ($n)."
                REPLY_GPU=amd; REPLY_DEVARGS=(--device "$n")
            else
                print_nvidia_cdi_setup
                die "NVIDIA CDI unavailable on this host; use --gpu amd"
            fi
            ;;
        amd|intel|/*)
            local n; n=$(resolve_render_node "$gpu") || die "could not resolve --gpu $gpu (see above)"
            REPLY_DEVARGS=(--device "$n") ;;
        *) die "--gpu must be amd|nvidia|intel|/dev/dri/renderD* (got '$gpu')" ;;
    esac
}

# resolve_split_gpu <nested_spec> <xr_spec> — split-GPU device plumbing for the
# --wivrn session. On a dual-GPU box the nested compositor must render on the
# HOST compositor's GPU (aquamarine nests on it) while WiVRn encodes on a
# DIFFERENT GPU (NVIDIA here); no single GPU satisfies both, so we expose BOTH and
# assign roles separately. Sets:
#   SPLIT_DEVARGS       podman --device args (union of both GPUs, deduped)
#   SPLIT_NESTED_SPEC   effective nested (host-compositor) spec  -> NESTED_GPU_NODE
#   SPLIT_XR_SPEC       effective XR/encode spec                 -> XR_GPU_NODE
# Both specs are re-resolved to concrete nodes INSIDE the container after boot
# (gpu_node_in_ctr) so a CDI-injected NVIDIA node is verified present by name.
# Single-GPU degradation: a box with only one GPU (no distinct NVIDIA + non-NVIDIA
# pair) collapses to that one node for BOTH roles (a genuine single-GPU --wivrn,
# which is fine when the host compositor already runs on the encode GPU). Dies
# with guidance only when a side is explicitly requested but truly unavailable.
resolve_split_gpu() {
    local nested_spec="$1" xr_spec="$2"
    SPLIT_DEVARGS=()
    SPLIT_NESTED_SPEC="$nested_spec"
    SPLIT_XR_SPEC="$xr_spec"

    local have_nvidia=0 have_other=0
    resolve_render_node nvidia >/dev/null 2>&1 && have_nvidia=1
    { resolve_render_node amd >/dev/null 2>&1 || resolve_render_node intel >/dev/null 2>&1; } && have_other=1

    # --- single-GPU degradation (no distinct NVIDIA + non-NVIDIA pair) ---
    if [[ $have_nvidia -eq 0 || $have_other -eq 0 ]]; then
        local node
        node=$(resolve_render_node "$nested_spec") \
            || die "split: no usable GPU node for '$nested_spec' (see above)"
        warn "split: only one GPU present ($node) — degrading to single-GPU for BOTH nested + XR roles."
        warn "       (fine if the host compositor already runs on the encode GPU; otherwise WiVRn may cross-GPU-crash.)"
        SPLIT_NESTED_SPEC="$nested_spec"
        SPLIT_XR_SPEC="$nested_spec"
        if [[ $have_nvidia -eq 1 && $have_other -eq 0 ]]; then
            nvidia_cdi_available || { print_nvidia_cdi_setup; die "split: the lone GPU is NVIDIA and needs a CDI spec (see above)"; }
            check_nvidia_cdi_freshness
            SPLIT_DEVARGS=(--device nvidia.com/gpu=all)
        else
            SPLIT_DEVARGS=(--device "$node")
        fi
        return 0
    fi

    # --- true dual-GPU split ---
    # Nested (host-compositor) side: a real DRM node bind-mounted as a --device
    # (never CDI-only — aquamarine opens it directly to nest).
    local nested_node
    nested_node=$(resolve_render_node "$nested_spec") \
        || die "split: could not resolve nested GPU '$nested_spec' (host-compositor side) — see above"
    SPLIT_DEVARGS+=(--device "$nested_node")

    # XR / encode side.
    if [[ "${xr_spec,,}" == nvidia ]]; then
        nvidia_cdi_available || {
            print_nvidia_cdi_setup
            die "split: the XR/encode side wants NVIDIA but its CDI spec is absent (see above); install it, or pass --xr-gpu <amd|intel|/dev/dri/renderDNNN>."
        }
        check_nvidia_cdi_freshness
        SPLIT_DEVARGS+=(--device nvidia.com/gpu=all)
    else
        local xr_node
        xr_node=$(resolve_render_node "$xr_spec") \
            || die "split: could not resolve XR GPU '$xr_spec' — see above"
        [[ "$xr_node" != "$nested_node" ]] && SPLIT_DEVARGS+=(--device "$xr_node")
    fi
}

# gpu_node_in_ctr <ctr> <gpu> — resolve the render node INSIDE a running
# container by re-running the shared resolver there (/src/scripts/lib/gpu.sh).
# For CDI NVIDIA the node keeps its host name but this verifies it is actually
# present; for AMD/explicit it confirms the bind-mounted node. Echoes the node.
gpu_node_in_ctr() {
    local ctr="$1" gpu="$2" node
    node=$(podman exec "$ctr" /usr/bin/bash -lc \
        "source /src/scripts/lib/gpu.sh && resolve_render_node '$gpu'" 2>/dev/null) \
        || die "could not resolve GPU node for '$gpu' inside container $ctr (in-container scan failed)"
    printf '%s\n' "$node"
}

# --- check-gpu ---------------------------------------------------------------
# Smoke-test GPU access inside a short-lived container: eglinfo + vulkaninfo.
check_gpu() {
    local gpu="${1:-amd}" img="${2:-$IMG_BASE}"
    podman image exists "$img" || die "image $img not built yet (run: $0 build)"

    # A missing NVIDIA CDI degrades to AMD here (smoke test is best-effort).
    gpu_devargs "$gpu" --allow-amd-fallback
    gpu="$REPLY_GPU"
    local -a devargs=("${REPLY_DEVARGS[@]}")

    log "GPU smoke test (--gpu $gpu, devices: ${devargs[*]})"
    local ctr="hypxrland-gpucheck-$$"
    podman rm -f "$ctr" >/dev/null 2>&1 || true
    # Short-lived, NON-systemd run — we just want the diagnostics tools.
    podman run --rm --name "$ctr" --userns=keep-id --security-opt label=disable \
        "${devargs[@]}" --entrypoint "" "$img" \
        bash -lc '
            echo "--- render nodes ---"; ls -l /dev/dri 2>/dev/null || echo "(no /dev/dri)"
            echo "--- eglinfo (platform devices) ---"
            if command -v eglinfo >/dev/null; then
                eglinfo -B 2>/dev/null | grep -iE "device|vendor|renderer|EGL_VERSION" | head -20 \
                    || eglinfo 2>/dev/null | grep -iE "vendor|renderer|device" | head -20
            else echo "(eglinfo not installed)"; fi
            echo "--- vulkaninfo --summary ---"
            if command -v vulkaninfo >/dev/null; then
                vulkaninfo --summary 2>/dev/null | grep -iE "deviceName|driverName|apiVersion|GPU" | head -20 \
                    || echo "(vulkaninfo produced no device summary)"
            else echo "(vulkaninfo not installed)"; fi
        ' || warn "GPU smoke test container exited non-zero (see output above)"
}

# --- build -------------------------------------------------------------------
cmd_build() {
    local mode="full" do_check_gpu=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --pkgs)      mode="pkgs" ;;
            --config)    mode="config" ;;
            --rebuild)   mode="rebuild" ;;
            --check-gpu) do_check_gpu=1 ;;
            -h|--help)   grep -E '^# ' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) die "build: unknown flag $1" ;;
        esac
        shift
    done

    [[ -f $CONTAINERFILE ]]   || die "missing $CONTAINERFILE"
    [[ -f $INSTALL_SCRIPT ]]  || die "missing $INSTALL_SCRIPT"

    local ctr="hypxrland-build-$$"
    cleanup_build() { podman rm -f "$ctr" >/dev/null 2>&1 || true; }
    trap cleanup_build EXIT INT TERM

    ensure_volumes "$VOL_PACMAN"

    # --config fast path: reuse :pkgs, re-run only the config tail.
    if [[ $mode == config ]]; then
        podman image exists "$IMG_PKGS" || die "--config needs $IMG_PKGS (run a full build first)"
        log "Fast config-only build: booting $IMG_PKGS"
        podman rm -f "$ctr" >/dev/null 2>&1 || true
        podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
            -v "$VOL_PACMAN:/var/cache/pacman/pkg" \
            -v "$INSTALL_SCRIPT:$INSTALL_IN_CTR:ro" \
            "$IMG_PKGS" >/dev/null
        wait_for_systemd "$ctr"
        run_stage "$ctr" config
        commit_systemd "$ctr" "$IMG_SESSION"
        cleanup_build; trap - EXIT INT TERM
        log "Done (config-only). Session image: $IMG_SESSION"
        return 0
    fi

    # --- 1. base image ---
    if [[ $mode == rebuild ]] || ! podman image exists "$IMG_BASE"; then
        log "Building base image $IMG_BASE (Containerfile.base)"
        # Context = containers/ (small; the Containerfile COPYs nothing, it clones
        # Omarchy from the web), so we don't ship the whole repo to the builder.
        podman build -t "$IMG_BASE" -f "$CONTAINERFILE" "$REPO/containers"
    else
        log "Base image $IMG_BASE present (use --rebuild to force)"
    fi

    # --- 2. packages stage: boot base, install desktop, commit :pkgs ---
    local need_pkgs=1
    if [[ $mode != rebuild ]] && podman image exists "$IMG_PKGS"; then
        log "Packages image $IMG_PKGS present (use --rebuild to force a repackage)"
        need_pkgs=0
    fi
    if [[ $need_pkgs -eq 1 ]]; then
        log "Booting $IMG_BASE under systemd (packages stage)"
        podman rm -f "$ctr" >/dev/null 2>&1 || true
        podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
            -v "$VOL_PACMAN:/var/cache/pacman/pkg" \
            -v "$INSTALL_SCRIPT:$INSTALL_IN_CTR:ro" \
            "$IMG_BASE" >/dev/null
        wait_for_systemd "$ctr"
        run_stage "$ctr" packages
        commit_systemd "$ctr" "$IMG_PKGS"
        podman rm -f "$ctr" >/dev/null 2>&1 || true
    fi

    if [[ $mode == pkgs ]]; then
        cleanup_build; trap - EXIT INT TERM
        log "Done (--pkgs). Packages image: $IMG_PKGS"
        [[ $do_check_gpu -eq 1 ]] && check_gpu amd "$IMG_PKGS"
        return 0
    fi

    # --- 3. config stage: boot :pkgs, apply config, commit :session ---
    log "Booting $IMG_PKGS under systemd (config stage)"
    podman rm -f "$ctr" >/dev/null 2>&1 || true
    podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
        -v "$VOL_PACMAN:/var/cache/pacman/pkg" \
        -v "$INSTALL_SCRIPT:$INSTALL_IN_CTR:ro" \
        "$IMG_PKGS" >/dev/null
    wait_for_systemd "$ctr"
    run_stage "$ctr" config
    commit_systemd "$ctr" "$IMG_SESSION"
    podman rm -f "$ctr" >/dev/null 2>&1 || true

    cleanup_build; trap - EXIT INT TERM
    log "Done. Images: $IMG_BASE, $IMG_PKGS, $IMG_SESSION"
    echo "   Open a dev shell:   $0 shell"
    echo "   Build inside it:    bash /src/containers/build-in-ctr.sh"

    [[ $do_check_gpu -eq 1 ]] && check_gpu amd "$IMG_SESSION"
    return 0
}

# --- shell -------------------------------------------------------------------
cmd_shell() {
    local gpu="amd"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --gpu) [[ $# -ge 2 ]] || die "--gpu needs an argument (nvidia|amd)"; gpu="$2"; shift ;;
            --gpu=*) gpu="${1#--gpu=}" ;;
            -h|--help) grep -E '^# ' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
            *) die "shell: unknown flag $1" ;;
        esac
        shift
    done
    podman image exists "$IMG_SESSION" || die "session image not built (run: $0 build)"

    gpu_devargs "$gpu"; gpu="$REPLY_GPU"
    local -a devargs=("${REPLY_DEVARGS[@]}")

    ensure_volumes "$VOL_BUILD" "$VOL_CCACHE"
    local ctr="hypxrland-shell-$$"
    cleanup_shell() { podman rm -f "$ctr" >/dev/null 2>&1 || true; }
    trap cleanup_shell EXIT INT TERM

    log "Booting $IMG_SESSION (shell, --gpu $gpu)"
    podman rm -f "$ctr" >/dev/null 2>&1 || true
    # /src is an OVERLAY mount (:O), NOT :ro: Hyprland's CMake generates
    # src/version.h + src/render/shaders/* + protocols in-source at configure
    # time, so a strictly read-only source can't configure. The overlay upper is
    # ephemeral (discarded when the container is removed) so the HOST tree stays
    # untouched — the isolation we actually want. Persistent build artifacts go
    # to the /build volume (monado redirected there too), not the overlay.
    podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
        --security-opt label=disable \
        "${devargs[@]}" \
        -v "$REPO:/src:O" \
        -v "$VOL_BUILD:/build" \
        -v "$VOL_CCACHE:$CCACHE_DIR_IN_CTR" \
        "$IMG_SESSION" >/dev/null
    wait_for_systemd "$ctr"
    # Named volumes come up root-owned; make them writable by dev (uid 1000).
    podman exec "$ctr" chown -R dev:dev /build "$CCACHE_DIR_IN_CTR" >/dev/null 2>&1 || true

    log "Entering logind shell as dev. Build with: bash /src/containers/build-in-ctr.sh"
    podman exec -it "$ctr" machinectl shell dev@.host || true

    cleanup_shell; trap - EXIT INT TERM
    log "Shell exited; container removed."
}

# --- test (WP2): hermetic in-container `hyprtester` ---------------------------
# Boots :session (no host wayland/X/wivrn mounts — hermetic by construction),
# ensures /build is populated (auto-builds if not), runs containers/test/
# run-{xr,full}-tests.sh as dev via machinectl (labwc headless nest -> hyprtester),
# reads the real exit code from a sentinel, copies artifacts out on failure, and
# removes the container on ALL exit paths.
cmd_test() {
    local gpu="amd" keep=0 do_build=0 suite="xr"
    local -a testnames=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --gpu)     [[ $# -ge 2 ]] || die "--gpu needs an argument (nvidia|amd)"; gpu="$2"; shift ;;
            --gpu=*)   gpu="${1#--gpu=}" ;;
            --keep)    keep=1 ;;
            --build)   do_build=1 ;;
            --full)    suite="full" ;;
            -h|--help) usage; exit 0 ;;
            -*)        die "test: unknown flag $1" ;;
            *)         testnames+=("$1") ;;   # hyprtester test-name filter (subset)
        esac
        shift
    done
    podman image exists "$IMG_SESSION" || die "session image not built (run: $0 build)"

    # Per-suite wiring: which runner, which sentinel/log, what the messages call it.
    local runner sentinel runlog label
    if [[ $suite == full ]]; then
        runner="$RUN_FULL_TESTS_SCRIPT"; sentinel="/tmp/hypxrland-full-tests.exit"
        runlog="/tmp/hypxrland-full-tests.log"; label="full (non-XR)"
    else
        runner="$RUN_XR_TESTS_SCRIPT";   sentinel="/tmp/hypxrland-xr-tests.exit"
        runlog="/tmp/hypxrland-xr-tests.log"; label="XR"
    fi
    [[ -f $runner ]] || die "missing $runner"

    # GPU device args (host-side). The render node the suite pins (gpu_node) is
    # resolved INSIDE the container after boot — see gpu_node_in_ctr below — so a
    # CDI-injected NVIDIA node is verified present rather than assumed by name.
    gpu_devargs "$gpu"; gpu="$REPLY_GPU"
    local -a devargs=("${REPLY_DEVARGS[@]}")
    local gpu_node=""

    ensure_volumes "$VOL_BUILD" "$VOL_CCACHE"
    local ctr="hypxrland-test-$$"
    local removed=0
    cleanup_test() { [[ $removed -eq 1 || $keep -eq 1 ]] || { podman rm -f "$ctr" >/dev/null 2>&1 || true; }; }
    trap cleanup_test EXIT INT TERM

    # HERMETIC: mounts are ONLY the overlay source, the build+ccache volumes and
    # the GPU device — NO host wayland/X11/wivrn sockets. (Proven in the report by
    # echoing this invocation.) --systemd=always gives real logind for machinectl.
    log "Booting $IMG_SESSION (test, --gpu $gpu)"
    podman rm -f "$ctr" >/dev/null 2>&1 || true
    podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
        --security-opt label=disable \
        "${devargs[@]}" \
        -v "$REPO:/src:O" \
        -v "$VOL_BUILD:/build" \
        -v "$VOL_CCACHE:$CCACHE_DIR_IN_CTR" \
        "$IMG_SESSION" >/dev/null
    wait_for_systemd "$ctr"
    podman exec "$ctr" chown -R dev:dev /build "$CCACHE_DIR_IN_CTR" >/dev/null 2>&1 || true

    # Resolve the render node the suite pins by scanning INSIDE the container
    # (CDI-injected NVIDIA keeps host names, but verify presence rather than assume).
    gpu_node=$(gpu_node_in_ctr "$ctr" "$gpu")
    log "In-container render node for --gpu $gpu: $gpu_node"

    # Ensure /build is populated. Auto-build if the binaries are missing, or if
    # --build was passed (force a fresh build-in-ctr run).
    local have_bins=1
    podman exec "$ctr" test -x /build/hyprtester/hyprtester -a -x /build/Hyprland || have_bins=0
    if [[ $do_build -eq 1 || $have_bins -eq 0 ]]; then
        [[ $have_bins -eq 0 ]] && log "/build has no hyprtester/Hyprland yet — building in-container (build-in-ctr.sh)"
        [[ $do_build -eq 1 ]] && log "--build requested — (re)building in-container (build-in-ctr.sh)"
        podman exec "$ctr" machinectl shell dev@.host /usr/bin/bash -lc \
            'bash /src/containers/build-in-ctr.sh' \
            || die "in-container build failed"
    else
        log "/build already populated (Hyprland + hyprtester present) — skipping build (use --build to force)"
    fi

    # Run the suite. machinectl shell always exits 0 -> real rc from the sentinel.
    log "Running hermetic $label suite in-container (labwc nest -> hyprtester)"
    podman exec "$ctr" rm -f "$sentinel" >/dev/null 2>&1 || true
    podman exec "$ctr" machinectl shell dev@.host /usr/bin/bash -lc \
        "XR_GPU_NODE='$gpu_node' bash /src/containers/test/$(basename "$runner") ${testnames[*]:-}" || true

    local rc
    rc=$(podman exec "$ctr" cat "$sentinel" 2>/dev/null || echo missing)
    log "$label suite sentinel exit code: $rc"

    if [[ "$rc" != 0 ]]; then
        # Preserve artifacts: the combined run log, every preserved
        # /tmp/hyprtester-xr-* dir (the XR harness keeps these on failure, incl.
        # hyprland logs + monado.log), and the per-test dumps hyprtester writes
        # into hyprtester/artifacts/ — which lives in the /src OVERLAY and would
        # otherwise die with the container.
        local ts stamp
        ts=$(date +%Y%m%d-%H%M%S)
        stamp="$ARTIFACTS_DIR/$ts"
        mkdir -p "$stamp"
        warn "$label suite failed (rc=$rc) — collecting artifacts into $stamp"
        podman cp "$ctr:$runlog" "$stamp/$(basename "$runlog")" >/dev/null 2>&1 || true
        # copy each preserved run dir
        local dirs
        dirs=$(podman exec "$ctr" bash -lc 'ls -d /tmp/hyprtester-xr-* 2>/dev/null' || true)
        local d
        for d in $dirs; do
            podman cp "$ctr:$d" "$stamp/$(basename "$d")" >/dev/null 2>&1 || true
        done
        podman exec "$ctr" bash -lc 'ls /src/hyprtester/artifacts >/dev/null 2>&1' \
            && podman cp "$ctr:/src/hyprtester/artifacts" "$stamp/test-artifacts" >/dev/null 2>&1 || true
        echo "   Artifacts: $stamp"
        ls -la "$stamp" 2>/dev/null | sed 's/^/     /' || true
    fi

    if [[ $keep -eq 1 ]]; then
        warn "--keep: leaving container $ctr running (remove with: podman rm -f $ctr)"
    else
        podman rm -f "$ctr" >/dev/null 2>&1 || true
        removed=1
    fi
    trap - EXIT INT TERM

    if [[ "$rc" == 0 ]]; then
        log "$label suite PASSED in-container (hermetic, --gpu $gpu)"
        return 0
    fi
    die "$label suite FAILED in-container (rc=$rc)"
}

# --- session -----------------------------------------------------------------
# find_free_tcp_port — echo an unused localhost TCP port (kernel-assigned).
find_free_tcp_port() {
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
        return 0
    fi
    # Fallback: probe a handful of high ports with ss.
    local p
    for _ in $(seq 1 50); do
        p=$(( (RANDOM % 20000) + 20000 ))
        ss -Htln "sport = :$p" 2>/dev/null | grep -q . || { printf '%s\n' "$p"; return 0; }
    done
    return 1
}

cmd_session() {
    local gpu="" use_wivrn=0 passthrough=0 user_conf="" env_spec=""
    local publish_remote=0 remote_port="" use_audio=1
    local nested_gpu="" xr_gpu=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --wivrn)      use_wivrn=1 ;;
            --no-audio)   use_audio=0 ;;
            --passthrough) passthrough=1 ;;
            --env)        [[ $# -ge 2 ]] || die "--env needs an argument (pano|forest|<path>)"; env_spec="$2"; shift ;;
            --env=*)      env_spec="${1#--env=}" ;;
            --gpu)        [[ $# -ge 2 ]] || die "--gpu needs an argument"; gpu="$2"; shift ;;
            --gpu=*)      gpu="${1#--gpu=}" ;;
            --nested-gpu) [[ $# -ge 2 ]] || die "--nested-gpu needs an argument"; nested_gpu="$2"; shift ;;
            --nested-gpu=*) nested_gpu="${1#--nested-gpu=}" ;;
            --xr-gpu)     [[ $# -ge 2 ]] || die "--xr-gpu needs an argument"; xr_gpu="$2"; shift ;;
            --xr-gpu=*)   xr_gpu="${1#--xr-gpu=}" ;;
            --conf)       [[ $# -ge 2 ]] || die "--conf needs a FILE argument"; user_conf="$2"; shift ;;
            --conf=*)     user_conf="${1#--conf=}" ;;
            --publish-remote)   publish_remote=1 ;;                       # ephemeral host port
            --publish-remote=*) publish_remote=1; remote_port="${1#--publish-remote=}" ;;
            -h|--help)    usage; exit 0 ;;
            *) die "session: unknown flag $1" ;;
        esac
        shift
    done
    podman image exists "$IMG_SESSION" || die "session image not built (run: $0 build)"

    # --nested-gpu/--xr-gpu are split-only role overrides -> they imply --gpu split.
    if [[ -n $nested_gpu || -n $xr_gpu ]]; then
        [[ -z $gpu || $gpu == split ]] || die "--nested-gpu/--xr-gpu apply only to --gpu split (got --gpu $gpu)"
        gpu=split
    fi

    # Default GPU mode. --wivrn is single-GPU-broken on a dual-GPU box (the nested
    # window must render on the HOST compositor's GPU while WiVRn encodes on a
    # DIFFERENT one) — so --wivrn defaults to `split`, which exposes both GPUs and
    # assigns them separately. Windowed default stays single-GPU AMD (no headset,
    # no CDI dependency, and the in-container Monado picks its own device). An
    # explicit --gpu always wins.
    if [[ -z $gpu ]]; then
        [[ $use_wivrn -eq 1 ]] && gpu=split || gpu=amd
    fi
    if [[ $use_wivrn -eq 1 && $gpu != split ]]; then
        warn "--wivrn with a single-GPU pin (--gpu $gpu) is known-broken on dual-GPU boxes:"
        warn "  --gpu amd -> nested backend up but cross-GPU swapchain SEGV; --gpu nvidia -> nested"
        warn "  CBackend::create() fails. Use --gpu split (default) unless your host compositor"
        warn "  already runs on the encode GPU."
    fi

    local -a devargs=()
    local gpu_node=""   # resolved in-container after boot (single-GPU path)
    if [[ $gpu == split ]]; then
        # nested = host-compositor GPU (default heuristic 'host'); xr = encode GPU
        # (default nvidia). resolve_split_gpu builds the combined --device set and
        # may degrade both roles to one node on a single-GPU machine.
        resolve_split_gpu "${nested_gpu:-host}" "${xr_gpu:-nvidia}"
        devargs=("${SPLIT_DEVARGS[@]}")
    else
        gpu_devargs "$gpu"; gpu="$REPLY_GPU"
        devargs=("${REPLY_DEVARGS[@]}")
    fi

    # --- host wayland socket (nested output) ---
    local host_xdg="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    local wl="${WAYLAND_DISPLAY:-wayland-1}"
    local host_sock
    [[ $wl == /* ]] && host_sock="$wl" || host_sock="$host_xdg/$wl"
    [[ -S $host_sock ]] || die "host wayland socket not found: $host_sock (run this from a wayland session)"
    local ctr_wl="$HOST_MNT/wayland-1"

    ensure_volumes "$VOL_BUILD" "$VOL_CCACHE"

    # --- assemble the podman run + launch env per mode ---
    local -a mounts=(
        -v "$REPO:/src:O"
        -v "$VOL_BUILD:/build"
        -v "$VOL_CCACHE:$CCACHE_DIR_IN_CTR"
        -v "$host_sock:$ctr_wl"
    )
    [[ -f $host_sock.lock ]] && mounts+=(-v "$host_sock.lock:$ctr_wl.lock:ro")

    # launch_env: passed INLINE to session-launch.sh (a `machinectl shell` login
    # does NOT inherit `podman run -e` env, so we can't rely on container env).
    # XR_GPU_NODE is appended after boot (resolved in-container by gpu_node_in_ctr).
    local -a launch_env=(
        "HL_WAYLAND_DISPLAY=$ctr_wl"
        "XR_PASSTHROUGH=$passthrough"
    )
    # --env: hypxrpaper draws an ambient background as the PRIMARY session and
    # HypXRland composites its monitors on top as an XR_EXTX_overlay (session-launch.sh
    # forces openxr:overlay=1 when XR_ENV is set). 'pano'/'forest' need no host files;
    # a custom <path> must be reachable INSIDE the container (e.g. under /src, or add a
    # bind mount) — the bundled scenes come from the /src submodule checkout.
    [[ -n $env_spec ]] && launch_env+=("XR_ENV=$env_spec")
    [[ -n $user_conf ]] && {
        [[ -f $user_conf ]] || die "--conf file not found: $user_conf"
        local ctr_conf="$HOST_MNT/user.conf"
        mounts+=(-v "$(cd "$(dirname "$user_conf")" && pwd)/$(basename "$user_conf"):$ctr_conf:ro")
        launch_env+=("XR_BASE_CONF=$ctr_conf")
    }

    # --- host audio (PipeWire / PulseAudio) — optional, --no-audio to skip -----
    # Share the HOST audio daemon by bind-mounting its client sockets read-only
    # under $HOST_MNT (the container's /run/user/1000 is a systemd tmpfs and can't
    # be mounted into directly — same reason the wayland/wivrn sockets land here);
    # session-launch.sh symlinks them onto the default in-container discovery paths.
    # NO audio daemon runs in the container — the host's serves everything. Two
    # sockets for maximal app coverage: pipewire-0 (native-PipeWire clients) and
    # pulse/native (PulseAudio-shim clients — chromium, pactl, paplay/parecord). A
    # read-only bind is sufficient: an AF_UNIX connect() is not a filesystem write
    # (verified — pactl/pw-cli work over the :ro mount). Audio is optional: a host
    # without a running PipeWire just gets a notice and the session runs silent
    # (apps degrade gracefully). WiVRn note: with a headset connected, the headset
    # speakers/mic appear as ordinary host PipeWire devices, so they work in here
    # automatically through this same share — no headset-specific plumbing.
    if [[ $use_audio -eq 1 ]]; then
        local host_pw="$host_xdg/pipewire-0" host_pulse="$host_xdg/pulse/native"
        local audio_any=0
        if [[ -S $host_pw ]]; then
            mounts+=(-v "$host_pw:$HOST_MNT/pipewire-0:ro")
            launch_env+=("HL_PIPEWIRE_SOCK=$HOST_MNT/pipewire-0")
            audio_any=1
        fi
        if [[ -S $host_pulse ]]; then
            mounts+=(-v "$host_pulse:$HOST_MNT/pulse-native:ro")
            launch_env+=("HL_PULSE_SOCK=$HOST_MNT/pulse-native")
            audio_any=1
        fi
        if [[ $audio_any -eq 1 ]]; then
            log "Audio: sharing host audio read-only (pipewire-0=$([[ -S $host_pw ]] && echo yes || echo no)  pulse/native=$([[ -S $host_pulse ]] && echo yes || echo no))"
        else
            warn "Audio: host has no PipeWire/Pulse socket ($host_pw / $host_pulse) — session runs silent (non-fatal; pass --no-audio to silence this)."
        fi
    else
        log "Audio: --no-audio — host audio NOT shared into the session."
    fi

    local -a portargs=()
    if [[ $use_wivrn -eq 1 ]]; then
        # --- WiVRn (real headset via host runtime) ---
        [[ -f $WIVRN_MANIFEST ]]   || die "WiVRn manifest not found: $WIVRN_MANIFEST (is wivrn installed?)"
        [[ -d $WIVRN_LIB_DIR ]]    || die "WiVRn lib dir not found: $WIVRN_LIB_DIR"
        local host_wivrn="$host_xdg/wivrn/comp_ipc"
        if ! pgrep -x wivrn-server >/dev/null; then
            warn "wivrn-server is NOT running on the host — start it (wivrn-dashboard) and connect the"
            warn "headset first, or XR session creation will fail. Continuing so you can see the error."
        fi
        [[ -S $host_wivrn ]] || warn "host wivrn socket $host_wivrn absent (headset not connected yet?)"
        mounts+=(
            -v "$WIVRN_LIB_DIR:$WIVRN_LIB_DIR:ro"
            -v "$WIVRN_MANIFEST:/usr/share/openxr/1/openxr_wivrn.json:ro"
        )
        [[ -S $host_wivrn ]] && mounts+=(-v "$host_wivrn:$HOST_MNT/wivrn-comp_ipc")
        launch_env+=(
            "XR_MODE=wivrn"
            "XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_wivrn.json"
            "WIVRN_HOST_SOCK=$HOST_MNT/wivrn-comp_ipc"
        )
    else
        # --- windowed Monado (no headset) ---
        # session-launch.sh runs a CONTAINER-LOCAL rooted Xwayland (connected to the
        # same host wayland socket) as Monado's X target, so we need NO host X mount
        # and no host XWayland running — the Monado window appears on the host as the
        # Xwayland screen window.
        launch_env+=(
            "XR_MODE=windowed"
            "XR_RUNTIME_JSON=/build/monado/openxr_monado-dev.json"
        )
        # The monado remote-driver port is NOT published by default (a fixed
        # 127.0.0.1:4242 publish once poisoned a concurrent host suite run). Opt in
        # with --publish-remote for an EPHEMERAL free host port (or a fixed one via
        # --publish-remote=PORT); container-side stays 4242.
        if [[ $publish_remote -eq 1 ]]; then
            if [[ -z $remote_port ]]; then
                remote_port=$(find_free_tcp_port) || die "could not find a free TCP port to publish"
            fi
            if [[ $remote_port == 4242 ]] && pgrep -x monado-service >/dev/null; then
                die "--publish-remote=4242 but a host monado-service owns TCP 4242; pick another port or omit for ephemeral."
            fi
            portargs=(-p "127.0.0.1:${remote_port}:4242")
        fi
    fi

    local ctr="hypxrland-session-$$"
    # Expand $ctr NOW: the EXIT trap can fire after this function's scope is gone (podman run
    # dying under set -e), where a `local` is unbound — and a partial container still needs
    # removing in exactly that case.
    trap "podman rm -f '$ctr' >/dev/null 2>&1 || true" EXIT INT TERM

    log "Booting $IMG_SESSION (session, --gpu $gpu, mode $([[ $use_wivrn -eq 1 ]] && echo wivrn || echo windowed), devices: ${devargs[*]})"
    podman rm -f "$ctr" >/dev/null 2>&1 || true
    podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
        --security-opt label=disable \
        "${devargs[@]}" "${portargs[@]}" "${mounts[@]}" \
        "$IMG_SESSION" >/dev/null
    wait_for_systemd "$ctr"
    # Named volumes come up root-owned; make them writable by dev (uid 1000).
    podman exec "$ctr" chown -R dev:dev /build "$CCACHE_DIR_IN_CTR" >/dev/null 2>&1 || true

    # Resolve the render node(s) in-container (verifies the injected/mounted nodes).
    local nested_node="" xr_node=""
    if [[ $gpu == split ]]; then
        nested_node=$(gpu_node_in_ctr "$ctr" "$SPLIT_NESTED_SPEC")
        xr_node=$(gpu_node_in_ctr "$ctr" "$SPLIT_XR_SPEC")
        log "In-container split GPU nodes: nested(AQ)=$nested_node  XR(openxr:gpu)=$xr_node"
        launch_env+=("NESTED_GPU_NODE=$nested_node" "XR_GPU_NODE=$xr_node")
    else
        gpu_node=$(gpu_node_in_ctr "$ctr" "$gpu")
        log "In-container render node for --gpu $gpu: $gpu_node"
        launch_env+=("XR_GPU_NODE=$gpu_node")
    fi

    print_session_banner "$ctr" "$use_wivrn" "$remote_port" "$gpu" "$nested_node" "$xr_node"

    # Build the inline env prefix for the machinectl login (which won't inherit env).
    local envstr=""; local kv
    for kv in "${launch_env[@]}"; do envstr+="$kv "; done

    log "Launching Omarchy XR session (Ctrl-C or exit to tear down)"
    podman exec -it "$ctr" machinectl shell dev@.host /usr/bin/bash -lc \
        "env $envstr bash /src/containers/session/session-launch.sh" || true

    podman rm -f "$ctr" >/dev/null 2>&1 || true
    trap - EXIT INT TERM
    log "Session exited; container '$ctr' removed (whole session tree reaped)."
}

print_session_banner() {
    local ctr="$1" use_wivrn="$2" remote_port="$3" gpu="${4:-}" nested_node="${5:-}" xr_node="${6:-}"
    cat <<EOF

  ============================ HypXRland session ============================
  Container: $ctr   (removed on exit)
EOF
    if [[ $gpu == split ]]; then
        cat <<EOF
  GPU: split — nested compositor (AQ_DRM_DEVICES) on $nested_node,
                XR encode (openxr:gpu) on $xr_node.
EOF
    fi
    if [[ $use_wivrn -eq 1 ]]; then
        cat <<EOF
  Mode: WiVRn (real headset via the HOST runtime).
    Put the headset ON — the XR monitors render there. A nested Omarchy window
    also appears on this desktop (the flat view + where keyboard/mouse go).
    If the session never reaches FOCUSED, the headset isn't connected or the
    host wivrn-server isn't running.
EOF
    else
        cat <<EOF
  Mode: windowed Monado (no headset). Expect TWO new windows on your desktop:
    - the nested Omarchy session (themed waybar at the top, wallpaper, …)
    - the Monado compositor window (the 3D XR view with the floating monitors)
EOF
        if [[ -n $remote_port ]]; then
            cat <<EOF
    Monado remote-driver port published on 127.0.0.1:$remote_port  (container 4242).
    Drive the fake HMD/controllers from the host:
      subprojects/monado/build/src/xrt/targets/gui/monado-gui remote 127.0.0.1:$remote_port
      (or the in-container gui: $0 exec monado-gui remote)
EOF
        else
            cat <<EOF
    (Remote-driver port NOT published — pass --publish-remote to drive the fake
     HMD/controllers with monado-gui from the host on an ephemeral port.)
EOF
        fi
    fi
    cat <<EOF

  Talk to the nested session from the host:
    $0 exec hyprctl openxr status
    $0 exec hyprctl monitors
    $0 exec hyprctl dispatch exec alacritty     # a terminal in the nested session
  Private-bus / isolation proof (all run INSIDE the container):
    $0 exec busctl --user list        # org.freedesktop.Notifications owner = container
    $0 exec ls /dev/input             # empty: no physical input devices
    $0 exec hyprctl devices           # only the nested wayland seat
  ===========================================================================
EOF
}

# --- exec: run a command in the RUNNING session container as dev --------------
cmd_exec() {
    [[ $# -ge 1 ]] || die "exec: need a command (e.g. $0 exec hyprctl openxr status)"
    local ctr
    ctr=$(podman ps --format '{{.Names}}' 2>/dev/null | grep '^hypxrland-session-' | head -1 || true)
    [[ -n $ctr ]] || die "no running hypxrland-session-* container (start one with: $0 session)"
    # Quote each arg so the remote bash -lc sees them intact.
    local q="" a
    for a in "$@"; do q+="$(printf '%q ' "$a")"; done
    podman exec -it "$ctr" machinectl shell dev@.host /usr/bin/bash -lc "$q"
}

# --- main --------------------------------------------------------------------
usage() { grep -E '^# ' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

[[ $# -ge 1 ]] || { usage; exit 1; }
sub="$1"; shift || true
case "$sub" in
    build)     cmd_build "$@" ;;
    shell)     cmd_shell "$@" ;;
    check-gpu)
        gpu="amd"
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --gpu) gpu="$2"; shift ;;
                --gpu=*) gpu="${1#--gpu=}" ;;
                *) die "check-gpu: unknown flag $1" ;;
            esac; shift
        done
        # Prefer the most-built image for the probe.
        img="$IMG_BASE"
        podman image exists "$IMG_SESSION" && img="$IMG_SESSION"
        check_gpu "$gpu" "$img" ;;
    session)   cmd_session "$@" ;;
    exec)      cmd_exec "$@" ;;
    test)      cmd_test "$@" ;;
    -h|--help) usage ;;
    *) usage; die "unknown subcommand: $sub" ;;
esac
