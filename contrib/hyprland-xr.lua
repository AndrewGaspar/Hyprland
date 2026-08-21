-- HypXRland session config, in Lua.
--
-- This is the line-for-line Lua translation of the classic `hyprland-xr.conf` XR session config:
-- the normal Omarchy setup with the OpenXR extension layered on top. Both front ends are supported
-- and produce identical state; pick one per session, because you cannot mix them (see COMPOSITION).
--
--   ~/code/hypxrland/build/Hyprland --config ~/.config/hypr/hyprland-xr.lua
--
-- Update the fishfood build with: ~/code/Hyprland/scripts/fishfood.sh update
--
--
-- ---- COMPOSITION: how this file relates to the main config ------------------------------------
--
-- A session has exactly ONE config manager, chosen by the extension of the MAIN config file:
-- `.lua` gets the Lua manager, anything else gets the classic hyprlang one. There is no bridge in
-- either direction — `source =` only ever parses hyprlang, and Lua's `require` only ever resolves
-- `.lua` / `init.lua`. So a `.lua` file cannot source a `.conf`, and a `.conf` cannot source a
-- `.lua`.
--
-- The classic file said:
--
--     source = ~/.config/hypr/hyprland.conf
--
-- The Lua equivalent, which requires the main Omarchy config to ALSO be Lua
-- (~/.config/hypr/hyprland.lua):
--
--     require("hyprland")
--
-- `require` resolves against the main config's own directory, and the required file is tracked by
-- the config watcher exactly as a sourced file is — so editing either one still triggers a reload.
--
-- Two ways to arrange it, both fine:
--
--   A. This file is the main config (launch with `--config ~/.config/hypr/hyprland-xr.lua`) and
--      pulls the desktop in with `require("hyprland")`. Direct analogue of the classic layout, and
--      what the line below does.
--   B. ~/.config/hypr/hyprland.lua is the main config and ends with `require("hyprland-xr")`. Then
--      the XR session needs no `--config` at all — but every non-XR login gets the XR config too.
--
-- Note that if both ~/.config/hypr/hyprland.lua and hyprland.conf exist, the .lua one wins for a
-- default launch. During a migration, keep the .conf as the fallback and launch the Lua session
-- explicitly with --config.

-- Everything from the regular session: theme, bindings, monitors, autostart.
require("hyprland")

-- GPU policy: AMD by default, NVIDIA by implicit app choice.
-- The AQ pin keeps aquamarine's DRM backend (and thus the compositor) off the NVIDIA card
-- entirely — without it, multi-GPU discovery opens every KMS card and holds the dGPU out of
-- D3cold. AQ_DRM_DEVICES inherits to children but is harmless (only aquamarine reads it), so apps
-- keep a clean GPU picture: games find the dGPU implicitly (DXVK discrete preference), while the
-- known gratuitous holders are individually masked at THEIR launch points — Chrome
-- (bin/google-chrome-stable shadow + .desktop override), hypxrhud (service drop-ins).
-- CAVEAT: displays wired to the NVIDIA card's ports won't light (AQ pin).
hl.env("AQ_DRM_DEVICES", "/dev/dri/card2")

-- ---- Debug logging ---------------------------------------------------------
-- This is the fishfood/debug session: keep full logs (Hyprland default is logging OFF).
-- Log file: $XDG_RUNTIME_DIR/hypr/<instance>/hyprlandd.log — tail the newest instance dir, or
-- `hyprctl rollinglog -f`.
--
-- Note that `debug { … }` sections become nested tables under hl.config: the config key is
-- `debug:disable_logs` classically and `debug.disable_logs` here.
hl.config({
    debug = {
        disable_logs = false,
        disable_time = false,
    },

    -- Upstream warns when Hyprland isn't launched via its start-hyprland watchdog. This session is
    -- launched bare by design — uwsm + wayland-wm@Hyprland.service already supervise it, and the
    -- watchdog's safe-mode restart would come back without this config (no XR monitors).
    misc = {
        disable_watchdog_warning = true,
    },
})

-- Meta PC VR games expose a flat mirror window in addition to their headset presentation. Keep
-- that preview available on a dedicated workspace without letting it cover or focus the virtual
-- monitors while the game is starting.
--
-- One classic `windowrule =` line is one effect plus its matchers. A Lua window rule is a whole
-- RULE: every effect that shares a matcher set belongs in one call, and `name` makes it addressable
-- later (hl.window_rule returns the rule object too).
hl.window_rule({
    name           = "meta-pcvr-mirror",
    workspace      = "name:preview silent",
    no_initial_focus = true,
    suppress_event = "activate",
    match          = { cmdline = "^C:.*MetaGames.*Software.*$" },
})

-- Ishimura (Dead Space decomp) renders packed stereo at the window's own resolution → half-SBS.
-- `always`: this is our own game, not a guessed match, so it stays 3D windowed too — 2D and 3D
-- windows share the desktop (docs 05 §8.6). Matched by the Wine cmdline (the exe is wine-preloader
-- for every Wine app, so cmdline is the real identity; FullMatch semantics need the leading .*) —
-- no other game on the box can collide with this.
hl.window_rule({
    name   = "ishimura-stereo",
    stereo = "hsbs always",
    match  = { cmdline = ".*Ishimura.game.Dead Space.exe" },
})

-- Container stereo metadata → layout, via the mpv tag script
-- (~/.config/mpv/scripts/mpv-hypxr-stereo.lua): mpv tags its own window from Matroska StereoMode /
-- ISOBMFF st3d, these static rules turn the tag into a stereo layout. Advisory — a hand-written
-- rule or `stereo off` wins (§8.6).
--
-- The pane split assumes the window SURFACE is the packed frame. A tiled or fullscreen player
-- letterboxes the video inside the surface and each eye gets a shifted crop (proven with a
-- zero-disparity reference, 2026-08-17). Float stereo-tagged windows so the player sizes them to
-- the video's native aspect: no bars, no stretch, split correct by construction. In the classic
-- file that was eight lines; here the float rides along in each rule.
for _, layout in ipairs({ "sbs", "hsbs", "tab", "htab" }) do
    hl.window_rule({
        name   = "mpv-stereo-" .. layout,
        stereo = layout .. " always",
        float  = true,
        match  = { tag = "stereo-" .. layout },
    })
end

-- Viewpoint portal authorization (doc 10 §13.1): head-pose-relative feedback is privacy-gated per
-- app; this belongs HERE, in the config file — a `hyprctl keyword windowrule` copy is wiped by
-- every config reload (2026-08-15: that wipe cost a demo 3 silent hours of "inactive:
-- not_authorized"). Under Lua the runtime equivalent is `hyprctl eval 'hl.window_rule{…}'`, and it
-- is wiped by a reload for exactly the same reason. Keep it in the file.
hl.window_rule({
    name      = "viewpoint-demo",
    viewpoint = true,
    match     = { class = "^(hypxr-viewpoint-demo)$" },
})

-- Stereo-by-default for the XReal: in 3D personality (3840-only EDID) this asserts the SBS pack; in
-- 2D personality the mode doesn't exist so the pack self-gates to a flat 1920 desktop. One line,
-- correct in both, and every firmware re-present heals back into the right state without manual
-- asserts.
hl.monitor({
    output   = "desc:Nreal Air 2 Ultra 0x88888800",
    mode     = "3840x1080@60",
    position = "auto",
    scale    = "1",
    stereo   = "sbs",
})

-- ---- OpenXR on top ---------------------------------------------------------
hl.config({
    openxr = {
        enabled = true,

        overlay = true,

        -- openxr.gpu is set per machine on each device branch — the XR EGL context must match the
        -- GPU the XR runtime's compositor renders on (cross-GPU swapchains crash the runtime).
        -- AMD-ONLY EVALUATION (Strix Point no-dGPU candidate): everything on the 890M iGPU — WiVRn
        -- is pinned to RADV + vaapi (see wivrn.service.d override + ~/.config/wivrn/config.json).
        -- To revert to the NVIDIA config, restore the line below and undo those two, then relog:
        --   gpu = "/dev/dri/renderD128",  -- (NVIDIA — WiVRn compositor on dGPU)
        gpu = "/dev/dri/renderD129",

        -- Passthrough: composite monitors over the real room (Quest 3 via WiVRn). Comment out for
        -- the classic opaque VR void, or flip live with
        --   hyprctl eval 'hl.config{ openxr = { blend_mode = "opaque" } }' \
        --     && hyprctl openxr disable && hyprctl openxr enable
        blend_mode = "alpha",

        -- Luma-keyed transparency ("black-as-alpha"): black desktop pixels dissolve into the room,
        -- so a monitor reads as an AR overlay rather than a panel. 0.2 keeps a faint scrim behind
        -- text instead of full see-through. Only does anything under blend_mode alpha/additive
        -- (force-disabled under opaque); both this and black_alpha_knee (default 0.10) are hot, so
        -- tune in-headset with
        --   hyprctl eval 'hl.config{ openxr = { black_alpha = 0.35 } }'
        -- and paste the winner here.
        black_alpha = 0.2,

        -- Grab/chrome/filter defaults are the live-tuned values shipped in the build — nothing to
        -- set here. Adaptive-anchoring feel knobs (openxr.adaptive_*) are hot-tunable through the
        -- same eval if the defaults (leave 1.5m / return 1.0m, dwell 400/800ms, 700ms blend) need
        -- taste.

        -- Hand tracking OFF by default (unintentional hand interactions were the #1 usability
        -- annoyance). Opt in per-session with SUPER+ALT+H (xrmonitor handinput toggle); auto/idle
        -- gating returns via `hyprctl eval 'hl.config{ openxr = { hand_input = "auto" } }'`.
        hand_input = "off",

        -- Depth desktop (X3/X4): every XR monitor composited once per eye so the depth tiers read
        -- as real 3D. This IS the build default — pinned here deliberately after the 2026-08-10
        -- container validation. Hot kill switch:
        --   hyprctl eval 'hl.config{ openxr = { depth_desktop = false } }'
        -- comfort knob is decoration.depth_scale (0.12).
        depth_desktop = true,
    },
})

-- Situational transparency (xrrule, docs 05 §3.5). Precedence: defaults -> rules (in order) ->
-- manual (`hyprctl openxr alpha <name> <v|auto>`). ORDER IS LOAD-BEARING: a later matching rule
-- overrides an earlier one per effect, so keep these in the order you want them resolved.

-- Walking/body-leash: always see the room.
hl.xr_rule({ alpha = 0.55, match = { anchorstate = "follow" } })

-- Seated gaming: full opaque, no luma keying. steam_proton covers games run through Proton directly
-- rather than Steam's appid wrapper (e.g. Ishimura).
hl.xr_rule({
    alpha      = 1.0,
    blackalpha = "off",
    match      = { anchorstate = "docked", focusclass = "^(steam_app_|steam_proton)", fullscreen = true },
})

-- Media players: keep uniform transparency but never luma-key dark scenes.
hl.xr_rule({ blackalpha = "off", match = { focusclass = "(mpv|vlc)" } })

-- XR monitor to bring up at launch. Arrange it in-headset by grabbing, then run
-- `hyprctl openxr layout` and persist the arrangement. That command still prints a paste-ready
-- CLASSIC `xrmonitor = …` line, which is why hl.xr_monitor also takes one as a string:
--
--   hl.xr_monitor("XR-main, 2560x1440@90, anchor:local pos:0.017,1.457,-1.408 yaw:5.3 …")
--
-- Paste the line, or transcribe it into the table below — the two are the same declaration.
--
-- XR-main is adaptive: world-docked at the desk, picks up into a body-follow when you walk >1.5m
-- away, re-docks when you come back.
hl.xr_monitor({
    name     = "XR-main",
    mode     = "2560x1440@90",
    anchor   = "local",
    pos      = { 0.017, 1.457, -1.408 },
    yaw      = 5.3,
    pitch    = 3.0,
    adaptive = true,
    roam     = "body",
    size     = 2.23,
})

-- Pin the declared mode as a persistent monitor rule (survives plug cycles).
hl.monitor({ output = "XR-main", mode = "2560x1440@90", position = "auto", scale = "1.25" })

-- ---- Autostart -------------------------------------------------------------
-- `exec-once` has no keyword form in Lua, and it must not: a Lua config is a SCRIPT that is re-run
-- from the top on every reload, so a bare hl.exec_cmd at file scope would respawn on each one.
-- The once-per-session equivalent is the hyprland.start event.
hl.on("hyprland.start", function()
    -- WiVRn: start the streaming server with this session (no dashboard GUI needed — pair new
    -- headsets with `wivrnctl`). The compositor's UNAVAILABLE re-probe picks the runtime up
    -- automatically once the service is running.
    hl.exec_cmd("systemctl --user start wivrn.service")

    -- hypxrva watcher: maintains $XDG_RUNTIME_DIR/hypxr/va-decode-block from XR monitor
    -- plug/unplug so hw video decode yields the VCN to WiVRn's encode while donned
    -- (github.com/AndrewGaspar/hypxrva). flock-guarded, safe to re-run.
    hl.exec_cmd("~/.local/bin/hypxrva-watcher")

    -- Laptop-panel autopilot: eDP off while the XR session is live, back on at doff (rides the
    -- hypxrva flag; transitions-only, manual toggles respected).
    hl.exec_cmd("~/.config/hypr/scripts/hypxr-display-autopilot.sh")
end)

-- ---- Keybinds --------------------------------------------------------------
-- The `xrmonitor` DISPATCHER (docs 05 §4) is hl.dsp.xrmonitor(), which takes the verb and its
-- arguments as one string — exactly the text that followed `xrmonitor,` in a classic bind.
-- `binde` becomes { repeating = true }; `bindd`'s description becomes { description = "…" }.
hl.bind("SUPER + SHIFT + G",  hl.dsp.xrmonitor("gazegrab"))
hl.bind("SUPER + ALT + equal", hl.dsp.xrmonitor("gazepush 0.1"),  { repeating = true })
hl.bind("SUPER + ALT + minus", hl.dsp.xrmonitor("gazepush -0.1"), { repeating = true })
hl.bind("SUPER + ALT + H",     hl.dsp.xrmonitor("handinput toggle"))
hl.bind("SUPER + ALT + M",     hl.dsp.xrmonitor("view toggle"), { description = "Toggle XR monitor view" })

hl.bind("SUPER + ALT + K", hl.dsp.exec_cmd("~/.config/hypr/scripts/hypxr-filming-toggle.sh"),
        { description = "Toggle filming mode (keys overlay + command ticker)" })
hl.bind("SUPER + ALT + C", hl.dsp.exec_cmd("~/.config/hypr/scripts/hypxr-recorder-toggle.sh"),
        { description = "Toggle Quest recording (consent in headset)" })

-- Deliberate "bring my desktop to me": rigid re-seat of the whole local monitor group onto the
-- current head (doc 03 §8.4). The Quest's own recenter button stays a room-holder
-- (openxr.recenter = "hold"); this chord is the intentional one.
hl.bind("SUPER + CTRL + Home", hl.dsp.xrmonitor("reseat"),
        { description = "Re-seat all XR monitors to me" })

hl.bind("SUPER + ALT + N", hl.dsp.exec_cmd("~/.config/hypr/scripts/hypxr-create-monitor.sh"),
        { description = "Create a new XR monitor" })
hl.bind("SUPER + ALT + X", hl.dsp.exec_cmd("~/.config/hypr/scripts/hypxr-create-monitor.sh 1080x1920@90 0.5"),
        { description = "Create a tall XR monitor for X" })
hl.bind("SUPER + ALT + SHIFT + N", hl.dsp.exec_cmd("~/.config/hypr/scripts/hypxr-destroy-monitor.sh"),
        { description = "Destroy the gazed XR monitor" })

hl.bind("SUPER + ALT + V", hl.dsp.exec_cmd("/home/ajg/code/hypxrvoice/build/hypxrvoicectl ptt toggle"),
        { description = "Voice push-to-talk" })
-- NOTE, migrating: hypxr-stereo-toggle.sh flips the XReal between its 2D and 3D personalities with
-- `hyprctl keyword monitor "desc:…, stereo:sbs"`, and `hyprctl keyword` does not exist under a Lua
-- config. Port those three lines in the script to the eval form before using this file:
--   hyprctl eval 'hl.monitor({ output = "desc:"..DESC, mode = "3840x1080@60", position = "auto", scale = "1", stereo = "sbs" })'
-- The XR scripts bound below otherwise use `hyprctl openxr …` and `hyprctl -j monitors`, which are
-- front-end independent and need no changes.
hl.bind("SUPER + ALT + S", hl.dsp.exec_cmd("~/.config/hypr/scripts/hypxr-stereo-toggle.sh"),
        { description = "Toggle XReal 2D/3D stereo desktop" })

hl.bind("SUPER + SHIFT + X", hl.dsp.exec_cmd("wivrnctl disconnect"))

-- Toggle the WiVRn in-headset menu: `tab settings` opens, `tab hidden` closes (wivrnctl has no
-- toggle verb, so track state in a tmp flag).
hl.bind("SUPER + SHIFT + M", hl.dsp.exec_cmd(
    [[sh -c 'f="$XDG_RUNTIME_DIR/.wivrn-menu"; if [ -e "$f" ]; then wivrnctl tab hidden; rm -f "$f"; else wivrnctl tab settings; touch "$f"; fi']]))

hl.bind("SUPER + Home", hl.dsp.xrmonitor("center"))
