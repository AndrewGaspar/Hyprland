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
#   shell [--gpu nvidia|amd]
#       Boot :session and drop into a real logind shell as `dev` (machinectl
#       shell). Repo is overlay-mounted at /src (host tree untouched), build
#       tree in the hypxrland-build volume at /build, ccache in hypxrland-ccache.
#       Build inside with:  bash /src/containers/build-in-ctr.sh
#       Container is removed on shell exit.
#
#   check-gpu [--gpu nvidia|amd]
#       Standalone GPU smoke test (same as build --check-gpu).
#
#   test [--gpu nvidia|amd] [--build] [--keep] [TEST_NAMES...]
#       Hermetic in-container `hyprtester --xr` (WP2). Boots :session with NO
#       host wayland/X/wivrn mounts, brings up a headless labwc as the nesting
#       host inside, runs the XR suite nested into it, and reports the real
#       exit code (sentinel file — machinectl shell always exits 0). Everything
#       (Hyprland + vendored monado null-compositor) runs inside the container.
#       --build   force an in-container build (build-in-ctr.sh) before testing;
#                 otherwise auto-builds only if /build has no binaries.
#       --keep    leave the container running afterwards (for debugging).
#       TEST_NAMES  optional subset of xr test names to run (else the whole
#                 group), passed straight through to hyprtester.
#       On failure, artifacts (run log + preserved /tmp/hyprtester-xr-* dirs
#       with hyprland + monado logs) are copied to containers/artifacts/<ts>/.
#
#   session [--wivrn] [--conf FILE] [--gpu nvidia|amd] [--passthrough]
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
#         --gpu nvidia|amd       single-GPU pin (default amd = /dev/dri/renderD129).
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
# GPU: NVIDIA needs the host CDI spec (nvidia-container-toolkit +
# `nvidia-ctk cdi generate`); if it's absent this script prints the exact setup
# commands and (for build/check-gpu) falls back to AMD. AMD uses a plain
# --device /dev/dri/renderD129.

set -euo pipefail

# --- constants ---------------------------------------------------------------
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
ARTIFACTS_DIR="$REPO/containers/artifacts"

# Host GPU render nodes (this box): renderD128 = NVIDIA, renderD129 = AMD.
AMD_RENDER_NODE="${HYPXRLAND_AMD_NODE:-/dev/dri/renderD129}"
NVIDIA_RENDER_NODE="${HYPXRLAND_NVIDIA_NODE:-/dev/dri/renderD128}"
CDI_NVIDIA_SPEC_YAML="/etc/cdi/nvidia.yaml"
CDI_NVIDIA_SPEC_JSON="/etc/cdi/nvidia.json"

# session mounts: host XR bits land under this container-side dir (a plain
# top-level mount point, NOT under systemd-managed /run, so nothing shadows it).
HOST_MNT="/hypxrland-host"
WIVRN_MANIFEST="${WIVRN_RUNTIME_JSON:-/usr/share/openxr/1/openxr_wivrn.json}"
WIVRN_LIB_DIR="/usr/lib/wivrn"
CDI_NVIDIA_SPEC_YAML="/etc/cdi/nvidia.yaml"
CDI_NVIDIA_SPEC_JSON="/etc/cdi/nvidia.json"

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

# resolve_gpu_args <gpu> — echo the podman --device args for the requested GPU,
# or fail with setup guidance. For nvidia, verifies the CDI spec exists.
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

# --- check-gpu ---------------------------------------------------------------
# Smoke-test GPU access inside a short-lived container: eglinfo + vulkaninfo.
check_gpu() {
    local gpu="${1:-amd}" img="${2:-$IMG_BASE}"
    podman image exists "$img" || die "image $img not built yet (run: $0 build)"

    local -a devargs=()
    case "$gpu" in
        nvidia)
            if nvidia_cdi_available; then
                devargs=(--device nvidia.com/gpu=all)
            else
                warn "requested --gpu nvidia but CDI is unavailable on this host."
                print_nvidia_cdi_setup
                warn "falling back to AMD ($AMD_RENDER_NODE) for the smoke test."
                gpu=amd
            fi
            ;;
    esac
    [[ $gpu == amd ]] && devargs=(--device "$AMD_RENDER_NODE")

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

    local -a devargs=()
    case "$gpu" in
        nvidia)
            nvidia_cdi_available || { print_nvidia_cdi_setup; die "NVIDIA CDI unavailable; use --gpu amd"; }
            devargs=(--device nvidia.com/gpu=all) ;;
        amd) devargs=(--device "$AMD_RENDER_NODE") ;;
        *) die "shell: --gpu must be nvidia or amd" ;;
    esac

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

# --- test (WP2): hermetic in-container `hyprtester --xr` ----------------------
# Boots :session (no host wayland/X/wivrn mounts — hermetic by construction),
# ensures /build is populated (auto-builds if not), runs containers/test/
# run-xr-tests.sh as dev via machinectl (labwc headless nest -> hyprtester --xr),
# reads the real exit code from a sentinel, copies artifacts out on failure, and
# removes the container on ALL exit paths.
cmd_test() {
    local gpu="amd" keep=0 do_build=0
    local -a testnames=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --gpu)     [[ $# -ge 2 ]] || die "--gpu needs an argument (nvidia|amd)"; gpu="$2"; shift ;;
            --gpu=*)   gpu="${1#--gpu=}" ;;
            --keep)    keep=1 ;;
            --build)   do_build=1 ;;
            -h|--help) usage; exit 0 ;;
            -*)        die "test: unknown flag $1" ;;
            *)         testnames+=("$1") ;;   # hyprtester test-name filter (subset)
        esac
        shift
    done
    podman image exists "$IMG_SESSION" || die "session image not built (run: $0 build)"
    [[ -f $RUN_XR_TESTS_SCRIPT ]] || die "missing $RUN_XR_TESTS_SCRIPT"

    # GPU device args + the render node the suite pins (single-GPU by construction).
    local gpu_node
    local -a devargs=()
    case "$gpu" in
        nvidia)
            nvidia_cdi_available || { print_nvidia_cdi_setup; die "NVIDIA CDI unavailable on this host; use --gpu amd"; }
            devargs=(--device nvidia.com/gpu=all)
            # CDI injects the driver; renderD128 is this box's NVIDIA node.
            gpu_node="${HYPXRLAND_NVIDIA_NODE:-/dev/dri/renderD128}" ;;
        amd)
            devargs=(--device "$AMD_RENDER_NODE")
            gpu_node="$AMD_RENDER_NODE" ;;
        *) die "test: --gpu must be nvidia or amd" ;;
    esac

    ensure_volumes "$VOL_BUILD" "$VOL_CCACHE"
    local ctr="hypxrland-test-$$"
    local removed=0
    cleanup_test() { [[ $removed -eq 1 || $keep -eq 1 ]] || { podman rm -f "$ctr" >/dev/null 2>&1 || true; }; }
    trap cleanup_test EXIT INT TERM

    # HERMETIC: mounts are ONLY the overlay source, the build+ccache volumes and
    # the GPU device — NO host wayland/X11/wivrn sockets. (Proven in the report by
    # echoing this invocation.) --systemd=always gives real logind for machinectl.
    log "Booting $IMG_SESSION (test, --gpu $gpu, node $gpu_node)"
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
    local sentinel="/tmp/hypxrland-xr-tests.exit"
    log "Running hermetic XR suite in-container (labwc nest -> hyprtester --xr)"
    podman exec "$ctr" rm -f "$sentinel" >/dev/null 2>&1 || true
    podman exec "$ctr" machinectl shell dev@.host /usr/bin/bash -lc \
        "XR_GPU_NODE='$gpu_node' bash /src/containers/test/run-xr-tests.sh ${testnames[*]:-}" || true

    local rc
    rc=$(podman exec "$ctr" cat "$sentinel" 2>/dev/null || echo missing)
    log "XR suite sentinel exit code: $rc"

    if [[ "$rc" != 0 ]]; then
        # Preserve artifacts: the combined run log + every preserved
        # /tmp/hyprtester-xr-* dir (the harness keeps these on failure, incl.
        # hyprland logs + monado.log).
        local ts stamp
        ts=$(date +%Y%m%d-%H%M%S)
        stamp="$ARTIFACTS_DIR/$ts"
        mkdir -p "$stamp"
        warn "XR suite failed (rc=$rc) — collecting artifacts into $stamp"
        podman cp "$ctr:/tmp/hypxrland-xr-tests.log" "$stamp/xr-tests.log" >/dev/null 2>&1 || true
        # copy each preserved run dir
        local dirs
        dirs=$(podman exec "$ctr" bash -lc 'ls -d /tmp/hyprtester-xr-* 2>/dev/null' || true)
        local d
        for d in $dirs; do
            podman cp "$ctr:$d" "$stamp/$(basename "$d")" >/dev/null 2>&1 || true
        done
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
        log "XR suite PASSED in-container (hermetic, --gpu $gpu)"
        return 0
    fi
    die "XR suite FAILED in-container (rc=$rc)"
}

# --- session -----------------------------------------------------------------
# resolve_gpu_devargs <gpu> — set REPLY_DEVARGS (array) + REPLY_GPU_NODE for the
# requested GPU, or die with setup guidance. (Shared by session; mirrors the
# inline logic in cmd_shell without restructuring it.)
resolve_gpu_devargs() {
    local gpu="$1"
    REPLY_DEVARGS=()
    case "$gpu" in
        nvidia)
            nvidia_cdi_available || { print_nvidia_cdi_setup; die "NVIDIA CDI unavailable; use --gpu amd"; }
            REPLY_DEVARGS=(--device nvidia.com/gpu=all)
            REPLY_GPU_NODE="$NVIDIA_RENDER_NODE" ;;
        amd)
            REPLY_DEVARGS=(--device "$AMD_RENDER_NODE")
            REPLY_GPU_NODE="$AMD_RENDER_NODE" ;;
        *) die "--gpu must be nvidia or amd" ;;
    esac
}

cmd_session() {
    local gpu="amd" use_wivrn=0 passthrough=0 user_conf=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --wivrn)      use_wivrn=1 ;;
            --passthrough) passthrough=1 ;;
            --gpu)        [[ $# -ge 2 ]] || die "--gpu needs an argument"; gpu="$2"; shift ;;
            --gpu=*)      gpu="${1#--gpu=}" ;;
            --conf)       [[ $# -ge 2 ]] || die "--conf needs a FILE argument"; user_conf="$2"; shift ;;
            --conf=*)     user_conf="${1#--conf=}" ;;
            -h|--help)    usage; exit 0 ;;
            *) die "session: unknown flag $1" ;;
        esac
        shift
    done
    podman image exists "$IMG_SESSION" || die "session image not built (run: $0 build)"

    resolve_gpu_devargs "$gpu"   # sets REPLY_DEVARGS + REPLY_GPU_NODE
    local gpu_node="$REPLY_GPU_NODE"
    local -a devargs=("${REPLY_DEVARGS[@]}")

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
    local -a launch_env=(
        "XR_GPU_NODE=$gpu_node"
        "HL_WAYLAND_DISPLAY=$ctr_wl"
        "XR_PASSTHROUGH=$passthrough"
    )
    [[ -n $user_conf ]] && {
        [[ -f $user_conf ]] || die "--conf file not found: $user_conf"
        local ctr_conf="$HOST_MNT/user.conf"
        mounts+=(-v "$(cd "$(dirname "$user_conf")" && pwd)/$(basename "$user_conf"):$ctr_conf:ro")
        launch_env+=("XR_BASE_CONF=$ctr_conf")
    }

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
        # Publish the monado remote-driver port so you can drive it with monado-gui
        # from the host — ONLY when the host isn't already using 4242 (one per box).
        if pgrep -x monado-service >/dev/null; then
            warn "a host monado-service is running (owns TCP 4242) — NOT publishing the port."
            warn "monado-gui remote drive from the host is unavailable this run."
        else
            portargs=(-p 127.0.0.1:4242:4242)
        fi
    fi

    local ctr="hypxrland-session-$$"
    cleanup_session() { podman rm -f "$ctr" >/dev/null 2>&1 || true; }
    trap cleanup_session EXIT INT TERM

    log "Booting $IMG_SESSION (session, --gpu $gpu, mode $([[ $use_wivrn -eq 1 ]] && echo wivrn || echo windowed))"
    podman rm -f "$ctr" >/dev/null 2>&1 || true
    podman run -d --name "$ctr" --systemd=always --userns=keep-id --user root \
        --security-opt label=disable \
        "${devargs[@]}" "${portargs[@]}" "${mounts[@]}" \
        "$IMG_SESSION" >/dev/null
    wait_for_systemd "$ctr"
    # Named volumes come up root-owned; make them writable by dev (uid 1000).
    podman exec "$ctr" chown -R dev:dev /build "$CCACHE_DIR_IN_CTR" >/dev/null 2>&1 || true

    print_session_banner "$ctr" "$use_wivrn" "${portargs[*]:-}"

    # Build the inline env prefix for the machinectl login (which won't inherit env).
    local envstr=""; local kv
    for kv in "${launch_env[@]}"; do envstr+="$kv "; done

    log "Launching Omarchy XR session (Ctrl-C or exit to tear down)"
    podman exec -it "$ctr" machinectl shell dev@.host /usr/bin/bash -lc \
        "env $envstr bash /src/containers/session/session-launch.sh" || true

    cleanup_session; trap - EXIT INT TERM
    log "Session exited; container '$ctr' removed (whole session tree reaped)."
}

print_session_banner() {
    local ctr="$1" use_wivrn="$2" ports="$3"
    cat <<EOF

  ============================ HypXRland session ============================
  Container: $ctr   (removed on exit)
EOF
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
        [[ -n $ports ]] && cat <<EOF
    Drive the fake HMD/controllers from the host (port published):
      subprojects/monado/build/src/xrt/targets/gui/monado-gui remote
      (or the in-container gui: xr-container.sh exec monado-gui remote)
EOF
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
