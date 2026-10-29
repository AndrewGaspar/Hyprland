# Forward-porting HypXRland: rules learned on the 0.57 rebase

Upstream Hyprland **0.57** removed the classic hyprlang (`.conf`) front end and `hyprctl keyword`,
made the renderer context-explicit, split `CWindow`, and moved HyprCtl. The rebase of HypXRland
onto v0.57.0 (branch `hypxrland-0.57.0`, October 2026) took three phases: replay the branch, make it
build, then port the tests/scripts and validate in the container. This page is the short list of
rules that came out of it — read it before the next forward-port, and before adding new XR config
surface.

## 1. Config surface: Lua only

- **There is no legacy layer.** `src/config/legacy/` is gone. Every new XR setting, rule or
  declaration is a Lua binding (`src/config/lua/bindings/`) over the shared layer
  (`src/config/shared/`, e.g. `xr/XRDeclarationManager`, `monitor/Parser.cpp`). Never add a
  front-end-specific code path again; put the semantics in the shared layer and keep the binding
  thin.
- Where XR config lives today:
  - options: `MS<…>("openxr:…")` in `src/config/values/ConfigValues.cpp` → `hl.config({ openxr = { … } })`;
  - declarations: `hl.xr_monitor`, `hl.xr_rule`, `hl.xr_defaults` (`LuaBindingsConfigRules.cpp`;
    `xr_monitor`/`xr_rule` also take the old `.conf` value string verbatim);
  - monitor fields `stereo`, `lease` (`MONITOR_FIELDS`), window-rule effects `stereo`, `depth`,
    `viewpoint` (`WINDOW_RULE_EFFECT_DESCS`, `LuaBindingsInternal.hpp`), layer-rule `depth`;
  - dispatcher: `hl.dsp.xrmonitor("verb args")`, registered with `Internal::setDispatcherFn`, body in
    the front-end-free `src/openxr/XRDispatch.cpp`.
- **Lua literals must match the declared type.** Bool → `true/false`, Int/Float → number,
  String → a quoted string even when it looks numeric (`inhibit_idle = "1"`,
  `monitors_follow_session = "visible"`, `gpu = "/dev/dri/renderD129"`). A wrong type is an
  `error: …` reply from `eval`, not a silent coercion.
- **`hl.monitor` merges** onto the output's existing rule (the classic keyword replaced it). A revert
  must name the field it clears: `mirror = ''`, `stereo = 'off'`, `lease = false`, or
  `disabled = false`.
- **Runtime `hl.config` is asynchronous.** It schedules a prop refresh; `config.props_refreshed` runs
  `COpenXRManager::onConfigReload()` one idle tick later, and for *every* `openxr` key (the old keyword
  hook special-cased a few). Tests must wait for the effect in `j/openxr`; product code must not assume
  a synchronous apply. `hl.config` does not emit `config.reloaded` — only a full reload does.
- **Reload re-runs the whole script** (fresh Lua state; `package.loaded` is cleared for user modules,
  so `require()`d files re-execute). A runtime error in a required module aborts the rest of the
  script — every `hl.xr_monitor` after it is missing on that reload (audit P3, still open).

## 2. IPC and scripts

| ≤ 0.56 | 0.57 |
|---|---|
| `hyprctl keyword openxr:k v` / socket `/keyword …` | `hyprctl eval 'hl.config({ openxr = { k = v } })'` / `/eval …` (reply `ok` or `error: …`) |
| `hyprctl keyword monitor …` / `windowrule …` / `xrrule …` | `hl.monitor{…}` / `hl.window_rule{…}` / `hl.xr_rule{…}` via `eval` |
| `hyprctl dispatch focusmonitor X` (any classic dispatcher) | `hyprctl dispatch 'hl.dsp.focus({ monitor = "X" })'` — `dispatch` evaluates its argument as Lua |
| `hyprctl dispatch xrmonitor VERB` | `hyprctl openxr VERB` (scripts) or `hyprctl dispatch 'hl.dsp.xrmonitor("VERB")'` |
| `source = file.conf`, `exec-once = …` | `require("/abs/file.lua")` / `dofile(…)`, `hl.on("hyprland.start", function() hl.exec_cmd("…") end)` |

- `hyprctl systeminfo` still prints `configProvider: lua`; scripts that probe it (the dotfiles-mirrored
  `scripts/lib/hypr-config.sh`, `scripts/xreal-mode.sh`) keep working, their classic branches are
  simply unreachable. Change the dotfiles copies first, then re-sync the repo mirrors.
- `hyprctl openxr layout` prints paste-ready `hl.xr_monitor("…")` lines.
- HyprCtl moved to `src/ipc/s1/Commands.cpp`; register fork commands with
  `IPC::Socket1::sock()->registerCommand(SCommand{…})` (`src/openxr/XRIpc.cpp`). Socket2 events go
  through `IPC::Socket2::sock()->postEvent(…)`.

## 3. Renderer

- Everything takes the render context: `m_renderData` → `ctx.m_data` (no-ctx helpers use
  `m_context.m_data`), `m_renderPass` → `ctx.m_pass` (`ctx.m_pass.render(ctx, damage)`),
  `m_bBlockSurfaceFeedback` → `ctx.m_blockSurfaceFeedback`, `m_fakeFrame` → `ctx.m_gl.fakeFrame`,
  `m_applyFinalShader` → `ctx.m_gl.applyFinalShader`, `m_offloadedFramebuffer` →
  `ctx.m_gl.offloadedFramebuffer`. Projection is `setProjectionType(ctx, RPT_MONITOR|RPT_OUTPUT|…)`
  plus `ctx.m_data.transformDamage`; push/popMonitorTransformEnabled are gone.
- **`bindFB(ctx, fb)` also sets `ctx.m_data.currentFB`.** Read any texture you need from the current
  framebuffer *before* binding another one. The 0.57 port of the stereo pack read
  `ctx.m_data.currentFB->getTexture()` inside the per-pane loop, after pane 0 had bound the output FB,
  and SEGVed on the first two-composite frame (fixed by capturing `LASTPANETEX` first). `bindTempFB`
  returns a scope that restores `currentFB` on destruction.
- `CRenderContext::reset()` (called by `begin` and `endRender`) clears `m_data`, so per-frame stereo
  state in `SRenderData` (`stereoPane`, `stereoPaneFBs`, `stereoPaneDamage`, `stereoContentDrawn`)
  resets by itself — but nested draw scopes (`saveDrawState`) restore it too; a witness that must
  survive them has to be written outside (fc7500c12).
- Upstream "render always in normal transform": render-target-sized `m_pixelSize` became
  `m_transformedSize`, which our one-pane stereo design already makes pane-sized — take upstream's
  form; keep `m_pixelSize` only for scanout-sized uses (pack viewport, direct-scanout checks).
- Work buffers come from a capped per-monitor pool (`getWorkBuffer(ctx)`, 8 entries); always handle
  `nullptr`.

## 4. Core API map (hunks you will touch)

- Logging: `Log::logger->log(L, …)` / `LOGM(…)` → `LOG(L, …)`. **Hazard:** a call whose only argument
  after the level is a `std::string` binds silently to `log(level, string_view loc, string_view str)`
  — convert mechanically, do not wait for compile errors.
- `CWindow` split into `src/desktop/view/window/*`: `m_isMapped`→`mapped()`, `m_class`→
  `metadata().appID()`, `m_title`→`metadata().title()`, `m_initialClass/Title`→
  `metadata().initialAppID()/initialTitle()`, `xdgTag()`→`backend().metadata().tag`, `m_isX11`→
  `backend().isX11()`, `getPID()`→`backend().pid()`, `m_pinned`→`m_state & WINDOW_STATE_PINNED`,
  `m_floatingOffset`/`m_alpha`/`m_dimPercent`→`presentation()…`, `m_isFloating`→`isFloating()`,
  `m_stableID`→`metadata().stableID()`, `m_swallowee`→`swallowing().swallowee()`,
  `m_tearingHint`→`m_hints & WINDOW_HINT_TEAR`.
- Singletons: `g_pPointerManager`→`Pointer::mgr()`, `g_pKeybindManager`→`Keybinds::mgr()`
  (`src/keybinds/`), workspace state/placement → `State::Workspace::state()/placementController()`,
  workspace `m_id`/`m_name` → `id()` / `displayName()|addressableName()`, pointer constraints →
  `g_pInputManager->m_pointerConstraints.apply(…)`, monitor (dis)connect workspace logic →
  `State::Workspace::monitorConnected/Disconnected`.
- `IConfigManager` gained pure virtuals (`configLoaded`, `warnDeprecated`, `clearDeprecations`,
  `deprecationNotices`); `SConfigOptionReply::setByUser` is `const bool*`; `CONFIG_LEGACY` is gone.

## 5. Dependencies and build

- 0.57 wants hyprutils ≥ 0.14.3 (we accept 0.14.2: nothing from 0.14.3 is used, baa46560f),
  hyprland-protocols ≥ 0.8.0 (build-time XML; no subproject fallback any more), aquamarine ≥ 0.15.
  On a box whose mirror lags, point `PKG_CONFIG_PATH` at a private prefix for hyprland-protocols only.
- The container image must be refreshed for a new upstream: build a **suffixed** set so the shared one
  survives (`HYPXR_CTR_SUFFIX=-057a scripts/xr-container.sh build --rebuild`, see
  `containers/README.md`). `--rebuild` bypasses podman's layer cache; without it a "rebuild" replays
  the old package set. The hyprtester helper clients are read from `clientNew()` in
  `hyprtester/CMakeLists.txt`, so new upstream clients are built automatically.
- The rebase itself: `--rebase-merges`, rerere disabled for the run, and re-apply evil merges by hand
  (record each one); compare the stereo/depth renderer against the pre-rebase tip by semantics, not by
  text.

## 6. Tests

- `hyprtester --xr` runs `hyprtester/xr-test.lua` through a generated `<run-dir>/xr-wrapper.lua`
  (`require` of the config, optional `hyprtester/xr-test-local.lua`, `HYPRTESTER_XR_GPU` pin). Use
  `/eval` and `/dispatch hl.dsp.…` in tests; docs/openxr/06-testing.md §2.2 has the translation table.
- Run the **container** suites after every forward-port, both `test --xr` and `test --full` (the
  non-XR suite holds the stereo pack, depth and screencopy-crop cases, and can only run there). The
  stereo/depth renderer regression above was invisible to the build and to the 1833 gtests; the first
  container run found it.
- Read a failing XR case's neighbours before blaming the product. The `--xr` cases share ONE
  compositor and one Monado head, so state leaks: after a failed don/doff case every ray case that
  follows self-SKIPs ("ray never registered a hover") because the anchor:local frame was left
  rotated, and `xr_plugged_follow_session` restores a pose from a stale seat. Re-run the failing case
  alone and with its predecessor (`scripts/xr-container.sh test NAME…`) to tell ordering from
  regression. The 0.57 run's `xr_don_settle_hides_until_still` failure was exactly that: it committed
  its doff seat from the previous case's backwards-facing dwell, because it never sat facing its own
  monitor before the disable.
- The first "Hyprland has crashed" in every container run is expected: the stock headless launch
  throws `CBackend::create() failed!` in seatless rootless podman, and `runXrSuite` then nests into
  labwc.
