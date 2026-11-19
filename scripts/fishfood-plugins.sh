#!/usr/bin/env bash
# Build the Hyprland plugins the hypxr-pkgs packages ship, against the FISHFOOD compositor build.
#
# A plugin only loads into the exact compositor build it was compiled against: its version check
# compares the Hyprland commit plus the aquamarine/hyprutils/hyprgraphics versions. The packaged
# plugins (/usr/lib/hypxrland/plugins/<pkg>/<name>.so) match the packaged compositor, so a fishfood
# session reports "Version mismatch" for every one of them. This script builds the same sources the
# same way, against the fishfood tree's headers, and deploys them into the directory the fishfood
# binary was configured to serve plugins from (CMake HYPXR_PLUGIN_DIR, see
# src/plugins/PluginPath.hpp). The loader then serves those copies for both the stock path
# (/usr/lib/omarchy-hyprland-titlebars/titlebars.so, which Omarchy loads) and the packaged path
# (/usr/lib/hypxrland/plugins/hyprshare/hyprshare.so, which hyprland-xr.lua loads).
#
#   fishfood-plugins.sh build    stage the plugin SDK, build and deploy every plugin
#   fishfood-plugins.sh status   report whether each deployed plugin matches the current build
#   fishfood-plugins.sh fingerprints  print each PKGBUILD's build-step fingerprint (see PLUGINS)
#
# fishfood.sh (setup/update) and hypxr-setup.sh (compositor) run `build` after the compositor build.
#
# PINS live in ONE place: the PKGBUILDs in hypxr-pkgs ($HYPXR_PKGS_DIR at $HYPXR_PKGS_REF). Each
# plugin's source URL and sha256 are read from its PKGBUILD on every run, so this always builds what
# the packages ship. The BUILD STEPS below are a transcription of each PKGBUILD's
# prepare/build/check, with the /usr/lib/hypxrland paths swapped for the stage; when a PKGBUILD's
# build steps change (flags, patches, a second source), change the matching build_* function too.
# A PKGBUILD that gains a source entry (a patch) makes its build fail here rather than build
# something the package does not.
#
# Environment (shared with fishfood.sh and hypxr-setup.sh):
#   HYPXRLAND_FISHFOOD          the fishfood worktree (default ~/code/hypxrland); built in ./build
#   HYPXRLAND_FISHFOOD_JOBS     build parallelism (default 8)
#   HYPXR_PKGS_DIR              the hypxr-pkgs clone (default ~/code/hypxr-pkgs)
#   HYPXR_PKGS_REF              the ref whose PKGBUILDs pin the plugins (default origin/master)
#   HYPXRLAND_FISHFOOD_PLUGIN_SRC  tarball cache (default ~/.cache/hypxrland-worktrees/fishfood-plugins-src)
# The deploy directory is NOT an option here: it is read from the build's CMakeCache.txt, so it is
# always the one the binary looks in. Choose it at configure time (HYPXRLAND_FISHFOOD_PLUGIN_DIR in
# fishfood.sh, default <build>/plugins).
#
# Exit status: 0 when every plugin was built and deployed; 1 otherwise. A failed plugin keeps its
# previously deployed .so, which no longer matches a rebuilt compositor; the summary says so.

set -euo pipefail

WORKTREE=${HYPXRLAND_FISHFOOD:-$HOME/code/hypxrland}
BUILD=$WORKTREE/build
JOBS=${HYPXRLAND_FISHFOOD_JOBS:-8} # this box has frozen twice under heavier parallelism
PKGS_DIR=${HYPXR_PKGS_DIR:-$HOME/code/hypxr-pkgs}
PKGS_REF=${HYPXR_PKGS_REF:-origin/master}
SRC_CACHE=${HYPXRLAND_FISHFOOD_PLUGIN_SRC:-${XDG_CACHE_HOME:-$HOME/.cache}/hypxrland-worktrees/fishfood-plugins-src}
LOCK=/tmp/hypxrland-build.lock
SDK=$BUILD/plugin-sdk     # mirrors the hypxrland-headers package: include/hyprland + pkgconfig
WORK=$BUILD/plugin-build  # per-plugin source trees (fresh every run) and build logs

# <PKGBUILD dir in hypxr-pkgs>  <deployed path under the plugin dir, as the package installs it>
#   <fingerprint of the PKGBUILD's build steps that the build_* function below was transcribed from>
# The fingerprint covers the PKGBUILD's functions and options, not its pins, so a pin bump builds
# silently and a build-step change warns. After reviewing a change, `fingerprints` prints the new
# values.
PLUGINS=(
    "hyprshare                         hyprshare/hyprshare.so                   e6954c400c561dfc"
    "omarchy-hyprland-titlebars-hypxr  omarchy-hyprland-titlebars/titlebars.so  cbfe2a22f4b57b08"
)

say()  { echo "==> $*"; }
warn() { echo "WARNING: $*" >&2; }

commit_of() { sed -n 's/^#define GIT_COMMIT_HASH *"\(.*\)"$/\1/p' "$1" 2>/dev/null || true; }

plugin_dir() {
    sed -n 's/^HYPXR_PLUGIN_DIR:STRING=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true
}

# The commit the compositor binary carries; empty when the tree and the binary disagree (a
# configure ran after the last successful build), because plugins built now would not match it.
compositor_commit() {
    local commit
    commit=$(commit_of "$WORKTREE/src/version.h" 2>/dev/null) || return 0
    [[ -n $commit && -x $BUILD/Hyprland ]] && grep -aFq "$commit" "$BUILD/Hyprland" && echo "$commit"
    return 0
}

# ---------------------------------------------------------------------------
# Plugin SDK: what the hypxrland-headers package installs, from the fishfood tree
# ---------------------------------------------------------------------------

# Same file set as package_hypxrland-headers: every *.h/*.hpp/*.inc under src/ and protocols/
# (version.h and the protocol headers are generated into the source tree by the build), plus the
# build's hyprland.pc with its prefix pointed at the stage. Rebuilt from scratch every time so a
# header deleted upstream does not linger.
stage_sdk() {
    local commit=$1 new=$SDK.new
    rm -rf "$new"
    mkdir -p "$new/include/hyprland" "$new/pkgconfig"
    (cd "$WORKTREE" && find src protocols -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -print0 |
        xargs -0 cp --parents -t "$new/include/hyprland") || return 1
    [[ $(commit_of "$new/include/hyprland/src/version.h") == "$commit" ]] || { warn "staged version.h is not $commit"; return 1; }
    # the prefix names the final location: the stage is only read once it has been moved there
    sed "s|^prefix=.*\$|prefix=$SDK/include|" "$BUILD/hyprland.pc" >"$new/pkgconfig/hyprland.pc"
    grep -Fxq "prefix=$SDK/include" "$new/pkgconfig/hyprland.pc" || { warn "could not rewrite the hyprland.pc prefix"; return 1; }
    rm -rf "$SDK"
    mv "$new" "$SDK"
    say "plugin SDK staged at $SDK (Hyprland $commit, version $(PKG_CONFIG_PATH=$SDK/pkgconfig pkg-config --modversion hyprland))"
}

# ---------------------------------------------------------------------------
# Sources, pinned by the PKGBUILDs
# ---------------------------------------------------------------------------

# Prints "<tarball name> <url> <sha256>" for a PKGBUILD with exactly one source. The PKGBUILD is
# evaluated in a subshell, the way makepkg reads it; these PKGBUILDs only assign at top level.
pkgbuild_source() {
    local text
    text=$(git -C "$PKGS_DIR" show "$PKGS_REF:pkgbuilds/$1/PKGBUILD") || return 1
    (
        set +eu
        eval "$text" >/dev/null 2>&1
        if ((${#source[@]} != 1 || ${#sha256sums[@]} != 1)); then
            echo "$1/PKGBUILD has ${#source[@]} sources; fishfood-plugins.sh only knows how to build one (teach it the patches)" >&2
            exit 1
        fi
        local entry=${source[0]}
        [[ $entry == *::* ]] || { echo "$1/PKGBUILD source has no <name>:: prefix: $entry" >&2; exit 1; }
        [[ ${sha256sums[0]} =~ ^[0-9a-f]{64}$ ]] || { echo "$1/PKGBUILD has no sha256 for its source" >&2; exit 1; }
        printf '%s %s %s\n' "${entry%%::*}" "${entry#*::}" "${sha256sums[0]}"
    )
}

# A fingerprint of what the PKGBUILD does at build time: its functions and options.
pkgbuild_steps() {
    local text
    text=$(git -C "$PKGS_DIR" show "$PKGS_REF:pkgbuilds/$1/PKGBUILD") || return 1
    (
        set +eu
        unset -f prepare build check package _plugin_so
        eval "$text" >/dev/null 2>&1
        # a PKGBUILD lacks some of these; that is not an error (and pipefail would make it one)
        { declare -f prepare build check package _plugin_so; declare -p options _pc_dir; true; } 2>/dev/null | sha256sum | cut -c1-16
    )
}

# fetch_source <pkg> -> prints the path of a fresh, empty-before-extraction source tree
fetch_source() {
    local pkg=$1 name url sha
    read -r name url sha < <(pkgbuild_source "$pkg") || return 1
    [[ -n $sha ]] || return 1
    local cache=$SRC_CACHE/$pkg tarball=$SRC_CACHE/$pkg/$name
    mkdir -p "$cache"
    if [[ ! -f $tarball ]] || ! echo "$sha  $tarball" | sha256sum -c --status; then
        echo "    fetching $url" >&2
        curl -fsSL --retry 2 -o "$tarball.part" "$url" || { echo "download failed: $url" >&2; return 1; }
        if ! echo "$sha  $tarball.part" | sha256sum -c --status; then
            echo "sha256 mismatch for $url (the PKGBUILD pins $sha)" >&2
            rm -f "$tarball.part"
            return 1
        fi
        mv -f "$tarball.part" "$tarball"
    fi
    local src=$WORK/$pkg/src
    rm -rf "$WORK/$pkg"
    mkdir -p "$src"
    # GitHub archives hold exactly one top-level directory; build inside it like the PKGBUILD does
    tar -xzf "$tarball" -C "$src" --strip-components=1 || return 1
    echo "    source: $name (sha256 ${sha:0:12}..., pinned by $PKGS_REF:pkgbuilds/$pkg/PKGBUILD)" >&2
    echo "$src"
}

# ---------------------------------------------------------------------------
# Build steps, transcribed from the PKGBUILDs
# ---------------------------------------------------------------------------

# makepkg's compile flags. The PKGBUILDs set options=('!lto' '!debug'), so neither LTOFLAGS nor
# DEBUG_*FLAGS apply. Read in a subshell in makepkg's own order; empty where makepkg is absent.
makepkg_env() {
    [[ -r /etc/makepkg.conf ]] || return 0
    (
        set +eu
        source /etc/makepkg.conf
        for f in /etc/makepkg.conf.d/*.conf; do [[ -r $f ]] && source "$f"; done
        if [[ -r ${XDG_CONFIG_HOME:-$HOME/.config}/pacman/makepkg.conf ]]; then
            source "${XDG_CONFIG_HOME:-$HOME/.config}/pacman/makepkg.conf"
        elif [[ -r $HOME/.makepkg.conf ]]; then
            source "$HOME/.makepkg.conf"
        fi
        printf 'CFLAGS=%s\0CXXFLAGS=%s\0LDFLAGS=%s\0' "$CFLAGS" "$CXXFLAGS" "$LDFLAGS"
    )
}

# check(): the plugin carries the hash of the headers it was built against, and not stock
# Hyprland's (a stray /usr/include/hyprland on the include path).
check_hash() {
    local so=$1 commit=$2 stock
    grep -aFq "$commit" "$so" || { echo "$so does not carry Hyprland $commit" >&2; return 1; }
    if [[ -r /usr/include/hyprland/src/version.h ]]; then
        stock=$(commit_of /usr/include/hyprland/src/version.h)
        if [[ -n $stock && $stock != "$commit" ]] && grep -aFq "$stock" "$so"; then
            echo "$so was built against stock Hyprland's headers" >&2
            return 1
        fi
    fi
}

# pkgbuilds/hyprshare/PKGBUILD
build_hyprshare() {
    local src=$1 commit=$2 version
    cd "$src" || return 1
    # prepare(): the Makefile hardcodes the compositor's HYPRLAND_VERSION define; it must match the
    # headers, or this HypXRland release needs a new hyprshare
    version=$(PKG_CONFIG_PATH=$SDK/pkgconfig pkg-config --modversion hyprland)
    grep -Fq "'-DHYPRLAND_VERSION=\"$version\"'" Makefile || { echo "hyprshare's Makefile does not target Hyprland $version" >&2; return 1; }
    # build(): include paths from HypXRland's pkg-config file; glaze is not reached through any
    # installed header
    flock -w 7200 "$LOCK" make -j"$JOBS" HYPRLAND_PC_DIR="$SDK/pkgconfig" GLAZE_INCLUDE= || return 1
    local sos=(build/hyprshare*.so)
    if ((${#sos[@]} != 1)) || [[ ! -f ${sos[0]} ]]; then
        echo "expected exactly one build/hyprshare*.so, found: ${sos[*]}" >&2
        return 1
    fi
    check_hash "${sos[0]}" "$commit" || return 1
    BUILT_SO=$src/${sos[0]}
}

# pkgbuilds/omarchy-hyprland-titlebars-hypxr/PKGBUILD
build_titlebars() {
    local src=$1 commit=$2
    # build(): only HypXRland's pkg-config file, so stock hyprland's /usr/share/pkgconfig entry
    # cannot win
    PKG_CONFIG_PATH=$SDK/pkgconfig \
        cmake -S "$src" -B "$src/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr || return 1
    grep -Fq "$SDK/include" "$src/build/CMakeCache.txt" || { echo "titlebars did not pick up $SDK/include" >&2; return 1; }
    flock -w 7200 "$LOCK" cmake --build "$src/build" -j"$JOBS" || return 1
    check_hash "$src/build/titlebars.so" "$commit" || return 1
    BUILT_SO=$src/build/titlebars.so
}

builder_for() {
    case $1 in
        hyprshare) echo build_hyprshare ;;
        omarchy-hyprland-titlebars-hypxr) echo build_titlebars ;;
        *) return 1 ;;
    esac
}

# Atomic replace: a running fishfood session keeps the old file mapped, and rename(2) leaves that
# inode intact where an in-place write would truncate it under the session.
deploy() {
    local from=$1 to=$2 info=$3
    mkdir -p "$(dirname "$to")" || return 1
    local tmp
    tmp="$(dirname "$to")/.$(basename "$to").new.$$"
    install -m755 "$from" "$tmp" || { rm -f "$tmp"; return 1; }
    mv -f "$tmp" "$to" || return 1
    printf '%s\n' "$info" >"$to.build-info.new.$$" && mv -f "$to.build-info.new.$$" "$to.build-info"
}

build_one() {
    local pkg=$1 dest=$2 commit=$3 fingerprint=$4 builder src steps
    builder=$(builder_for "$pkg") || { echo "no build steps for $pkg" >&2; return 1; }
    steps=$(pkgbuild_steps "$pkg") || return 1
    if [[ $steps != "$fingerprint" ]]; then
        echo "WARNING: pkgbuilds/$pkg/PKGBUILD changed its build steps since fishfood-plugins.sh" >&2
        echo "         transcribed them (fingerprint $steps, expected $fingerprint). Building with the" >&2
        echo "         old steps; compare $builder with the PKGBUILD, then update the fingerprint." >&2
    fi
    src=$(fetch_source "$pkg") || return 1
    # a subshell keeps the builder's cd and makepkg's flags to this plugin
    (
        while IFS= read -r -d '' kv; do export "${kv?}"; done < <(makepkg_env)
        BUILT_SO=""
        "$builder" "$src" "$commit" || exit 1
        echo "$BUILT_SO" >"$WORK/$pkg/built-so"
    ) || return 1
    local so url
    so=$(cat "$WORK/$pkg/built-so")
    url=$(pkgbuild_source "$pkg" | cut -d' ' -f2)
    deploy "$so" "$dest" "built $(date -Is) for Hyprland $commit
from $url
pinned by $PKGS_DIR $PKGS_REF ($(git -C "$PKGS_DIR" rev-parse --short "$PKGS_REF")):pkgbuilds/$pkg/PKGBUILD"
}

# ---------------------------------------------------------------------------

cmd_build() {
    local dir commit
    dir=$(plugin_dir)
    if [[ -z $dir ]]; then
        warn "$BUILD/CMakeCache.txt has no HYPXR_PLUGIN_DIR: configure the fishfood build first"
        return 1
    fi
    case $dir in
        /usr | /usr/*)
            warn "this build serves plugins from $dir, a packaged location; reconfigure it with a"
            warn "user-owned -DHYPXR_PLUGIN_DIR (fishfood.sh uses <build>/plugins). Nothing built."
            return 1
            ;;
    esac
    commit=$(compositor_commit)
    if [[ -z $commit ]]; then
        warn "$BUILD/Hyprland does not carry the commit in $WORKTREE/src/version.h: the last"
        warn "compositor build failed or is stale, and plugins built now would not load into it."
        return 1
    fi
    if ! git -C "$PKGS_DIR" rev-parse --verify -q "$PKGS_REF^{commit}" >/dev/null; then
        warn "no hypxr-pkgs at $PKGS_DIR ($PKGS_REF), which pins the plugins; nothing built"
        return 1
    fi

    say "plugins for $BUILD/Hyprland ($commit), deployed to $dir"
    say "pins: $PKGS_DIR at $PKGS_REF ($(git -C "$PKGS_DIR" log -1 --format='%h %cs' "$PKGS_REF"))"
    if ! stage_sdk "$commit"; then
        warn "could not stage the plugin SDK; no plugin rebuilt"
        cmd_status >&2 || true
        return 1
    fi

    local failed=() entry pkg rel fp log
    mkdir -p "$WORK"
    for entry in "${PLUGINS[@]}"; do
        read -r pkg rel fp <<<"$entry"
        log=$WORK/$pkg.log
        say "building $pkg (log: $log)"
        if build_one "$pkg" "$dir/$rel" "$commit" "$fp" > >(tee "$log") 2>&1; then
            say "deployed $dir/$rel"
        else
            failed+=("$pkg")
            warn "$pkg FAILED to build; see $log"
        fi
    done

    if ((${#failed[@]})); then
        echo >&2
        warn "=================================================================================="
        warn "plugin build FAILED: ${failed[*]}"
        warn "The previous .so (if any) was left in place, but it was built for another compositor"
        warn "build: the fishfood session will report \"Version mismatch\" and run without it."
        warn "=================================================================================="
        cmd_status >&2 || true
        return 1
    fi
    cmd_status
}

cmd_status() {
    local dir commit entry pkg rel f bad=0
    dir=$(plugin_dir)
    commit=$(compositor_commit)
    say "plugin status for $BUILD/Hyprland (${commit:-NO MATCHING BINARY}), plugin dir ${dir:-UNCONFIGURED}"
    for entry in "${PLUGINS[@]}"; do
        read -r pkg rel _ <<<"$entry"
        f=$dir/$rel
        if [[ -z $dir || ! -f $f ]]; then
            echo "    MISSING  $rel: the session falls back to the packaged/stock build (Version mismatch)"
            bad=1
        elif [[ -z $commit ]] || ! grep -aFq "$commit" "$f"; then
            echo "    STALE    $f: built for another compositor build (Version mismatch)"
            bad=1
        else
            echo "    ok       $f"
        fi
    done
    return $bad
}

cmd_fingerprints() {
    local entry pkg
    for entry in "${PLUGINS[@]}"; do
        read -r pkg _ <<<"$entry"
        printf '%-34s %s\n' "$pkg" "$(pkgbuild_steps "$pkg")"
    done
}

case ${1:-} in
    build) cmd_build ;;
    status) cmd_status ;;
    fingerprints) cmd_fingerprints ;;
    *)
        sed -n '2,21p' "${BASH_SOURCE[0]}"
        exit 1
        ;;
esac
