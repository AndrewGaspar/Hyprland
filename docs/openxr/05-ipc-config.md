# 05 — IPC, Config, Dispatchers, and Idle Integration

This document specifies the *entire* user-facing control surface of the Hyprland OpenXR
extension ("HypXRland"): the `openxr:*` config section, the `xrmonitor` config keyword, the
`xrmonitor` dispatcher, the `hyprctl openxr` command, the socket2 event set, the idle-inhibit
hook, and consumer recipes (waybar, hypridle, shell scripts).

It is self-contained. Where behavior is designed elsewhere it cites the sibling doc
(`00-overview.md` for the lifecycle state machine and thread model, `02-virtual-monitors.md`
for monitor lifecycle, `03-anchoring.md` for anchor semantics, `04-input.md` for the
frame→main event queue). All names here are authoritative and must be used verbatim across
the implementation.

Everything in this doc runs on the **main thread**. The XR frame thread never touches config,
IPC, or the event manager directly — it enqueues state-change notifications onto the
frame→main SPSC queue (eventfd on the wayland event loop, see `04-input.md`), and the
main-thread drain handler fires the IPC events described in §5.

---

## 1. The `openxr:*` config section

### 1.1 Where config vars are registered (current codebase reality)

Historically Hyprland registered vars via a `registerConfigVar(...)` cluster plus a separate
`ConfigDescriptions.hpp`. **That is no longer the mechanism.** On current `main`, all config
values are declared *once* — name, description, default, and constraints together — in:

- `src/config/values/ConfigValues.cpp` — `Values::getConfigValues()` returns a
  `std::vector<SP<IValue>>` of `MS<Type>(name, description, default, {options})` entries
  (`MS` = `makeConfigValue`, `#define`d at the top of the function). Sections are grouped
  by `/* section: */` comments (see the `gestures:` block near line ~397 for a model).
- `src/config/values/ConfigValues.hpp` — the type aliases: `Bool`, `Int`, `Float`, `String`,
  `Vec2`, etc., and option structs (`SFloatValueOptions{.min,.max,.refresh}`, …).

The legacy config manager consumes this list automatically: `src/config/legacy/ConfigManager.cpp`
(constructor, the `for (const auto& v : Values::CONFIG_VALUES)` loop around line ~488) maps
each entry onto `registerConfigVar(...)`. The Lua config manager (`src/config/lua/`) picks the
same list up. **Adding entries to `getConfigValues()` is the only registration step needed** —
descriptions are inline (they surface via `hyprctl descriptions`), and both config front-ends
get the var.

Reading a value in `src/openxr/` code uses the standard accessor
(`src/config/ConfigValue.hpp`):

```cpp
static auto PENABLED = CConfigValue<Hyprlang::INT>("openxr:enabled");
if (*PENABLED) { ... }
```

### 1.2 The full variable table

Add one block to `getConfigValues()` under a new `/* openxr: */` section comment:

| Name | Type | Default | Description (verbatim for the `MS<>` entry) | Reload behavior |
|---|---|---|---|---|
| `openxr:enabled` | `Bool` | `false` | enable the OpenXR integration (session starts when a runtime is available) | **hot** — toggling starts/stops the session (§1.3) |
| `openxr:gpu` | `String` | `""` | DRM render node to use for XR (e.g. /dev/dri/renderD128). Empty = follow Hyprland's primary GPU | **start-only** — read at session start; changing it takes effect on the next start |
| `openxr:blend_mode` | `String` | `"auto"` | environment blend mode: `auto` (runtime preferred) \| `opaque` \| `alpha` (passthrough) \| `additive`. `auto` = the runtime's first-enumerated mode; an explicit mode the runtime doesn't advertise falls back to the preferred with a WARN (doc 01) | **start-only** — read at session start; changing it takes effect on the next start |
| `openxr:floor_offset` | `Float` | `1.5` | fallback eye height in meters when the runtime lacks LOCAL_FLOOR | start-only |
| `openxr:default_size` | `Float` | `1.6` | default width of a new XR monitor quad, in meters | hot (affects subsequently created monitors) |
| `openxr:default_distance` | `Float` | `1.5` | default distance from the viewer for newly placed monitors, in meters | hot (affects subsequently created monitors) |
| `openxr:leash_response` | `Float` | `0.35` | head/body leash spring response time in seconds (smaller = snappier) | **hot-live** — anchor solver re-reads per snapshot |
| `openxr:leash_deadzone_angle` | `Float` | `15.0` | head/body leash angular deadzone in degrees | hot-live |
| `openxr:leash_deadzone_distance` | `Float` | `0.25` | head/body leash positional deadzone in meters | hot-live |
| `openxr:body_leash_follow_height` | `Bool` | `false` | body-leashed monitors also follow vertical head movement | hot-live |
| `openxr:pointer` | `Bool` | `true` | enable the XR ray pointer device | hot — device created/destroyed on change |
| `openxr:pointer_trigger_threshold` | `Float` | `0.7` | trigger analog value that presses the pointer button | hot-live |
| `openxr:pointer_trigger_threshold_release` | `Float` | `0.4` | trigger analog value that releases the pointer button (hysteresis) | hot-live |
| `openxr:grab_threshold` | `Float` | `0.7` | squeeze analog value that starts a grab | hot-live |
| `openxr:grab_threshold_release` | `Float` | `0.4` | squeeze analog value that ends a grab (hysteresis) | hot-live |
| `openxr:scroll_speed` | `Float` | `1.0` | multiplier for thumbstick scrolling on XR monitors | hot-live |
| `openxr:inhibit_idle` | `Bool` | `true` | inhibit idle (hypridle etc.) while the XR session is focused | hot — triggers an idle recheck (§6) |
| `openxr:monitors_follow_session` | `String` | `visible` | when XR-created monitors behave like UNPLUGGED external monitors (created/held disabled — workspaces evacuate — then re-enabled/returned by name as the headset comes and goes): `off` = never (old always-present behavior); `session` = while no session exists (research/18); `visible` = while the session is not VISIBLE/FOCUSED, so a doffed/standby headset reads as unplugged (report-18 addendum). Legacy `false/0`→off, `true/1`→session | hot — `updateMonitorsPlugged()` re-asserts on reload/keyword |
| `openxr:monitor_unplug_grace_ms` | `Int` | `20000` | under `monitors_follow_session = visible`, ms the headset must stay doffed/standby (session not VISIBLE) before its monitors unplug and workspaces evacuate — anti-flap for a quick doff-and-don / proximity-sensor churn (report-18 addendum). Donning re-plugs immediately | hot-live (re-read at each arm) |
| `openxr:destroy_monitors_on_stop` | `Bool` | `false` | destroy XR-created virtual monitors when the session stops, instead of keeping them (unplugged under `monitors_follow_session != off`, plain headless outputs otherwise). The research/18 option-(a) escape hatch | hot-live (consulted at stop time) |
| `openxr:overlay` | `Bool` | `false` | run as an `XR_EXTX_overlay` session so monitors composite ON TOP of another XR client (a game, or `hypxrpaper`). Requires a runtime that advertises `XR_EXTX_overlay` (Monado/WiVRn); requested-but-unsupported downgrades to a normal exclusive session with a WARN, never failing startup (doc 01) | **start-only** — read at session start; changing it takes effect on the next start |
| `openxr:overlay_z` | `Int` | `1` | overlay composition placement (`XR_EXTX_overlay` `sessionLayersPlacement`); higher composites later / on top. On Monado this maps straight into the layer `z_order` (primary pinned to `INT64_MIN`), so any value puts our quads above the primary | **start-only** — read at session start |

Float entries should carry sane `{.min, .max}` options (e.g. thresholds `{.min = 0.0, .max = 1.0}`,
`leash_response {.min = 0.01, .max = 5.0}`). "hot-live" means the code reads the
`CConfigValue` accessor at point-of-use every time, so no listener is needed; "hot" means a
listener must react (below).

### 1.3 The `openxr:enabled` hot toggle

`COpenXRManager` (see `00-overview.md`; global `g_pOpenXRManager`,
`src/openxr/OpenXRManager.{hpp,cpp}`) owns two idempotent main-thread methods:

- `start()` — DISABLED → (UNAVAILABLE | STARTING …) per the lifecycle state machine.
- `stop()` — any running state → STOPPING → DISABLED. Consults
  `openxr:destroy_monitors_on_stop`.

There is no per-var callback mechanism; the idiom for reacting to reloads is a bus listener
(examples: `src/managers/KeybindManager.cpp:165`, `src/protocols/XDGOutput.cpp:40`):

```cpp
// in COpenXRManager's constructor (main thread)
m_configReloadListener = Event::bus()->m_events.config.reloaded.listen([this] { onConfigReload(); });
```

`onConfigReload()` compares `*PENABLED` against the current lifecycle state and calls
`start()`/`stop()` accordingly, then runs `xrmonitor` declared-set reconciliation (§2.4) and
`g_pInputManager->recheckIdleInhibitorStatus()` (§6). A full config reload (`/reload`, or the
file watcher) reaches it via the `config.reloaded` listener above; `onConfigReload()` is also
public and idempotent so `hyprctl openxr enable|disable` (§4) can call `start()`/`stop()`
directly — all paths funnel to the same two methods.

**Legacy-parser gap and its fix (as built, WP13 reconciliation):** the original plan was that
`hyprctl keyword openxr:enabled 1` would update the value and fire
`Event::bus()->m_events.config.props_refreshed`, which `COpenXRManager` also listens to. In
practice, under the **classic hyprlang config path**, `props_refreshed` is Lua-config-only
plumbing (only the Lua config-rules bindings ever call `CPropRefresher::scheduleRefresh()`); a
bare `hyprctl keyword <var> <value>` against the legacy parser goes through
`CConfigManager::parseKeyword()` and fires **neither** `config.reloaded` nor
`config.props_refreshed` for an ordinary var. `openxr:inhibit_idle` hit this same gap first (WP12,
`xr_idle_inhibit`): `hyprctl keyword openxr:inhibit_idle 0/1` silently did nothing until a full
reload. Both are fixed identically, with a small special-case cluster in `parseKeyword()`
(`src/config/legacy/ConfigManager.cpp`, next to the existing `monitor`/`gaps_`/`dwindle:`/
`master:` special cases that exist for the same reason):

```cpp
if (COMMAND == "openxr:inhibit_idle")
    g_pInputManager->recheckIdleInhibitorStatus();

#ifdef HAVE_OPENXR
if (COMMAND == "openxr:enabled" && g_pOpenXRManager)
    g_pOpenXRManager->onConfigReload();
#endif
```

The `props_refreshed` listener in `COpenXRManager`'s constructor is still correct and harmless
(it fires, and `onConfigReload()` is idempotent, under the Lua config path or a scheduled
refresh) — just not sufficient on its own for a legacy-parser `hyprctl keyword`. A full
`hyprctl reload` always works regardless of config front-end, since it fires `config.reloaded`
unconditionally.

---

## 2. The `xrmonitor` config keyword

Declares that an XR virtual monitor should *exist* and where it lives in 3D space. It does
**not** carry 2D output properties beyond the mode — scale, bit depth, VRR, mirroring, etc.
come from ordinary `monitor =` / `monitorv2` rules matched by name, exactly as for any
headless output (`Config::monitorRuleMgr()` applies by name; see `02-virtual-monitors.md`).

### 2.1 Registration

Follow the `monitor` keyword pattern exactly. In `src/config/legacy/ConfigManager.cpp`:

1. A static trampoline next to `handleMonitor` (line ~275):

```cpp
static Hyprlang::CParseResult handleXRMonitor(const char* c, const char* v) {
    const std::string VALUE = v, COMMAND = c;
    const auto RESULT = Config::Legacy::mgr()->handleXRMonitor(COMMAND, VALUE);
    Hyprlang::CParseResult result;
    if (RESULT.has_value())
        result.setError(RESULT.value().c_str());
    return result;
}
```

2. Registered in the constructor's keyword cluster (next to
`m_config->registerHandler(&::handleMonitor, "monitor", {false});`, line ~606):

```cpp
m_config->registerHandler(&::handleXRMonitor, "xrmonitor", {false});
```

3. `CConfigManager::handleXRMonitor(command, args)` (declared in the header alongside
`handleMonitor`) does **no XR work itself**: it calls the pure parser (§2.3) and, on success,
appends the resulting `SXRMonitorParams` to the manager's declared list. The trampoline and
member function must compile with and without `HAVE_OPENXR`; only the reconcile step (§2.4)
is a no-op without a built XR module (guarded call).

Note the two config front-ends: the classic hyprlang path above is the v1 target. A Lua
config binding is explicitly out of scope for v1 (tracked as WP13 polish in `07-roadmap.md`).

### 2.2 Grammar

```
xrmonitor = <name>, <mode>, <anchor-spec>[, <kv>]...

<name>        ::= output name, conventionally "XR-" prefixed (e.g. XR-code)
<mode>        ::= <W> "x" <H> [ "@" <Hz> ]              ; pixels, optional refresh (default 60)
<anchor-spec> ::= "anchor:local"  <local-args>
                | "anchor:head"   <offset-arg>
                | "anchor:body"   <offset-arg>
                | "anchor:device:" ("left"|"right") <offset-arg>
<local-args>  ::= SP "pos:" <f> "," <f> "," <f> [ SP "yaw:" <deg> ] [ SP "pitch:" <deg> ]
<offset-arg>  ::= SP "offset:" <f> "," <f> "," <f>
<kv>          ::= "size:" <meters>                       ; quad width; default openxr:default_size
```

Top-level fields are comma-separated (parse with `CVarList2` as `handleMonitor` does,
`src/config/legacy/ConfigManager.cpp:1293`); *within* the anchor-spec field, sub-tokens are
space-separated `key:value` pairs. Coordinates are meters in the OpenXR convention: +X right,
+Y up, −Z forward. `pos` for `anchor:local` is in the LOCAL_FLOOR reference space (Y is height
above the floor). `offset` for `head`/`body`/`device` is in the respective leash/device frame
(see `03-anchoring.md` for the exact frames). `yaw`/`pitch` are degrees; unspecified means
face the origin-facing default described in `03-anchoring.md`.

### 2.3 Parser location and unit tests

The parser is a **pure function, compiled unconditionally** (no OpenXR headers, no
`HAVE_OPENXR` guard) so `hyprland_gtests` can always exercise it:

- `src/openxr/XRMonitorConfig.hpp/.cpp` —
  `namespace OpenXR { std::expected<SXRMonitorParams, std::string> parseXRMonitorLine(const std::string& args); }`
  plus the `SXRMonitorParams` struct itself (name, width, height, refresh, anchor mode enum,
  pose/offset, yaw, pitch, size_m — the same struct `COpenXRManager::createXRMonitor` accepts,
  see `02-virtual-monitors.md`). This file sits with the pure-math siblings `XRAnchor.{hpp,cpp}`
  and `XRMath.hpp`, which are likewise unconditional.
- Unit tests: `tests/xr/parser.cpp` (gtest, discovered automatically by the root
  `CMakeLists.txt` `file(GLOB_RECURSE TESTFILES "tests/*.cpp")` at line ~688). Cover: the four
  examples below round-trip; refresh defaulting; each anchor mode; yaw/pitch optionality;
  malformed inputs return errors (missing pos, bad floats, unknown anchor mode, unknown kv key,
  device side other than left/right).

### 2.4 Examples (one per anchor mode)

```ini
# fixed in the room: 1.4 m up, 1.5 m in front of the origin, facing +Z (the user)
xrmonitor = XR-code, 2560x1440@90, anchor:local pos:0,1.4,-1.5 yaw:0, size:1.8

# head-leashed HUD: follows gaze with deadzone + spring, slightly right and below center
xrmonitor = XR-chat, 1280x720, anchor:head offset:0.4,-0.2,-1.0, size:0.6

# body-leashed: yaw-only follow, stays at torso height to the left
xrmonitor = XR-music, 1920x1080@60, anchor:body offset:-0.8,1.2,-1.2, size:0.9

# locked to the left controller (zero-latency: quad space = grip action space)
xrmonitor = XR-palette, 800x800, anchor:device:left offset:0,0.08,-0.05, size:0.25
```

### 2.5 Reload reconciliation semantics

The declared list is rebuilt from scratch on every parse (clear it in the same place other
keyword state resets: `CConfigManager::resetHLConfig()`), then reconciled once after a
successful reload, from `COpenXRManager::onConfigReload()`:

- Let **D** = declared set (this parse), **L** = live XR monitors *created by config*
  (each `CXRMonitorLayer` records `m_declaredByConfig`).
- `D \ L` → create via `COpenXRManager::createXRMonitor(params)` (marked declared).
- `D ∩ L` → diff params: anchor mode/pose/offset/size changed → re-anchor in place
  (`03-anchoring.md`); mode (WxH@Hz) changed → mode-change swapchain-recreate protocol
  (`02-virtual-monitors.md`). Unchanged → untouched (no flicker).
- `L \ D` → destroy.
- **Monitors created at runtime via the dispatcher or hyprctl (`m_declaredByConfig == false`)
  are never touched by reconciliation.** Removing their `xrmonitor` line does nothing because
  they never had one.

If no session is running (DISABLED/UNAVAILABLE), declared monitors still materialize as plain
headless outputs; their quads bind lazily when a session starts (`02-virtual-monitors.md`).

**Two distinct trigger paths, as built (WP13 reconciliation):** "reconciled once after a
successful reload" above covers a **full** reload (`/reload`, file watcher, `hyprctl reload`) —
`CConfigManager::handleXRMonitor()` (the trampoline's target, §2.1) only *accumulates* into the
declared list per line during parsing; it never reconciles per-line for a full reload.
Reconciliation for that path runs exactly once, after all keyword state has settled, from
`COpenXRManager::onConfigReload()` (reached via the `config.reloaded` listener, §1.3).

A **dynamic** `hyprctl keyword xrmonitor ...` is a second, separate path: like `openxr:enabled`
(§1.3), it never fires `config.reloaded`/`config.props_refreshed` under the legacy parser, so
`onConfigReload()` is never reached that way. `handleXRMonitor()` detects this case explicitly
(`g_pHyprCtl->m_currentRequestParams.isDynamicKeyword`) and defers a direct reconcile call to the
next event-loop iteration:

```cpp
if (g_pHyprCtl && g_pHyprCtl->m_currentRequestParams.isDynamicKeyword && g_pEventLoopManager)
    g_pEventLoopManager->doLater([] {
        if (g_pOpenXRManager)
            g_pOpenXRManager->reconcileDeclaredMonitors();
    });
```

The `doLater` deferral (not an inline call) matters: it lets a batch of dynamic keyword changes
(e.g. `hyprctl --batch "keyword xrmonitor ...; keyword xrmonitor ..."`) all land in the declared
list before reconciliation runs once, rather than reconciling — and potentially flickering — after
every individual line.

---

## 3. The `xrmonitor` dispatcher

One dispatcher, verb-based, bindable from `bind =` and callable via `hyprctl dispatch
xrmonitor <verb> ...`. Registered in the in-tree dispatcher map,
`src/config/legacy/DispatcherTranslator.cpp` — add to the `CDispatcherTranslator`
constructor (line ~915):

```cpp
m_dispMap["xrmonitor"] = ::xrmonitorDispatch;
```

**Two registration sites, as built (WP13 reconciliation):** the line above alone is not
sufficient for `bind =` to reach the dispatcher. `bind =` resolution goes through
`CKeybindManager::m_dispatchers`, not `CDispatcherTranslator::m_dispMap` directly — legacy
dispatcher names have to be separately listed in `CKeybindManager`'s constructor
(`src/managers/KeybindManager.cpp:111`, in the array of names each forwarded via
`Config::Legacy::translator()->run(n, args)`) for `bind = ..., xrmonitor, ...` to work at all.
`hyprctl dispatch xrmonitor ...` reaches the translator's `m_dispMap` through a different path
and only needs the first registration. Both are required in practice; `xrmonitor` is present in
both lists.

`xrmonitorDispatch(const std::string& args)` returns `SDispatchResult`
(`src/SharedDefs.hpp:51` — `{passEvent, success, error}`); split `args` on spaces, first token
is the verb. The dispatcher body is a thin shim: **every verb funnels into a
`COpenXRManager` method — the exact same methods the hyprctl subcommands (§4) call.** No
logic lives in the dispatcher itself. Without `HAVE_OPENXR` the dispatcher is still registered
and returns `{.success = false, .error = "Hyprland was built without OpenXR support"}`.

### 3.1 Verb table

| Verb | Args | Semantics |
|---|---|---|
| `create` | `<name> [WxH[@Hz]] [anchor-spec]` | Create an XR monitor. Mode defaults to `1920x1080@60`; anchor defaults to `anchor:local` placed `openxr:default_distance` meters along the current gaze, at `openxr:default_size` width, facing the user. Anchor-spec grammar identical to §2.2. Created monitors are runtime-owned (not reconciled). |
| `destroy` | `<name\|active>` | Destroy the named monitor, or the selected one (`active`). Refuses non-XR outputs. |
| `select` | `<name\|next\|prev>` | Set the explicitly-selected XR monitor. `next`/`prev` cycle creation order. |
| `anchor` | `<name\|active> <mode-spec>` | Re-anchor: `local` (freeze current world pose into LOCAL_FLOOR), `head [offset:x,y,z]`, `body [offset:x,y,z]`, `device:left\|right [offset:x,y,z]`. Omitted offset = keep the monitor visually where it is (compute offset from current pose, `03-anchoring.md`). Fires `xrmonitoranchor` (§5). |
| `move` | `<dx> <dy> <dz>` | Translate the selected monitor by meters **in its anchor frame** (local: LOCAL_FLOOR axes; leashed: offset delta). |
| `rotate` | `<dyaw> [dpitch]` | Rotate the selected monitor, degrees. Pitch clamped to ±85°. |
| `scale` | `<f\|+d\|-d>` | Bare number `f` multiplies the quad width by `f`; explicitly signed `+d`/`-d` adds/subtracts `d` meters. Result clamped to 0.2–4.0 m (matches grab-resize limits, `04-input.md`). |
| `distance` | `<±m>` | Push/pull the selected monitor along the viewer→quad ray by `m` meters; distance clamped to 0.3–5.0 m. Accepts `+0.25` / `-0.5` relative syntax (bare numbers are relative too). |
| `center` | *(none)* | Re-place the selected monitor centered in the current view at `openxr:default_distance`, facing the user. Anchor mode is preserved (offset recomputed). |

### 3.2 Selected-monitor resolution order

Verbs that operate on "the" monitor (`select`d target, `active`, and all of
`move/rotate/scale/distance/center`) resolve their target in this order:

1. **Explicit selection** — the monitor last set via `xrmonitor select` (cleared if destroyed).
2. **Last ray-hovered** — the XR monitor most recently hit by the ray pointer (`04-input.md`).
3. **Focused-if-XR** — if `Desktop::focusState()`'s focused monitor is an XR monitor, use it.
4. Otherwise the verb fails with `SDispatchResult{.success = false, .error = "no XR monitor selected"}`.

### 3.3 Example binds block

```ini
# Keyboard-driven XR monitor management (see 04-input.md for the controller-side grab)
bind = SUPER,       X,            xrmonitor, create XR-scratch
bind = SUPER SHIFT, X,            xrmonitor, destroy active
bind = SUPER,       bracketright, xrmonitor, select next
bind = SUPER,       bracketleft,  xrmonitor, select prev
bind = SUPER,       Home,         xrmonitor, center
binde = SUPER,      equal,        xrmonitor, distance -0.25
binde = SUPER,      minus,        xrmonitor, distance +0.25
binde = SUPER SHIFT, equal,       xrmonitor, scale 1.1
binde = SUPER SHIFT, minus,       xrmonitor, scale 0.9
bind = SUPER,       H,            xrmonitor, anchor active head
bind = SUPER,       L,            xrmonitor, anchor active local
```

(`binde` = repeat-on-hold, natural for `distance`/`scale`/`rotate`.)

---

## 4. The `hyprctl openxr` command

### 4.1 Registration

`src/debug/HyprCtl.hpp:22` — `CHyprCtl::registerCommand(SHyprCtlCommand)`;
`SHyprCtlCommand` is `{name, exact, fn(eHyprCtlOutputFormat, std::string request)}`
(`src/SharedDefs.hpp:46`). Because `openxr` takes subcommands in the request string, register
with `exact = false` (the same pattern as `output`, registered at
`src/debug/HyprCtl.cpp:2018`: `registerCommand(SHyprCtlCommand{"output", false, dispatchOutput});`).

The registration lives in `src/openxr/XRIpc.cpp` (called from `COpenXRManager` init), not in
HyprCtl.cpp — keep the XR footprint outside `src/openxr/` at zero for this command. Without
`HAVE_OPENXR` no command is registered and `hyprctl openxr` returns hyprctl's stock unknown-
request error. `-j` support comes for free: the handler receives
`eHyprCtlOutputFormat::FORMAT_JSON` and branches, exactly like `cursorPosRequest`
(`src/debug/HyprCtl.cpp:1274`). Model the arg splitting on `dispatchOutput`
(`src/debug/HyprCtl.cpp:1743`): `CVarList vars(request, 0, ' ')`, `vars[0] == "openxr"`,
`vars[1]` = subcommand.

### 4.2 Subcommands

| Subcommand | Behavior |
|---|---|
| `status` *(default when no subcommand)* | Session + monitor report. This is the bar-pollable surface. |
| `enable` / `disable` | `g_pOpenXRManager->start()` / `stop()`. Does **not** change the config value — a subsequent reload re-applies `openxr:enabled` (document this in the wiki note, WP13). Returns `ok` or an error string. |
| `create <name> [WxH[@Hz]] [anchor-spec]` | Same code path as the dispatcher verb (§3.1). |
| `destroy <name\|active>` | ditto |
| `select <name\|next\|prev>` | ditto |
| `anchor <name\|active> <mode-spec>` | ditto |
| `move <dx> <dy> <dz>` / `rotate <dyaw> [dpitch]` / `scale <f>` / `distance <±m>` / `center` | ditto |
| `layout` | Emits paste-ready `xrmonitor = ...` lines for every live XR monitor (both declared and runtime-created), current poses baked in — the v1 persistence story (`03-anchoring.md` §persistence). |

The mutation subcommands literally call the same `COpenXRManager` methods as §3 and return
the `SDispatchResult.error` on failure — one implementation, two transports.

### 4.3 `status` output

Normal format (one line per field, hyprctl house style):

```
state: focused
runtime: Monado(XRT) by Collabora et al.
system: Simulated HMD
blend mode: opaque
monitors follow session: visible
monitor XR-code (ID 3): 2560x1440@90.00 size 1.80m anchor local pos [0.00, 1.40, -1.50] grabbed: no (none) hovered: yes plugged: yes
```

JSON (`hyprctl -j openxr` / `hyprctl -j openxr status`) — full schema, all keys always
present:

```json
{
    "state": "focused",
    "runtimeName": "Monado(XRT) by Collabora et al.",
    "systemName": "Simulated HMD",
    "blendMode": "opaque",
    "overlay": false,
    "monitorsFollowSession": "visible",
    "monitorUnplugPendingMs": -1,
    "inhibitingIdle": true,
    "monitors": [
        {
            "name": "XR-code",
            "id": 3,
            "size_m": 1.80,
            "anchor": {
                "mode": "local",
                "pose": {
                    "pos": [0.0, 1.4, -1.5],
                    "quat": [0.0, 0.0, 0.0, 1.0]
                }
            },
            "grabbed": false,
            "hovered": true,
            "plugged": true
        }
    ]
}
```

- `state` — one of `disabled`, `unavailable`, `starting`, `idle`, `visible`, `focused`,
  `stopping` (the lifecycle states of `00-overview.md`, lowercased; RUNNING's sub-states are
  reported directly).
- `runtimeName`/`systemName` — from `xrInstanceProperties`/`xrSystemProperties`; empty
  strings when no session (`disabled`/`unavailable`).
- `blendMode` — the active environment blend mode: `opaque` | `alpha` | `additive` (doc 01).
  Selected from `openxr:blend_mode` against the runtime's enumerated modes at session start;
  `opaque` (the default) when there is no session.
- `overlay` — whether the current session is an `XR_EXTX_overlay` session (doc 01). Reflects the
  ACTUAL state, not the `openxr:overlay` request: `false` when there is no session, or when overlay
  was requested but the runtime didn't advertise the extension (downgraded to exclusive). Also
  emitted in the text form as `overlay: yes|no`.
- `inhibitingIdle` (added WP12/WP13 reconciliation) — mirrors `shouldInhibitIdle()`'s own
  predicate (`openxr:inhibit_idle && state == focused`, §6.1) as a read-only observability field.
  There is otherwise no queryable surface for "is the compositor's idle-inhibit bit currently
  raised because of XR" (`CIdleNotifyProtocol::isInhibited` is private with no getter, and folds
  every inhibitor source anyway) — this lets tests and status consumers assert idle-inhibit
  end-to-end without polling wall-clock idle timers.
- `id` — the Hyprland `MONITORID` of the backing headless output; `-1` if the output is not
  (yet) mapped.
- `plugged` (research/18 + report-18 addendum) — whether the backing headless output is currently
  enabled. `openxr:monitors_follow_session` decides WHEN XR monitors are unplugged (disabled,
  workspaces evacuated — like an unplugged external monitor): `off` = never; `session` = whenever
  no session exists; `visible` (**default**) = whenever the session is not `VISIBLE`/`FOCUSED`, so
  a doffed/standby headset (whose runtime keeps a session alive on the shelf) reads as unplugged.
  Under `visible`, an unplug on a visibility drop is deferred by `openxr:monitor_unplug_grace_ms`
  (default 20000) so a quick doff-and-don never evacuates workspaces; donning re-plugs immediately.
  Text form: `plugged: yes|no`.
- `monitorsFollowSession` (report-18 addendum) — the active follow mode: `off` | `session` |
  `visible`. Text form: `monitors follow session: <mode>`.
- `monitorUnplugPendingMs` (report-18 addendum) — ms remaining before a pending grace-period
  unplug fires (headset doffed under `visible` mode), or `-1` when no unplug is pending. Text form
  appends `(unplug in <n>ms)` to the follow-mode line while pending.
- `anchor.mode` — `local` | `head` | `body` | `device:left` | `device:right`.
- `anchor.pose` — for `local`: pose in LOCAL_FLOOR. For leashed/device modes: the configured
  offset in the leash frame as `pos` and the relative rotation as `quat`. **WP8 deviation (as
  built, WP13 reconciliation):** while a monitor is grabbed, `anchor.pose` instead reports the
  *live world-composed pose* (`CXRAnchor::lastWorld()`) for **every** anchor mode, not the frozen
  configured offset — the offset field would otherwise go stale for the whole duration of the
  grab (the grab override lives in a separate `m_grabOffset`, doc 03 §4.2, and is only folded
  back into the persistent `anchorPose`/offset on release). This makes any status-polling
  consumer (a bar, `hyprctl openxr layout`, tests) track the controller live during a grab; not
  originally specified here (doc 05 predates grab, WP8), noted for WP13.
- `quat` order is `[x, y, z, w]` (OpenXR convention).

---

## 5. socket2 events

Posted via `g_pEventManager->postEvent(SHyprIPCEvent{.event = "...", .data = "..."})`
(`src/managers/EventManager.hpp:7-17`; wire format `EVENT>>DATA\n`). Call-site pattern
throughout the tree, e.g. `src/Compositor.cpp:908`. **All posts happen on the main thread.**
Transitions detected on the XR frame thread (session state changes from the event pump, grab
begin/end, hover changes) travel through the frame→main state-event queue
(`00-overview.md`/`04-input.md`) and are posted from the drain handler; transitions that
originate on the main thread (create/destroy/anchor verbs, enable/disable) post inline.

| Event | Data payload | Fired when / from where |
|---|---|---|
| `openxrsessionstate` | `disabled` \| `unavailable` \| `starting` \| `idle` \| `visible` \| `focused` \| `stopping` | Every lifecycle transition. start()/stop() edges post inline; runtime-driven `XrEventDataSessionStateChanged` transitions (idle↔visible↔focused, LOSS_PENDING→stopping) arrive via the frame→main queue. |
| `openxractive` | `1` \| `0` | Derived boolean: active ⇔ state ∈ {`visible`, `focused`}. Posted only when the boolean flips (immediately after the corresponding `openxrsessionstate`). This is the "is someone in the headset" signal for bars. |
| `xrmonitoradded` | `<name>` | After an XR monitor is fully created (headless output up + registered with the manager). Fires from `createXRMonitor` on the main thread — for all three origins (keyword reconcile, dispatcher, hyprctl). Note the stock `monitoradded` event *also* fires for the underlying output; this event is the XR-specific one. |
| `xrmonitorremoved` | `<name>` | After destruction of the XR monitor (any origin, including `destroy_monitors_on_stop` teardown). |
| `xrmonitoranchor` | `<name>,<mode>` — mode as in §4.3 | Anchor **mode** changed (not every pose mutation): the `anchor` verb (§3.1, when the resolved mode actually differs from the current one — `OpenXRManager.cpp` `cmdAnchor`'s `modeChanged` gate), or a reload reconcile whose declared anchor mode differs from the live one (`OpenXRManager.cpp`'s `anchorModeChanged` gate, §2.5). **WP13 reconciliation note: grab release does NOT fire this event**, even though `endGrab()` re-derives the persistent anchor representation (`03-anchoring.md` §4.4) — a normal grab where the mode is unchanged (e.g. local → local) only fires `xrmonitorgrab ...,0`; only the `anchor` verb / reload-reconcile paths above post `xrmonitoranchor`, both on the main thread (no frame→main queue involvement for this event). |
| `xrmonitorgrab` | `<name>,1` \| `<name>,0` | Grab began / ended on a monitor (`04-input.md` grab state machine; frame thread → queue → main-thread post). |
| `xrmonitorquad` | `<name>,1` \| `<name>,0` | Quad reactivated (`1`) / suspended (`0`) under the runtime layer cap (`02-virtual-monitors.md` layer-cap policy: oldest quads suspended first; the monitor keeps rendering as a plain headless output). Frame thread → queue → main-thread post. |

---

## 6. Idle integration

Design: **the XR module never calls `PROTO::idle->setInhibit()` itself.** It exposes one
predicate and pokes the existing recheck; the single authoritative writer of the inhibit bit
stays `CInputManager::recheckIdleInhibitorStatus()`.

### 6.1 The predicate

```cpp
// COpenXRManager, main thread
bool COpenXRManager::shouldInhibitIdle() {
    static auto PINHIBIT = CConfigValue<Hyprlang::INT>("openxr:inhibit_idle");
    return *PINHIBIT && m_state == XR_STATE_RUNNING_FOCUSED;
}
```

FOCUSED (and only FOCUSED) — the headset is on and this session has input focus. VISIBLE
(e.g. runtime dashboard open in front) intentionally does not inhibit.

### 6.2 The hook — exact placement

`src/managers/input/IdleInhibitor.cpp`, inside
`CInputManager::recheckIdleInhibitorStatus()`. The function is a chain of early-returning
`setInhibit(true)` checks **ending in an unconditional `PROTO::idle->setInhibit(false)`**
(as built: the `#ifdef HAVE_OPENXR` block sits just before it, with the unconditional `false`
immediately after the block's `#endif`). The XR check MUST live inside this function, before that final `false` — calling
`setInhibit(true)` from XR code directly would be clobbered by the very next recheck (which
fires on every inhibitor/window change). This is the **only** XR touch outside `src/openxr/`
apart from registration lines:

```cpp
    // check manual user-set inhibitors
    for (auto const& w : Desktop::windowState()->windows()) {
        if (isWindowInhibiting(w)) {
            PROTO::idle->setInhibit(true);
            return;
        }
    }

#ifdef HAVE_OPENXR
    if (g_pOpenXRManager && g_pOpenXRManager->shouldInhibitIdle()) {
        PROTO::idle->setInhibit(true);
        return;
    }
#endif

    PROTO::idle->setInhibit(false);
}
```

### 6.3 Recheck triggering

`COpenXRManager` calls `g_pInputManager->recheckIdleInhibitorStatus()` on **every**
session-state transition (in the same main-thread code that posts `openxrsessionstate`, §5)
and from `onConfigReload()` (so toggling `openxr:inhibit_idle` takes effect immediately).
`PROTO::idle->setInhibit()` (`src/protocols/IdleNotify.hpp:42`) is cheap and edge-detects
internally, so redundant calls are fine.

### 6.4 Activity is free

No explicit `PROTO::idle->onActivity()` calls are needed for XR input: `CXRPointerDevice`
injects motion/button/axis through the normal `g_pInputManager` device path (`04-input.md`),
and that path already fires `PROTO::idle->onActivity()` like any mouse. Using XR controllers
therefore resets idle timers exactly as moving a physical mouse does.

---

## 7. Consumer recipes

### 7.1 waybar module (polling)

```jsonc
// ~/.config/waybar/config
"custom/openxr": {
    "exec": "hyprctl -j openxr | jq -r '\"\\(.state) \\(.monitors | length)\"'",
    "interval": 5,
    "format": "XR: {}",
    "exec-if": "hyprctl -j openxr | jq -e '.state != \"disabled\"'"
}
```

`hyprctl -j openxr` is guaranteed cheap (main-thread state snapshot; no XR calls), so polling
at 1–5 s is fine. For event-driven updates, wrap the socket2 listener below in a
`"exec"` streaming module instead.

### 7.2 socket2 listener (event-driven, shell)

```sh
socat -U - "UNIX-CONNECT:$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket2.sock" \
  | grep --line-buffered -E '^(openxrsessionstate|openxractive|xrmonitor)' \
  | while IFS='>>' read -r ev _ data; do
        echo "XR event: $ev = $data"   # e.g. notify-send, waybar signal, ...
    done
```

### 7.3 hypridle

No configuration needed for the default behavior: hypridle uses `ext-idle-notify-v1` and, by
default, **obeys idle inhibitors** (`CExtIdleNotification` is created with `obeyInhibitors`,
`src/protocols/IdleNotify.hpp:12`). Since §6 raises the compositor-side inhibit bit while the
XR session is focused (and `openxr:inhibit_idle = 1`, the default), hypridle's listeners
simply never fire while you're in the headset — screens don't blank, the lock doesn't kick in.

Opposite policies are scriptable via the events instead:

- *"Lock the desktop as soon as I put the headset on"*: listen for `openxractive>>1` (§7.2)
  and run `loginctl lock-session`.
- *"Blank physical outputs while in XR"*: on `openxractive>>1` run
  `hyprctl dispatch dpms off`, on `openxractive>>0` run `hyprctl dispatch dpms on` — with
  `openxr:inhibit_idle` still keeping hypridle quiet.

---

## Context files to read before implementing

- `docs/openxr/00-overview.md` — lifecycle states, thread model, frame→main queue contract
- `docs/openxr/02-virtual-monitors.md` — `createXRMonitor(SXRMonitorParams)`, mode-change recreate
- `docs/openxr/03-anchoring.md` — anchor frames, re-anchor math, `layout` persistence
- `docs/openxr/04-input.md` — pointer device, grab machine, event-queue drain handler
- `src/config/values/ConfigValues.cpp` + `src/config/values/ConfigValues.hpp` — where vars are declared (`getConfigValues()`); entry/option format
- `src/config/legacy/ConfigManager.cpp` — `handleMonitor` trampoline (~275), keyword registration cluster (~606), `resetHLConfig()`, `reload()` + `config.reloaded` emission (~1072)
- `src/config/legacy/DispatcherTranslator.cpp` + `.hpp` — `m_dispMap` registration (~790), `SDispatchResult` usage
- `src/debug/HyprCtl.cpp` — `dispatchOutput` (1743), `cursorPosRequest` (1274, JSON branch), `registerCommand` cluster (~1987)
- `src/managers/EventManager.hpp` — `SHyprIPCEvent`/`postEvent`
- `src/managers/input/IdleInhibitor.cpp` + `src/protocols/IdleNotify.hpp` — the recheck function and inhibit API
