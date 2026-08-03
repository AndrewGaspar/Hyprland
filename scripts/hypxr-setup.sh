#!/usr/bin/env bash
# hypxr-setup.sh — bootstrap (or update) the whole HypXRland stack on a machine.
#
# The compositor is only one of eight moving parts. A working HypXRland desk is
# the compositor + the WiVRn server + the voice daemon + the shared HUD + the
# VA-API gate + the ambient-background client + the dotfiles device branch + a
# session environment that ties them together. This script installs, updates and
# VERIFIES all of them, component by component, on Arch/Omarchy and on
# Fedora/omedora.
#
#   hypxr-setup.sh --check              report on every component, change nothing
#   hypxr-setup.sh --check wivrn hud    report on just those
#   hypxr-setup.sh all                  install/update everything (except monado)
#   hypxr-setup.sh compositor voice     install/update just those
#   hypxr-setup.sh monado               the OPTIONAL XREAL runtime (opt-in only)
#
# Components (in dependency order):
#   deps        build prerequisites — PRINTS the package command, never runs it
#   dotfiles    ~/.config/hypr (device branch) + setup-xr-display.sh
#   env         ~/.config/uwsm/env session-environment additions
#   compositor  ~/code/hypxrland worktree + build + the wayland-session entry
#   wivrn       the patched wivrn-server worktree + unit override + config + pairing
#   voice       hypxrvoice (models, build, config, user unit)
#   hud         hypxrhud (build, /usr/local install, two user units)
#   va          hypxrva (user-local install + session env + watcher)
#   paper       hypxrpaper (ambient background, used by overlay mode + tests)
#   monado      OPTIONAL: the XREAL Air 2 Ultra 3DoF runtime flavor
#
# TWO THINGS THIS SCRIPT NEVER DOES
#   1. sudo. Anything needing root (a wayland-session .desktop, /usr/local
#      installs, packages) is PRINTED for you to run and re-checked afterwards.
#   2. Destroy your work. A repo with uncommitted changes is never reset; the
#      component stops and reports instead.
#
# PER-MACHINE VARIABLES live in ~/.config/hypxr/setup.env (a plain shell file,
# sourced if present). Everything in it is auto-detected when unset, and every
# resolved value is printed under "Machine profile" so you can see what this
# script believes about the box. Write the file when a guess is wrong. See
# docs/openxr/08-machine-setup.md for the full table.
#
# UPDATING AN EXISTING INSTALL is the common case, not the rare one, and the
# hypxrland branch HAS been force-pushed (the v0.56.0 rebase). Fetch + hard
# reset is therefore the documented update path, and any component that resets a
# repo also DELETES its build directory: stale CMake/ninja caches survive a
# history swap and fail later with missing-rule / soname errors that look like
# source bugs. This script does both, together, every time.

set -uo pipefail

# ---------------------------------------------------------------------------
# Presentation
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    B=$'\e[1m'; R=$'\e[0m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RED=$'\e[31m'; DIM=$'\e[2m'; CYA=$'\e[36m'
else
    B=""; R=""; GRN=""; YEL=""; RED=""; DIM=""; CYA=""
fi

n_ok=0; n_did=0; n_warn=0; n_fail=0
declare -a MANUAL_STEPS=()

ok()     { echo "  ${GRN}✓${R} $*"; n_ok=$((n_ok + 1)); }
did()    { echo "  ${GRN}+${R} $*"; n_did=$((n_did + 1)); }
warn()   { echo "  ${YEL}!${R} $*"; n_warn=$((n_warn + 1)); }
fail()   { echo "  ${RED}✗${R} $*"; n_fail=$((n_fail + 1)); }
note()   { echo "    ${DIM}$*${R}"; }
header() { echo; echo "${B}$*${R}"; }
manual() { MANUAL_STEPS+=("$1"); echo "  ${CYA}»${R} run yourself: ${CYA}$1${R}"; }

tilde() { printf '%s' "${1/#$HOME/\~}"; }

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------

ALL_COMPONENTS=(deps dotfiles env compositor wivrn voice hud va paper)
OPTIONAL_COMPONENTS=(monado)

CHECK_ONLY=0
declare -a WANT=()

usage() { sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --check)     CHECK_ONLY=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        all)         WANT+=("${ALL_COMPONENTS[@]}"); shift ;;
        deps|dotfiles|env|compositor|wivrn|voice|hud|va|paper|monado)
                     WANT+=("$1"); shift ;;
        -*)          echo "hypxr-setup.sh: unknown option '$1'" >&2; exit 2 ;;
        *)           echo "hypxr-setup.sh: unknown component '$1'" >&2
                     echo "valid: ${ALL_COMPONENTS[*]} ${OPTIONAL_COMPONENTS[*]} all" >&2; exit 2 ;;
    esac
done

# `--check` with no component list means "audit the whole box" (monado is
# optional, so it is only checked when asked for, or when it is clearly in use).
if [[ ${#WANT[@]} -eq 0 ]]; then
    if ((CHECK_ONLY)); then
        WANT=("${ALL_COMPONENTS[@]}")
    else
        usage; exit 2
    fi
fi

wants() { local c; for c in "${WANT[@]}"; do [[ $c == "$1" ]] && return 0; done; return 1; }

# ---------------------------------------------------------------------------
# Machine profile — auto-detected, overridable from ~/.config/hypxr/setup.env
# ---------------------------------------------------------------------------

PROFILE_FILE="${HYPXR_PROFILE:-${XDG_CONFIG_HOME:-$HOME/.config}/hypxr/setup.env}"
# shellcheck disable=SC1090
[[ -r $PROFILE_FILE ]] && . "$PROFILE_FILE"

detect_distro() {
    local id="" like=""
    if [[ -r /etc/os-release ]]; then
        id=$(. /etc/os-release 2>/dev/null; printf '%s' "${ID:-}")
        like=$(. /etc/os-release 2>/dev/null; printf '%s' "${ID_LIKE:-}")
    fi
    case "$id" in
        arch)   echo arch; return ;;
        fedora) echo fedora; return ;;
    esac
    case " $like " in
        *" fedora "*) echo fedora; return ;;
        *" arch "*)   echo arch; return ;;
    esac
    echo unknown
}

# Kernel driver behind a render node ("amdgpu", "i915", "xe", "nvidia", ...).
node_driver() {
    local node="${1##*/}"
    sed -n 's/^DRIVER=//p' "/sys/class/drm/$node/device/uevent" 2>/dev/null | head -1
}

# The VA-API driver name matching a kernel driver. Mirrors hypxrva's own
# mapping (src/shim/resolve.c) so the wivrn bypass and the shim agree.
vaapi_for_driver() {
    case "$1" in
        amdgpu)     echo radeonsi ;;
        radeon)     echo radeonsi ;;
        i915|xe)    echo iHD ;;
        nvidia)     echo nvidia ;;
        *)          echo "" ;;
    esac
}

# The Vulkan ICD manifest matching a kernel driver, when one is installed.
icd_for_driver() {
    local d=$1 j
    case "$d" in
        amdgpu|radeon) j=radeon_icd.json ;;
        i915|xe)       j=intel_icd.json ;;
        nvidia)        j=nvidia_icd.json ;;
        *)             return 1 ;;
    esac
    [[ -e /usr/share/vulkan/icd.d/$j ]] && { echo "/usr/share/vulkan/icd.d/$j"; return 0; }
    return 1
}

DISTRO="${HYPXR_DISTRO:-$(detect_distro)}"

# All render nodes, and the "XR" one: the node the XR runtime's compositor
# renders on. Default = the first non-NVIDIA node (WiVRn's Vulkan compositor is
# pinned to the integrated/AMD GPU on every machine we run today); a single-GPU
# box trivially resolves to its only node.
declare -a RENDER_NODES=()
while IFS= read -r n; do RENDER_NODES+=("$n"); done < <(ls -1 /dev/dri/renderD* 2>/dev/null)

default_gpu_node() {
    local n
    for n in "${RENDER_NODES[@]}"; do
        [[ $(node_driver "$n") == nvidia ]] && continue
        echo "$n"; return
    done
    [[ ${#RENDER_NODES[@]} -gt 0 ]] && echo "${RENDER_NODES[0]}"
}

HYPXR_GPU_NODE="${HYPXR_GPU_NODE:-$(default_gpu_node)}"
GPU_DRIVER="$(node_driver "${HYPXR_GPU_NODE:-none}")"
HYPXR_VAAPI_DRIVER="${HYPXR_VAAPI_DRIVER:-$(vaapi_for_driver "$GPU_DRIVER")}"
HYPXR_VK_ICD="${HYPXR_VK_ICD:-$(icd_for_driver "$GPU_DRIVER" || true)}"

# The system VA driver directory. Arch: /usr/lib/dri. Fedora: /usr/lib64/dri.
# libva itself is the authority; the distro guess is only a fallback.
default_libva_dir() {
    local d
    d=$(pkg-config --variable=driverdir libva 2>/dev/null)
    [[ -n $d && -d $d ]] && { echo "$d"; return; }
    [[ -d /usr/lib64/dri ]] && { echo /usr/lib64/dri; return; }
    echo /usr/lib/dri
}
HYPXR_LIBVA_DRIVERS_PATH="${HYPXR_LIBVA_DRIVERS_PATH:-$(default_libva_dir)}"

# Hybrid box => the GPU-policy masks (AQ pin, per-app EGL/VK pins, the hud
# no-nvidia drop-ins) are load-bearing. Single-GPU box => they are all noise.
HYBRID_GPU=0
for n in "${RENDER_NODES[@]}"; do
    [[ $(node_driver "$n") == nvidia ]] && HYBRID_GPU=1
done

# Repo locations. Every one is overridable; the defaults are what this stack
# has used since the beginning.
CODE="${HYPXR_CODE_DIR:-$HOME/code}"
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)          # this Hyprland checkout
FISHFOOD="${HYPXRLAND_FISHFOOD:-$CODE/hypxrland}"
FISHFOOD_BRANCH="${HYPXRLAND_FISHFOOD_BRANCH:-fishfood}"
FISHFOOD_TRACK="${HYPXRLAND_FISHFOOD_TRACK:-hypxrland}"
WIVRN_TAG="${HYPXR_WIVRN_TAG:-26.6.2}"
WIVRN_SRC="${HYPXR_WIVRN_SRC:-$CODE/wivrn}"
WIVRN_WT="${HYPXR_WIVRN_WORKTREE:-$CODE/wivrn-$WIVRN_TAG}"
WIVRN_BRANCH="${HYPXR_WIVRN_BRANCH:-hypxr-patches-$WIVRN_TAG}"
VOICE_DIR="${HYPXR_VOICE_DIR:-$CODE/hypxrvoice}"
HUD_DIR="${HYPXR_HUD_DIR:-$CODE/hypxrhud}"
HUD_PREFIX="${HYPXR_HUD_PREFIX:-/usr/local}"
VA_DIR="${HYPXR_VA_DIR:-$CODE/hypxrva}"
PAPER_DIR="${HYPXR_PAPER_DIR:-$CODE/hypxrpaper}"
DOTFILES="${HYPXR_DOTFILES:-${XDG_CONFIG_HOME:-$HOME/.config}/hypr}"
DOTFILES_URL="${HYPXR_DOTFILES_URL:-https://github.com/AndrewGaspar/omarchy-hyprland-config}"
XRCONF="${HYPXRLAND_FISHFOOD_CONF:-$DOTFILES/hyprland-xr.conf}"
JOBS="${HYPXRLAND_FISHFOOD_JOBS:-8}"    # this box has frozen twice under heavier parallelism
LOCK=/tmp/hypxrland-build.lock

CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
UNIT_DIR="$CONFIG_HOME/systemd/user"
LAUNCH_DIR="$DATA_HOME/hypxrland"

GH="${HYPXR_GITHUB:-https://github.com/AndrewGaspar}"

print_profile() {
    header "Machine profile"
    echo "  ${DIM}(override any of these in $(tilde "$PROFILE_FILE"))${R}"
    printf '    %-26s %s\n' "distro"                 "$DISTRO"
    printf '    %-26s %s\n' "render nodes"           "${RENDER_NODES[*]:-none} $(
        for n in "${RENDER_NODES[@]}"; do printf '[%s=%s] ' "${n##*/}" "$(node_driver "$n")"; done)"
    printf '    %-26s %s\n' "HYPXR_GPU_NODE"         "${HYPXR_GPU_NODE:-<none>} (driver: ${GPU_DRIVER:-?})"
    printf '    %-26s %s\n' "HYPXR_VAAPI_DRIVER"     "${HYPXR_VAAPI_DRIVER:-<unknown>}"
    printf '    %-26s %s\n' "HYPXR_VK_ICD"           "${HYPXR_VK_ICD:-<none>}"
    printf '    %-26s %s\n' "HYPXR_LIBVA_DRIVERS_PATH" "$HYPXR_LIBVA_DRIVERS_PATH"
    printf '    %-26s %s\n' "GPU policy"             "$( ((HYBRID_GPU)) && echo 'hybrid (NVIDIA present — masks apply)' || echo 'single GPU (no masks needed)')"
    printf '    %-26s %s\n' "dotfiles branch"        "$(git -C "$DOTFILES" rev-parse --abbrev-ref HEAD 2>/dev/null || echo '<no checkout>')"
    printf '    %-26s %s\n' "compositor worktree"    "$(tilde "$FISHFOOD") [$FISHFOOD_BRANCH]"
    printf '    %-26s %s\n' "wivrn worktree"         "$(tilde "$WIVRN_WT") [$WIVRN_BRANCH]"
}

# ---------------------------------------------------------------------------
# Generic helpers
# ---------------------------------------------------------------------------

have() { command -v "$1" >/dev/null 2>&1; }

# clone_or_sync <dir> <url> <branch> [--worktree-of <repo>]
#
# The update path. Fetches, then hard-resets the branch onto its remote tip —
# which is REQUIRED here, because hypxrland and the wivrn patch branch have both
# been force-pushed (history rewrites) and a merge/pull cannot cross that. Sets
# REPO_CHANGED=1 when HEAD actually moved, so the caller can nuke its build dir.
#
# Refuses outright on a dirty tree: losing hand-edits to a live config or a
# work-in-progress patch is not an acceptable cost of "setup".
REPO_CHANGED=0
clone_or_sync() {
    local dir="$1" url="$2" branch="$3" label; label="$(tilde "$dir")"
    REPO_CHANGED=0

    if [[ ! -e $dir/.git ]]; then
        if ((CHECK_ONLY)); then
            fail "$label is not a git checkout"
            note "run without --check to clone $url"
            return 1
        fi
        git clone "$url" "$dir" || { fail "clone of $url failed"; return 1; }
        git -C "$dir" checkout -B "$branch" "origin/$branch" || true
        did "$label cloned at $(git -C "$dir" rev-parse --short HEAD)"
        REPO_CHANGED=1
        return 0
    fi

    local cur; cur=$(git -C "$dir" rev-parse --abbrev-ref HEAD 2>/dev/null)
    local dirty; dirty=$(git -C "$dir" status --porcelain --untracked-files=no 2>/dev/null | wc -l)

    if ((CHECK_ONLY)); then
        [[ $cur == "$branch" ]] && ok "$label on branch $branch ($(git -C "$dir" rev-parse --short HEAD))" \
                                || warn "$label is on '$cur', expected '$branch'"
        ((dirty)) && warn "$label has $dirty uncommitted change(s) — an update would refuse to reset"
        # Behind/diverged, without touching the network.
        local up; up=$(git -C "$dir" rev-parse --abbrev-ref '@{u}' 2>/dev/null)
        if [[ -n $up ]]; then
            local counts; counts=$(git -C "$dir" rev-list --left-right --count "$up...HEAD" 2>/dev/null)
            local behind=${counts%%[[:space:]]*} ahead=${counts##*[[:space:]]}
            if [[ ${behind:-0} -gt 0 || ${ahead:-0} -gt 0 ]]; then
                note "vs $up: $behind behind, $ahead ahead (a fetch may change this)"
            fi
        fi
        return 0
    fi

    if ((dirty)); then
        fail "$label has $dirty uncommitted change(s) — refusing to touch it"
        note "commit or stash them, then re-run this component"
        return 1
    fi

    local before; before=$(git -C "$dir" rev-parse HEAD)
    git -C "$dir" fetch origin --prune || { fail "fetch failed in $label"; return 1; }
    if ! git -C "$dir" rev-parse --verify -q "origin/$branch" >/dev/null; then
        fail "$label: origin/$branch does not exist"
        return 1
    fi
    git -C "$dir" checkout -B "$branch" --no-track "origin/$branch" >/dev/null 2>&1 \
        || git -C "$dir" checkout "$branch" >/dev/null 2>&1
    git -C "$dir" branch --set-upstream-to "origin/$branch" "$branch" >/dev/null 2>&1
    # Hard reset: crosses a force-push, which is exactly what we need.
    git -C "$dir" reset --hard "origin/$branch" >/dev/null || { fail "reset failed in $label"; return 1; }
    # Submodule URLs live in the checkout, not in .gitmodules: sync BEFORE update
    # or a moved submodule remote fails with "not our ref".
    if [[ -f $dir/.gitmodules ]]; then
        git -C "$dir" submodule sync --recursive >/dev/null
        git -C "$dir" submodule update --init --recursive >/dev/null || warn "$label: submodule update reported errors"
    fi
    local after; after=$(git -C "$dir" rev-parse HEAD)
    if [[ $before == "$after" ]]; then
        ok "$label already at origin/$branch ($(git -C "$dir" rev-parse --short HEAD))"
    else
        did "$label updated $(git -C "$dir" rev-parse --short "$before") -> $(git -C "$dir" rev-parse --short HEAD)"
        REPO_CHANGED=1
    fi
}

# A build tree that survived a history swap is a liability, not an optimization:
# ninja keeps rules for files that no longer exist and CMake caches sonames that
# the system libraries have since bumped. Both fail late and confusingly.
drop_stale_build() {
    local d="$1"
    [[ -d $d ]] || return 0
    ((CHECK_ONLY)) && return 0
    rm -rf -- "$d"
    did "removed stale build tree $(tilde "$d") (history moved; ccache keeps the rebuild cheap)"
}

# file_present <path> <description>
file_present() {
    if [[ -e $1 ]]; then ok "$2"; return 0; fi
    fail "$2 — missing: $(tilde "$1")"; return 1
}

exe_present() {
    if [[ -x $1 ]]; then ok "$2"; return 0; fi
    fail "$2 — missing or not executable: $(tilde "$1")"; return 1
}

unit_state() { systemctl --user "$1" "$2" 2>/dev/null; }

# check_unit <unit> [--want-enabled]
check_unit() {
    local u="$1" want_enabled="${2:-}"
    local en ac
    en=$(unit_state is-enabled "$u"); ac=$(unit_state is-active "$u")
    if [[ -z $(systemctl --user show -p FragmentPath --value "$u" 2>/dev/null) ]]; then
        fail "$u: no unit file found"
        return 1
    fi
    if [[ $want_enabled == --want-enabled && $en != enabled ]]; then
        warn "$u is $en (expected enabled)"
    else
        ok "$u: $en / $ac"
        return 0
    fi
}

enable_unit() {
    ((CHECK_ONLY)) && return 0
    systemctl --user daemon-reload
    systemctl --user enable --now "$1" >/dev/null 2>&1 \
        && did "$1 enabled and started" \
        || warn "$1 could not be enabled (is a graphical session running?)"
}

# Write a file only if absent or different; never in --check mode.
install_file() {
    local dst="$1" desc="$2"; shift 2
    local content; content="$(cat)"
    if [[ -f $dst ]] && [[ "$(cat "$dst")" == "$content" ]]; then
        ok "$desc already in place"
        return 0
    fi
    if ((CHECK_ONLY)); then
        [[ -e $dst ]] && warn "$desc differs from the shipped template — leaving it alone" \
                      || fail "$desc missing: $(tilde "$dst")"
        return 1
    fi
    if [[ -e $dst ]]; then
        warn "$desc exists and differs — NOT overwriting $(tilde "$dst")"
        return 1
    fi
    mkdir -p -- "$(dirname -- "$dst")"
    printf '%s\n' "$content" >"$dst"
    did "$desc written to $(tilde "$dst")"
}

cmake_build() {  # cmake_build <src> <build> <target...> -- <extra cmake args>
    local src="$1" bld="$2"; shift 2
    local -a targets=() extra=()
    while [[ $# -gt 0 && $1 != "--" ]]; do targets+=("$1"); shift; done
    [[ ${1:-} == "--" ]] && shift
    extra=("$@")
    cmake -S "$src" -B "$bld" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        "${extra[@]}" >/dev/null || return 1
    # One shared build mutex for the whole stack: two heavy builds at once has
    # hard-frozen this class of laptop more than once.
    flock -w 7200 "$LOCK" cmake --build "$bld" -j"$JOBS" ${targets[@]+--target "${targets[@]}"} >/dev/null
}

# ===========================================================================
# deps — packages. Printed, never installed.
# ===========================================================================

comp_deps() {
    header "deps — build prerequisites ($DISTRO)"

    local missing=0
    for t in git cmake ninja ccache pkg-config; do
        have "$t" && ok "$t" || { fail "$t not found"; missing=1; }
    done
    if pkg-config --exists openxr 2>/dev/null; then
        ok "openxr loader: $(pkg-config --modversion openxr)"
    else
        fail "no OpenXR loader (pkg-config openxr) — the compositor would build WITHOUT XR support"
        missing=1
    fi
    pkg-config --exists vulkan 2>/dev/null && ok "vulkan loader" || warn "vulkan loader not found via pkg-config"
    [[ -d /usr/share/vulkan/icd.d ]] && ok "vulkan ICDs: $(ls /usr/share/vulkan/icd.d | tr '\n' ' ')" \
                                     || warn "no /usr/share/vulkan/icd.d"
    # vainfo is a diagnostic aid, not a stack component — its absence never
    # breaks anything, it just makes the encode-capability check unavailable.
    have vainfo && ok "vainfo present (VA-API introspection)" \
                || note "vainfo not installed (libva-utils) — install it to verify hw ENCODE entrypoints"

    # Package commands are reference material on a box that already has
    # everything; they only become action items when something is missing.
    local pkg=note; ((missing)) && pkg=manual

    case "$DISTRO" in
      arch)
        note "Arch has no -devel split: the runtime packages carry the headers."
        $pkg "sudo pacman -S --needed base-devel git cmake ninja ccache openxr vulkan-headers vulkan-icd-loader libva-utils inotify-tools jq"
        $pkg "sudo pacman -S --needed hyprland   # pulls every compositor dependency (aquamarine, hyprlang, hyprutils, hyprcursor, hyprgraphics, hyprwayland-scanner, glaze, ...)"
        $pkg "sudo pacman -S --needed wivrn-server   # the packaged unit + OpenXR manifest this stack overrides"
        note "GPU stack: vulkan-radeon (AMD) / vulkan-intel + intel-media-driver (Intel) / nvidia-utils (NVIDIA)"
        ;;
      fedora)
        note "omedora already enables COPR agaspar/omedora-4, which ships the hypr* stack — so"
        note "\`dnf builddep hyprland\` resolves the whole compositor dependency set in one shot."
        $pkg "sudo dnf copr enable -y agaspar/omedora-4   # (already on if this box runs omedora)"
        $pkg "sudo dnf builddep -y hyprland"
        $pkg "sudo dnf install -y git cmake ninja-build ccache gcc-c++ openxr-devel vulkan-headers vulkan-loader-devel libva-devel libva-utils inotify-tools jq"
        note "UNVERIFIED from this box: the exact Fedora name of the OpenXR loader devel package."
        note "  verify with:  dnf provides '*/pkgconfig/openxr.pc'"
        note "WiVRn is NOT packaged for Fedora — the wivrn component builds it from source and"
        note "installs the user unit + OpenXR manifest itself. Its build deps are in the WiVRn"
        note "tree at docs/building.md; \`dnf builddep\` has no spec to work from."
        note "Intel media: stock intel-media-driver may lack H.264/H.265 ENCODE entrypoints."
        note "  verify with:  vainfo | grep -i 'EncSlice\\|EncSliceLP'"
        note "  if absent, install RPM Fusion's freeworld build of intel-media-driver."
        ;;
      *)
        warn "unrecognized distro — install the equivalents of: openxr loader+headers, vulkan"
        warn "headers+loader, the hypr* libraries, cmake/ninja/ccache, libva, jq, inotify-tools"
        ;;
    esac
    ((missing)) && note "install the above before running the compositor/wivrn components"
    return 0
}

# ===========================================================================
# dotfiles — ~/.config/hypr, on this machine's device branch
# ===========================================================================

comp_dotfiles() {
    header "dotfiles — $(tilde "$DOTFILES")"

    local branch="${HYPXR_DOTFILES_BRANCH:-}"
    if [[ -z $branch ]]; then
        branch=$(git -C "$DOTFILES" rev-parse --abbrev-ref HEAD 2>/dev/null)
        [[ -z $branch || $branch == HEAD ]] && branch=master
    fi

    if [[ ! -e $DOTFILES/.git ]]; then
        fail "$(tilde "$DOTFILES") is not a checkout of $DOTFILES_URL"
        note "this repo IS the live config; clone it in place only on a fresh machine:"
        manual "git clone $DOTFILES_URL $(tilde "$DOTFILES")"
        note "then create this machine's device branch off master and check it out:"
        manual "git -C $(tilde "$DOTFILES") checkout -b <device-branch> origin/master"
        return 1
    fi

    clone_or_sync "$DOTFILES" "$DOTFILES_URL" "$branch"

    if [[ $branch == master ]]; then
        warn "on master — every machine should run its OWN device branch rebased on master"
        note "see AGENTS.md in this repo: master = shared, <device> = machine-specific overrides"
        note "existing device branches: $(git -C "$DOTFILES" for-each-ref --format='%(refname:short)' refs/remotes/origin | sed 's#origin/##' | grep -v '^HEAD$\|^master$' | tr '\n' ' ')"
    else
        ok "device branch: $branch"
    fi

    file_present "$XRCONF" "hyprland-xr.conf (the XR session front-end config)"

    # The per-machine bits inside hyprland-xr.conf. These are the lines that are
    # WRONG by construction when a device branch is copied between machines.
    if [[ -r $XRCONF ]]; then
        local gpu aq
        gpu=$(sed -n 's/^[[:space:]]*gpu[[:space:]]*=[[:space:]]*//p' "$XRCONF" | tail -1)
        aq=$(sed -n 's/^[[:space:]]*env[[:space:]]*=[[:space:]]*AQ_DRM_DEVICES,//p' "$XRCONF" | tail -1)
        if [[ -z $gpu ]]; then
            warn "openxr:gpu is not set in hyprland-xr.conf"
            note "set it to the node the XR runtime renders on: ${HYPXR_GPU_NODE:-?}"
        elif [[ $gpu == "${HYPXR_GPU_NODE:-}" ]]; then
            ok "openxr:gpu = $gpu (matches this machine's XR GPU)"
        else
            warn "openxr:gpu = $gpu but this machine's XR GPU looks like ${HYPXR_GPU_NODE:-?}"
            note "a cross-GPU EGL context crashes the runtime at swapchain time — fix before donning"
        fi
        if ((HYBRID_GPU)); then
            if [[ -n $aq ]]; then
                ok "AQ_DRM_DEVICES = $aq (hybrid box: keeps aquamarine off the dGPU)"
            else
                warn "hybrid GPU box with no AQ_DRM_DEVICES pin — the dGPU will never reach D3cold"
            fi
        else
            [[ -n $aq ]] && note "AQ_DRM_DEVICES = $aq — harmless on a single-GPU box, but unnecessary" \
                         || ok "single-GPU box: no AQ_DRM_DEVICES pin needed"
        fi
    fi

    # The XR-aware display-toggle machinery has its own per-machine installer.
    local setup="$DOTFILES/scripts/setup-xr-display.sh"
    if [[ -x $setup ]]; then
        if ((CHECK_ONLY)); then
            if [[ -L $HOME/.local/bin/omarchy-hw-external-monitors && -L $CONFIG_HOME/environment.d/10-local-bin.conf ]]; then
                ok "setup-xr-display.sh deployed (PATH shadow + environment.d drop-in linked)"
            else
                warn "setup-xr-display.sh has not been run on this machine"
                manual "$(tilde "$setup")"
            fi
        else
            "$setup" | sed 's/^/    /'
            did "ran setup-xr-display.sh"
        fi
    else
        warn "no scripts/setup-xr-display.sh in the dotfiles checkout"
    fi
}

# ===========================================================================
# env — the session environment (~/.config/uwsm/env)
# ===========================================================================

comp_env() {
    header "env — session environment"

    local envf="$CONFIG_HOME/uwsm/env"
    if [[ ! -r $envf ]]; then
        fail "$(tilde "$envf") missing — uwsm sources this for the whole graphical session"
        return 1
    fi

    # 1. ~/.local/bin ahead of omarchy's bin. uwsm REBUILDS PATH in this file, so
    #    an environment.d drop-in alone is not enough — it gets buried.
    if grep -q 'PATH="\$HOME/.local/bin:\$PATH"' "$envf"; then
        ok "~/.local/bin prepended to PATH in uwsm/env"
    else
        warn "uwsm/env does not prepend ~/.local/bin to PATH"
        note 'add:  export PATH="$HOME/.local/bin:$PATH"   (AFTER the omarchy PATH line)'
        note "without it the XR-aware omarchy-hw-external-monitors shadow loses to the stock one"
    fi

    # 2. hypxrva's libva interception.
    if grep -q '^export LIBVA_DRIVER_NAME=hypxr' "$envf"; then
        ok "LIBVA_DRIVER_NAME=hypxr (hypxrva shim selected)"
    else
        warn "LIBVA_DRIVER_NAME=hypxr not exported — hypxrva will never load"
    fi
    if grep -q '^export LIBVA_DRIVERS_PATH=' "$envf"; then
        local p; p=$(sed -n 's/^export LIBVA_DRIVERS_PATH=//p' "$envf" | tail -1)
        ok "LIBVA_DRIVERS_PATH=$p"
        [[ $p == *hypxrva* ]] || warn "expected it to point at the hypxrva shim directory"
    else
        warn "LIBVA_DRIVERS_PATH not exported"
    fi

    # 3. Live session sanity, when there is one.
    if have systemctl; then
        local mp; mp=$(systemctl --user show-environment 2>/dev/null | sed -n 's/^PATH=//p')
        if [[ -n $mp ]]; then
            [[ ":$mp:" == *":$HOME/.local/bin:"* ]] \
                && ok "systemd user environment carries ~/.local/bin" \
                || warn "systemd user environment lacks ~/.local/bin (log out and back in)"
        fi
    fi
}

# ===========================================================================
# compositor — the fishfood worktree, its build, and the session entry
# ===========================================================================

comp_compositor() {
    header "compositor — $(tilde "$FISHFOOD") [$FISHFOOD_BRANCH]"

    # The fishfood tree is a WORKTREE of this repo, not an independent clone:
    # that is what makes `fishfood.sh update` a fast-forward instead of a fetch.
    if [[ ! -e $FISHFOOD/.git ]]; then
        if ((CHECK_ONLY)); then
            fail "no fishfood worktree at $(tilde "$FISHFOOD")"
            manual "$(tilde "$REPO")/scripts/fishfood.sh setup"
            return 1
        fi
        git -C "$REPO" worktree add "$FISHFOOD" -b "$FISHFOOD_BRANCH" "$FISHFOOD_TRACK" \
            || { fail "worktree add failed"; return 1; }
        did "fishfood worktree created"
        REPO_CHANGED=1
    else
        ok "$(tilde "$FISHFOOD") is a checkout ($(git -C "$FISHFOOD" rev-parse --abbrev-ref HEAD) @ $(git -C "$FISHFOOD" rev-parse --short HEAD))"
        if ! ((CHECK_ONLY)); then
            # hypxrland has been force-pushed (v0.56.0 rebase). fishfood.sh's
            # `merge --ff-only` cannot cross that; a fetch + hard reset can.
            local dirty; dirty=$(git -C "$FISHFOOD" status --porcelain --untracked-files=no | wc -l)
            if ((dirty)); then
                fail "fishfood worktree has $dirty uncommitted change(s) — refusing to reset"
                note "commit or stash, then re-run"
                return 1
            fi
            local before; before=$(git -C "$FISHFOOD" rev-parse HEAD)
            git -C "$FISHFOOD" fetch origin --prune >/dev/null 2>&1 || \
              git -C "$REPO" fetch origin --prune >/dev/null 2>&1
            local tip
            tip=$(git -C "$FISHFOOD" rev-parse --verify -q "origin/$FISHFOOD_TRACK" || git -C "$FISHFOOD" rev-parse --verify -q "$FISHFOOD_TRACK")
            if [[ -n $tip ]]; then
                git -C "$FISHFOOD" reset --hard "$tip" >/dev/null
                git -C "$FISHFOOD" submodule sync --recursive >/dev/null
                git -C "$FISHFOOD" submodule update --init >/dev/null || warn "submodule update reported errors"
                if [[ $before != $(git -C "$FISHFOOD" rev-parse HEAD) ]]; then
                    did "fishfood reset onto $FISHFOOD_TRACK ($(git -C "$FISHFOOD" rev-parse --short HEAD))"
                    REPO_CHANGED=1
                fi
            else
                warn "could not resolve the $FISHFOOD_TRACK tip"
            fi
        fi
    fi

    # Vendored submodules are REQUIRED to build (wayland-protocols + hyprutils
    # are pinned here because the system versions have burned us).
    for sm in subprojects/wayland-protocols subprojects/hyprutils subprojects/hyprland-protocols; do
        [[ -n $(ls -A "$FISHFOOD/$sm" 2>/dev/null) ]] && ok "submodule present: $sm" \
                                                      || fail "submodule NOT initialized: $sm"
    done

    local build="$FISHFOOD/build"
    if ((REPO_CHANGED)); then drop_stale_build "$build"; fi

    if ((CHECK_ONLY)); then
        exe_present "$build/Hyprland" "compositor binary"
        exe_present "$build/bin/hyprctl" "session-local hyprctl shim (PATH-prepended by the launcher)"
        if [[ -r $build/CMakeCache.txt ]]; then
            grep -q '^openxr_dep_FOUND:INTERNAL=1$' "$build/CMakeCache.txt" \
                && ok "build has OpenXR support compiled in" \
                || fail "build has NO OpenXR support — the loader was missing at configure time"
        fi
    else
        cmake -S "$FISHFOOD" -B "$build" -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache >/dev/null \
            || { fail "cmake configure failed"; return 1; }
        # The build treats OpenXR as optional and silently compiles it out. A
        # HypXRland build without XR is never what anyone wanted — fail loudly.
        if ! grep -q '^openxr_dep_FOUND:INTERNAL=1$' "$build/CMakeCache.txt"; then
            fail "OpenXR loader not found — this build would have NO openxr support"
            note "install it first (see the deps component) and re-run"
            return 1
        fi
        flock -w 7200 "$LOCK" cmake --build "$build" --target Hyprland hyprctl -j"$JOBS" \
            || { fail "compositor build failed"; return 1; }
        mkdir -p "$build/bin"
        ln -sf "$build/hyprctl/hyprctl" "$build/bin/hyprctl"
        did "built $(tilde "$build")/Hyprland ($(git -C "$FISHFOOD" rev-parse --short HEAD))"
    fi

    # ---- the session launcher + the wayland-session entry -------------------
    local launcher="$LAUNCH_DIR/launch.sh"
    if ((CHECK_ONLY)); then
        exe_present "$launcher" "session launcher"
    else
        mkdir -p "$LAUNCH_DIR"
        cat >"$launcher" <<EOF
#!/bin/sh
export PATH="$build/bin:\$PATH"
export HYPXRLAND_SESSION=1
exec uwsm start -e -D Hyprland -- $build/Hyprland --config $XRCONF
EOF
        chmod +x "$launcher"
        did "session launcher written to $(tilde "$launcher")"
    fi

    # Any .desktop in the system dir whose Exec is our launcher counts — the
    # entry has been called both hypxrland.desktop and omarchy-xr.desktop.
    local found=""
    for d in /usr/share/wayland-sessions/*.desktop; do
        [[ -r $d ]] || continue
        grep -qF "Exec=$launcher" "$d" && { found="$d"; break; }
    done
    if [[ -n $found ]]; then
        ok "wayland-session entry installed: $found ($(sed -n 's/^Name=//p' "$found"))"
    else
        warn "no wayland-session entry points at $(tilde "$launcher")"
        local name="${HYPXR_SESSION_NAME:-Omarchy XR}"
        local out="$LAUNCH_DIR/hypxrland.desktop"
        if ! ((CHECK_ONLY)); then
            cat >"$out" <<EOF
[Desktop Entry]
Name=$name
Comment=Hyprland + OpenXR (fishfood build from $FISHFOOD)
Exec=$launcher
TryExec=uwsm
DesktopNames=Hyprland
Type=Application
Keywords=tiling;wayland;compositor;openxr;vr;
EOF
            did "session entry generated at $(tilde "$out")"
        fi
        note "DesktopNames stays 'Hyprland' so portals/theming treat it exactly like the stock session"
        manual "sudo install -m644 $(tilde "$out") /usr/share/wayland-sessions/hypxrland.desktop"
        case "$DISTRO" in
          fedora) note "omedora PACKAGES its session entry (hyprland-omedora owns omedora.desktop);" ;
                  note "this one is an extra hand-installed file alongside it — GDM lists both." ;;
          arch)   note "SDDM lists it next to 'Hyprland' and 'Omedora/Omarchy' at the greeter." ;;
        esac
    fi
}

# ===========================================================================
# wivrn — the patched server, its unit override, config, and pairing
# ===========================================================================

comp_wivrn() {
    header "wivrn — patched server + streaming config"

    # The fork clone, then a worktree pinned to the release we patch against.
    if [[ ! -e $WIVRN_SRC/.git ]]; then
        if ((CHECK_ONLY)); then
            fail "no WiVRn fork at $(tilde "$WIVRN_SRC")"
            manual "git clone $GH/WiVRn.git $(tilde "$WIVRN_SRC")"
        else
            git clone "$GH/WiVRn.git" "$WIVRN_SRC" && did "cloned the WiVRn fork" || fail "clone failed"
        fi
    else
        ok "WiVRn fork at $(tilde "$WIVRN_SRC")"
    fi

    if [[ ! -e $WIVRN_WT/.git ]]; then
        if ((CHECK_ONLY)); then
            fail "no patch worktree at $(tilde "$WIVRN_WT") [$WIVRN_BRANCH]"
            manual "git -C $(tilde "$WIVRN_SRC") worktree add $(tilde "$WIVRN_WT") $WIVRN_BRANCH"
        else
            git -C "$WIVRN_SRC" fetch origin --prune >/dev/null
            git -C "$WIVRN_SRC" worktree add "$WIVRN_WT" "$WIVRN_BRANCH" \
                && { did "worktree $WIVRN_BRANCH created"; REPO_CHANGED=1; } || fail "worktree add failed"
        fi
    else
        # This branch is force-pushed on every rebase onto a new upstream tag.
        clone_or_sync "$WIVRN_WT" "$GH/WiVRn.git" "$WIVRN_BRANCH"
    fi

    local bld="$WIVRN_WT/build-server"
    local bin="$bld/server/wivrn-server"
    ((REPO_CHANGED)) && drop_stale_build "$bld"

    if ((CHECK_ONLY)); then
        exe_present "$bin" "patched wivrn-server binary"
        if [[ -r $bld/CMakeCache.txt ]]; then
            local desc; desc=$(sed -n 's/^GIT_DESC:UNINITIALIZED=//p' "$bld/CMakeCache.txt")
            if [[ $desc == "v$WIVRN_TAG" ]]; then
                ok "GIT_DESC=v$WIVRN_TAG baked into the build"
            else
                fail "GIT_DESC is '${desc:-unset}', expected 'v$WIVRN_TAG'"
                note "LOAD-BEARING: the client refuses to pair/stream when the server's version"
                note "string does not match the installed APK's. Reconfigure with -DGIT_DESC=v$WIVRN_TAG."
            fi
        fi
    else
        cmake -S "$WIVRN_WT" -B "$bld" -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
            -DWIVRN_BUILD_CLIENT=OFF -DWIVRN_BUILD_DASHBOARD=OFF -DWIVRN_BUILD_SERVER=ON \
            -DWIVRN_BUILD_WIVRNCTL=ON -DWIVRN_USE_SYSTEM_OPENXR=ON \
            -DGIT_DESC=v"$WIVRN_TAG" >/dev/null \
            || { fail "wivrn configure failed"; return 1; }
        flock -w 7200 "$LOCK" cmake --build "$bld" -j"$JOBS" >/dev/null \
            && did "built $(tilde "$bin")" || { fail "wivrn build failed"; return 1; }
    fi

    # ---- the base unit --------------------------------------------------
    # The drop-in below only makes sense on top of a base wivrn.service. Arch
    # gets it from the wivrn-server package; Fedora has no such package.
    local base; base=$(systemctl --user show -p FragmentPath --value wivrn.service 2>/dev/null)
    if [[ -n $base ]]; then
        ok "base wivrn.service: $base"
    else
        fail "no wivrn.service on this machine"
        case "$DISTRO" in
          arch)   manual "sudo pacman -S --needed wivrn-server" ;;
          *)      note "WiVRn is not packaged here — install the unit and the OpenXR manifest by hand:"
                  manual "install -Dm644 $(tilde "$WIVRN_WT")/server/dist/wivrn.service.in ~/.config/systemd/user/wivrn.service  # then edit ExecStart"
                  note "(the override below sets ExecStart anyway, so a minimal unit is enough)" ;;
        esac
    fi

    # ---- the drop-in: which binary, which GPU, which VA driver ------------
    local dropdir="$UNIT_DIR/wivrn.service.d"
    local drop="$dropdir/override.conf"
    local vk_line="" va_lines=""
    [[ -n $HYPXR_VK_ICD ]] && ((HYBRID_GPU)) && vk_line="Environment=VK_DRIVER_FILES=$HYPXR_VK_ICD"
    if [[ -n $HYPXR_VAAPI_DRIVER ]]; then
        va_lines="Environment=LIBVA_DRIVER_NAME=$HYPXR_VAAPI_DRIVER
Environment=LIBVA_DRIVERS_PATH=$HYPXR_LIBVA_DRIVERS_PATH"
    fi

    if [[ -r $drop ]]; then
        ok "wivrn.service drop-in present"
        local es; es=$(sed -n 's/^ExecStart=\(.\+\)$/\1/p' "$drop" | tail -1)
        [[ $es == "$bin" ]] && ok "  ExecStart -> the patched binary" \
                            || fail "  ExecStart is '$es', expected '$bin'"
        if [[ -n $HYPXR_VAAPI_DRIVER ]]; then
            local dn dp
            dn=$(sed -n 's/^Environment=LIBVA_DRIVER_NAME=//p' "$drop" | tail -1)
            dp=$(sed -n 's/^Environment=LIBVA_DRIVERS_PATH=//p' "$drop" | tail -1)
            [[ $dn == "$HYPXR_VAAPI_DRIVER" ]] && ok "  LIBVA_DRIVER_NAME=$dn (hypxrva bypass for the ENCODER)" \
                                               || warn "  LIBVA_DRIVER_NAME=${dn:-unset}, expected $HYPXR_VAAPI_DRIVER"
            if [[ $dp == "$HYPXR_LIBVA_DRIVERS_PATH" ]]; then
                ok "  LIBVA_DRIVERS_PATH=$dp (back to the SYSTEM dri dir)"
            else
                fail "  LIBVA_DRIVERS_PATH=${dp:-unset}, expected $HYPXR_LIBVA_DRIVERS_PATH"
                note "  the session env points LIBVA_DRIVERS_PATH at the shim-only directory, where"
                note "  the real driver does not exist — the encoder must be pointed back at $HYPXR_LIBVA_DRIVERS_PATH"
                note "  (Arch: /usr/lib/dri, Fedora: /usr/lib64/dri — this is a real portability trap)"
            fi
        fi
        if ((HYBRID_GPU)); then
            grep -q '^Environment=VK_DRIVER_FILES=' "$drop" \
                && ok "  VK_DRIVER_FILES pin present (keeps the compositor off the dGPU)" \
                || warn "  no VK_DRIVER_FILES pin on a hybrid box — WiVRn may pick the dGPU"
        else
            grep -q '^Environment=VK_DRIVER_FILES=' "$drop" \
                && note "  VK_DRIVER_FILES pin present; unnecessary on a single-GPU box" \
                || ok "  single-GPU box: no VK_DRIVER_FILES pin needed"
        fi
    else
        {
            echo "# HypXRland: run the locally patched wivrn-server ($WIVRN_BRANCH):"
            echo "#   - headset Battery property on io.github.wivrn.Server (feeds hypxrhud's gauge)"
            echo "#   - mic jitter buffer (fixes shredded wivrn.source audio; upstream issue #1012)"
            echo "# Revert: systemctl --user revert wivrn.service && systemctl --user restart wivrn.service"
            echo "[Service]"
            echo "ExecStart="
            echo "ExecStart=$bin"
            [[ -n $vk_line ]] && { echo "# Keep the server's Vulkan compositor on the XR GPU."; echo "$vk_line"; }
            [[ -n $va_lines ]] && {
                echo "# hypxrva bypass: the session env routes libva through the gating shim, but"
                echo "# WiVRn's OWN vaapi ENCODE must always hit the real driver directly."
                echo "$va_lines"
            }
        } | install_file "$drop" "wivrn.service drop-in"
        ((CHECK_ONLY)) || { systemctl --user daemon-reload; note "restart it when convenient: systemctl --user restart wivrn.service"; }
    fi

    # ---- the streaming config (encoder is per-machine) --------------------
    local wconf="$CONFIG_HOME/wivrn/config.json"
    if [[ -r $wconf ]]; then
        ok "$(tilde "$wconf") present"
        if have jq; then
            local enc dev
            enc=$(jq -r '.encoder.encoder // empty' "$wconf" 2>/dev/null)
            dev=$(jq -r '.encoder.device // empty' "$wconf" 2>/dev/null)
            note "encoder=${enc:-<default>} codec=$(jq -r '.encoder.codec // "-"' "$wconf") device=${dev:-<auto>}"
            if [[ $enc == vaapi && -n $dev && $dev != "$HYPXR_GPU_NODE" ]]; then
                warn "vaapi encoder device ($dev) is not this machine's XR GPU (${HYPXR_GPU_NODE:-?})"
            fi
        fi
    else
        {
            printf '{\n "encoder": {\n  "encoder": "%s",\n  "codec": "h265",\n  "device": "%s"\n },\n' \
                "${HYPXR_WIVRN_ENCODER:-vaapi}" "${HYPXR_GPU_NODE:-/dev/dri/renderD128}"
            printf ' "bit-depth": 8,\n "tcp-only": true,\n "inhibit": "worn"\n}'
        } | install_file "$wconf" "WiVRn streaming config"
        note "encoder is PER-MACHINE: vaapi (AMD/Intel) or nvenc (NVIDIA, drop the device key)"
    fi

    # ---- the OpenXR runtime manifest --------------------------------------
    local active="$CONFIG_HOME/openxr/1/active_runtime.json"
    local devjson="$bld/openxr_wivrn-dev.json"
    if [[ -e $active ]]; then
        local tgt; tgt=$(readlink -f "$active")
        if [[ $tgt == "$(readlink -f "$devjson" 2>/dev/null)" ]]; then
            ok "active OpenXR runtime -> the patched build's manifest"
        else
            warn "active OpenXR runtime is $tgt (not the patched build's $(tilde "$devjson"))"
            note "the packaged /usr/share/openxr/1/openxr_wivrn.json points at /usr/bin's server;"
            note "point it at the dev manifest so the runtime library matches the patched server"
        fi
    else
        if ((CHECK_ONLY)); then
            warn "no per-user active_runtime.json — the loader will fall back to the system default"
        else
            mkdir -p "$(dirname "$active")"
            ln -sfn "$devjson" "$active"
            did "active OpenXR runtime -> $(tilde "$devjson")"
        fi
    fi

    # ---- pairing ----------------------------------------------------------
    local keys="$CONFIG_HOME/wivrn/known_keys.json"
    if [[ -s $keys ]] && grep -q '"key"' "$keys" 2>/dev/null; then
        local names; names=$(sed -n 's/.*"name":"\([^"]*\)".*/\1/p' "$keys" 2>/dev/null | tr '\n' ',')
        [[ -z $names ]] && have jq && names=$(jq -r '[.[].name] | join(", ")' "$keys" 2>/dev/null)
        ok "paired headset(s): ${names:-yes}"
    else
        warn "no paired headset yet"
        note "pair from the desk, with the headset on the same network and the client open:"
        manual "wivrnctl pair            # prints a PIN; enter it in the headset client"
        note "the patched client APK is headset-side and is NOT installed by this script —"
        note "sideload the matching v$WIVRN_TAG build onto the headset once, then it stays."
    fi

    check_unit wivrn.service
    note "wivrn.service is started by the session (exec-once in hyprland-xr.conf), not enabled"
}

# ===========================================================================
# voice — hypxrvoice
# ===========================================================================

comp_voice() {
    header "voice — hypxrvoice"

    clone_or_sync "$VOICE_DIR" "$GH/hypxrvoice.git" "${HYPXR_VOICE_BRANCH:-master}" || return 1
    local changed=$REPO_CHANGED
    local bld="$VOICE_DIR/build"
    ((changed)) && drop_stale_build "$bld"

    # whisper.cpp + llama.cpp are submodules; the models are NOT in git.
    for sm in subprojects/whisper.cpp subprojects/llama.cpp; do
        [[ -n $(ls -A "$VOICE_DIR/$sm" 2>/dev/null) ]] && ok "submodule present: $sm" \
                                                       || fail "submodule NOT initialized: $sm"
    done

    local models="${HYPXRVOICE_MODEL_DIR:-$VOICE_DIR/models}"
    if [[ -s $models/ggml-base.en.bin ]]; then
        ok "ASR model present ($(tilde "$models")/ggml-base.en.bin)"
    else
        fail "ASR model missing — the daemon cannot transcribe without it"
        if ((CHECK_ONLY)); then
            manual "$(tilde "$VOICE_DIR")/scripts/fetch-models.sh"
        else
            "$VOICE_DIR/scripts/fetch-models.sh" >/dev/null && did "fetched the ASR model" || fail "fetch-models.sh failed"
        fi
    fi
    [[ -s $models/Qwen2.5-3B-Instruct-Q4_K_M.gguf ]] \
        && ok "intent GGUF present (optional: only for intent.backend = llama)" \
        || note "no intent GGUF — fine while intent.backend = rule (the default)"

    if ((CHECK_ONLY)); then
        exe_present "$bld/hypxrvoiced" "hypxrvoiced binary"
        exe_present "$bld/hypxrvoicectl" "hypxrvoicectl (the PTT keybind target)"
    else
        cmake_build "$VOICE_DIR" "$bld" -- && did "built hypxrvoice" || { fail "hypxrvoice build failed"; return 1; }
    fi

    local vconf="$CONFIG_HOME/hypxrvoice/config.toml"
    if [[ -r $vconf ]]; then
        ok "$(tilde "$vconf") present"
        grep -q '^model = ' "$vconf" && ok "  asr.model configured" || warn "  asr.model not set — transcription is disabled"
        grep -q '^dry_run = false' "$vconf" && ok "  executor actuates (dry_run = false)" \
                                            || note "  executor is in dry-run: it logs argv and changes nothing"
    else
        if ((CHECK_ONLY)); then
            fail "no $(tilde "$vconf")"
            manual "install -Dm644 $(tilde "$VOICE_DIR")/examples/config.toml $(tilde "$vconf")   # then set asr.model"
        else
            local ex="$VOICE_DIR/examples/config.toml"
            [[ -r $ex ]] || ex=$(ls "$VOICE_DIR"/examples/*.toml 2>/dev/null | head -1)
            if [[ -r $ex ]]; then
                mkdir -p "$(dirname "$vconf")"
                sed "s|^model = .*|model = \"$models/ggml-base.en.bin\"|" "$ex" >"$vconf"
                did "seeded $(tilde "$vconf") from the example (asr.model pointed at the fetched model)"
            else
                fail "no example config to seed from"
            fi
        fi
    fi

    # The unit is bound to graphical-session.target so a relog re-resolves the
    # compositor instance instead of hyprctl'ing a dead one.
    local unit="$UNIT_DIR/hypxrvoiced.service"
    if [[ -r $unit ]]; then
        # Do not compare byte-for-byte: the deployed unit carries its own history
        # in comments. Check the two things that have to be true.
        ok "hypxrvoiced.service unit installed"
        local es; es=$(sed -n 's/^ExecStart=//p' "$unit" | tail -1)
        [[ $es == "$bld/hypxrvoiced" ]] && ok "  ExecStart -> $(tilde "$es")" \
                                        || warn "  ExecStart is '$es', expected $(tilde "$bld")/hypxrvoiced"
        grep -q 'graphical-session.target' "$unit" \
            && ok "  bound to graphical-session.target (re-resolves the compositor on relog)" \
            || warn "  not bound to graphical-session.target — after a relog it hyprctl's a dead instance"
    else
    {
        echo "# hypxrvoiced — HypXRland voice-control daemon (systemd user service)."
        echo "# Bound to the graphical session so it starts on login and restarts on relog."
        echo "# Logs: journalctl --user -u hypxrvoiced"
        echo "[Unit]"
        echo "Description=HypXRland voice control daemon"
        echo "PartOf=graphical-session.target"
        echo "After=graphical-session.target"
        echo
        echo "[Service]"
        echo "Type=simple"
        echo "ExecStart=$bld/hypxrvoiced"
        echo "Restart=on-failure"
        echo "RestartSec=2"
        echo
        echo "[Install]"
        echo "WantedBy=graphical-session.target"
    } | install_file "$unit" "hypxrvoiced.service" || true
    fi
    if ((CHECK_ONLY)); then
        check_unit hypxrvoiced.service --want-enabled
    else
        enable_unit hypxrvoiced.service
    fi
}

# ===========================================================================
# hud — hypxrhud (+ the battery client)
# ===========================================================================

comp_hud() {
    header "hud — hypxrhud"

    clone_or_sync "$HUD_DIR" "$GH/hypxrhud.git" "${HYPXR_HUD_BRANCH:-master}" || return 1
    local changed=$REPO_CHANGED
    local bld="$HUD_DIR/build"
    ((changed)) && drop_stale_build "$bld"

    if ((CHECK_ONLY)); then
        exe_present "$bld/hypxrhud" "hypxrhud built binary"
    else
        cmake_build "$HUD_DIR" "$bld" -- -DCMAKE_INSTALL_PREFIX="$HUD_PREFIX" \
            && did "built hypxrhud" || { fail "hypxrhud build failed"; return 1; }
    fi

    # hypxrhud installs itself via CMake: binaries, the two user units, and the
    # D-Bus activation file. A /usr/local prefix means the install step is the
    # one root action in this component.
    local installed=0
    for f in "$HUD_PREFIX/bin/hypxrhud" "$HUD_PREFIX/bin/hypxrhud-battery"; do
        [[ -x $f ]] && { ok "installed: $f"; installed=$((installed + 1)); } || fail "not installed: $f"
    done
    [[ -e $HUD_PREFIX/share/dbus-1/services/io.github.andrewgaspar.hypxrhud.service ]] \
        && ok "D-Bus activation file installed (the daemon starts on the first CreatePanel)" \
        || warn "no D-Bus activation file — the HUD only appears if the unit is started eagerly"
    if ((installed < 2)); then
        if [[ $HUD_PREFIX == /usr* ]]; then
            manual "sudo cmake --install $(tilde "$bld")"
            note "or install user-locally instead: reconfigure with -DCMAKE_INSTALL_PREFIX=\$HOME/.local"
            note "(then the units land in ~/.local/lib/systemd/user, no root needed)"
        else
            ((CHECK_ONLY)) || { cmake --install "$bld" >/dev/null && did "installed to $HUD_PREFIX"; }
        fi
    fi

    # Per-machine: pin the HUD's EGL context to the XR runtime's GPU. On a
    # single-GPU box the auto-scan cannot get this wrong, so the file is optional.
    local hconf="$CONFIG_HOME/hypxrhud/hypxrhud.toml"
    if [[ -r $hconf ]]; then
        local g; g=$(sed -n 's/^gpu[[:space:]]*=[[:space:]]*"\(.*\)"/\1/p' "$hconf" | tail -1)
        if [[ -z $g ]]; then
            ok "$(tilde "$hconf") present (gpu auto-scan)"
        elif [[ $g == "$HYPXR_GPU_NODE" ]]; then
            ok "HUD gpu pinned to $g (matches the XR runtime's GPU)"
        else
            warn "HUD gpu = $g but this machine's XR GPU looks like ${HYPXR_GPU_NODE:-?}"
        fi
    elif ((HYBRID_GPU)); then
        {
            echo "# Pin the HUD overlay's EGL context to the XR runtime's GPU — auto-scan can"
            echo "# otherwise land on the wrong node first on a hybrid box."
            echo "[hud]"
            echo "gpu = \"$HYPXR_GPU_NODE\""
        } | install_file "$hconf" "hypxrhud gpu pin" || true
    else
        ok "single-GPU box: no hypxrhud.toml gpu pin needed"
    fi

    # The no-nvidia drop-ins exist purely so EGL vendor enumeration never opens
    # /dev/nvidia* and blocks dGPU D3cold. Meaningless without an NVIDIA card.
    for u in hypxrhud hypxrhud-battery; do
        local dd="$UNIT_DIR/$u.service.d/no-nvidia.conf"
        if ((HYBRID_GPU)); then
            if [[ -r $dd ]]; then
                ok "$u: no-nvidia drop-in present"
            else
                {
                    echo "# hypxrhud never needs the NVIDIA GPU (it renders on the XR runtime's GPU),"
                    echo "# but EGL vendor enumeration would open /dev/nvidia* and block dGPU D3cold."
                    echo "# Pin it to the Mesa vendor. Delete this drop-in to revert."
                    echo "[Service]"
                    echo "Environment=__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json"
                    [[ -n $HYPXR_VK_ICD ]] && echo "Environment=VK_DRIVER_FILES=$HYPXR_VK_ICD"
                } | install_file "$dd" "$u no-nvidia drop-in" || true
            fi
        else
            [[ -r $dd ]] && note "$u: no-nvidia drop-in present but unnecessary (no NVIDIA GPU here)" \
                         || ok "$u: no no-nvidia drop-in needed (single-GPU box)"
        fi
    done

    if ((CHECK_ONLY)); then
        check_unit hypxrhud.service --want-enabled
        check_unit hypxrhud-battery.service --want-enabled
    else
        enable_unit hypxrhud.service
        enable_unit hypxrhud-battery.service
    fi
    note "the battery gauge needs the PATCHED wivrn-server (it publishes the Battery property)"
}

# ===========================================================================
# va — hypxrva (VA-API decode gating)
# ===========================================================================

comp_va() {
    header "va — hypxrva (VA-API decode gating)"

    clone_or_sync "$VA_DIR" "$GH/hypxrva.git" "${HYPXR_VA_BRANCH:-master}" || return 1
    local changed=$REPO_CHANGED
    ((changed)) && drop_stale_build "$VA_DIR/build"

    local shim="$HOME/.local/lib/hypxrva/hypxr_drv_video.so"
    if ((CHECK_ONLY)); then
        file_present "$shim" "libva shim installed"
        exe_present "$HOME/.local/bin/hypxrva-watcher" "hypxrva-watcher"
        exe_present "$HOME/.local/bin/hypxrva-vaprobe" "hypxrva-vaprobe"
    else
        "$VA_DIR/install.sh" >/dev/null && did "hypxrva built and installed under ~/.local" \
                                       || { fail "hypxrva install.sh failed"; return 1; }
    fi

    # The shim directory must contain NOTHING else: LIBVA_DRIVERS_PATH points at it.
    local extra; extra=$(ls -1 "$HOME/.local/lib/hypxrva" 2>/dev/null | grep -cv '^hypxr_drv_video.so$')
    [[ ${extra:-0} -eq 0 ]] && ok "shim directory contains only the shim" \
                            || warn "$extra extra file(s) in ~/.local/lib/hypxrva — libva would try to load them"

    # The real driver it delegates to is discovered from the DRM fd's kernel
    # driver (amdgpu -> radeonsi, i915/xe -> iHD/i965, ...), so no per-machine
    # configuration is needed here — but the SEARCH path is distro-shaped.
    ok "delegation target for this GPU: ${HYPXR_VAAPI_DRIVER:-<unknown>} (kernel driver ${GPU_DRIVER:-?})"
    [[ -e $HYPXR_LIBVA_DRIVERS_PATH/${HYPXR_VAAPI_DRIVER}_drv_video.so ]] \
        && ok "real driver found: $HYPXR_LIBVA_DRIVERS_PATH/${HYPXR_VAAPI_DRIVER}_drv_video.so" \
        || warn "no ${HYPXR_VAAPI_DRIVER}_drv_video.so in $HYPXR_LIBVA_DRIVERS_PATH — install the GPU's VA driver"

    # The watcher is autostarted by the compositor config, not by systemd.
    if [[ -r $XRCONF ]] && grep -q 'hypxrva-watcher' "$XRCONF"; then
        ok "hypxrva-watcher autostarted from hyprland-xr.conf"
    else
        warn "hyprland-xr.conf has no hypxrva-watcher exec-once"
        note "add:  exec-once = ~/.local/bin/hypxrva-watcher"
    fi
    if have hypxrva-vaprobe; then
        note "verify end to end with: hypxrva-vaprobe | tail -1"
    fi
}

# ===========================================================================
# paper — hypxrpaper (ambient background / overlay-mode companion)
# ===========================================================================

comp_paper() {
    header "paper — hypxrpaper (ambient background)"

    # Two valid shapes: a standalone clone, or the vendored submodule inside the
    # compositor tree (which is what the container test suite uses).
    local sub="$FISHFOOD/subprojects/hypxrpaper"
    local bin=""
    for cand in "$PAPER_DIR/build/hypxrpaper" "$sub/build/hypxrpaper" "$(command -v hypxrpaper 2>/dev/null)"; do
        [[ -n $cand && -x $cand ]] && { bin="$cand"; break; }
    done

    if [[ -n $bin ]]; then
        ok "hypxrpaper binary: $(tilde "$bin")"
    else
        if [[ -e $PAPER_DIR/.git ]] || ! ((CHECK_ONLY)); then
            clone_or_sync "$PAPER_DIR" "$GH/hypxrpaper.git" "${HYPXR_PAPER_BRANCH:-master}" || return 1
            ((REPO_CHANGED)) && drop_stale_build "$PAPER_DIR/build"
            if ((CHECK_ONLY)); then
                fail "hypxrpaper is not built"
                manual "cmake -S $(tilde "$PAPER_DIR") -B $(tilde "$PAPER_DIR")/build && cmake --build $(tilde "$PAPER_DIR")/build"
            else
                cmake_build "$PAPER_DIR" "$PAPER_DIR/build" -- && did "built hypxrpaper" || fail "hypxrpaper build failed"
            fi
        else
            warn "hypxrpaper not present"
            note "optional: it is the ambient background for overlay mode and the overlay test"
            manual "git clone $GH/hypxrpaper.git $(tilde "$PAPER_DIR") && cmake -S $(tilde "$PAPER_DIR") -B $(tilde "$PAPER_DIR")/build && cmake --build $(tilde "$PAPER_DIR")/build"
        fi
    fi
    note "it is a primary OpenXR session, not a service — scripts/preview-xr.sh --env and"
    note "openxr:overlay launch it; set \$HYPXRPAPER_BIN to point at a build elsewhere"
}

# ===========================================================================
# monado — OPTIONAL, XREAL Air 2 Ultra only
# ===========================================================================

comp_monado() {
    header "monado — XREAL 3DoF runtime ${DIM}(OPTIONAL)${R}"
    note "Only needed for the XREAL Air 2 Ultra display rig (docs/openxr/07-xreal.md)."
    note "A Quest/WiVRn-only machine should SKIP this component entirely."

    local sub="$FISHFOOD/subprojects/monado"
    local out="${MONADO_XREAL_BUILD:-$sub/build-xreal}"
    local svc="$out/src/xrt/targets/service/monado-service"

    if [[ ! -d $sub/.git && ! -f $sub/.git ]]; then
        fail "the monado submodule is not initialized in $(tilde "$FISHFOOD")"
        manual "git -C $(tilde "$FISHFOOD") submodule update --init subprojects/monado"
        return 1
    fi
    ok "monado submodule present ($(git -C "$sub" rev-parse --short HEAD 2>/dev/null))"

    if ((CHECK_ONLY)); then
        exe_present "$svc" "xreal-flavor monado-service"
    else
        flock -w 7200 "$LOCK" env HYPXRLAND_BUILD_JOBS="$JOBS" "$FISHFOOD/scripts/build-monado.sh" --xreal \
            && did "built the xreal monado flavor" || { fail "build-monado.sh --xreal failed"; return 1; }
    fi

    # NEVER setcap monado-service. A file capability makes the loader drop the
    # environment (secure-execution mode), which silently discards VK_DRIVER_FILES
    # and __GLX_VENDOR_LIBRARY_NAME — and the runtime then picks the wrong GPU and
    # crashes at swapchain time. See docs/openxr/07-xreal.md §1.4b.
    if [[ -x $svc ]] && have getcap; then
        local cap; cap=$(getcap "$svc" 2>/dev/null)
        [[ -z $cap ]] && ok "monado-service has NO file capabilities (correct)" \
                      || { fail "monado-service is capped: $cap"; note "strip it: sudo setcap -r $svc"; }
    fi

    local envf="$CONFIG_HOME/xreal/monado.env"
    if [[ -r $envf ]]; then
        ok "$(tilde "$envf") present"
        local p; p=$(sed -n 's/^MONADO_XREAL_SERVICE=//p' "$envf" | tail -1)
        [[ $p == "$svc" ]] && ok "  MONADO_XREAL_SERVICE -> the built service" \
                           || warn "  MONADO_XREAL_SERVICE=$p (expected $svc)"
    else
        {
            echo "MONADO_XREAL_SERVICE=$svc"
            [[ -n $HYPXR_VK_ICD ]] && echo "VK_DRIVER_FILES=$HYPXR_VK_ICD"
            echo "__GLX_VENDOR_LIBRARY_NAME=mesa"
        } | install_file "$envf" "xreal monado env" || true
    fi

    local unit="$UNIT_DIR/monado-xreal.service"
    if [[ -r $unit ]]; then
        ok "monado-xreal.service installed"
    else
        warn "monado-xreal.service not installed"
        manual "install -Dm644 $(tilde "$FISHFOOD")/contrib/xreal/monado-xreal.service $(tilde "$unit") && systemctl --user daemon-reload"
    fi
    note "scripts/xreal-mode.sh starts/stops the unit; you do not start it by hand"
}

# ===========================================================================
# Run
# ===========================================================================

echo "${B}HypXRland stack setup${R} — $( ((CHECK_ONLY)) && echo "${YEL}--check (report only, nothing will be modified)${R}" || echo "apply mode" )"
print_profile

for c in "${WANT[@]}"; do
    case "$c" in
        deps)       comp_deps ;;
        dotfiles)   comp_dotfiles ;;
        env)        comp_env ;;
        compositor) comp_compositor ;;
        wivrn)      comp_wivrn ;;
        voice)      comp_voice ;;
        hud)        comp_hud ;;
        va)         comp_va ;;
        paper)      comp_paper ;;
        monado)     comp_monado ;;
    esac
done

# ---------------------------------------------------------------------------
# Verification hints + summary
# ---------------------------------------------------------------------------

header "Verify a live session"
note "hyprctl openxr status -j | jq            # runtime, session state, monitors"
note "systemctl --user status wivrn hypxrhud hypxrhud-battery hypxrvoiced"
note "journalctl --user -u wivrn -b --no-pager | tail -40"
note "hypxrva-vaprobe | tail -1                # which VA driver a decode context lands on"
note "vainfo | grep -i enc                     # hw ENCODE entrypoints (WiVRn needs these)"

header "Summary"
printf '  %s%d ok%s  %s%d changed%s  %s%d warning(s)%s  %s%d failure(s)%s\n' \
    "$GRN" "$n_ok" "$R" "$GRN" "$n_did" "$R" "$YEL" "$n_warn" "$R" "$RED" "$n_fail" "$R"

if [[ ${#MANUAL_STEPS[@]} -gt 0 ]]; then
    echo
    echo "  ${B}Manual steps (this script never runs sudo, and never guesses for you):${R}"
    for s in "${MANUAL_STEPS[@]}"; do echo "    ${CYA}$s${R}"; done
fi

if ((CHECK_ONLY)); then
    echo
    echo "  ${DIM}--check: nothing was modified. Re-run without --check to apply.${R}"
fi

# A failure is a component that cannot work as-is; warnings are things to look at.
((n_fail)) && exit 1
exit 0
