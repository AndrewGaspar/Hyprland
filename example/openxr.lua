-- Example configuration for the Hyprland OpenXR extension ("HypXRland").
--
-- Hyprland 0.57+ is Lua-only: the classic hyprlang `.conf` front end is gone, and with it
-- `source =`, the `openxr { }` section syntax, the `xrmonitor =` / `xrrule =` keywords and
-- `hyprctl keyword`. This file is the Lua form of what used to be example/openxr.conf. Load it
-- from your Lua config in either of two ways:
--
--   * copy it next to your main config as ~/.config/hypr/openxr.lua and add
--         require("openxr")
--     to hyprland.lua (or your hyprland-xr.lua) — `require` resolves against the main config's
--     directory; or
--   * require it in place by explicit path:
--         require("~/code/Hyprland/example/openxr.lua")
--
-- Either way the required file is tracked by the config watcher, so editing it triggers a reload.
-- It is a REFERENCE, though — it declares several demo XR monitors and binds a lot of chords — so
-- in practice copy the parts you want into your own hyprland-xr.lua rather than loading it whole.
--
-- Worth knowing when composing configs:
--   * hl.config MERGES per key: a later hl.config({ openxr = { ... } }) overrides only the keys it
--     names, so you can load this file and then override a handful of options after it.
--   * Runtime changes go through `hyprctl eval 'hl.config({ openxr = { ... } })'` (the old
--     `hyprctl keyword openxr:...`). Equally hot, but wiped by the next config reload — keep what
--     should persist in the file.
--   * The classic option `openxr:gpu` is the Lua key openxr.gpu, i.e.
--     hl.config({ openxr = { gpu = "..." } }). Every value must have the option's declared type:
--     booleans are true/false, numbers are bare, and string-typed options are QUOTED even when the
--     value looks numeric or boolean (hand_input = "off", gpu = "/dev/dri/renderD128"). Colors
--     are strings too ("0x66aaaaaa", ARGB) — a bare 0x… number is rejected.
--
-- Full reference: docs/openxr/05-configuration.md (config vars, declaration grammar, dispatcher,
-- hyprctl, events), or `hyprctl descriptions | grep openxr`. The hl.xr_monitor / hl.xr_rule strings
-- below are the declaration grammar verbatim, run through the same parser as before
-- (src/openxr/XRMonitorConfig.cpp, covered by its unit tests).


-- ---- hl.config: the openxr options ------------------------------------------------------------

hl.config({
    openxr = {
        -- Master switch. Session starts automatically once a runtime is available; this is a HOT
        -- toggle — flipping it with `hyprctl eval 'hl.config({ openxr = { enabled = false } })'`
        -- (or `= true`), or a config reload, starts or stops the session live, no restart needed.
        enabled = true,

        -- Keep re-probing in the background while enabled but no runtime/headset is available
        -- yet, so the session comes up automatically once you start the runtime (e.g.
        -- wivrn-server) or don the headset — no need to toggle enabled. Also auto-reconnects after
        -- a session/runtime loss. The backoff for "waiting for the runtime" grows from
        -- reprobe_interval_ms up to 30s; "waiting for the headset" (runtime up, not donned) polls
        -- at the fixed interval. `hyprctl openxr status` shows e.g.
        -- `unavailable (waiting for headset, retrying in 1800ms)`.
        reprobe = true,
        -- reprobe_interval_ms = 2000,

        -- On the FIRST time you don the headset in a session, re-seat anchor:local monitors
        -- relative to your current head pose instead of the runtime's LOCAL_FLOOR origin (which is
        -- arbitrary under WiVRn boundaryless/standby — monitors otherwise appear wherever that
        -- origin happens to be, often far away). The monitors land in front of you at their
        -- configured height/distance; a multi-monitor layout is recentered rigidly (relative
        -- arrangement preserved). A brief doff-and-don within the SAME session does NOT re-seat.
        recenter_on_plug = true,

        -- What the HEADSET's own recenter button does to your monitors.
        --
        --   "hold"   (default) they stay where they are IN THE ROOM. Recentering moves the
        --            runtime's origin, which you cannot see, so your monitors correctly do not
        --            move. Pair this with the SUPER+CTRL+Home bind below
        --            (hl.dsp.xrmonitor("reseat")), which brings them to you only when you ask.
        --   "follow" a runtime recenter ALSO re-seats your whole anchor:local layout onto your
        --            current head — the recenter button becomes "bring my monitors to me".
        --
        -- The tradeoff with "follow": the runtime does not tell us whether a reference-space
        -- change was deliberate, so INVOLUNTARY ones re-seat too — putting the headset back on, a
        -- guardian re-derive. If that would annoy you, keep "hold" and use the keybind.
        recenter = "hold",

        -- DRM render node to use for the XR session's EGL context. Empty (default) = follow
        -- Hyprland's primary GPU render node — correct on single-GPU machines.
        --
        -- On a MULTI-GPU / hybrid machine, this often needs to be set explicitly: the OpenXR
        -- runtime (e.g. Monado) may run its compositor on a different GPU than the one Hyprland
        -- renders with. Point this at whichever render node your runtime actually uses, e.g.:
        --
        --   gpu = "/dev/dri/renderD128",
        --
        -- A wrong value is safe: at session start the runtime's GPU is probed and a detected
        -- mismatch refuses to start (with a log line naming both GPUs) instead of crashing the
        -- graphics driver.
        -- gpu = "/dev/dri/renderD128",

        -- Start XR even when the kernel has ALREADY TAKEN AN OOPS this boot
        -- (/proc/sys/kernel/tainted bit 7, TAINT_DIE). Default false: XR refuses to start in that
        -- state and says so in `hyprctl openxr status` as a `blocked:` line.
        --
        -- Why it refuses rather than just trying: bring-up cannot avoid initializing EVERY
        -- installed GPU vendor driver. libglvnd loads them all on the first eglGetProcAddress and
        -- contacts their kernel drivers on the count-only eglQueryDevicesEXT — before any device
        -- handle exists, so `gpu` above cannot filter it. Once the kernel has oopsed, the only real
        -- protection is not starting. The ordinary taint bits (proprietary/out-of-tree/unsigned
        -- modules, 12288 on a stock NVIDIA box) are ignored — only TAINT_DIE blocks.
        --
        -- Development escape hatch only; it still warns at every start. The fix is to reboot.
        -- ignore_kernel_taint = true,

        -- Cross-GPU buffer format: when the XR GPU (above) differs from the GPU the desktop
        -- renders on, XR monitor buffers must be LINEAR for the runtime's GPU to import them.
        -- "auto" (default) detects the split and forces linear only then; "on"/"off" override.
        -- Linear costs some compositing throughput — the accepted multi-GPU cost.
        -- `hyprctl openxr status` shows `(linear)` per monitor when active.
        -- force_linear = "auto",

        -- Damage-gated pacing: an XR monitor is composited only when something on it changed, and a
        -- static one costs nothing while its quad keeps showing the last image. Set to true to
        -- composite every visible XR monitor on every runtime frame instead (the old behaviour;
        -- diagnostics only).
        -- pace_static_monitors = false,

        -- Environment blend mode: "auto" | "opaque" | "alpha" | "additive". Read at session start
        -- only.
        --   auto     (default) — use the runtime's preferred/first-advertised mode.
        --   opaque   — monitors float over a black void (classic VR).
        --   alpha    — PASSTHROUGH: composite monitors over your real room, on runtimes that
        --              support it (e.g. WiVRn on Quest 3). Falls back (with a warning) if the
        --              runtime can't do it.
        --   additive — for additive / optical see-through displays.
        -- blend_mode = "alpha",

        -- Fallback eye height in meters, used only when the runtime lacks XR_EXT_local_floor (in
        -- which case Y=0 is derived rather than being the runtime's own floor level).
        floor_offset = 1.5,

        -- Default quad width (meters) and default placement distance (meters) for newly created
        -- XR monitors that don't specify size:/pos: explicitly (e.g. via the `create` dispatcher
        -- verb with no anchor-spec, or `hyprctl openxr create NAME` with no arguments).
        default_size     = 1.6,
        default_distance = 1.5,

        -- Head/body leash tuning (docs/openxr/03-anchoring.md) — applies to `anchor:head` and
        -- `anchor:body` monitors. leash_response is the spring's response time in seconds
        -- (smaller = snappier follow, larger = lazier/smoother); the deadzones stop the quad from
        -- drifting on small head wobble, only "waking up" once you turn/move past the threshold.
        leash_response          = 0.35,
        leash_deadzone_angle    = 15.0, -- degrees
        leash_deadzone_distance = 0.25, -- meters

        -- If true, body-leashed monitors also follow your head's vertical movement (standing up /
        -- sitting down); if false (default) they stay at the height captured when body mode was
        -- entered — good for a "desk height" panel that shouldn't jump when you lean over.
        body_leash_follow_height = false,

        -- Adaptive anchoring (docs/openxr/03-anchoring.md) — an `anchor:local` monitor with
        -- `adaptive:on` (table form: `adaptive = true`) stays world-docked at your desk, then picks
        -- itself up and head/body-follows when you WALK AWAY, and re-docks when you come back. The
        -- switch is a horizontal geofence around the desk "seat" (the head position captured when
        -- it last docked) with hysteresis + dwell so pacing past the boundary doesn't flap; the
        -- handoff is an eased pose blend, not a snap. Vertical movement (standing up) does NOT
        -- count by default. All of these are hot-tunable (set one with `hyprctl eval` and it
        -- applies live), which is the point — tune them in-headset.
        adaptive_leave_radius    = 1.5,          -- meters from the desk seat before it undocks and follows (R_out)
        adaptive_return_radius   = 1.0,          -- meters back within the seat before it re-docks (R_in < R_out)
        adaptive_leave_dwell_ms  = 400,          -- hold beyond R_out this long before undocking (anti-flap)
        adaptive_return_dwell_ms = 800,          -- hold within R_in this long before re-docking (deliberate return)
        adaptive_transition_ms   = 700,          -- dock<->follow eased blend duration
        adaptive_transition_ease = "smoothstep", -- "smoothstep" | "linear" | "ease_out"
        adaptive_roam_mode       = "body",       -- follow behavior while roaming: "body" (yaw-only) | "head" (gaze)
        adaptive_use_height      = false,        -- include vertical head movement in the geofence distance
        adaptive_carry_offset    = false,        -- roam at the offset captured when undocking (default: a comfy eye-level offset)

        -- The synthetic ray-pointer device (docs/openxr/04-input.md). Disable if you only want XR
        -- monitors as passive displays (e.g. pure mirror targets) with no controller-driven cursor.
        pointer = true,

        -- Ray-pointer click hysteresis (Schmitt trigger): press engages at/above the first value,
        -- release disengages at/below the second — keeps analog trigger jitter around the
        -- threshold from double-clicking.
        pointer_trigger_threshold         = 0.7,
        pointer_trigger_threshold_release = 0.4,

        -- Grab hysteresis: same idea, for the squeeze/grab gesture that picks up and moves a
        -- monitor.
        grab_threshold         = 0.7,
        grab_threshold_release = 0.4,

        -- CONTROLLERS: whether a squeeze on a monitor's CONTENT body moves it (true, default), or
        -- moving is confined to the bottom move-bar with resizing on the corner handles (false).
        -- The bar and corners always grab regardless. Hands use hand_grab_anywhere below instead.
        --   grab_anywhere = true,

        -- Hand-tracking grab gesture (XR_EXT_hand_interaction, e.g. Quest 3 + WiVRn with hand
        -- tracking on). "pinch" = thumb-index pinch, anchored to the stable pinch pose so opening
        -- the pinch to release doesn't lurch the window; "grasp" = fist curl (wrist-anchored);
        -- "both" (default) = either. Hands grab from the bottom move-bar / corner handles; pair
        -- with hand_grab_anywhere below for body grabs. Controllers are unaffected.
        hand_grab = "both",

        -- Which hand GESTURE may grab from a monitor's CONTENT body (the hand analog of
        -- grab_anywhere): "none" | "grasp" (default — a fist body-grabs, a pinch stays chrome-only
        -- and keeps its click) | "pinch" | "both". Caveat for pinch/both: a body pinch will BOTH
        -- click and grab.
        --   hand_grab_anywhere = "grasp",

        -- Release-lurch rejection: on release, re-anchor from the pose sampled this many ms BEFORE
        -- the release edge, so the grip/hand swing of letting go doesn't move the panel. 0 = use
        -- the release frame.
        --   grab_release_latency_ms = 100,

        -- Release-jerk rejection (RATIO, not m/s): on release, if the panel's peak speed in the
        -- last ~80 ms is more than this multiple of its typical carry speed, re-anchor from before
        -- the jerk (kills the fist-open / flick-and-let-go lurch). Being relative, a deliberate
        -- fast flick is kept. 0 disables. Default 3.0.
        --   grab_release_velocity_reject = 3.0,

        -- Move-grab carry smoothing (1-euro low-pass): removes tracking jitter while you carry a
        -- panel, at the cost of ~1 frame of latency. On by default. scope "all" filters BOTH hands
        -- and controllers (the default — controllers jitter too); scope "hands" keeps controllers
        -- on the zero-latency device late-latch. Tune with min_cutoff / beta.
        --   grab_filter            = true,
        --   grab_filter_scope      = "all",
        --   grab_filter_min_cutoff = 1.0,
        --   grab_filter_beta       = 0.025,

        -- Transparent chrome around each XR monitor: an auto-hiding move-bar under the content +
        -- corner resize handles, in meters. `size` still means CONTENT meters — the chrome rides in
        -- a transparent margin, so it never covers a pixel of desktop. Larger = easier to grab.
        --   chrome_enabled        = true,
        --   chrome_margin         = 0.10,
        --   chrome_bar_height     = 0.08,
        --   chrome_bar_width_frac = 0.8,
        --   chrome_corner_size    = 0.09,
        -- Fade behavior + state colors (idle / ray-hovering-an-element / grabbed):
        --   chrome_hide_delay_ms = 1500,
        --   chrome_fade_ms       = 150,
        --   chrome_col_idle  = "0x66aaaaaa",
        --   chrome_col_hover = "0xcc66aaff",
        --   chrome_col_grab  = "0xff66aaff",

        -- Ray aim assist — the big fix for "hard to grab the bar/corners, especially with a
        -- controller." HypXRland used to draw no ray or cursor at all, so you were aiming an
        -- invisible ray at sub-2-degree handles. Four forgiveness/feedback layers, all
        -- hot-tunable:
        --
        --  * Endpoint cursor: a small dot where each hand's ray hits a monitor. It colors by state
        --    (over content / over a grab handle / while pressing) and grows on press. Left hand
        --    tinted cooler so two rays are distinguishable. Costs nothing when no ray hovers a
        --    quad.
        --   cursor               = true,
        --   cursor_size          = 0.02,  -- meters (constant physical size)
        --   cursor_press_scale   = 1.6,
        --   cursor_col_idle      = "0x80ffffff",
        --   cursor_col_grabbable = "0xcc66aaff",
        --   cursor_col_press     = "0xffffffff",
        --   cursor_col_grab      = "0xff66aaff",
        --   cursor_per_hand_tint = true,
        --
        --  * Magnetism: a near-miss of a grab handle still highlights + grab-enables it (chrome
        --    only — desktop clicks stay pixel-precise). magnet_angle is the cone half-angle in
        --    degrees.
        --   magnet       = true,
        --   magnet_angle = 2.0,
        --
        --  * Sticky hover: once the ray lands a handle, keep it highlighted until the ray leaves
        --    the handle by more than hover_hysteresis meters — stops the highlight flickering at
        --    edges.
        --   hover_hysteresis     = 0.03,
        --   hover_dropout_frames = 2,
        --
        --  * Aim filter: 1-euro-smooth the aim pose before hit-testing (controllers AND hands), and
        --    damp it while you commit a press/pinch so the click doesn't yank the aim off-target.
        --   aim_filter            = true,
        --   aim_filter_min_cutoff = 1.5,
        --   aim_filter_beta       = 0.01,
        --   aim_pinch_damping     = 0.4,

        -- Controller haptics: a short tick on grab/press and (haptic_hover) when the ray first
        -- lands a grab handle — the tactile "you're on the handle" cue. Hands have no actuator
        -- (unaffected).
        --   haptics          = true,
        --   haptic_hover     = true,
        --   haptic_amplitude = 0.5,

        -- Thumbstick scroll multiplier while hovering an XR monitor (not grabbing it).
        scroll_speed = 1.0,

        -- WHEN to inhibit hypridle/idle timers. Uses the normal idle-inhibitor machinery, so
        -- hypridle's obey-inhibitors default just works — see docs/openxr/05-configuration.md §7.
        --   "off"     = never
        --   "focused" = while the session has input focus (a runtime dashboard in front does NOT
        --               inhibit). The old `inhibit_idle = true` behavior; the legacy strings
        --               "true"/"1" still map to it (a bare Lua `true` is a type error — this option
        --               is a string).
        --   "present" = (default) while the headset is actually WORN — also covers
        --               worn-but-not-focused (dashboard, overlay mode). Needs XR_EXT_user_presence
        --               (WiVRn has it); on a runtime without it this falls back to "focused".
        inhibit_idle = "present",

        -- XR monitors behave like UNPLUGGED external monitors while the headset is not actually in
        -- use: held disabled (workspaces evacuate to your remaining monitors, exactly like
        -- `hl.monitor({ output = NAME, disabled = true })`) and re-enabled (workspaces return by
        -- name, like replugging a physical display) as the headset comes and goes.
        -- Anchors/placement persist across the cycle. Three modes:
        --   "off"     - never; every XR monitor is an enabled headless output from startup
        --   "session" - unplug only while NO OpenXR session exists
        --   "visible" - (default) plug only while the headset is actually WORN. The plug gate
        --               requires BOTH the session to be VISIBLE/FOCUSED AND — when the runtime
        --               exposes XR_EXT_user_presence (e.g. WiVRn) — user presence. Requiring both
        --               is deliberate: WiVRn's presence can STICK 'present' with the headset doffed
        --               in standby, but the session reliably drops out of VISIBLE on a doff, so
        --               requiring visibility too still unplugs. A session created with the headset
        --               on the shelf never plugs even though WiVRn briefly sprints to FOCUSED.
        -- Legacy values still parse, as strings: "false"/"0" => off, "true"/"1" => session (a bare
        -- Lua boolean is a type error — this option is a string).
        monitors_follow_session = "visible",

        -- With monitors_follow_session = "visible", how long the headset must stay doffed
        -- (session not VISIBLE, or presence-absent) before its monitors actually unplug and their
        -- workspaces evacuate, in milliseconds. This anti-flap grace absorbs a quick doff-and-don
        -- and proximity-sensor churn so a brief glance away never rearranges your workspaces.
        -- Donning re-plugs IMMEDIATELY regardless. Default 20000 (20s).
        monitor_unplug_grace_ms = 20000,

        -- With monitors_follow_session = "visible": how long the session must stay continuously
        -- VISIBLE before the FIRST plug of a session, in milliseconds. Suppresses the
        -- session-create visibility blip (some runtimes sprint to FOCUSED at startup while still
        -- doffed), so the first plug always waits for sustained visibility. Only consulted on
        -- runtimes WITHOUT user presence; ignored after the first plug (the grace above governs
        -- later edges). Default 1500 (1.5s). `hyprctl openxr status` shows both raw signals:
        -- `visible:` and `presence:`.
        monitor_plug_settle_ms = 1500,

        -- Escape hatch: DESTROY XR-created virtual monitors when the session stops (headset off /
        -- `hyprctl openxr disable`) instead of keeping them. Declared XR monitors (hl.xr_monitor)
        -- re-materialize on the next session start. Default false — with monitors_follow_session
        -- above they are kept and merely unplugged, which preserves their anchor state and is the
        -- more reliable path.
        destroy_monitors_on_stop = false,

        -- Overlay mode: composite these monitors ON TOP of another VR app (a game, or the
        -- hypxrpaper ambient-background app) instead of owning the whole view. Needs a runtime
        -- that supports XR_EXTX_overlay — Monado and WiVRn do; SteamVR-Linux does NOT (there it
        -- silently falls back to a normal exclusive session with a warning). Read at session start
        -- only (`hyprctl openxr disable && hyprctl openxr enable` to re-apply). See
        -- docs/openxr/05-configuration.md. Off by default.
        --   overlay = true,
        -- Stacking order among overlay sessions (XR_EXTX_overlay sessionLayersPlacement): higher =
        -- composited later / on top. The primary app is always beneath. Only meaningful with
        -- overlay = true.
        --   overlay_z = 1,

        -- ---- conditional hand input (stop false hand gestures while typing) --------------------
        -- When hand TRACKING drives the XR pointer/grabs. Controllers are NEVER gated by this.
        --   "on"   : hands always active
        --   "off"  : hands never active (controllers still work)
        --   "auto" : (default) hands active only when you're AWAY from the keyboard — so typing at
        --            your desk never fires accidental pinch/grab gestures. Bind
        --            hl.dsp.xrmonitor("handinput toggle") (below) to force hands on for a moment
        --            while at the keyboard.
        hand_input = "auto",
        -- auto: seconds of physical-keyboard silence before hands re-enable. Any keypress re-gates
        -- them instantly. (A short window feels responsive; raise it if hands re-enable while you
        -- pause typing.)
        hand_input_idle_s = 15,
        -- auto: also enable hands whenever you ROAM beyond the adaptive seat geofence, so
        -- wandering the house always has hands even mid-typing-timeout. Needs an adaptive:on
        -- monitor to have a seat.
        hand_input_roam_enables = true,

        -- ---- gaze grab: keyboard-driven monitor manipulation steered by where you LOOK ---------
        -- Look at a monitor, tap a key to grab it (it then follows your gaze), tap keys to
        -- push/pull it, tap again to drop it. See the gaze* binds at the bottom of this file. On
        -- Quest 3 (no eye tracking) the gaze ray is your HEAD forward vector — aim by centering
        -- the monitor in view.
        --   gaze_source = "view",       -- "view" (head forward) | "eye" (reserved; auto-falls-back to view)
        --   gaze_dwell_ms = 200,        -- rest this long on a monitor before it's grab-eligible (anti-flick)
        --   gaze_hysteresis_deg = 3.0,  -- sticky-selection margin so adjacent monitors don't flicker
        --   gaze_follow = true,         -- true: the monitor rides your gaze while held. false: it detaches
        --                               --   at grab and only the push/pull keys move it (deliberate reposition)
        --   gaze_dist_step = 0.1,       -- default push/pull step (m) for `gazepush` with no arg
        --   gaze_filter = true,         -- 1-euro-smooth the gaze ray so the selection doesn't jitter
        --   gaze_cursor = true,         -- draw a dot at the gaze point on the carried monitor
        --   gaze_cursor_col = "0xffcc66ff",
        -- The dwell-stable monitor you're looking at is highlighted via the normal chrome
        -- (move-bar lights up); a grabbed monitor glows in the grab color. Tune any of these live
        -- with `hyprctl eval 'hl.config({ openxr = { gaze_dwell_ms = 300 } })'`.
    },
})


-- ---- Ambient background (overlay over hypxrpaper) ---------------------------------------------
--
-- With `overlay = true` above, HypXRland composites its monitors ON TOP of another OpenXR app.
-- `hypxrpaper` is a tiny standalone app that draws just an ambient backdrop (gradient sky, a
-- panorama image, or a 3D scene) for your monitors to float over. It's a SEPARATE project — build
-- it from its own repo, then autostart it here. It runs as the PRIMARY session; HypXRland's overlay
-- goes on top. See docs/openxr/05-configuration.md.
--
-- `exec-once` has no Lua keyword: a Lua config is re-run from the top on every reload, so the
-- once-per-session form is a hyprland.start handler. Uncomment `overlay = true` in the block above
-- AND this handler with one of the commands:
-- hl.on("hyprland.start", function()
--     hl.exec_cmd("hypxrpaper --scene forest-clearing")          -- bundled 3D scene backdrop
--     -- hl.exec_cmd("hypxrpaper ~/pictures/panoramas/sky.hdr")  -- equirectangular panorama image
--     -- hl.exec_cmd("hypxrpaper")                               -- built-in gradient sky (no args)
--     -- If your session doesn't export XR_RUNTIME_JSON, prefix it:
--     -- hl.exec_cmd("env XR_RUNTIME_JSON=/path/to/runtime.json hypxrpaper --scene forest-clearing")
-- end)


-- ---- hl.xr_monitor: declare virtual monitors + placement --------------------------------------
--
-- Grammar (docs/openxr/05-configuration.md §3):
--   hl.xr_monitor("<name>, <WxH[@Hz]>, <anchor-spec>[, size:<meters>]")
-- The string is the old `xrmonitor =` value verbatim — the same form `hyprctl openxr layout`
-- prints, so an in-headset arrangement pastes straight back in. The table form is the same
-- declaration with one field per grammar key (see the XR-video example below, and doc 05 §3
-- "In Lua"). Coordinates are meters, OpenXR convention: +X right, +Y up, -Z forward.
-- Ordinary hl.monitor rules (scale, vrr, bitdepth, ...) still apply to these outputs by name, same
-- as any headless monitor — hl.xr_monitor only controls existence + 3D placement.

-- anchor:local — fixed in the room (LOCAL_FLOOR space): a code editor bolted 1.4m up, 1.5m in
-- front of the room's tracking origin, facing +Z (i.e. facing you if you're standing at the
-- origin looking down -Z). pos: is required; yaw:/pitch: are optional (default: facing the
-- origin), degrees.
hl.xr_monitor("XR-code, 2560x1440@90, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.8")

-- anchor:head — a HUD that follows your gaze with a deadzone + critically-damped spring (the
-- leash_* tuning above): stays slightly right and below center until you turn far enough that it
-- has to catch up. offset: is in VIEW space (the direction you're facing); no yaw:/pitch: here —
-- display orientation is always lookAt-driven so the quad faces you.
hl.xr_monitor("XR-chat, 1280x720, anchor:head offset:0.4,-0.2,-1.0, size:0.6")

-- anchor:body — yaw-only follow: turns with you but doesn't tip with head pitch, and (with
-- body_leash_follow_height = false above) stays at a fixed height as you sit/stand. offset: is in
-- the yaw-only body frame; only yaw: is meaningful (pitch/roll are always flattened to 0).
hl.xr_monitor("XR-music, 1920x1080@60, anchor:body offset:-0.8,1.2,-1.2, size:0.9")

-- anchor:device:left|right — rigidly locked to a controller's grip pose (zero added latency: the
-- quad's XrSpace literally is the controller's action space). Good for a palette/toolbar you hold
-- up to glance at. offset: is in the grip frame; yaw:/pitch: optional.
hl.xr_monitor("XR-palette, 800x800, anchor:device:left offset:0,0.08,-0.05, size:0.25")

-- Workspace assignments that SURVIVE a logout: the in-session unplug/replug cycle already returns
-- workspaces to their XR monitor by name (monitors_follow_session above), but that memory is
-- runtime-only. To pin a workspace to an XR monitor PERSISTENTLY — across sessions, restarts and
-- logouts — use an ordinary workspace rule, exactly as for a physical monitor (XR monitors are
-- name-keyed like any other output). It also re-applies automatically every time the monitors
-- plug in at session start:
-- hl.workspace_rule({ workspace = "9", monitor = "XR-code", default = true })
-- hl.workspace_rule({ workspace = "10", monitor = "XR-chat" })

-- adaptive — an anchor:local desk screen that FOLLOWS you around the house
-- (docs/openxr/03-anchoring.md). It stays bolted to the desk pose while you sit there, then (once
-- your head crosses adaptive_leave_radius for adaptive_leave_dwell_ms) picks itself up and
-- body-follows you; when you return it re-docks to exactly this pose. Optional per-monitor tokens
-- override the openxr.adaptive_* globals (table-form field in brackets):
--   adaptive:on                     enable the decorator (default off)        [adaptive = true]
--   roam:head|body                  follow behavior while roaming (default: openxr.adaptive_roam_mode)
--                                                                             [roam = "body"]
--   roam_offset:x,y,z roam_yaw:deg  the comfortable follow placement (default: a bit in front, eye
--                                   level)                       [roam_offset = { x, y, z }, roam_yaw = deg]
--   leave:R  return:R               per-monitor geofence radii (return must be < leave)
--                                   [leave = R, return_radius = R — `return` is a Lua keyword]
--   carry:on|off                    roam at the offset captured on undock instead of the roam
--                                   offset                                    [carry = true|false]
-- A movie you watch at the desk but want trailing you when you get up to grab a snack:
hl.xr_monitor("XR-video, 1920x1080@60, anchor:local pos:0.3,1.05,-1.2 yaw:0, size:1.8, adaptive:on roam:body roam_offset:0,1.35,-1.2 roam_yaw:0")
-- ...which is the same declaration as this table (use one or the other, not both):
-- hl.xr_monitor({
--     name        = "XR-video",
--     mode        = "1920x1080@60",
--     anchor      = "local",
--     pos         = { 0.3, 1.05, -1.2 },
--     yaw         = 0,
--     size        = 1.8,
--     adaptive    = true,
--     roam        = "body",
--     roam_offset = { 0, 1.35, -1.2 },
--     roam_yaw    = 0,
-- })


-- ---- hl.xr_rule: situational per-monitor transparency (doc 05 §3.5) ---------------------------
--
-- Grammar (docs/openxr/05-configuration.md §3.5), two equivalent forms:
--   hl.xr_rule("<effects>, <conditions>")                       -- the old `xrrule =` value, verbatim
--   hl.xr_rule({ <effect> = v, ..., match = { <condition> = v, ... } })
-- Effects (string form: space-separated `name value`; table form: top-level fields):
--   alpha <0..1>              uniform monitor opacity (1.0 = opaque)
--   blackalpha <0..1|off>     per-monitor luma key ("black-as-alpha"); off = no keying
--                             (table form: a number, or the string "off")
--   blackalpha_knee <v>       per-monitor luma knee
-- Conditions (string form: space-separated `key:value`; table form: fields of `match`. ALL must
-- match; omitted = wildcard):
--   monitor:<regex>  anchorstate:<docked|follow|carried>  focusclass:<regex>
--   focustitle:<regex>  fullscreen:<0|1>   (table form: fullscreen = true|false)
-- Regexes are RE2 matched as a SEARCH (anchor with ^...$ for an exact match). Precedence is
-- defaults (openxr.black_alpha etc) -> rules in CONFIG ORDER, later wins per effect -> a manual
-- `hyprctl openxr alpha|blackalpha` override (sticky until cleared with `auto`). Every change
-- eases over openxr.transparency_blend_ms.

-- >>> THE SAFETY RULE — uncomment this if you use head/body leashing or adaptive anchoring. <<<
-- A leashed monitor TRAVELS WITH YOU: without this, a large opaque panel is pinned in front of
-- your face while you walk through a room you cannot fully see. At 0.55 the desktop stays readable
-- and you can still see the doorway, the coffee table and the cat. It is deliberately NOT a
-- hardcoded default (silently changing what your monitors look like would be worse), so it lives
-- here. Lower (0.4) sees the room better; higher (0.7) keeps text crisper.
-- hl.xr_rule("alpha 0.55, anchorstate:follow")
--   (table form: hl.xr_rule({ alpha = 0.55, match = { anchorstate = "follow" } }))

-- Fullscreen games get the panel back at full strength, with no luma keying.
-- hl.xr_rule("alpha 1.0 blackalpha off, monitor:.* anchorstate:docked focusclass:^steam_app_ fullscreen:1")

-- A dedicated video monitor should never dissolve its own letterboxing.
-- hl.xr_rule("blackalpha off, monitor:XR-video focusclass:(mpv|vlc)")

-- Runtime overrides (sticky; `auto` hands the monitor back to the rules):
-- hl.bind("SUPER + SHIFT + comma",  hl.dsp.exec_cmd("hyprctl openxr alpha active 0.4"))
-- hl.bind("SUPER + SHIFT + period", hl.dsp.exec_cmd("hyprctl openxr alpha active auto"))


-- ---- Mirroring an XR monitor onto a physical output -------------------------------------------
--
-- Zero XR-specific code involved — this is the ordinary hl.monitor `mirror` mechanism
-- (docs/openxr/02-virtual-monitors.md). Anyone standing at a physical screen sees exactly what's
-- on the XR-code virtual monitor above.
-- hl.monitor({ output = "eDP-1", mode = "preferred", position = "auto", scale = "1", mirror = "XR-code" })


-- ---- binds: the `xrmonitor` dispatcher (keyboard-driven) --------------------------------------
--
-- Verb table (docs/openxr/05-configuration.md §4): create, destroy, select, anchor, move, rotate,
-- scale, distance, center, adaptive, dock, undock, roam, view. hl.dsp.xrmonitor() takes the verb
-- and its arguments as ONE string — exactly the text that followed `xrmonitor,` in a classic bind.
-- Every verb also works as `hyprctl dispatch 'hl.dsp.xrmonitor("<verb> ...")'` or
-- `hyprctl openxr <verb> ...` — same implementation, three transports. "active"/omitted target
-- resolves via selection > last ray-hovered > focused-if-XR.

-- Spin up a scratch monitor on demand, and tear it down again.
hl.bind("SUPER + X",         hl.dsp.xrmonitor("create XR-scratch"))
hl.bind("SUPER + SHIFT + X", hl.dsp.xrmonitor("destroy active"))

-- Cycle which XR monitor subsequent verbs (move/rotate/scale/.../destroy active) target.
hl.bind("SUPER + bracketright", hl.dsp.xrmonitor("select next"))
hl.bind("SUPER + bracketleft",  hl.dsp.xrmonitor("select prev"))

-- Re-center the selected monitor in front of you at openxr.default_distance.
hl.bind("SUPER + Home", hl.dsp.xrmonitor("center"))

-- "Bring my monitors to me." Rigidly moves your WHOLE anchor:local layout so it sits in front of
-- wherever you are now facing, keeping its relative arrangement — nothing is reset to its config
-- line. This is the answer to "I swivelled my chair / moved to the couch and my monitors are behind
-- me"; the headset's own recenter button deliberately does NOT do it unless you set
-- openxr.recenter = "follow" (see the openxr block above). Idempotent: pressing it again from the
-- same spot is a no-op.
hl.bind("SUPER + CTRL + Home", hl.dsp.xrmonitor("reseat"))

-- Hide/show every XR monitor without stopping the overlay session or moving any workspaces.
hl.bind("SUPER + ALT + M", hl.dsp.xrmonitor("view toggle"))

-- Push/pull along the view ray and rescale — `{ repeating = true }` (the classic `binde`) repeats
-- while the key is held, natural for these continuous adjustments.
hl.bind("SUPER + equal",         hl.dsp.xrmonitor("distance -0.25"), { repeating = true })
hl.bind("SUPER + minus",         hl.dsp.xrmonitor("distance +0.25"), { repeating = true })
hl.bind("SUPER + SHIFT + equal", hl.dsp.xrmonitor("scale 1.1"),      { repeating = true })
hl.bind("SUPER + SHIFT + minus", hl.dsp.xrmonitor("scale 0.9"),      { repeating = true })

-- Nudge in view-relative meters, and rotate in place (LOCAL/DEVICE) or orbit the head (HEAD/BODY).
hl.bind("SUPER + ALT + left",  hl.dsp.xrmonitor("move -0.1 0 0"))
hl.bind("SUPER + ALT + right", hl.dsp.xrmonitor("move 0.1 0 0"))
hl.bind("SUPER + ALT + up",    hl.dsp.xrmonitor("move 0 0.1 0"))
hl.bind("SUPER + ALT + down",  hl.dsp.xrmonitor("move 0 -0.1 0"))
hl.bind("SUPER + ALT + prior", hl.dsp.xrmonitor("rotate -5"), { repeating = true })
hl.bind("SUPER + ALT + next",  hl.dsp.xrmonitor("rotate 5"),  { repeating = true })

-- Re-anchor the selected monitor into a different mode without moving it visually.
hl.bind("SUPER + H", hl.dsp.xrmonitor("anchor active head"))
hl.bind("SUPER + L", hl.dsp.xrmonitor("anchor active local"))

-- Adaptive anchoring verbs — operate on the selected monitor.
--   adaptive on|off|toggle  : enable/disable the desk<->follow decorator (recaptures the seat on enable)
--   undock                  : force it to pick up and follow NOW (skips the geofence dwell)
--   dock                    : force it back to the saved desk pose NOW
--   dock here               : redefine the desk pose to where it is now + recapture the seat
--   roam head|body          : change the follow behavior live
hl.bind("SUPER + A",         hl.dsp.xrmonitor("adaptive toggle"))
hl.bind("SUPER + SHIFT + U", hl.dsp.xrmonitor("undock"))
hl.bind("SUPER + SHIFT + D", hl.dsp.xrmonitor("dock"))
hl.bind("SUPER + CTRL + D",  hl.dsp.xrmonitor("dock here"))


-- ---- Gaze grab: look at a monitor + keys to manipulate ----------------------------------------
--
-- Look at a monitor and TAP the grab key — it grabs whatever you're looking at (dwell-stabilised),
-- and the monitor then follows your gaze. Tap the push/pull keys to change its distance. TAP THE
-- GRAB KEY AGAIN to drop it where it is (gazegrab is a TOGGLE). `gazerelease` is an explicit drop
-- if you'd rather bind a separate key. These are HEAD-gaze on Quest 3 (no eye tracking) — aim by
-- centering the monitor in your view. Works regardless of the hand_input gating above.
--
-- NB: the specific chords below are free on a stock Omarchy 3.8.2 layout, but VERIFY against your
-- own binds first — `omarchy menu keybindings --print` (or grep your ~/.config/hypr/bindings.lua
-- and the defaults it pulls in). Pick any free chord you like; the dispatcher verbs are what
-- matter.
hl.bind("SUPER + SHIFT + G",   hl.dsp.xrmonitor("gazegrab"))                         -- tap: grab the looked-at monitor / tap again: drop it
-- hl.bind("SUPER + CTRL + G", hl.dsp.xrmonitor("gazerelease"))                      -- optional: an explicit "drop it" key
hl.bind("SUPER + ALT + equal", hl.dsp.xrmonitor("gazepush 0.1"),  { repeating = true }) -- push farther (hold to repeat)
hl.bind("SUPER + ALT + minus", hl.dsp.xrmonitor("gazepush -0.1"), { repeating = true }) -- pull nearer  (hold to repeat)

-- Alternative HOLD-to-carry (instead of the toggle above): hold one key, monitor follows your gaze;
-- let go to drop it. `{ release = true }` (the classic `bindr`) fires the dispatcher on key RELEASE.
-- hl.bind("SUPER + SHIFT + G", hl.dsp.xrmonitor("gazegrab"))
-- hl.bind("SUPER + SHIFT + G", hl.dsp.xrmonitor("gazerelease"), { release = true })


-- ---- Conditional hand input: toggle hands on/off at the keyboard ------------------------------
--
-- With `hand_input = "auto"` (above) hands are OFF while you're typing and ON when you step away.
-- This key-chord forces them on right now (tap again to return to auto). `on`/`off`/`auto` set the
-- policy explicitly. Free on stock Omarchy 3.8.2 — verify against your own binds as noted above.
hl.bind("SUPER + ALT + H", hl.dsp.xrmonitor("handinput toggle"))            -- flip hands on/off while at the keyboard
-- hl.bind("SUPER + ALT + SHIFT + H", hl.dsp.xrmonitor("handinput auto"))  -- back to the automatic keyboard/roam gate
