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

set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORKTREE=${HYPXRLAND_FISHFOOD:-$HOME/code/hypxrland}
BRANCH=${HYPXRLAND_FISHFOOD_BRANCH:-fishfood}
TRACK=${HYPXRLAND_FISHFOOD_TRACK:-hypxrland}
CONF=${HYPXRLAND_FISHFOOD_CONF:-$HOME/.config/hypr/hyprland-xr.conf}
BUILD=$WORKTREE/build
DESKTOP_OUT=${XDG_DATA_HOME:-$HOME/.local/share}/hypxrland/hypxrland.desktop
JOBS=${HYPXRLAND_FISHFOOD_JOBS:-8} # deliberately modest: this box has frozen twice under heavy parallel builds

build() {
    cmake -S "$WORKTREE" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
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
export PATH="$BUILD/bin:\$PATH"
export HYPXRLAND_SESSION=1
exec uwsm start -e -D Hyprland -- $BUILD/Hyprland --config $CONF
EOF
    chmod +x "$launcher"
    # DesktopNames stays "Hyprland" so portals/theming/apps treat the session
    # exactly like the stock one; only the entry Name differs at the greeter.
    cat >"$DESKTOP_OUT" <<EOF
[Desktop Entry]
Name=HypXRland
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
    echo "      sudo install -m644 $DESKTOP_OUT /usr/share/wayland-sessions/hypxrland.desktop"
    echo "    Then pick 'HypXRland' from the session menu at the SDDM login screen."
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
        git -C "$WORKTREE" submodule update --init
        build
        echo "==> done. Log out and pick HypXRland again (a running session keeps its old binary image)."
        ;;
    gen-session)
        gen_session
        ;;
    *)
        sed -n '2,15p' "${BASH_SOURCE[0]}"
        exit 1
        ;;
esac
