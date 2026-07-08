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
#   session / test
#       Stubs — implemented in WP2 (test) and WP3 (session).
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

# Host GPU render nodes (this box): renderD128 = NVIDIA, renderD129 = AMD.
AMD_RENDER_NODE="${HYPXRLAND_AMD_NODE:-/dev/dri/renderD129}"
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
    session)   die "session: not implemented in WP1 (interactive session lands in WP3)" ;;
    test)      die "test: not implemented in WP1 (hermetic --xr suite lands in WP2)" ;;
    -h|--help) usage ;;
    *) usage; die "unknown subcommand: $sub" ;;
esac
