#!/bin/bash

# xreal-mode — the ONE place that knows how to put the XREAL Air 2 Ultra glasses
# into a mode, and the ONE place that answers "which mode are they in?".
#
#   xreal-mode status [--json]   machine-readable state (see FIELDS below)
#   xreal-mode off               glasses stop being a desktop output
#   xreal-mode 2d                flat 1920x1080 head-locked desktop
#   xreal-mode 3d                3840x1080 SBS stereo desktop (compositor pack)
#   xreal-mode xr                OpenXR HMD via the xreal-flavor Monado, in
#                                DRM-lease direct mode (--window: the old
#                                fullscreen-window mode)
#   xreal-mode toggle            2d <-> 3d, direction taken from the DRM personality
#
#   --dry-run     print every action without doing any of it. Safe to run always.
#   --foreground  run the transition attached instead of detached, so its output
#                 comes back on this terminal. For hand-testing; the default is
#                 detached (see note 7).
#
# Everything that touches the glasses goes through here: the bar plugin
# (~/.config/omarchy/plugins/andrewgaspar.xreal), the SUPER+ALT+S bind
# (scripts/hypxr-stereo-toggle.sh), and anything else added later. The tray is
# thin on purpose — this script is the source of truth.
#
# ---------------------------------------------------------------------------
# WHY THIS EXISTS (hard-won, do not re-derive by hurting the hardware)
#
# 1. NEVER rapid-cycle the firmware personality. Every entry point serializes on
#    one lock and paces consecutive `xreal-ctl mode` calls at least
#    XREAL_MIN_FLIP_MS apart (default 5s).
#
# 2. The firmware REFUSES a display-mode switch (button AND HID alike) while the
#    incoming signal is a mode its CURRENT personality does not advertise. So
#    every flip is: drive a mode the current personality advertises -> flip ->
#    wait for the kernel's mode list to change -> assert the new rule. Ordering
#    is load-bearing in both directions.
#
# 3. NEVER trust the HID ack. `xreal-ctl mode 3d` returns success long before
#    (or without) anything happening on the wire. The only real signal is the
#    DRM mode list changing: /sys/class/drm/card*-<conn>/modes.
#
# 4. State detection comes from DRM/EDID + systemd, NEVER from the compositor's
#    `stereo` field. There is a live bug where the stereo token sticks after a
#    mode fallback (sticky `sbs` on a 1920 scanout), and the old toggle script
#    read exactly that field to pick its direction — so it sent the wrong way
#    round precisely when things were already wrong. `stereo` is REPORTED here
#    (as advisory) and used for NOTHING.
#
# 5. After a personality flip the compositor can sit on a stale scanout until
#    the monitor rule is re-asserted. Every transition therefore ends with an
#    explicit re-assert through scripts/lib/hypr-config.sh.
#
# 6. Config pushes go through scripts/lib/hypr-config.sh, never bare
#    `hyprctl keyword` — that is dead under the Lua config manager AND STILL
#    EXITS 0, so a stale call fails silently.
#
# 7. A transition must OUTLIVE WHOEVER STARTED IT. The bar widget spawns these
#    as children of the Quickshell shell; that shell crashed three times on
#    2026-08-31 and took a transition with it, mid-flip. Mutating verbs
#    therefore detach into their own session (see the entry point) and the
#    "something is running" signal is the LOCK, not a flag file — a flag file
#    cannot die with its owner, and the one that did not left the menu greyed
#    out forever.
#
# 8. THE WEAR SENSOR POWERS THE HID SIDE. Off your head, the glasses' whole USB
#    device leaves the bus in seconds while DP video stays up: still a working
#    monitor, still a tray icon, and `xreal-ctl` finds nothing. Every verb that
#    needs a firmware flip refuses up front with `hid=asleep` rather than
#    starting something that cannot finish. Verbs that only re-assert a monitor
#    rule keep working, which is how a wedged desktop gets fixed without
#    putting the glasses on.
#
# ---------------------------------------------------------------------------
# FIELDS printed by `status` (key=value, one per line; --json for the same data)
#
#   connected     yes|no      an XREAL is attached (DRM/EDID match OR USB present)
#   drm           yes|no      the DP connector is present and EDID-matched
#   usb           yes|no      the 3318:0426 HID device is enumerated
#   hid           awake|asleep|absent   can the firmware be TOLD anything?
#                             asleep = video up, HID powered down by the wear
#                             sensor. No flip can succeed until they are worn.
#   connector     DP-5|""     kernel connector name (the port varies! never cache it)
#   personality   2d|3d|unknown   from the kernel mode list (3840 present => 3d)
#   scanout       WxH@R|""    what the COMPOSITOR believes it is driving
#   enabled       yes|no|unknown  is it an enabled desktop output right now
#   stereo        sbs|off|unknown ADVISORY ONLY — see note 4 above
#   xr_service    active|inactive|failed|...   systemctl --user is-active
#   xr_session    <state word>  first word of `hyprctl openxr status`
#   xr_runtime    xreal|other|none   which manifest the live session is on
#   mode          absent|off|2d|3d|xr|unknown   the derived answer
#   busy          ""|<verb>   a transition is in flight right now (lock-backed)
#   busy_stale    yes|no      a marker outlived its process and was cleared
#   divergence    comma list  where the two sources disagree; see below
#
# `divergence` never changes what the other fields say — it records that they
# disagree, so nothing has to silently pick a winner:
#
#   sticky-stereo               the rule carries stereo:sbs but the glasses are
#                               not in the 3D personality: a flat panel being
#                               side-by-side packed. `2d` clears it.
#   scanout-wider-than-modelist hyprctl reports >=3840 while the kernel mode
#                               list offers no such mode. The compositor is on a
#                               stale or forced mode; the kernel list is what
#                               says whether the firmware switched.
#   hid-asleep                  see `hid` above.
#   stale-busy-cleared          a crashed transition's marker was just cleaned up.
#
# `mode` is what the menu ticks. It is derived, in this order:
#   no glasses            -> absent
#   xr service + xreal runtime -> xr
#   output disabled       -> off
#   personality 3d        -> 3d
#   personality 2d        -> 2d
#   otherwise             -> unknown
#
# Exit codes: 0 ok/started · 2 usage · 69 glasses asleep · 75 already in flight

# REPO MIRROR. The canonical copy is bin/xreal-mode in the user's Hyprland
# dotfiles (github.com/AndrewGaspar/omarchy-hyprland-config, installed as
# ~/.config/hypr/bin/xreal-mode); this file is that script verbatim (as of
# dotfiles ed90a50) plus this note, with scripts/lib/hypr-config.sh mirrored
# beside it. Change the canonical copy first, then re-sync this one. It
# replaces the old WP-XR1 toggle (window-mode SBS default, --mono, classic
# `hyprctl dispatch dpms`, which is a Lua syntax error under the Lua config).

set -uo pipefail

self_dir="$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)"

# Front-end-agnostic config pushes (hypr_set_option / hypr_set_monitor). Under
# the Lua config manager these emit `hyprctl eval 'hl...'`; on a machine still
# on the classic .conf front end they emit `hyprctl keyword`.
# shellcheck source=../scripts/lib/hypr-config.sh
. "$self_dir/../scripts/lib/hypr-config.sh"

# ---- configuration (every value overridable; nothing here is host-specific) --

DESC="${XREAL_DESC:-Nreal Air 2 Ultra 0x88888800}"
# Substring used to recognise the glasses in an EDID blob and in Hyprland's
# EDID-derived description. Short enough to fit EDID's 13-byte product-name
# descriptor, specific enough not to collide with anything else.
MATCH="${XREAL_MATCH:-Air 2 Ultra}"
# How every compositor call names the glasses. Never the kernel connector name:
# that changes across the flip hotplug (DP-3 one day, DP-5 the next) and between
# machines, and two GPUs can each carry a DP-1. The EDID description travels
# with the glasses. Hyprland matches `desc:` as a prefix.
MON_SEL="desc:$DESC"

USB_VID="${XREAL_USB_VID:-3318}"
USB_PID="${XREAL_USB_PID:-0426}"

FLAT_MODE="${XREAL_FLAT_MODE:-1920x1080@60}"
FLAT_SCALE="${XREAL_FLAT_SCALE:-1}"
SBS_MODE="${XREAL_SBS_MODE:-3840x1080@60}"
SBS_WIDTH=3840

# Minimum gap between two firmware personality flips, milliseconds. The glasses
# do not enjoy being cycled; when a request lands inside the window we WAIT it
# out rather than refuse, because we already hold the lock.
MIN_FLIP_MS="${XREAL_MIN_FLIP_MS:-5000}"

XR_UNIT="${XREAL_SERVICE_UNIT:-monado-xreal.service}"
XR_DROPIN="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/$XR_UNIT.d/10-xreal-direct.conf"

# How `xr` reaches the glasses. direct = Monado leases the DP connector and
# scans out itself (DRM lease, wp_drm_lease_v1); window = Monado draws a
# fullscreen window the compositor composites onto a 3840 desktop. --direct /
# --window override per call.
XR_BACKEND="${XREAL_XR_BACKEND:-direct}"

# The HypXRland checkout that carries the xreal Monado build and xreal-ctl.
for _r in "${HYPXRLAND_REPO:-}" "$HOME/code/Hyprland" "$HOME/code/hypxrland"; do
    [[ -n $_r && -d $_r/subprojects/monado ]] && { REPO="$_r"; break; }
done
REPO="${REPO:-$HOME/code/Hyprland}"
MONADO_XREAL_BUILD="${MONADO_XREAL_BUILD:-$REPO/subprojects/monado/build-xreal}"
RUNTIME_JSON="${XREAL_RUNTIME_JSON:-$MONADO_XREAL_BUILD/openxr_monado-dev.json}"

# xreal-ctl: PATH first (it may be installed to /usr/local/bin), then the repo.
XREAL_CTL="${XREAL_CTL:-}"
if [[ -z $XREAL_CTL ]]; then
    if command -v xreal-ctl >/dev/null 2>&1; then
        XREAL_CTL="$(command -v xreal-ctl)"
    else
        for _c in "$REPO/contrib/xreal/xreal-ctl" "$HOME/code/Hyprland/contrib/xreal/xreal-ctl" \
                  "$HOME/code/hypxrland/contrib/xreal/xreal-ctl"; do
            [[ -x $_c ]] && { XREAL_CTL="$_c"; break; }
        done
    fi
fi

STATE_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/hypxr"
LOCK="$STATE_DIR/xreal-mode.lock"
BUSY_FILE="$STATE_DIR/xreal-mode.busy"
FLIP_STAMP="$STATE_DIR/xreal-mode.lastflip"
LOG="$STATE_DIR/xreal-mode.log"
# Present while the glasses' monitor rule carries `lease = true`. Holds the
# compositor instance it was set in: a rule does not outlive its compositor, so
# a marker from an earlier login means nothing.
LEASE_MARK="$STATE_DIR/xreal-mode.lease"

DRY_RUN=0
FOREGROUND=0

# Exit codes, so a caller can tell the failures apart without parsing prose.
EX_TEMPFAIL=75      # another transition is in flight
EX_HID_ASLEEP=69    # glasses present but their HID side is powered down

# ---- plumbing ---------------------------------------------------------------

log() { printf '  %s\n' "$*"; }
err() { printf '!! %s\n' "$*" >&2; }

notify() {
    [[ $DRY_RUN -eq 1 ]] && { log "[notify] $1"; return 0; }
    if command -v omarchy-notification-send >/dev/null 2>&1; then
        omarchy-notification-send -g 󰌸 "$1" >/dev/null 2>&1 || true
    else
        notify-send -u low "XReal" "$1" >/dev/null 2>&1 || true
    fi
}

run() {
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] %s\n' "$*"
        return 0
    fi
    printf '  + %s\n' "$*"
    "$@"
}

# Dry-run-aware wrappers around lib/hypr-config.sh. EVERY config push goes
# through one of these three — a bare hypr_* call would fire during --dry-run.
#
# The option value is a Lua literal, so a string needs its own quotes:
#   set_option openxr:gpu '"/dev/dri/renderD129"'
set_option() {
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] hypr_set_option %s %s\n' "$1" "$2"
        return 0
    fi
    printf '  + hypr_set_option %s %s\n' "$1" "$2"
    hypr_set_option "$1" "$2"
}

set_monitor() { # <output> <mode> <position> <scale> [stereo]
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] hypr_set_monitor %s\n' "$*"
        return 0
    fi
    printf '  + hypr_set_monitor %s\n' "$*"
    hypr_set_monitor "$@"
}

disable_monitor() { # <output>
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] hypr_disable_monitor %s\n' "$1"
        return 0
    fi
    printf '  + hypr_disable_monitor %s\n' "$1"
    hypr_disable_monitor "$1"
}

have() { command -v "$1" >/dev/null 2>&1; }

now_ms() { printf '%s' "$(( $(date +%s%N) / 1000000 ))"; }

# ---- detection: DRM / EDID is the ground truth ------------------------------

# The mode-flip hotplug can RENAME the connector (DP-3 one day, DP-5 the next),
# so nothing may cache a /sys path across a flip. Re-find it by EDID identity
# on every call, tolerating the window where it is briefly absent.
#
# NOT `[[ -s $e ]]`. A sysfs binary attribute reports stat size 0 while reading
# back its real contents -- every edid file on this box stats as 0 and reads
# 128/256 bytes -- so an `-s` guard skips ALL of them, silently disables this
# whole path, and leaves the hyprctl fallback below quietly doing the work it
# was supposed to be double-checking. Test readability; let grep judge content.
sys_dir() {
    local e d
    for e in /sys/class/drm/card*-*/edid; do
        [[ -r $e ]] || continue
        if /usr/bin/grep -a -q -- "$MATCH" "$e" 2>/dev/null; then
            d="$(dirname "$e")"
            [[ "$(cat "$d/status" 2>/dev/null)" == connected ]] || continue
            printf '%s' "$d"
            return 0
        fi
    done
    # Fallback: ask the compositor, which derives the same string from the same
    # EDID, and map its monitor name back to a /sys dir. Covers a kernel that
    # exposes no readable edid file.
    local name
    name="$(hypr_monitor_field name)" || return 1
    [[ -n $name ]] || return 1
    for d in /sys/class/drm/card*-"$name"; do
        [[ -e $d ]] && { printf '%s' "$d"; return 0; }
    done
    return 1
}

connector_name() { # <sysdir> -> DP-5
    local base
    base="$(basename "$1")"   # card2-DP-5
    printf '%s' "${base#*-}"
}

usb_present() {
    local d vid pid
    for d in /sys/bus/usb/devices/*; do
        [[ -r $d/idVendor && -r $d/idProduct ]] || continue
        read -r vid < "$d/idVendor" 2>/dev/null || continue
        read -r pid < "$d/idProduct" 2>/dev/null || continue
        [[ ${vid,,} == "${USB_VID,,}" && ${pid,,} == "${USB_PID,,}" ]] && return 0
    done
    return 1
}

# ---- the wear sensor, and why every firmware verb has to ask about it -------
#
# THE GLASSES' USB/HID SIDE IS POWERED BY THE WEAR SENSOR. Take them off, put
# them on the desk, and the whole 3318:0426 device leaves the bus within
# seconds while DP video stays up -- so the panel is still a working monitor,
# the tray icon is still there, and `xreal-ctl` says "no supported XREAL device
# found". Observed live 2026-08-31: the device re-enumerated at 19:55:05,
# 19:55:37, 19:58:01 and 20:00:39 and was absent in between, purely from the
# glasses sitting on a desk. It reads like a flapping cable and is not one.
#
# This is a UX trap, because the moment you are most likely to open a menu is
# the moment you are NOT wearing them. So:
#
#   * `hid` is a first-class status field, not an internal detail.
#   * Any verb needing a firmware flip refuses BEFORE touching anything when
#     hid is asleep, and says what to do about it.
#   * Verbs that only re-assert a monitor rule (`off`, and `2d`/`3d` when
#     already in that personality) do NOT need HID and must keep working --
#     that is how you clear a sticky-stereo desktop without putting the glasses
#     on, which is exactly the state a failed transition leaves behind.
#
# Cheap by design: sysfs enumeration only, no HID open. Opening the control
# interface every poll would contend with monado during an XR session, so the
# real `xreal-ctl detect` probe happens once, in the preflight of a flip.
hid_state() { # awake|asleep|absent
    if usb_present; then
        printf 'awake'
    elif [[ $S_DRM == yes ]]; then
        printf 'asleep'   # video is up but the HID side is powered down
    else
        printf 'absent'
    fi
}

HID_HINT="glasses asleep — put them on (or cover the wear sensor) so their USB/HID side powers up, then retry"

# The personality, straight off the kernel's mode list. 3840 present => the
# glasses re-presented their 3D EDID; that is the ONLY trustworthy signal that
# the firmware actually switched.
personality() { # <sysdir>
    local modes="$1/modes"
    [[ -r $modes ]] || { printf 'unknown'; return; }
    if awk -F x '$1+0 >= 3840 { found = 1 } END { exit !found }' "$modes" 2>/dev/null; then
        printf '3d'
    elif awk -F x '$1+0 >= 1280 { found = 1 } END { exit !found }' "$modes" 2>/dev/null; then
        printf '2d'
    else
        printf 'unknown'
    fi
}

modes_list() { tr '\n' ' ' < "$1/modes" 2>/dev/null; }

# One hyprctl read, cached for the life of the process, so `status` costs one
# subprocess rather than five.
MON_JSON=""
mon_json() {
    [[ -n $MON_JSON ]] && { printf '%s' "$MON_JSON"; return 0; }
    MON_JSON="$(hyprctl -j monitors all 2>/dev/null || true)"
    printf '%s' "$MON_JSON"
}
mon_json_refresh() { MON_JSON=""; mon_json >/dev/null; }

hypr_monitor_field() { # <jq-field> -> value for the glasses' monitor, "" if absent
    have jq || return 1
    local v
    v="$(mon_json | jq -r --arg m "$MATCH" --arg f "$1" '
        [ .[] | select((.description // "") | contains($m)) ] | (.[0][$f] // empty)' 2>/dev/null)" || return 1
    printf '%s' "$v"
}

scanout() {
    have jq || { printf ''; return; }
    mon_json | jq -r --arg m "$MATCH" '
        [ .[] | select((.description // "") | contains($m)) ]
        | (.[0] | if . == null then empty else "\(.width)x\(.height)@\(.refreshRate | floor)" end)' 2>/dev/null
}

monitor_width() {
    local w
    w="$(hypr_monitor_field width)" || w=0
    [[ $w =~ ^[0-9]+$ ]] || w=0
    printf '%s' "$w"
}

# NOTE the explicit `== true` rather than jq's `//` default operator: `false //
# "x"` yields "x" in jq, so `.disabled // empty` swallows exactly the case we
# care about (an enabled monitor) and reports it as unknown.
output_enabled() { # yes|no|unknown
    have jq || { printf 'unknown'; return; }
    mon_json | jq -r --arg m "$MATCH" '
        [ .[] | select((.description // "") | contains($m)) ]
        | if length == 0 then "unknown"
          elif (.[0].disabled == true) then "no"
          else "yes" end' 2>/dev/null || printf 'unknown'
}

stereo_token() { # ADVISORY ONLY. Never branch on this — see note 4 in the header.
    have jq || { printf 'unknown'; return; }
    mon_json | jq -r --arg m "$MATCH" '
        [ .[] | select((.description // "") | contains($m)) ]
        | if length == 0 then "unknown"
          elif ((.[0].stereo // "") | length) > 0 then .[0].stereo
          else "off" end' 2>/dev/null || printf 'unknown'
}

# Is there another enabled monitor to look at? The `off` guard: never blank the
# only display. Mirrors bin/hypxr-monitor-desk's inverted-guard reasoning.
other_enabled_monitor() {
    have jq || return 0   # can't tell -> don't block the user
    mon_json | jq -e --arg m "$MATCH" '
        any(.[]; ((.description // "") | contains($m) | not) and ((.disabled // false) | not))' >/dev/null 2>&1
}

# ---- detection: the XR side -------------------------------------------------

xr_service_state() { systemctl --user is-active "$XR_UNIT" 2>/dev/null || true; }

openxr_status() { hyprctl openxr status 2>/dev/null || true; }

xr_session_state() { # first word of "state: ..."
    openxr_status | awk '/^state:/ { print $2; exit }'
}

xr_runtime_kind() { # xreal|other|none
    local line
    line="$(openxr_status | awk -F': ' '/^runtime json:/ { print $2; exit }')"
    if [[ -z $line || $line == "(loader default)" ]]; then
        printf 'none'
    elif [[ $line == *"$(basename "$MONADO_XREAL_BUILD")/$(basename "$RUNTIME_JSON")" || $line == "$RUNTIME_JSON" ]]; then
        printf 'xreal'
    else
        printf 'other'
    fi
}

# True while Monado may hold a DRM lease on the connector (direct mode, entered
# by `xr` here or by the repo's scripts/xreal-mode.sh --direct). Such a session
# must be torn down lease-first or the compositor deadlocks and the whole
# session hangs. See docs/openxr/07-xreal.md.
direct_mode_active() {
    [[ -f $XR_DROPIN ]] || return 1
    systemctl --user is-active --quiet "$XR_UNIT"
}

# ---- "is a transition in flight?", answered crash-safely --------------------
#
# The first version of this was a bare flag file, and on 2026-08-31 that cost a
# live session: a transition died mid-flight (the shell that spawned it
# crashed), the flag survived with no process behind it, and the menu greyed
# itself out FOREVER -- an indeterminate state with no way back except deleting
# a file by hand. A flag file cannot express "in flight", because it cannot die
# with its owner.
#
# What can is the LOCK. flock lives on an open file description, so the kernel
# drops it when the last holder exits -- SIGKILL, crash, power of positive
# thinking, all the same. So:
#
#   the lock is the truth   -- held == a transition is genuinely running
#   the file is the label   -- it only carries pid/starttime/verb for display
#
# and a file with no lock behind it is by definition stale: cleared on sight and
# reported as `busy_stale=yes` rather than papered over. The pid+starttime pair
# is a second, independent liveness check (start time defeats pid reuse).

# True iff someone currently holds the transition lock. Takes it non-blockingly
# on a throwaway fd in a subshell and drops it immediately, which cannot disturb
# a real holder -- a FAILED acquisition is itself the answer.
lock_is_held() {
    [[ -e $LOCK ]] || return 1
    ( exec 8>"$LOCK"; flock -n 8 ) && return 1 || return 0
}

proc_starttime() { # <pid> -> field 22 of /proc/<pid>/stat, empty if gone
    local st
    [[ -r /proc/$1/stat ]] || return 1
    st="$(< "/proc/$1/stat")" || return 1
    # comm can contain spaces and parens, so count from the LAST ')': starttime
    # is overall field 22, which is field 20 of the remainder.
    printf '%s' "$(printf '%s' "${st##*) }" | awk '{ print $20 }')"
}

busy_write() { # <verb> <pid>
    local st
    st="$(proc_starttime "$2" 2>/dev/null)" || st=0
    printf '%s %s %s\n' "$2" "${st:-0}" "$1" > "$BUSY_FILE"
}

BUSY_STALE=no

# Sets BUSY_VERB and BUSY_STALE. Deliberately NOT a stdout-returning function:
# `x=$(busy_verb)` would run it in a subshell, where the stale-marker discovery
# could not propagate back -- the file would still be cleaned up, but nobody
# would ever be told that a transition had been killed mid-flight.
BUSY_VERB=""
busy_probe() {
    BUSY_STALE=no
    BUSY_VERB=""

    # The lock is the ONLY authority on "is something running". Nothing else on
    # the system takes it, so held == a transition is in flight, full stop.
    if ! lock_is_held; then
        if [[ -e $BUSY_FILE ]]; then
            rm -f "$BUSY_FILE" 2>/dev/null || true
            BUSY_STALE=yes     # a marker outlived its process: the 2026-08-31 wedge
        fi
        return 0
    fi

    # Lock held. The file is only a LABEL for what is running, so a missing or
    # out-of-date label must never read as "nothing is running" -- that would
    # re-enable the menu underneath a live firmware flip, the one thing this
    # lock exists to prevent.
    [[ -r $BUSY_FILE ]] || { BUSY_VERB=in-flight; return 0; }

    local pid st verb live_st
    read -r pid st verb < "$BUSY_FILE" 2>/dev/null || true
    [[ -n ${verb:-} ]] || verb=in-flight

    # A pid/starttime mismatch means the LABEL is stale, not the transition --
    # it happens for a moment while the detached child rewrites the file with
    # its own pid -- so flag it and keep reporting busy.
    if [[ -n ${pid:-} && $pid =~ ^[0-9]+$ ]]; then
        if ! live_st="$(proc_starttime "$pid")" || { [[ ${st:-0} != 0 ]] && [[ $live_st != "${st:-0}" ]]; }; then
            BUSY_STALE=yes
        fi
    fi

    BUSY_VERB="$verb"
    return 0
}

# ---- state assembly ---------------------------------------------------------

S_DRM=no S_USB=no S_CONN="" S_PERS=unknown S_SCAN="" S_EN=unknown S_STEREO=unknown
S_XRSVC=unknown S_XRSESS="" S_XRRT=none S_MODE=unknown S_CONNECTED=no S_SYSDIR=""
S_HID=absent S_DIVERGE="" S_BUSY=""

# Two sources answer two DIFFERENT questions, and conflating them is how the old
# toggle got its direction wrong. Keep them apart, and when they contradict each
# other SAY SO rather than silently picking a winner:
#
#   /sys/.../modes  is the SINK. Regenerated from the EDID on every connector
#                   probe, so it is authoritative for PERSONALITY -- what the
#                   glasses' firmware is actually presenting.
#   hyprctl         is the COMPOSITOR. It reports the mode it believes it is
#                   driving, which can be a forced/unadvertised modeline, or
#                   simply stale between the connector dropping and the rule
#                   pass reconciling.
#
# So when hyprctl says 3840 and the kernel mode list has no 3840 (observed live
# 2026-08-31), hyprctl is not lying about ITS state -- it is answering a
# different question, and only the kernel list can say whether the firmware
# switched. Personality therefore always comes from the kernel list, and the
# disagreement is published in `divergence` for a human (or the widget) to see.
diverge_add() { S_DIVERGE="${S_DIVERGE:+$S_DIVERGE,}$1"; }

detect_divergence() {
    S_DIVERGE=""

    # The compositor thinks it is driving a wide mode the sink does not offer.
    local w="${S_SCAN%%x*}"
    if [[ $w =~ ^[0-9]+$ ]] && (( w >= SBS_WIDTH )) && [[ $S_PERS != 3d ]]; then
        diverge_add scanout-wider-than-modelist
    fi

    # The sticky-stereo bug: the rule's stereo token survived a mode fallback,
    # so the desktop is being side-by-side packed onto a flat panel. This is
    # what "squished" looks like from the outside, and `2d` clears it.
    if [[ $S_STEREO == sbs && $S_PERS != 3d ]]; then
        diverge_add sticky-stereo
    fi

    # Video up, HID gone: not an error, but it is why a flip would refuse.
    [[ $S_HID == asleep ]] && diverge_add hid-asleep

    [[ $BUSY_STALE == yes ]] && diverge_add stale-busy-cleared
    return 0
}

collect() {
    local d
    # Reset first: collect() is re-run after every teardown step, and a stale
    # `yes` left over from the previous pass would outlive the thing it described.
    S_DRM=no S_USB=no S_CONN="" S_PERS=unknown S_SCAN="" S_EN=unknown S_STEREO=unknown
    S_XRSVC=unknown S_XRSESS="" S_XRRT=none S_MODE=unknown S_CONNECTED=no S_SYSDIR=""
    S_HID=absent S_DIVERGE=""

    if d="$(sys_dir)"; then
        S_SYSDIR="$d"
        S_DRM=yes
        S_CONN="$(connector_name "$d")"
        S_PERS="$(personality "$d")"
    fi
    usb_present && S_USB=yes
    S_HID="$(hid_state)"

    # "Connected" is deliberately DRM-or-USB. In 2D with the glasses off your
    # face the wear sensor powers the panel down and the connector can read
    # `disconnected` at the DRM layer (docs/openxr/07-xreal.md §0.7) — the cable
    # is still plugged in and the tray icon must not flicker away.
    [[ $S_DRM == yes || $S_USB == yes ]] && S_CONNECTED=yes

    S_SCAN="$(scanout)"
    S_EN="$(output_enabled)"
    S_STEREO="$(stereo_token)"
    S_XRSVC="$(xr_service_state)"
    S_XRSESS="$(xr_session_state)"
    S_XRRT="$(xr_runtime_kind)"

    if [[ $S_CONNECTED == no ]]; then
        S_MODE=absent
    elif [[ $S_XRSVC == active && $S_XRRT == xreal ]]; then
        S_MODE=xr
    elif [[ $S_EN == no ]]; then
        S_MODE=off
    elif [[ $S_PERS == 3d ]]; then
        S_MODE=3d
    elif [[ $S_PERS == 2d ]]; then
        S_MODE=2d
    else
        S_MODE=unknown
    fi

    # busy_verb() clears a stale marker and sets BUSY_STALE, so it has to run
    # before divergence if `stale-busy-cleared` is to be reported at all.
    busy_probe
    S_BUSY="$BUSY_VERB"
    detect_divergence
}

print_status_kv() {
    cat <<EOF
connected=$S_CONNECTED
drm=$S_DRM
usb=$S_USB
hid=$S_HID
connector=$S_CONN
personality=$S_PERS
scanout=$S_SCAN
enabled=$S_EN
stereo=$S_STEREO
xr_service=$S_XRSVC
xr_session=$S_XRSESS
xr_runtime=$S_XRRT
mode=$S_MODE
busy=$S_BUSY
busy_stale=$BUSY_STALE
divergence=$S_DIVERGE
xreal_ctl=$XREAL_CTL
runtime_json=$RUNTIME_JSON
runtime_built=$([[ -f $RUNTIME_JSON ]] && echo yes || echo no)
EOF
    # Notes for the human reading this in a terminal. Machine consumers read the
    # `divergence` field above; these lines say what it means and what to do.
    local note
    for note in ${S_DIVERGE//,/ }; do
        case "$note" in
            sticky-stereo)
                err "note: sticky-stereo — the monitor rule still carries stereo:sbs while the"
                err "      glasses are NOT in the 3D personality, so a flat panel is being"
                err "      side-by-side packed ('squished'). Fix with: xreal-mode 2d" ;;
            scanout-wider-than-modelist)
                err "note: scanout-wider-than-modelist — hyprctl reports a >=${SBS_WIDTH}px mode that"
                err "      the kernel mode list does not advertise. The compositor is on a stale"
                err "      or forced mode; personality above comes from the kernel list, and it"
                err "      is the one that says whether the firmware actually switched." ;;
            hid-asleep)
                err "note: hid-asleep — video is up, but the glasses' USB/HID side is powered down."
                err "      $HID_HINT" ;;
            stale-busy-cleared)
                err "note: stale-busy-cleared — a busy marker outlived its process (a transition"
                err "      was killed mid-flight). Cleared automatically; nothing is in flight." ;;
        esac
    done
}

json_str() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }

print_status_json() {
    printf '{'
    printf '"connected":%s,' "$([[ $S_CONNECTED == yes ]] && echo true || echo false)"
    printf '"drm":%s,' "$([[ $S_DRM == yes ]] && echo true || echo false)"
    printf '"usb":%s,' "$([[ $S_USB == yes ]] && echo true || echo false)"
    printf '"hid":%s,' "$(json_str "$S_HID")"
    printf '"hid_hint":%s,' "$(json_str "$([[ $S_HID == asleep ]] && printf '%s' "$HID_HINT")")"
    printf '"connector":%s,' "$(json_str "$S_CONN")"
    printf '"personality":%s,' "$(json_str "$S_PERS")"
    printf '"scanout":%s,' "$(json_str "$S_SCAN")"
    printf '"enabled":%s,' "$(json_str "$S_EN")"
    printf '"stereo":%s,' "$(json_str "$S_STEREO")"
    printf '"xr_service":%s,' "$(json_str "$S_XRSVC")"
    printf '"xr_session":%s,' "$(json_str "$S_XRSESS")"
    printf '"xr_runtime":%s,' "$(json_str "$S_XRRT")"
    printf '"mode":%s,' "$(json_str "$S_MODE")"
    printf '"busy":%s,' "$(json_str "$S_BUSY")"
    printf '"busy_stale":%s,' "$([[ $BUSY_STALE == yes ]] && echo true || echo false)"
    printf '"divergence":%s,' "$(json_str "$S_DIVERGE")"
    printf '"runtime_built":%s' "$([[ -f $RUNTIME_JSON ]] && echo true || echo false)"
    printf '}\n'
}

# ---- the firmware flip ------------------------------------------------------

# Pace consecutive personality flips. We hold the lock, so waiting is free and
# strictly better than refusing the user's click.
pace_flip() {
    local last now wait_ms
    last=0
    [[ -r $FLIP_STAMP ]] && last="$(cat "$FLIP_STAMP" 2>/dev/null)"
    [[ $last =~ ^[0-9]+$ ]] || last=0
    now="$(now_ms)"
    wait_ms=$(( MIN_FLIP_MS - (now - last) ))
    if (( wait_ms > 0 )); then
        (( wait_ms > MIN_FLIP_MS )) && wait_ms=$MIN_FLIP_MS   # clock went backwards
        log "pacing the firmware: waiting $(( wait_ms ))ms since the last mode flip"
        [[ $DRY_RUN -eq 0 ]] && sleep "$(awk -v m="$wait_ms" 'BEGIN{printf "%.3f", m/1000}')"
    fi
    [[ $DRY_RUN -eq 0 ]] && now_ms > "$FLIP_STAMP"
    return 0
}

# Wait until the kernel's mode list says the glasses are in the wanted
# personality. Gating on personality() rather than "does width N appear" is the
# correct inverse in BOTH directions: the 3D EDID collapses to 3840-only, so
# waiting for 2D means waiting for 3840 to DISAPPEAR — waiting for a 1920 line
# to appear would be satisfied instantly by any host that still advertises one,
# and we would assert the flat rule against glasses that never switched.
#
# NEVER cache the /sys path across a flip: the connector can be renamed
# (DP-3 one day, DP-5 the next) and is briefly absent while the link
# renegotiates, so it is re-found by EDID identity on every poll.
wait_personality() { # <2d|3d> [poll-count, 0.25s each]
    local want="$1" tries="${2:-40}" i d
    for (( i = 0; i < tries; i++ )); do
        sleep 0.25
        d="$(sys_dir)" || continue
        [[ "$(personality "$d")" == "$want" ]] && return 0
    done
    return 1
}

# Drive the output at a mode the CURRENT personality advertises, so the firmware
# will accept the switch. This is the step whose absence makes a flip silently
# no-op: the firmware refuses while the incoming signal is unadvertised.
signal_advertised_mode() {
    case "$S_PERS" in
        3d) log "driving $SBS_MODE (advertised in the 3D personality) before the flip"
            set_monitor "desc:$DESC" "$SBS_MODE" auto 1 sbs ;;
        2d) log "driving $FLAT_MODE (advertised in the 2D personality) before the flip"
            set_monitor "desc:$DESC" "$FLAT_MODE" auto "$FLAT_SCALE" off ;;
        *)  log "personality unknown — driving $FLAT_MODE and hoping"
            set_monitor "desc:$DESC" "$FLAT_MODE" auto "$FLAT_SCALE" off ;;
    esac
    [[ $DRY_RUN -eq 0 ]] && sleep 1
    return 0
}

# Refuse a doomed transition BEFORE anything is driven at the glasses. Callers
# check this at the top of a verb, once collect() has told them a flip will
# actually be needed -- a verb that only re-asserts a rule must NOT be blocked
# by a sleeping HID, since that is precisely how a wedged desktop gets fixed
# while the glasses sit on the desk.
require_hid_awake() {
    [[ $S_HID == awake ]] && return 0
    if [[ $S_HID == asleep ]]; then
        err "$HID_HINT"
        err "(video is up on ${S_CONN:-the glasses}, but USB $USB_VID:$USB_PID is not enumerated —"
        err " the wear sensor powers the HID side down when they are off your head, so the"
        err " firmware cannot be told anything. Nothing was changed.)"
        notify "Glasses asleep — put them on to switch modes"
    else
        err "no XREAL HID device present ($USB_VID:$USB_PID not enumerated). Nothing was changed."
        notify "glasses not reachable over HID"
    fi
    return "$EX_HID_ASLEEP"
}

flip_firmware() { # <2d|3d>
    [[ -n $XREAL_CTL ]] || { err "xreal-ctl not found — build contrib/xreal, put it on PATH, or set XREAL_CTL"; return 1; }
    # Last line of defence. The device can leave the bus between the preflight
    # and here -- it re-enumerates on its own every couple of minutes while the
    # glasses idle on a desk -- and a flip into a vanished HID is a silent no-op
    # that we would then sit and wait ten seconds for.
    if [[ $DRY_RUN -eq 0 ]] && ! usb_present; then
        err "the glasses' HID left the bus just before the flip — $HID_HINT"
        return 1
    fi
    pace_flip
    run "$XREAL_CTL" mode "$1"
    return 0
}

# Re-assert the monitor rule. Bug (3) from the 2026-08-29 cluster: after a
# personality flip the compositor can sit on a stale scanout until the rule is
# pushed again, so every transition ends here.
assert_rule() { # <2d|3d|xr>
    case "$1" in
        2d) set_monitor "desc:$DESC" "$FLAT_MODE" auto "$FLAT_SCALE" off ;;
        3d) set_monitor "desc:$DESC" "$SBS_MODE" auto 1 sbs ;;
        # XR window mode wants a plain 3840 DESKTOP: the Monado surface is
        # already a stereo frame, so a compositor-side `sbs` pack on top would
        # double-pack it. stereo MUST be off here.
        xr) set_monitor "desc:$DESC" "$(xr_wide_mode)" auto 1 off ;;
    esac
}

# Prefer the NATIVE wide mode the glasses re-present (297MHz DTD). A forced CVT
# modeline makes the internal L/R scaler mis-sample -> diagonal striping and no
# stereo alignment, so we only fall back to the nominal string.
xr_wide_mode() {
    local m=""
    if have jq; then
        m="$(mon_json | jq -r --arg d "$MATCH" --argjson w "$SBS_WIDTH" '
            [ .[] | select((.description // "") | contains($d)) | .availableModes[]?
              | select((split("x")[0] | tonumber) >= $w) ] | (.[0] // empty)' 2>/dev/null | sed 's/Hz$//')"
    fi
    printf '%s' "${m:-$SBS_MODE}"
}

# Wake a panel that slept across a mode switch.
#
# NOT `hyprctl dispatch dpms off <mon>`. That is classic-front-end syntax, and
# under the Lua config manager `hyprctl dispatch` evaluates its argument AS LUA.
# The live run at 2026-08-31 20:21:24 produced, twice:
#
#   error: [string "return hl.dispatch(dpms off DP-3)"]:1: ')' expected near 'off'
#
# so BOTH nudges were no-ops while the script cheerfully logged
# "+ hyprctl dispatch dpms off DP-3". The same trap as `hyprctl keyword`, one
# layer along: it looks like it worked. bin/hypxr-monitor-desk already had the
# right form; this now matches it, and goes through the front-end probe so a
# machine on the classic parser still gets the syntax it wants.
#
# UNTESTED: written after live actuation was halted for the night.
hypr_dpms() { # <enable|disable> [monitor]
    local action=$1 mon=${2:-}
    if hypr_config_is_lua; then
        local spec="action = [[$action]]"
        [[ -n $mon ]] && spec="$spec, monitor = [[$mon]]"
        hyprctl dispatch "hl.dsp.dpms({ $spec })"
    else
        local arg=off
        [[ $action == enable ]] && arg=on
        hyprctl dispatch dpms "$arg" ${mon:+"$mon"}
    fi
}

# True while the compositor lists an output MON_SEL resolves to. The dpms
# dispatcher treats a selector that matches nothing as "no monitor given" and
# acts on EVERY output, so a nudge aimed at absent glasses would blank the
# laptop panel too. Check first.
compositor_has_glasses() {
    have jq || return 1
    hyprctl -j monitors all 2>/dev/null | jq -e --arg d "$DESC" \
        'any(.[]; (.description // "") | startswith($d))' >/dev/null 2>&1
}

dpms_nudge() {
    [[ -n $S_CONN ]] || return 0
    if [[ $DRY_RUN -eq 1 ]]; then
        log "[dry-run] dpms off/on $MON_SEL"
        return 0
    fi
    if ! compositor_has_glasses; then
        log "the compositor does not list $MON_SEL — skipping the dpms nudge"
        return 0
    fi
    log "+ dpms off $MON_SEL"
    hypr_dpms disable "$MON_SEL"
    sleep 0.3
    log "+ dpms on $MON_SEL"
    hypr_dpms enable "$MON_SEL"
    return 0
}

# ---- DRM lease (direct mode) ------------------------------------------------

# Mark the glasses' output leasable (true), or hand it back to the desktop
# (false). Under Lua, hl.monitor MERGES onto the output's existing rule: a rule
# that leaves `lease` out keeps whatever was set last. So the way back has to
# say `lease = false` in so many words, and LEASE_MARK exists so that every exit
# from direct mode knows it has to. The classic parser replaces the whole rule,
# so there a rule without the flag is the way back.
#
# `hyprctl eval` replies "ok" even when the compositor rejects a field (a build
# without the Lua `lease` binding raises a notification instead), so callers
# judge the outcome from the monitor list, never from this return code.
set_lease() { # <true|false>
    local want=$1 out="desc:$DESC"
    if [[ $DRY_RUN -eq 1 ]]; then
        printf '  [dry-run] lease %s on %s\n' "$want" "$out"
        return 0
    fi
    printf '  + lease %s on %s\n' "$want" "$out"
    if hypr_config_is_lua; then
        hyprctl eval "hl.monitor({ output = [[$out]], mode = [[preferred]], position = [[auto]], scale = [[1]], stereo = [[off]], lease = $want })" >/dev/null
    elif [[ $want == true ]]; then
        hyprctl keyword monitor "$out, preferred, auto, 1, lease" >/dev/null
    else
        hyprctl keyword monitor "$out, preferred, auto, 1" >/dev/null
    fi
    if [[ $want == true ]]; then
        printf '%s\n' "${HYPRLAND_INSTANCE_SIGNATURE:-unknown}" > "$LEASE_MARK"
    else
        rm -f "$LEASE_MARK"
    fi
}

lease_marked() {
    [[ -f $LEASE_MARK ]] || return 1
    [[ "$(cat "$LEASE_MARK" 2>/dev/null)" == "${HYPRLAND_INSTANCE_SIGNATURE:-unknown}" ]] && return 0
    rm -f "$LEASE_MARK"   # left by an earlier compositor; its rule died with it
    return 1
}

# True once the compositor has stopped driving the glasses as a desktop, which
# is what a leasable output does. hyprctl has no lease field, so this is the
# observable: the output is listed and disabled.
output_left_desktop() {
    have jq || return 1
    hyprctl -j monitors all 2>/dev/null | jq -e --arg d "$MATCH" \
        'any(.[]; ((.description // "") | contains($d)) and (.disabled // false))' >/dev/null 2>&1
}

# The DRM link-training failure that preceded the 2026-07-15 direct-mode freeze:
# leasing a connector whose link just failed to train tipped Monado's bring-up
# into the session-start deadlock. A physical replug clears it.
dp_link_recently_failed() {
    journalctl -k --since "-90 seconds" --no-pager -q 2>/dev/null |
        /usr/bin/grep -q "dpcd_set_link_settings.*failed"
}

# Monado's direct backend, selected by a drop-in on the unit. Written on the way
# into direct mode, removed by xr_teardown (or by window mode).
write_direct_dropin() {
    if [[ $DRY_RUN -eq 1 ]]; then
        log "[dry-run] write $XR_DROPIN (direct-wayland backend, connector $S_CONN) + daemon-reload"
        return 0
    fi
    mkdir -p "$(dirname "$XR_DROPIN")"
    cat > "$XR_DROPIN" <<EOF
# Written by ~/.config/hypr/bin/xreal-mode for DRM-lease direct mode; removed on
# the way out. See docs/openxr/07-xreal.md in the HypXRland repo.
#
# UNSET the base unit's XRT_COMPOSITOR_FORCE_WAYLAND, never set it empty: monado
# reads it with getenv(), debug_string_to_bool("") is TRUE, and force_wayland is
# checked after force_wayland_direct, so an empty value silently selects the
# window backend again.
[Service]
UnsetEnvironment=XRT_COMPOSITOR_FORCE_WAYLAND
Environment=XRT_COMPOSITOR_FORCE_WAYLAND_DIRECT=1
Environment=XRT_COMPOSITOR_WAYLAND_CONNECTOR=$S_CONN
# MEDIUM is monado's default; stated so nothing can raise it by accident. A
# realtime queue on the iGPU that also drives the desktop starved the machine on
# 2026-07-15.
Environment=XRT_COMPOSITOR_VK_GLOBAL_PRIORITY=medium
# Pacing for direct scanout on RADV (no VK_GOOGLE_display_timing, so the fake
# pacer locks to FIRST_PIXEL_OUT vblanks): 1ms present->display, and an 8ms
# compositor budget so iGPU contention does not slip whole 60Hz periods.
Environment=U_PACING_COMP_PRESENT_TO_DISPLAY_OFFSET_MS=1.0
Environment=U_PACING_COMP_MIN_TIME_MS=8.0
EOF
    log "+ wrote $XR_DROPIN (direct-wayland, connector $S_CONN)"
    systemctl --user daemon-reload
}

# Offer the glasses' connector for lease. Returns non-zero, with the desktop
# restored, if the compositor never let go of it.
xr_offer_lease() {
    # Monado's direct backend picks its mode from the LEASED connector's own
    # list, once. Lease it while the kernel still lists only the 1920 mode and
    # the SBS frame is clamped into 1920 ("Ignoring given extent 3840x1080").
    # So gate on the sysfs list, the one Monado enumerates.
    if [[ $DRY_RUN -eq 0 ]]; then
        local i d
        for (( i = 0; i < 40; i++ )); do
            d="$(sys_dir)" && [[ "$(personality "$d")" == 3d ]] && break
            d=""; sleep 0.25
        done
        if [[ -z $d ]]; then
            err "the connector never listed a ${SBS_WIDTH}-wide mode; leasing now would clamp to 1920"
            return 1
        fi
        # The flip hotplug can rename the connector; the drop-in needs today's.
        S_SYSDIR="$d"
        S_CONN="$(connector_name "$d")"

        # The check below reads "disabled" as "leasable", so start from an
        # enabled output — after `off` it is disabled for the other reason.
        if output_left_desktop; then
            log "the output is disabled — bringing it up before offering the lease"
            assert_rule 3d
            local k
            for (( k = 0; k < 12; k++ )); do
                sleep 0.25
                output_left_desktop || break
            done
            if output_left_desktop; then
                err "could not bring $S_CONN up as a desktop to start from"
                return 1
            fi
        fi
    fi

    log "offering $S_CONN for DRM lease — it leaves the desktop"
    set_lease true

    [[ $DRY_RUN -eq 1 ]] && return 0
    local j
    for (( j = 0; j < 12; j++ )); do
        sleep 0.25
        output_left_desktop && { log "$S_CONN is leasable"; return 0; }
    done
    err "the compositor kept $S_CONN as a desktop after lease = true."
    err "this Hyprland build may not have the Lua lease binding (hypxrland d0e13220f) — rebuild and log in again."
    set_lease false
    return 1
}

# ---- XR teardown, shared by off / 2d / 3d -----------------------------------

# Stop the xreal runtime, lease-first if it is in direct mode. ORDERING IS
# LOAD-BEARING: disabling the XR session or modesetting the connector while
# Monado holds its DRM lease deadlocks the compositor and hangs the whole
# session (hard reboot). See docs/openxr/07-xreal.md.
xr_teardown() {
    local svc rt
    svc="$(xr_service_state)"
    rt="$(xr_runtime_kind)"

    if direct_mode_active; then
        log "direct mode is active — stopping Monado FIRST to release the DP lease"
        run systemctl --user stop "$XR_UNIT"
        if [[ $DRY_RUN -eq 0 ]]; then
            local i
            for (( i = 0; i < 20; i++ )); do
                systemctl --user is-active --quiet "$XR_UNIT" || break
                sleep 0.1
            done
            sleep 0.5   # let the compositor's wayland loop drop the lease
        fi
    fi

    if [[ $rt == xreal ]]; then
        run hyprctl openxr disable
    elif [[ $rt == other ]]; then
        log "the live XR session is NOT the xreal runtime — leaving it alone"
    fi

    [[ $svc == active ]] && run systemctl --user stop "$XR_UNIT"

    # Monado is gone, so nothing holds the lease: hand the connector back to the
    # desktop. The caller re-asserts the real rule right after; this only
    # clears the merged `lease = true`, which no later rule would.
    if lease_marked; then
        log "returning $S_CONN from lease to the desktop"
        set_lease false
    fi

    # The next start picks its backend afresh.
    if [[ -f $XR_DROPIN ]]; then
        run rm -f "$XR_DROPIN"
        run systemctl --user daemon-reload
    fi
    return 0
}

# ---- personality transition -------------------------------------------------

# Take the glasses to <2d|3d>, doing nothing if they are already there. Returns
# non-zero if the DRM mode list never changed (i.e. the firmware refused).
ensure_personality() { # <2d|3d>
    local want="$1" width
    [[ $want == 3d ]] && width=3840 || width=1920

    if [[ $S_PERS == "$want" ]]; then
        log "already in the $want personality — no firmware flip"
        return 0
    fi

    # The output has to be an enabled desktop for us to drive an advertised
    # mode at it, which is the precondition for the firmware accepting the flip.
    if [[ $S_EN == no ]]; then
        log "output is disabled — re-enabling it so the firmware has a signal to accept"
        signal_advertised_mode
        mon_json_refresh
    else
        signal_advertised_mode
    fi

    notify "switching to ${want^^}…"
    flip_firmware "$want" || return 1

    if [[ $DRY_RUN -eq 1 ]]; then
        log "[dry-run] would wait for the ${width}-wide personality in /sys/class/drm/card*-<conn>/modes"
        return 0
    fi

    if ! wait_personality "$want"; then
        err "the panel never re-presented in $want (DRM mode list unchanged)."
        err "current modes: $(modes_list "$(sys_dir 2>/dev/null || echo /nonexistent)")"
        if ! usb_present; then
            err "the glasses' HID left the bus during the flip — $HID_HINT"
        else
            err "check the glasses are worn/awake and retry; a wedged link needs a physical replug."
        fi
        return 1
    fi
    log "kernel mode list now advertises ${width}-wide — the firmware really switched"
    return 0
}

# ---- verbs ------------------------------------------------------------------

cmd_status() {
    collect
    if [[ "${1:-}" == "--json" ]]; then print_status_json; else print_status_kv; fi
}

cmd_off() {
    collect
    [[ $S_CONNECTED == yes ]] || { err "no XREAL connected — nothing to turn off"; return 1; }

    xr_teardown
    mon_json_refresh
    collect

    if [[ $S_EN == no ]]; then
        log "the glasses' output is already disabled"
        notify "XReal display off"
        return 0
    fi

    if ! other_enabled_monitor; then
        err "refusing: the glasses are the only enabled display"
        notify "Can't disable the only active display"
        return 1
    fi

    # No firmware flip here on purpose: `off` is about the compositor output,
    # not the glasses' personality. Leaving the personality alone means a later
    # `2d`/`3d` costs at most one flip, and `off` costs none.
    log "disabling the glasses' desktop output"
    disable_monitor "desc:$DESC"
    notify "XReal display off"
    return 0
}

cmd_2d() {
    collect
    [[ $S_CONNECTED == yes ]] || { err "no XREAL connected"; return 1; }

    # Only a personality CHANGE needs the HID. Re-asserting the flat rule does
    # not, and must stay available: it is the fix for a sticky-stereo desktop,
    # which you want to clear while the glasses lie on the desk asleep.
    if [[ $S_PERS != 2d ]]; then
        require_hid_awake || return $?
    elif [[ $S_DIVERGE == *sticky-stereo* ]]; then
        log "clearing sticky stereo (rule says sbs, glasses are in the 2D personality)"
    fi

    xr_teardown
    mon_json_refresh
    collect

    ensure_personality 2d || { notify "stayed in ${S_PERS^^} — the firmware refused"; return 1; }

    log "asserting the flat desktop rule"
    assert_rule 2d
    [[ $DRY_RUN -eq 0 ]] && sleep 0.5
    # Re-assert once: the first push can land while the connector is still
    # renegotiating, which leaves the compositor on a stale scanout.
    assert_rule 2d
    notify "Flat 2D desktop"
    return 0
}

cmd_3d() {
    collect
    [[ $S_CONNECTED == yes ]] || { err "no XREAL connected"; return 1; }

    [[ $S_PERS != 3d ]] && { require_hid_awake || return $?; }

    xr_teardown
    mon_json_refresh
    collect

    ensure_personality 3d || { notify "stayed in ${S_PERS^^} — the firmware refused"; return 1; }

    log "asserting the SBS stereo rule"
    assert_rule 3d
    [[ $DRY_RUN -eq 0 ]] && sleep 0.5
    assert_rule 3d

    if [[ $DRY_RUN -eq 0 ]]; then
        mon_json_refresh
        local w; w="$(monitor_width)"
        if [[ $w != "$SBS_WIDTH" ]]; then
            log "scanout is ${w}px wide, not $SBS_WIDTH — nudging dpms and re-asserting"
            dpms_nudge
            assert_rule 3d
        fi
    fi
    notify "Stereo 3D desktop"
    return 0
}

cmd_xr() {
    collect
    [[ $S_CONNECTED == yes ]] || { err "no XREAL connected"; return 1; }

    # XR always needs the HID: the flip aside, monado's prober silently falls
    # back to a Simulated HMD when 3318:0426 is not on the bus. A dry run is for
    # reading the plan, so there the failed preflight only warns.
    if [[ $S_HID != awake ]]; then
        if [[ $DRY_RUN -eq 0 ]]; then
            require_hid_awake || return $?
        else
            err "the glasses' USB device ($USB_VID:$USB_PID) is not enumerated."
            err "Monado's prober would silently fall back to a Simulated HMD."
        fi
    fi
    if [[ ! -f $RUNTIME_JSON ]]; then
        err "the xreal Monado runtime manifest is missing: $RUNTIME_JSON"
        err "build it with: $REPO/scripts/build-monado.sh --xreal"
        return 1
    fi
    if [[ -n $XREAL_CTL && $DRY_RUN -eq 0 ]] && ! "$XREAL_CTL" detect >/dev/null 2>&1; then
        err "no XREAL device over HID — check the udev rule and that nothing else holds the device"
        return 1
    fi

    # Refuse to steal a live session from another runtime (WiVRn/Quest). An
    # idle/unavailable one is fair game; a running one is someone's headset.
    local rt sess
    rt="$(xr_runtime_kind)"; sess="$(xr_session_state)"
    if [[ $rt == other && -n $sess && $sess != unavailable ]]; then
        err "another OpenXR runtime has a live session (state: $sess) — refusing to take it over."
        err "end that session first, then retry."
        notify "another OpenXR runtime is live"
        return 1
    fi

    case $XR_BACKEND in
        direct|window) ;;
        *) err "unknown XR backend '$XR_BACKEND' (direct|window)"; return 2 ;;
    esac
    if [[ $XR_BACKEND == direct && $DRY_RUN -eq 0 ]] && dp_link_recently_failed; then
        err "the kernel logged DP link-training failures in the last 90s (dpcd_set_link_settings ... failed)."
        err "leasing a sick link is how the 2026-07-15 freeze started. Replug the glasses, wait, retry."
        notify "XR not started — replug the glasses first"
        return 1
    fi

    notify "starting XR mode ($XR_BACKEND)…"

    # Free the HID device and make Monado re-read the display mode on restart:
    # the xreal_air driver picks stereo-vs-mono view geometry ONCE, at device
    # create time, from whatever the glasses report over HID. So the personality
    # flip MUST happen before Monado (re)starts, or comp_main comes up mono.
    xr_teardown
    mon_json_refresh
    collect

    ensure_personality 3d || { notify "XR aborted — the firmware refused 3D"; return 1; }

    if [[ $XR_BACKEND == direct ]]; then
        # Monado does its own modeset on the leased connector, so there is no
        # desktop to bring up and no panel to nudge: the compositor stops
        # driving the output the moment it is leasable.
        if ! xr_offer_lease; then
            assert_rule 3d
            notify "XR aborted — the compositor would not lease the glasses"
            return 1
        fi
    else
        # A plain 3840 desktop, NOT stereo:sbs — the Monado surface is already the
        # stereo frame. Scale is pinned to 1: any fractional scale resamples the SBS
        # frame and the per-eye split softens/skews.
        log "bringing the output up as a plain ${SBS_WIDTH}-wide desktop"
        mon_json_refresh   # so xr_wide_mode sees the post-flip availableModes
        assert_rule xr
        if [[ $DRY_RUN -eq 0 ]]; then
            local i w=0
            for (( i = 0; i < 16; i++ )); do
                sleep 0.25
                mon_json_refresh
                w="$(monitor_width)"
                [[ $w == "$SBS_WIDTH" ]] && break
            done
            if [[ $w != "$SBS_WIDTH" ]]; then
                err "the output never came up ${SBS_WIDTH}-wide (width=$w). Reverting to flat 2D."
                set_monitor "desc:$DESC" "$FLAT_MODE" auto "$FLAT_SCALE" off
                [[ -n $XREAL_CTL ]] && { pace_flip; "$XREAL_CTL" mode 2d >/dev/null 2>&1 || true; }
                notify "XR aborted — no 3840 scanout"
                return 1
            fi
            log "output is up at ${SBS_WIDTH}x1080"
        fi

        dpms_nudge
    fi

    # Point the compositor's OpenXR EGL/composite node at the GPU that actually
    # scans out the glasses. Cross-GPU EGL import crashes the runtime, and on a
    # hybrid box the session default may be the other GPU.
    local gpu
    if gpu="$(render_node_for_connector)"; then
        log "openxr:gpu -> $gpu (the GPU that drives $S_CONN)"
        set_option openxr:gpu "\"$gpu\""
    else
        log "could not resolve $S_CONN's render node — leaving openxr:gpu as configured"
    fi
    # The birdbath is additive/see-through: without an opaque blend the black
    # scene background stays transparent and the surface can't go solitary.
    set_option openxr:blend_mode '"opaque"'

    # The backend is fixed when Monado starts, by the drop-in or its absence
    # (xr_teardown above already removed any old one).
    [[ $XR_BACKEND == direct ]] && write_direct_dropin

    run systemctl --user import-environment WAYLAND_DISPLAY XDG_RUNTIME_DIR
    run systemctl --user restart "$XR_UNIT"

    if [[ $DRY_RUN -eq 0 ]]; then
        # `systemctl restart` returns once the process is forked; comp_main needs
        # a beat to init Vulkan and bind its IPC socket. Enabling early fails
        # with "OpenXR runtime unavailable".
        local sock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/monado_comp_ipc" j
        for (( j = 0; j < 40; j++ )); do
            [[ -S $sock ]] && break
            sleep 0.25
        done
        [[ -S $sock ]] || log "monado_comp_ipc never appeared — enabling anyway"
        sleep 0.5
    fi

    set_option openxr:runtime_json "\"$RUNTIME_JSON\""
    run hyprctl openxr enable

    if [[ $XR_BACKEND == window ]]; then
        place_openxr_window
        notify "XR mode on"
        return 0
    fi

    # Direct mode has no window to place, and a session that never comes up
    # leaves the glasses leased to nobody useful. Give it a bounded time, then
    # tear down lease-first and put the SBS desktop back.
    if [[ $DRY_RUN -eq 0 ]]; then
        local k st=""
        for (( k = 0; k < 80; k++ )); do
            st="$(xr_session_state)"
            case $st in synchronized|visible|focused) break ;; esac
            sleep 0.25
        done
        case $st in
            synchronized|visible|focused) log "XR session is $st (direct)" ;;
            *)
                err "the XR session never came up in direct mode (state: ${st:-none}) — reverting to 3D SBS"
                xr_teardown
                mon_json_refresh
                collect
                assert_rule 3d
                dpms_nudge
                notify "XR direct mode failed — back to 3D"
                return 1
                ;;
        esac
    fi
    notify "XR mode on (direct)"
    return 0
}

# The GPU that scans out the glasses, as a DRM render node.
render_node_for_connector() {
    [[ -n $S_SYSDIR ]] || return 1
    local card pci r
    card="$(basename "$S_SYSDIR")"          # card2-DP-5
    card="/sys/class/drm/${card%%-*}"       # /sys/class/drm/card2
    pci="$(readlink -f "$card/device" 2>/dev/null)" || return 1
    [[ -n $pci ]] || return 1
    for r in /sys/class/drm/renderD*; do
        [[ -e $r/device ]] || continue
        if [[ "$(readlink -f "$r/device" 2>/dev/null)" == "$pci" ]]; then
            printf '/dev/dri/%s' "$(basename "$r")"
            return 0
        fi
    done
    return 1
}

# Land Monado's comp_main window (app-id "openxr") fullscreen on the glasses.
# A window rule alone only fires at map time; the explicit move+fullscreen by
# address is the part that reliably works, so we do both.
place_openxr_window() {
    [[ -n $S_CONN ]] || return 0

    if [[ $DRY_RUN -eq 1 ]]; then
        log "[dry-run] window rule + move/fullscreen the openxr window onto $MON_SEL"
        return 0
    fi

    if hypr_config_is_lua; then
        hyprctl eval "hl.window_rule({ name = [[xreal-openxr]], monitor = [[$MON_SEL]], fullscreen = true, border_size = 0, no_anim = true, no_blur = true, immediate = true, match = { class = [[^(openxr)$]] } })" >/dev/null 2>&1 || true
    else
        hyprctl keyword windowrule "monitor $MON_SEL, fullscreen 1, border_size 0, no_anim 1, no_blur 1, immediate 1, match:class ^(openxr)\$" >/dev/null 2>&1 || true
    fi

    have jq || { log "jq missing — relying on the window rule to place the openxr window"; return 0; }

    local addr="" i
    for (( i = 0; i < 40; i++ )); do
        addr="$(hyprctl -j clients 2>/dev/null | jq -r '.[] | select(.class == "openxr") | .address' | head -n1)"
        [[ -n $addr && $addr != null ]] && break
        addr=""; sleep 0.25
    done
    [[ -n $addr ]] || { log "the openxr window never appeared — check: hyprctl clients"; return 1; }

    # Classic dispatcher syntax is a Lua parse error under the Lua front end (see
    # hypr_dpms), so each front end gets its own spelling.
    if hypr_config_is_lua; then
        hyprctl dispatch "hl.dsp.window.move({ monitor = [[$MON_SEL]], window = [[address:$addr]] })" >/dev/null 2>&1 || true
        sleep 0.4
        hyprctl dispatch "hl.dsp.focus({ window = [[address:$addr]] })" >/dev/null 2>&1 || true
        hyprctl dispatch "hl.dsp.window.fullscreen_state({ internal = 2, client = 2, action = [[set]], window = [[address:$addr]] })" >/dev/null 2>&1 || true
    else
        hyprctl dispatch focuswindow "address:$addr" >/dev/null 2>&1 || true
        hyprctl dispatch movewindow "mon:$MON_SEL" >/dev/null 2>&1 || true
        sleep 0.4
        hyprctl dispatch focuswindow "address:$addr" >/dev/null 2>&1 || true
        hyprctl dispatch fullscreenstate 2 2 >/dev/null 2>&1 || true
    fi
    log "openxr window placed fullscreen on $MON_SEL"
    return 0
}

cmd_toggle() {
    collect
    [[ $S_CONNECTED == yes ]] || { err "no XREAL connected"; notify "glasses not connected"; return 1; }

    if [[ $S_MODE == xr ]]; then
        notify "XR mode is active — pick a mode from the bar to leave it"
        log "refusing to toggle out of XR mode implicitly"
        return 0
    fi

    # Direction comes from the DRM personality, NOT from the compositor's
    # `stereo` field. That field is the one the old script read, and it sticks
    # `sbs` on a fallback scanout — so it said "we are in 3D" while the glasses
    # were flat, and the toggle then sent the wrong way round.
    if [[ $S_PERS == 3d ]]; then cmd_2d; else cmd_3d; fi
}

# ---- entry point ------------------------------------------------------------

# The header block only: stop at the first line that is not a comment, so the
# in-body commentary does not end up in --help.
usage() {
    awk 'NR == 1 { next }
         /^$/ && !started { next }
         /^#/ { started = 1; sub(/^# ?/, ""); print; next }
         started { exit }' "$(readlink -f "$0")"
}

VERB=""
for arg in "$@"; do
    case "$arg" in
        status|off|2d|3d|xr|toggle) VERB="$arg" ;;
        flat) VERB=2d ;;                # scripts/xreal-mode.sh vocabulary
        sbs|stereo) VERB=3d ;;
        --dry-run) DRY_RUN=1 ;;
        --foreground|--fg) FOREGROUND=1 ;;
        --direct) XR_BACKEND=direct ;;
        --window) XR_BACKEND=window ;;
        --json) : ;;                    # consumed by cmd_status
        -h|--help|help) usage; exit 0 ;;
        *) err "unknown argument: $arg"; exit 2 ;;
    esac
done
[[ -n $VERB ]] || { usage >&2; exit 2; }

mkdir -p "$STATE_DIR" 2>/dev/null || true

# `status` is read-only: never block on the lock, and never claim it. (It may
# still CLEAR a stale busy marker — that is a repair, not a transition, and the
# whole point is that it happens without anyone having to ask for it.)
if [[ $VERB == status ]]; then
    JSON=""
    for arg in "$@"; do [[ $arg == --json ]] && JSON=--json; done
    cmd_status "$JSON"
    exit 0
fi

# --dry-run is read-only too, but it walks the same code, so let it run
# unserialised rather than fight a real transition for the lock.
if [[ $DRY_RUN -eq 1 ]]; then
    echo "== xreal-mode $VERB (dry run — nothing will be changed) =="
    "cmd_$VERB"
    exit $?
fi

# ---------------------------------------------------------------------------
# ONE transition at a time, and it must OUTLIVE WHOEVER STARTED IT.
#
# The bar widget spawns these as children of the Quickshell shell process. That
# shell crashed three times on 2026-08-31, once with a transition in flight: the
# child died between the HID flip and the mode re-assert, leaving the glasses in
# one state and the compositor asserting another. Being killed halfway through a
# firmware flip is the worst outcome available for this hardware, and no caller
# should be able to cause it merely by crashing.
#
# So the invocation splits in two:
#
#   parent  takes the lock, spawns the real run in its OWN SESSION via setsid,
#           records the child's pid, and exits immediately. Killing the parent --
#           or its whole process group, which is what a dying shell does -- cannot
#           reach the child, because setsid moved it to another session and
#           process group.
#   child   inherits fd 9 ALREADY LOCKED. flock lives on the open file
#           description, and fork duplicates the descriptor rather than the
#           description, so the parent closing its copy on exit does not drop the
#           lock: it stays held until the child exits, however it exits.
#
# That ordering also closes the window where the widget could momentarily see
# busy="" after the parent returned but before the child claimed anything.
#
# `--foreground` opts out, for terminal use and the validation script, where the
# output is wanted live and attached.
# ---------------------------------------------------------------------------

# NOTE the guard. The child re-execs this same script, and a bare `exec 9>` would
# REOPEN fd 9 -- a fresh, unlocked open file description -- silently throwing
# away the very lock it just inherited. Only the parent may touch fd 9.
if [[ ${XREAL_MODE_CHILD:-0} != 1 ]]; then
    exec 9>"$LOCK"
    if ! flock -n 9; then
        busy_probe
        err "another xreal-mode transition is in flight (busy: ${BUSY_VERB:-unknown}) — ignoring this one"
        exit "$EX_TEMPFAIL"
    fi
fi

run_verb() {
    echo "== xreal-mode $VERB ==  [$(date '+%F %T')  pid $$]"
    "cmd_$VERB"
    local rc=$?
    echo "== xreal-mode $VERB finished rc=$rc =="
    return $rc
}

if [[ ${XREAL_MODE_CHILD:-0} == 1 ]]; then
    # The detached half. fd 9 arrived already locked; hold it until we exit.
    busy_write "$VERB" "$$"
    trap 'rm -f "$BUSY_FILE"' EXIT
    run_verb
    exit $?
fi

if [[ $FOREGROUND -eq 1 ]] || ! command -v setsid >/dev/null 2>&1; then
    busy_write "$VERB" "$$"
    trap 'rm -f "$BUSY_FILE"' EXIT
    run_verb
    exit $?
fi

# Detach. Output goes to the log rather than to a parent that may not live long
# enough to read it; the log is what `status` points at and what the validation
# script tails.
{ printf '\n'; date '+---- %F %T  xreal-mode '"$VERB"' ----'; } >> "$LOG" 2>/dev/null || true
XREAL_MODE_CHILD=1 XREAL_XR_BACKEND="$XR_BACKEND" setsid "$(readlink -f "$0")" "$VERB" >> "$LOG" 2>&1 &
child=$!

# Claim the marker on the child's behalf before returning, so there is never an
# instant in which the lock is held but nothing says by whom. The child rewrites
# it with its own pid as soon as it starts.
busy_write "$VERB" "$child"

echo "xreal-mode $VERB started detached (pid $child); output: $LOG"
exit 0
