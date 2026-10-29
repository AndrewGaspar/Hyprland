-- HypXRland profile for the XREAL Air 2 Ultra as a 3DoF OpenXR display (WP-XR1).
--
-- Hyprland 0.57+ is Lua-only (no classic `.conf`, no `source =`); this is the Lua form of what used
-- to be example/xreal.conf. Load it from your Lua config (alongside, or instead of,
-- example/openxr.lua), either in place by explicit path:
--
--     require("~/code/Hyprland/example/xreal.lua")
--
-- or copy it to ~/.config/hypr/xreal.lua and `require("xreal")` — or copy the parts you want into
-- your hyprland-xr.lua. A required file is tracked by the config watcher, so editing it reloads.
-- If you load it alongside example/openxr.lua, load it AFTER: hl.config merges per key, so the
-- openxr values here (pointer = false, hand_input = "off", ...) override only the keys they name.
-- Runtime `hyprctl eval` changes are wiped by the next reload — persist them here.
--
-- This tunes HypXRland for a 3DoF, orientation-only, controller-less, hands-less birdbath HMD: a
-- curved arc of world-fixed panels you look around by turning your head, driven by keyboard + gaze +
-- voice (the device exposes an HMD only — no controllers, no hand tracking). See
-- docs/openxr/07-xreal.md for the full rig (udev, build, the flat<->XR toggle) and
-- docs/openxr/research/XREAL-3DOF.md for why.
--
-- The flat<->XR toggle (scripts/xreal-mode.sh) sets openxr.runtime_json and enables the session at
-- runtime; you do NOT hard-code the runtime path here. Bind the toggle to a key at the bottom.


-- ---- FLAT side-by-side stereo (no OpenXR needed) ----------------------------------------------
--
-- The glasses' OTHER mode. After the HID `mode 3d` switch (scripts/xreal-mode.sh) the Air 2 Ultra
-- re-presents a NATIVE 3840x1080@60 EDID and hardware-splits every scanline into left/right eyes.
-- The `stereo = "sbs"` monitor field makes Hyprland drive that as ONE head-locked 1920x1080 logical
-- monitor and pack the two panes at scanout — no XR session, no runtime, no quads. Uncomment to use
-- it:
--
-- hl.monitor({ output = "DP-5", mode = "3840x1080@60", position = "auto", scale = "1", stereo = "sbs" })
--
-- Rules that are NOT optional here:
--   * scale MUST be "1" — one buffer pixel to one physical pixel, or the per-eye split
--     softens/skews (an "auto" scale picked 1.25 on the FW16 and did exactly that). Hyprland warns
--     if you don't.
--   * The mode MUST be the panel's native 3840x1080@60. A forced/unadvertised wide modeline stripes
--     diagonally with no L/R alignment, and 3840-wide above ~72 Hz exceeds the 2-lane DP-alt
--     budget.
--   * DP-5 is the connector on the AMD box — check `hyprctl monitors` (it also reports
--     `stereo: sbs (scanout 3840x1080)` once this is live).
--   * Avoid wdisplays/nwg-displays on this output: their write path can silently drop the packing.
-- Full semantics (capture, wl_output.mode, cursor, limitations) in
-- docs/openxr/05-configuration.md §8.5.

-- STEREO *CONTENT* on that output — the second half (docs/openxr/05-configuration.md §8.6). The
-- field above makes the OUTPUT two-paned; these make one WINDOW's content differ between the panes,
-- by sampling half of its already-packed frame into each eye. Nothing engages without a rule.
--
-- The cooperative one, and the only rule you need if the app cooperates: a client that sets its
-- xdg-toplevel-tag to `stereo:sbs` / `stereo:hsbs` / `stereo:tab` / `stereo:htab` / `stereo:mono`
-- announces its own layout (a game mod does this in one Wayland call), "auto" reads it, and a
-- client that declared for itself is trusted windowed as well as fullscreen.
--
-- hl.window_rule({ stereo = "auto", match = { xdg_tag = "^stereo:.*" } })
--
-- The manual ones. Without `always` a rule waits until the window is fullscreen-and-covering,
-- because half-cropping a windowed desktop looks like a compositor bug. hl.window_rule match values
-- are FULL-match regexes, hence the `.*` wrapping. Later rules win, so keep the "off" (2D) override
-- last.
--
-- hl.window_rule({ stereo = "hsbs", match = { class = "^(mpv|vlc)$", title = ".*[._ -](h-?sbs|half-?sbs|3dh?sbs)[._ -].*" } })
-- hl.window_rule({ stereo = "sbs",  match = { class = "^(mpv|vlc)$", title = ".*[._ -](f-?sbs|full-?sbs|lrf)[._ -].*" } })
-- hl.window_rule({ stereo = "off",  match = { class = "^(mpv|vlc)$", title = ".*[._ -]2d[._ -].*" } })
--
-- The metadata one: copy contrib/mpv-hypxr-stereo.lua to ~/.config/mpv/scripts/ and mpv will tag
-- its own window from the container's stereo flag (Matroska StereoMode / st3d), which these turn
-- into a layout. Advisory only — an explicit rule above still wins.
--
-- hl.window_rule({ stereo = "sbs always",  match = { tag = "stereo-sbs" } })
-- hl.window_rule({ stereo = "hsbs always", match = { tag = "stereo-hsbs" } })
-- hl.window_rule({ stereo = "tab always",  match = { tag = "stereo-tab" } })
-- hl.window_rule({ stereo = "htab always", match = { tag = "stereo-htab" } })
--
-- `hyprctl clients` reports the resolved layout per window as `stereo`, which is how you tell "my
-- rule did not match" from "it matched and the fullscreen gate is holding it". Full SBS at the
-- native 3840 mode is the sharp path: half-SBS gives each eye half the horizontal samples and looks
-- soft.


-- ---- DEPTH — the 2.5D desktop on that stereo output -------------------------------------------
--
-- Once the output above is stereo, the two eyes are still seeing the SAME picture. Depth is what
-- makes them differ: every window and layer carries a 0..1 height above the wallpaper plane, and
-- each raised element is drawn a few pixels left in one eye and right in the other. Nothing here
-- needs uncommenting — the ladder below is the shipped default and is already running on a stereo
-- output. It is listed so you can tune it, and so the numbers are in front of you when you do.
--
-- hl.config({
--     decoration = {
--         depth_focused   = 0.6,   -- the focused window: the rise IS the focus indicator (1.9 px per eye)
--         depth_unfocused = 0.2,   -- every other window, one small step off the page (0.6 px per eye)
--         depth_layers    = 0.8,   -- top/overlay layers — waybar, mako, walker      (2.6 px per eye)
--         depth_scale     = 0.12,  -- metres of rise at depth 1.0. THE comfort knob
--     },
-- })
--
-- TASTEFUL DEPTH IS SINGLE-DIGIT PIXELS, and that is physiology, not a limit of the implementation:
-- the eye fuses a STEP of about 0.1 degrees (~3 px total here) between two things it is looking
-- at, even though it will happily verge onto a plane a full degree off. So keep the rungs few and
-- the gaps small; turning depth_scale up until it "looks 3D" is how you get a headache. A hard
-- 1 degree ceiling and an edge clamp (decoration.depth_edge_slack) stop the worst of it regardless.
--
-- The A/B switch, and the way to get the flat presenter's exact cost back — a stereo output with
-- anything raised composites the desktop TWICE (watch `stereoComposites` in `hyprctl monitors`):
--   hyprctl eval 'hl.config({ decoration = { depth_scale = 0 } })'   # same ladder, no rise, one composite
--
-- ...one composite unless a STEREO WINDOW is on the output: that owes the second composite for its
-- own reason (the section above), and depth_scale has no say in it. `stereoContent` tells the two
-- apart when `stereoComposites` stays at 2.
--
-- Per-window and per-layer overrides, if the tiers are not what you want:
-- hl.window_rule({ depth = 0.9, match = { class = "^(mpv)$" } })        -- the video sits proud of everything
-- hl.window_rule({ depth = 0,   match = { class = "^(Alacritty)$" } })  -- ...this one stays on the page
-- hl.layer_rule({ depth = 1.0, match = { namespace = "^(walker)$" } })  -- the launcher floats above the bar
--
-- Depth moves on discrete events (focus, fullscreen, a rule change), never on hover. To ease it,
-- note that easeOutQuint is NOT a built-in curve name — the hl.curve line is required:
-- hl.curve("depthEase", { type = "bezier", points = { { 0.23, 1 }, { 0.32, 1 } } })
-- hl.animation({ leaf = "windowsDepth", enabled = true, speed = 4, bezier = "depthEase" })
--
-- The defaults are PROVISIONAL: they come from the literature and the arithmetic, not from a day of
-- wear. Full semantics, the disparity table and the caveats: docs/openxr/05-configuration.md §8.7.


hl.config({
    openxr = {
        enabled = true,

        -- Keep re-probing so the session comes up automatically once the xreal monado runtime
        -- starts.
        reprobe = true,

        -- 3DoF has no positional tracking, so the arc is re-seated to your head pose
        -- (yaw-dominant, since there is no translation to zero) the first time you don the glasses
        -- in a session. Re-don (or the toggle's disable+enable) recenters the whole arc in front of
        -- you.
        recenter_on_plug = true,

        -- GPU: leave UNSET on this rig. The glasses' DisplayPort output hangs off the SAME GPU
        -- HypXRland renders the desktop with (on the AMD box, DP-5 is on the 890M iGPU /
        -- renderD129, which is also the primary; on a single-GPU laptop like Lunar Lake there is
        -- only one GPU). The wrong-GPU guard in start() verifies this at session start and refuses
        -- safely if it is ever wrong. Only set openxr.gpu if your glasses' DP connector is driven
        -- by a DIFFERENT GPU than Hyprland's primary.
        -- gpu = "/dev/dri/renderD129",

        -- Additive/see-through: birdbath optics are optical see-through. If you want the panels to
        -- composite over the real world rather than a black void, try additive (falls back to
        -- opaque with a warning if the runtime doesn't advertise it). Opaque (default) is the safe
        -- start.
        -- blend_mode = "additive",

        -- No controllers and no hands on this device — the driver exposes an HMD only. Turn off
        -- the ray pointer and gate hands OFF so nothing waits on input devices that will never
        -- appear. Keyboard + gaze + voice (hypxrvoice) are THE interaction model here.
        pointer    = false,
        hand_input = "off",

        -- HARDWARE don/doff (WP-XR-DONDOFF): the Air 2 Ultra's proximity sensor DOES surface wear
        -- over the control HID interface (DISPLAY_TOGGLED, 0x6C04), and the vendored monado now
        -- emits it as XR_EXT_user_presence — donning reports present, doffing reports absent, live
        -- and instant. So gate the XR monitors on presence: DOFF (set them on the desk) idles the
        -- XR monitors (workspaces evacuate to the laptop) WITHOUT pulling the USB cable, and DON
        -- re-plugs them. USB-unplug is still the hard backstop (session-loss also unplugs;
        -- docs/openxr/07-xreal.md §doff). If your runtime ever lacks the presence signal,
        -- "visible" falls back to session-visibility. Verify with `hyprctl openxr status`
        -- (presence: yes when worn / no when doffed).
        monitors_follow_session = "visible",

        -- The hardware presence edge is instant and accurate, so a short anti-flap grace is plenty
        -- (absorbs a momentary glance-away / brow adjustment without evacuating workspaces).
        -- Donning always re-plugs immediately regardless of this.
        monitor_unplug_grace_ms = 3000,

        -- A comfortable default size/distance for the seated arc.
        default_size     = 1.1,
        default_distance = 1.4,
    },
})


-- ---- 3DoF curved arc — three world-fixed panels -----------------------------------------------
--
-- anchor:local panels arranged on a ~1.4 m cylinder at seated eye height, spread ~±35° (inside the
-- 46°/eye FOV so the panel you face fills the lens). They are WORLD-FIXED: you look around the arc
-- by turning your head. pos: is meters (OpenXR: +X right, +Y up, -Z forward); with yaw: omitted
-- each panel faces the tracking origin (you). recenter_on_plug re-seats the whole arc in front of
-- you on don. Each string is the declaration grammar verbatim (docs/openxr/05-configuration.md §3);
-- the table form of hl.xr_monitor is equivalent.
--
-- Positions (cylinder r=1.4, height y=1.30): left/center/right at yaw -35°/0/+35°.
hl.xr_monitor("XR-left,   1920x1080@90, anchor:local pos:-0.80,1.30,-1.15, size:1.1")
hl.xr_monitor("XR-center, 1920x1080@90, anchor:local pos:0,1.30,-1.40,    size:1.2")
hl.xr_monitor("XR-right,  1920x1080@90, anchor:local pos:0.80,1.30,-1.15,  size:1.1")

-- Keep the panels SPARSE — the 46° FOV is narrow, so fewer, larger panels beat a dense grid. Add a
-- 4th overhead/below panel only if you actually use it. Tune radius/height/spread by editing pos:
-- above, or live with the `center`/`move`/`distance`/`scale` binds below, then read back
-- `hyprctl openxr layout` (it prints paste-ready hl.xr_monitor lines).

-- Pin your primary workspaces to the panels so they survive a logout (ordinary workspace rules —
-- XR monitors are name-keyed like any output):
-- hl.workspace_rule({ workspace = "1", monitor = "XR-center", default = true })
-- hl.workspace_rule({ workspace = "2", monitor = "XR-left" })
-- hl.workspace_rule({ workspace = "3", monitor = "XR-right" })


-- ---- comp_main window placement (the toggle's XR side) ----------------------------------------
--
-- When `xreal-mode xr` runs, the xreal Monado's real compositor (comp_main) opens its output as an
-- ordinary Wayland toplevel with app-id "openxr" / title "OpenXR application". This rule
-- fullscreens it on the glasses' DisplayPort output so it scans out to the lenses. Change DP-5 to
-- your connector (DP-5 on the AMD box; check `hyprctl monitors`). Keyed on app-id (class) so it is
-- robust to the title. One hl.window_rule carries every effect that shares the matcher set, with
-- the matchers in `match`. `immediate = true` = tearing/lowest-latency present (a single solitary
-- surface on DP-5 also qualifies for direct scanout). The name is the one scripts/xreal-mode.sh
-- uses when it pushes this rule at runtime for the detected connector, so the two update ONE rule
-- instead of stacking two.
hl.window_rule({
    name        = "xreal-openxr",
    monitor     = "DP-5",
    fullscreen  = true,
    border_size = 0,
    no_anim     = true,
    no_blur     = true,
    immediate   = true,
    match       = { class = "^(openxr)$" },
})


-- ---- binds — keyboard-centric 3DoF control ----------------------------------------------------

-- Cycle which panel subsequent verbs target, and recenter a panel in front of you.
hl.bind("SUPER + bracketright", hl.dsp.xrmonitor("select next"))
hl.bind("SUPER + bracketleft",  hl.dsp.xrmonitor("select prev"))
-- Recenter WHILE WEARING the glasses, looking straight ahead at where you want the screen (it
-- captures the live head pose). NOTE: SUPER+Home is taken by walker in Omarchy, so this uses
-- SUPER+SHIFT+Home; the raw command is `hyprctl openxr center` (or
-- `hyprctl dispatch 'hl.dsp.xrmonitor("center")'`).
hl.bind("SUPER + SHIFT + Home", hl.dsp.xrmonitor("center"))

-- Push/pull + rescale the selected panel (`{ repeating = true }` repeats while held).
hl.bind("SUPER + equal",         hl.dsp.xrmonitor("distance -0.15"), { repeating = true })
hl.bind("SUPER + minus",         hl.dsp.xrmonitor("distance +0.15"), { repeating = true })
hl.bind("SUPER + SHIFT + equal", hl.dsp.xrmonitor("scale 1.1"),      { repeating = true })
hl.bind("SUPER + SHIFT + minus", hl.dsp.xrmonitor("scale 0.9"),      { repeating = true })

-- Gaze grab: look at a panel, tap to grab it (it follows your gaze), tap again to drop. This is the
-- controller-free way to rearrange the arc; on these glasses the gaze ray is your HEAD forward
-- vector (no eye tracking) — aim by centering the panel in view. Pairs naturally with voice.
hl.bind("SUPER + SHIFT + G",   hl.dsp.xrmonitor("gazegrab"))
hl.bind("SUPER + ALT + equal", hl.dsp.xrmonitor("gazepush 0.1"),  { repeating = true })
hl.bind("SUPER + ALT + minus", hl.dsp.xrmonitor("gazepush -0.1"), { repeating = true })

-- The flat<->XR toggle. FLAT = ordinary head-locked 1920x1080 monitor on the glasses' DP; XR = this
-- 3DoF arc. Bind both (adjust the chord to taste; verify it is free in your layout):
hl.bind("SUPER + F12",         hl.dsp.exec_cmd("~/code/Hyprland/scripts/xreal-mode.sh xr"))
hl.bind("SUPER + SHIFT + F12", hl.dsp.exec_cmd("~/code/Hyprland/scripts/xreal-mode.sh flat"))
-- Direct (DRM-lease) mode misbehaving? Fall back to the fullscreen-window path:  xreal-mode.sh xr --window


-- ---- Lunar Lake laptop (single-GPU) notes -----------------------------------------------------
--
-- This profile is machine-agnostic. On the Intel Lunar Lake laptop:
--   * There is ONE GPU, so openxr.gpu stays unset (as above) — nothing to pin.
--   * The glasses' DP connector name differs (NOT DP-5). Find it with `hyprctl monitors`, then
--     either set XREAL_MONITOR=<name> for the toggle and edit the `monitor = "DP-5"` field of the
--     window rule above, or rely on the toggle's auto-detection (it matches the "Air 2 Ultra" EDID
--     description regardless of name).
--   * The xreal monado build lives at a different repo path — set MONADO_XREAL_SERVICE in
--     ~/.config/xreal/monado.env (see contrib/xreal/monado-xreal.service) and MONADO_XREAL_BUILD
--     for the toggle. Everything else is identical.
