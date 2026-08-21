#!/usr/bin/env bash
# Fishfood HypXRland as a real desktop session alongside the distro Hyprland.
#
# The fishfood checkout is a git worktree (branch `fishfood`) SIBLING to the
# main repo, built RelWithDebInfo, and launched by a wayland-session desktop
# entry that points straight at the built binary with the user's XR front-end
# config (~/.config/hypr/hyprland-xr.conf by default — sources the normal
# config and layers the openxr bits).
#
#   fishfood.sh setup        one-time: create worktree + build + emit session file
#   fishfood.sh update       fast-forward fishfood -> hypxrland tip and rebuild
#   fishfood.sh gen-session  (re)generate the .desktop file + print install cmd
#
# The session file install needs root; this script never runs sudo itself —
# it prints the command for the user to run.
#
# SCOPE: this script owns the COMPOSITOR only, and `update` is a fast-forward.
# For the whole HypXRland stack (WiVRn, hypxrvoice, hypxrhud, hypxrva,
# hypxrpaper, the dotfiles device branch, the session environment), and for the
# update path that survives a force-push of the hypxrland branch — fetch + hard
# reset + stale-build-tree removal — use scripts/hypxr-setup.sh instead. It
# shares this script's worktree/branch/config/jobs environment variables
# (HYPXRLAND_FISHFOOD*), so the two agree about where things live.
# See docs/openxr/08-machine-setup.md.

set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORKTREE=${HYPXRLAND_FISHFOOD:-$HOME/code/hypxrland}
BRANCH=${HYPXRLAND_FISHFOOD_BRANCH:-fishfood}
TRACK=${HYPXRLAND_FISHFOOD_TRACK:-hypxrland}
CONF=${HYPXRLAND_FISHFOOD_CONF:-$HOME/.config/hypr/hyprland-xr.lua}
BUILD=$WORKTREE/build
DESKTOP_OUT=${XDG_DATA_HOME:-$HOME/.local/share}/hypxrland/omarchy-xr-fishfood.desktop
JOBS=${HYPXRLAND_FISHFOOD_JOBS:-8} # deliberately modest: this box has frozen twice under heavy parallel builds

build() {
    cmake -S "$WORKTREE" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    # The main build treats OpenXR as optional and silently compiles it out
    # when the loader is missing — a fishfood build without XR is never what
    # we want, so fail loudly here instead of shipping a vanilla Hyprland.
    if ! grep -q '^openxr_dep_FOUND:INTERNAL=1$' "$BUILD/CMakeCache.txt"; then
        echo "ERROR: OpenXR loader not found — this build would have NO openxr support." >&2
        echo "       Install it (Arch: sudo pacman -S openxr vulkan-headers) and re-run." >&2
        exit 1
    fi
    # Shared build mutex: all HypXRland builds on this box serialize through it.
    flock -w 7200 /tmp/hypxrland-build.lock \
        cmake --build "$BUILD" --target Hyprland hyprctl -j"$JOBS"
    # In-session utilities: this dir is prepended to PATH by the session
    # launcher so `hyprctl` (with the openxr command + matching IPC surface)
    # wins over the distro package inside the HypXRland session only.
    mkdir -p "$BUILD/bin"
    ln -sf "$BUILD/hyprctl/hyprctl" "$BUILD/bin/hyprctl"
    echo "==> fishfood binary: $BUILD/Hyprland ($(git -C "$WORKTREE" rev-parse --short HEAD))"
}

# The XReal runtime (monado-service + libopenxr_monado.so) lives in this worktree's
# subprojects/monado/build-xreal tree and ~/.config/xreal/monado.env points straight at it —
# a submodule pointer bump without a rebuild leaves the SERVICE on a stale binary while the
# sources move (the partial-deploy trap, docs/openxr/07-xreal.md). Rebuild whenever the xreal
# flavor has been built before; build-monado.sh builds BOTH targets together and re-applies
# the monado-service setcap via the scoped sudoers rule (sudo -n, never prompts).
build_monado_xreal() {
    if [[ -d $WORKTREE/subprojects/monado/build-xreal ]]; then
        flock -w 7200 /tmp/hypxrland-build.lock \
            env HYPXRLAND_BUILD_JOBS="$JOBS" "$WORKTREE/scripts/build-monado.sh" --xreal
    fi
}

gen_session() {
    mkdir -p "$(dirname "$DESKTOP_OUT")"
    local launcher="$(dirname "$DESKTOP_OUT")/launch.sh"
    # Launcher indirection: the installed .desktop Exec never changes, so PATH
    # tweaks / flag changes only need regenerating this user-owned script.
    # PATH is exported BEFORE uwsm start — uwsm imports the caller environment
    # into the systemd user manager, so Hyprland, binds (uwsm-app) and
    # terminals all see the fishfood bin dir first.
    cat >"$launcher" <<EOF
#!/bin/sh
BIN=$BUILD/Hyprland
# Fail LOUDLY instead of bouncing silently back to the greeter. A system upgrade
# that bumps a shared-library soname (aquamarine 0.13 -> 0.14 on 2026-08-21)
# leaves this binary unable to load at all; the greeter just shows the login
# screen again with no diagnostics. Detect it here, log it, start the STOCK
# session so the box stays usable, and say what happened once a notifier is up.
# Two checks: ldd catches a soname bump; --version catches a binary truncated by
# a reboot or crash mid-link (ldd happily resolves a half-written file).
missing=\$(ldd "\$BIN" 2>&1 | grep 'not found')
if [ -z "\$missing" ] && ! "\$BIN" --version >/dev/null 2>&1; then
    missing="\$BIN --version failed (truncated or corrupt binary?)"
fi
if [ -n "\$missing" ]; then
    log=\$HOME/.local/state/hypxrland/launch-failure.log
    mkdir -p "\$(dirname "\$log")"
    { echo "=== \$(date -Is) fishfood binary cannot load:"; echo "\$missing"; echo "fix: $REPO/scripts/fishfood.sh update"; } >>"\$log"
    ( for _ in 1 2 3 4 5 6; do sleep 10; notify-send -u critical -t 0 "HypXRland fishfood build is broken" \\
        "\$(echo "\$missing" | head -3)
Started the STOCK session instead. Fix: $REPO/scripts/fishfood.sh update  (log: \$log)" && break; done ) &
    exec uwsm start -e -D Hyprland -- /usr/bin/Hyprland
fi
export PATH="$BUILD/bin:\$PATH"
export HYPXRLAND_SESSION=1
exec uwsm start -e -D Hyprland -- \$BIN --config $CONF
EOF
    chmod +x "$launcher"
    # DesktopNames stays "Hyprland" so portals/theming/apps treat the session
    # exactly like the stock one; only the entry Name differs at the greeter.
    # The file and Name carry "fishfood" so a packaged HypXRland (custom Arch
    # repo, 2026-08) can own the plain hypxrland / omarchy-xr entries without
    # colliding with this developer session.
    cat >"$DESKTOP_OUT" <<EOF
[Desktop Entry]
Name=Omarchy XR (fishfood)
Comment=Hyprland + OpenXR (fishfood build from $WORKTREE)
Exec=$launcher
TryExec=uwsm
DesktopNames=Hyprland
Type=Application
Keywords=tiling;wayland;compositor;openxr;vr;
EOF
    echo "==> session launcher generated at $launcher"
    echo "==> session file generated at $DESKTOP_OUT"
    echo "    Install it (needs root):"
    echo "      sudo install -m644 $DESKTOP_OUT /usr/share/wayland-sessions/omarchy-xr-fishfood.desktop"
    echo "    Then pick 'Omarchy XR (fishfood)' from the session menu at the SDDM login screen."
}

case ${1:-} in
    setup)
        if [[ ! -d $WORKTREE/.git && ! -f $WORKTREE/.git ]]; then
            git -C "$REPO" worktree add "$WORKTREE" -b "$BRANCH" "$TRACK"
        fi
        git -C "$WORKTREE" submodule update --init
        build
        gen_session
        ;;
    update)
        git -C "$WORKTREE" merge --ff-only "$TRACK"
        # sync BEFORE update: submodule remotes keep whatever URL they were cloned with, so a
        # .gitmodules URL change (e.g. monado -> the AndrewGaspar fork carrying our commits) never
        # reaches an existing checkout without it -> "fatal: not our ref <sha>" on fetch.
        git -C "$WORKTREE" submodule sync --recursive
        git -C "$WORKTREE" submodule update --init
        build
        build_monado_xreal
        echo "==> done. Log out and pick Omarchy XR (fishfood) again (a running session keeps its old binary image)."
        ;;
    gen-session)
        gen_session
        ;;
    *)
        sed -n '2,15p' "${BASH_SOURCE[0]}"
        exit 1
        ;;
esac
