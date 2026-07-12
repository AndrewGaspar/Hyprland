# Research — Spatial → 2D Monitor Layout (derive Hyprland's layout plane from the XR arrangement)

Design research for making HypXRland **derive Hyprland's native 2D monitor-layout plane from the 3D
spatial arrangement of the XR quads**, so that mouse crossing and directional focus/movement between XR
monitors match spatial intuition: a monitor floating to your upper-right in the headset sits to the
upper-right in the 2D layout, and the cursor leaves `XR-main`'s right edge and lands on `XR-side` exactly
where your eyes expect.

Status: **research / design only. Nothing here is implemented.** No live runs, no code changes. Distinct
from `research/08-auto-layout.md`, which is the *inverse* problem (arrange the quads *in XR space*); this
doc takes the XR arrangement as given and asks how to *project it down* into the 2D layout the OS uses
for pointer/focus adjacency. The two compose: 08 decides where quads go in 3D, 12 keeps the 2D plane in
sync with wherever they ended up. Naming is provisional and defers to the authoritative docs
(`00..07-*.md`).

---

## TL;DR

1. **XR monitors land in a dumb right-appended row today.** A new XR monitor is a headless `CMonitor`
   with the default monitor rule (`m_offset = {-INT32_MAX,-INT32_MAX}` ⇒ "auto"), so
   `CMonitorPositionController::arrange` appends it flush to the right edge of the layout in *creation
   order*, at `y=0`, edge-touching. Its 2D position has **nothing to do with its 3D pose**
   (`MonitorRule.hpp:44`, `MonitorPositionController.cpp:61-101`, `OpenXRManager.cpp:1191-1313`).

2. **The hard constraint is `STICKS`.** Directional `movefocus`/monitor adjacency
   (`CMonitorQueryCore::directionLookup`) requires the two monitors' facing edges to be within **2 px**
   (`#define STICKS(a,b) abs((a)-(b)) < 2`, `macros.hpp:42`) **and** to overlap on the perpendicular
   axis. Gaps > 2 px silently kill `movefocus` between them. Cursor crossing is softer — the pointer is
   clamped to the *union* of monitor boxes (`CPointerManager::closestValid`), so gaps are *crossable* but
   produce a visible jump, and perpendicular misalignment means the cursor slides/snaps rather than
   crossing where you expect. **Conclusion: the projection must emit an edge-touching, gap-free,
   overlap-free layout — a raw angular unwrap is not directly usable; it must pass through a compaction
   pass.** This is exactly what every OS display arranger (GNOME mutter, KDE KScreen) does with dragged
   monitor rectangles.

3. **The projection is a viewer-centric equirectangular unwrap.** For each quad take its world center,
   compute (azimuth, elevation) about the user in a **stable reference frame**, map azimuth→x and
   elevation→y via a `px_per_degree` constant, **position centers by angle but keep each monitor's native
   pixel size**. That gives correct *ordering + rough proportions + tiers*; a **compaction/snap pass**
   then turns it into a valid `STICKS`-satisfying layout (like GNOME's "no gaps, no overlaps" normalize).

4. **The frame problem is the subtle part.** World/`local` monitors' azimuth relative to the user
   changes as the user turns — if we used instantaneous head yaw the 2D layout would churn every frame.
   Fix: measure world monitors in a **latched "desk orientation" reference frame** (LOCAL_FLOOR forward
   captured at the last `center`/recenter), measure `head`/`body` monitors in their own follow frame
   (their offset angle is already constant there), and **merge both sets onto one plane** — they coincide
   when the user faces the reference forward, which is the pose the 2D map represents. Combine with
   **freeze-on-event**: recompute only on discrete events, never per-frame.

5. **Cleanest integration: inject computed offsets as `explicitPosition`, then let Hyprland arrange.**
   Set each XR monitor's `m_activeMonitorRule.m_offset` to the projected (x,y) and call
   `scheduleRecheck()`. The existing `arrange()` pipeline then does placement, overlap check, xdg-output
   update, and `monitor.layoutChanged` emission for free. Live monitor moves are **graceful** — `moveTo`
   shifts only that monitor's floating windows by the delta; tiled windows relayout in place; workspaces
   stay attached; the cursor is re-clamped (`Monitor.cpp:1694-1713`).

6. **Two work packages:** WP-S1 = pure projection + normalization math with gtests (no runtime deps);
   WP-S2 = reference-frame capture, debounced recompute funnel, offset injection, `hyprctl openxr
   sync-layout` verb, config + events, and an arc-slot fast path that shares with research/08.

---

## 1. Current behavior (code cites)

### 1.1 Where an XR monitor lands in the 2D plane

`COpenXRManager::createXRMonitor` (`src/openxr/OpenXRManager.cpp:1191`) creates the backing output with
no layout hint:

```cpp
// OpenXRManager.cpp:1241-1247
for (auto const& impl : g_pCompositor->m_aqBackend->getImplementations()) {
    if (impl->type() == Aquamarine::AQ_BACKEND_HEADLESS) {
        impl->createOutput(params.m_name);   // name only — no position
        ...
```

`newOutput → monitorState()->add() → CMonitor ctor + onConnect` runs synchronously, and `onConnect`
applies the monitor rule matched by name. Unless the user wrote an explicit `monitor = XR-1, …, XxY, 1`
rule, the rule is the default `CMonitorRule` with:

```cpp
// src/config/shared/monitor/MonitorRule.hpp:41,44
eAutoDirs m_autoDir = DIR_AUTO_NONE;                 // "None will be treated as right."
Vector2D  m_offset  = Vector2D(-INT32_MAX, -INT32_MAX);
```

`CMonitor::explicitPosition()` returns `nullopt` for that sentinel offset, and `autoDirection()` returns
`DIR_AUTO_NONE`:

```cpp
// src/output/Monitor.cpp:1245-1254
std::optional<Vector2D> CMonitor::explicitPosition() const {
    if (m_activeMonitorRule.m_offset == Vector2D{-INT32_MAX, -INT32_MAX})
        return {};
    return m_activeMonitorRule.m_offset;
}
Config::eAutoDirs CMonitor::autoDirection() const { return m_activeMonitorRule.m_autoDir; }
```

`CMonitorPositionController::arrange` (`src/state/MonitorPositionController.cpp:16`) places all
explicit-position monitors first, then walks the auto monitors and for `DIR_AUTO_NONE`/`DIR_AUTO_RIGHT`
does `newPosition.x = maxXOffsetRight` (`:69-70`) — i.e. **appends flush to the current right edge at
y=0**. So XR monitors form a horizontal, creation-ordered, edge-touching row to the right of everything
else. Position is unrelated to the quad's 3D pose. (Step 5 of `createXRMonitor`,
`OpenXRManager.cpp:1285-1298`, only touches *resolution*, never offset: "`xrmonitor=` owns existence + XR
placement only.")

### 1.2 create / destroy / reload

- **Create:** as above — auto-append right. Emits `xrmonitoradded` (`OpenXRManager.cpp:1302`).
- **Destroy:** `finalizeLayerRemoval` destroys the output; the layout controller reflows the rest
  (surviving auto monitors shift left to close the gap on the next `arrange()`); emits
  `xrmonitorremoved`.
- **Reload:** `CConfigManager::reload → reloadRules` clears and re-adds monitor rules
  (`ConfigManager.cpp:746,893`) then `ensureMonitorStatus()` re-applies them, re-running `arrange()`.
  **Any runtime XR position we set is lost on reload** unless it is (a) baked into a `monitor=` rule
  offset, or (b) re-injected after `config.reloaded`. This is a required trigger for the sync engine
  (§4).
- **Relayout funnel:** `CMonitorLayoutController::scheduleRecheck()` (`MonitorLayoutController.cpp:26`)
  debounces via `g_pEventLoopManager->doLater([]{ arrange(); checkOverlapsAndNotify(); })`. `arrange()`
  (`:55`) rebuilds positions, calls `PROTO::xdgOutput->updateAllOutputs()`, and emits
  `monitor.layoutChanged`. **Overlaps are only *warned*, never prevented** — `checkOverlapsAndNotify`
  (`:39`) logs an error + shows a notification but leaves the overlapping layout in place. So the sync
  engine is responsible for producing a clean layout; Hyprland won't fix it for us.

### 1.3 `hyprctl keyword monitor NAME,…,XxY,1` at runtime (the relayout mechanism auto-sync would drive)

Goes through `handleMonitor → CMonitorRuleParser → applyMonitorRule` with an explicit offset, ending in
`CMonitor::moveTo` (`src/output/Monitor.cpp:1694`):

```cpp
void CMonitor::moveTo(const Vector2D& pos) {
    if (m_position == pos) return;
    const auto OLD_POSITION = m_position;
    m_position = pos;
    if (OLD_POSITION == Vector2D{-1, -1}) return;
    const auto DELTA = pos - OLD_POSITION;
    for (const auto& w : Desktop::windowState()->windows()) {
        if (!validMapped(w) || !w->m_isFloating || w->m_monitor != m_self) continue;
        w->layoutTarget()->setPositionGlobal(w->layoutTarget()->position().translate(DELTA)); // float only
    }
}
```

**Pitfalls, verified against the code — all mild:**
- **Workspaces are not reshuffled.** Workspaces belong to the monitor object; moving the monitor's
  layout position doesn't detach them. Windows stay on their monitor.
- **Floating windows** are translated by the move delta (so they visually stick to the monitor); **tiled
  windows** get re-laid-out by their layout against the new monitor box — no data loss, just a relayout.
- **Cursor** is re-clamped on the next pointer event by `closestValid` (§1.4); a monitor that moves out
  from under the cursor pulls the cursor to the nearest valid box. A live re-sync therefore may *nudge
  the cursor* — an argument for event-driven + debounced recompute (§4), not per-frame.
- **Overlap** during a multi-monitor move is allowed and only warned; a good compaction pass avoids it.

So the runtime relayout primitive is safe to drive repeatedly. The durable way to set it (survives the
next `arrange()`/recheck) is to set the **rule offset** (`explicitPosition`), not a bare `moveTo` — a
raw `moveTo` is overwritten on the next recheck because `arrange()` recomputes from `explicitPosition()`.

### 1.4 How adjacency actually resolves

**Cursor crossing** — `CPointerManager::closestValid` (`src/pointer/PointerManager.cpp:696`):

```cpp
if (INSIDE_LAYOUT(hotBox)) return pos;          // inside union of monitor boxes → keep
Vector2D leader = NEAREST_LAYOUT(pos);          // else snap to nearest box's closest point
...                                             // then nudge hotbox fully inside
return hotBox.middle();
```

The pointer lives in the global layout plane; each motion delta is applied then clamped to the **union**
of `monitorBoxes`. Consequences:
- **Gaps are crossable but jumpy.** A point in a gap snaps to the nearest box; moving right across a gap,
  the cursor pins to A's right edge until it passes the gap's midpoint, then jumps to B's left edge — a
  discontinuity of ~gap-width. Tolerable for small gaps, ugly for large ones.
- **Perpendicular overlap matters.** If B is offset vertically and the cursor is outside B's y-range,
  crossing right keeps the cursor's y and it clamps back onto A (or snaps to B's nearest corner) — it
  won't "flow" into B. Smooth crossing needs the shared edge to overlap in the perpendicular axis.

**Directional `movefocus`/monitor lookup** — `CMonitorQueryCore::directionLookup`
(`src/state/MonitorQueryCore.cpp:133`):

```cpp
case Math::DIRECTION_RIGHT:
    if (STICKS(POSA.x + SIZEA.x, POSB.x)) {                 // right edge of A ~ left edge of B (±2 px)
        const auto INTERSECTLEN = std::max(0.0, std::min(POSA.y+SIZEA.y, POSB.y+SIZEB.y)
                                                 - std::max(POSA.y, POSB.y));
        if (INTERSECTLEN > longestIntersect) { … pick this monitor … }
    }
```

with `#define STICKS(a, b) abs((a) - (b)) < 2` (`src/macros.hpp:42`). So a right-neighbor is only found
if A's right edge and B's left edge are **within 2 px** *and* their y-ranges **overlap**. **Any gap > 2 px
breaks `movefocus` outright** (the `closestTo` distance fallback in `MonitorQueryCore.cpp:99` is used for
"which monitor is this point on", not for directional focus). `focusmonitor`/`movewindow`-to-direction
use the same `directionLookup`.

**Net requirement for the projection:** to make both cursor crossing *and* `movefocus` behave to
intuition, the emitted layout must have neighbors **edge-touching within 2 px** with **perpendicular
overlap**. This is non-negotiable and is why a naive angle→pixel map (which leaves gaps and fractional
overlaps) must be run through a normalization/compaction pass.

---

## 2. The projection design (3D pose → 2D layout coordinates)

### 2.1 Inputs available (no new plumbing)

- **Per-monitor world pose:** `CXRMonitorLayer::m_anchor` (`XRMonitorLayer.hpp:90`) exposes
  `lastWorld()` — the live solved quad pose in LOCAL_FLOOR (position in meters, +Y up, −Z forward). For
  `LOCAL` monitors this equals `state().anchorPose` when not grabbed; `layoutDump()` already relies on
  this (`OpenXRManager.cpp:1864-1880`). For `HEAD`/`BODY`, `state().anchorPose` *is* the offset in the
  follow frame (§3).
- **Quad size:** `state().widthMeters` + the monitor's pixel mode give the quad's metric width/height ⇒
  its angular subtense at its distance.
- **Reference view:** `currentVerbContext()` → `m_lastVerbCtx` (`OpenXRManager.cpp:1602`) is a
  main-thread copy of the last frame's `SXRVerbContext { view, viewValid, … }` — the user's head pose in
  LOCAL_FLOOR. Safe to read on the main thread; no frame-thread refcount hazard.
- **Math:** `XRMath.hpp` has `qYawOf(q, fallback)` (yaw about +Y, 0 faces −Z, `= atan2(-f.x,-f.z)`),
  `qRotate`, `Vec3`, etc. — enough to compute azimuth/elevation with no new math.

### 2.2 The viewer-centric unwrap

Treat the user's eye as the origin and unroll the cylinder/sphere of monitors around them into a plane
(equirectangular projection of monitor **centers**):

For a monitor with world center `c` and reference eye `e`, reference yaw `θ₀` (§3):
```
d      = c - e                                  // vector from eye to monitor center (m)
az     = atan2(d.x, -d.z) - θ₀                  // azimuth relative to the reference forward (rad)
                                                //   wrapped to (-π, π]
el     = atan2(d.y, len(d.x, d.z))              // elevation above eye level (rad)
x_c    = deg(az) * PX_PER_DEG                    // layout x of the CENTER (px)
y_c    = -deg(el) * PX_PER_DEG                   // layout y of the CENTER (px); screen-down is +y
box    = native pixel size (pxW × pxH), unchanged
```

**Position centers by angle; keep native pixel dimensions.** This is the whole point: a 4K monitor and a
720p monitor at the same distance subtend the same angle but have very different pixel sizes; we place
them by where they *are* (angle) and let them keep how *big* they are (native px). The size mismatch is
reconciled by the compaction pass, not by rescaling monitors.

**Vertical mapping — recommendation: elevation angle (option shown above), not world-height.** Using
elevation keeps the whole map a clean spherical unwrap (both axes are angles ⇒ one `PX_PER_DEG`
constant, symmetric behaviour). World-height (`y_c = -(d.y)·PX_PER_M`) is the alternative and can feel
better when monitors sit at wildly different radii (a low, close reference monitor vs a high, far
status display), but it introduces a second unit/constant and couples vertical placement to distance in
a way users don't reason about. Recommend **elevation by default**, expose world-height as
`openxr:layout2d:vertical = elevation | world_height` (§5). Comfort literature (research/08 §2) says
vertical *curvature* is disorienting, which is an argument against ever mapping monitors onto a full
vertical sphere in XR — but that's about the 3D arrangement, not this 2D projection; here elevation is
just a scalar for row placement.

### 2.3 `PX_PER_DEG` — what makes gaps feel right

The constant sets how far apart two centers are per degree of angular separation. It cannot, by itself,
make arbitrary monitors edge-touch (their native px widths differ from their angular widths), so its job
is to establish **ordering and rough proportions**, after which compaction (§2.4) snaps edges. Two
sensible anchors for the default:

- A "reference monitor" (1080p-ish at `default_distance`=1.5 m, `default_size`=1.6 m) subtends
  `2·atan(0.8/1.5) ≈ 56°` and is 1920 px wide ⇒ `1920/56 ≈ 34 px/deg`. At this value, two monitors that
  are *angularly adjacent in 3D* (edges nearly touching) come out *nearly edge-touching in 2D* before
  compaction, so compaction only nudges by a few px — the layout barely deviates from a true unwrap.
- Recommend **`PX_PER_DEG = 35`** default; expose as `openxr:layout2d:px_per_degree`. Larger ⇒ monitors
  spread out (more pre-compaction gap, more emphasis on angular spacing); smaller ⇒ they crowd/overlap
  pre-compaction (compaction pushes them apart, so relative *order* dominates and absolute spacing is
  lost). The exact value matters little because compaction is authoritative for adjacency; it mostly
  affects how "proportional" the pre-compaction spacing looks and how tier/column grouping thresholds
  scale.

### 2.4 Normalization / compaction to a valid Hyprland layout

The raw unwrap has gaps and fractional overlaps ⇒ unusable per §1.4. Run a compaction pass that
**preserves relative order + rough vertical tiers** but produces an edge-touching, gap-free,
overlap-free integer-px layout:

1. **Rows (tiers):** cluster monitors by `y_c` — monitors whose center-elevations differ by less than a
   tier threshold (e.g. `< 0.5·mean monitor height in deg`, or a config `row_merge_deg`) and whose
   azimuth ranges interleave belong to the same row. This yields a small set of horizontal rows,
   top-to-bottom by elevation.
2. **Columns within a row:** sort by `az`. Lay monitors left→right at **native px width**, each placed
   so its left edge equals the previous monitor's right edge (`STICKS`-exact, gap 0). This guarantees
   horizontal `movefocus`.
3. **Row stacking:** stack rows top→bottom, each row's top edge = previous row's bottom edge, so
   vertically-adjacent monitors touch. Horizontally align rows to preserve the sense of "this one is
   above-and-to-the-right": offset each row so a monitor's x-range overlaps the monitor it sits above/
   below (needed for vertical `movefocus`' `INTERSECTLEN > 0`). When a monitor would have *no*
   perpendicular overlap with any neighbour in the target direction, nudge it into partial overlap
   (minimum overlap = a config `min_overlap_px`, e.g. 64 px) — the analogue of GNOME/KDE refusing a
   configuration where a shared edge has zero contact.
4. **Attach to the physical block (see §2.5):** translate the whole XR block so it sits at the chosen
   seam relative to the physical monitors.

This is a small, deterministic constraint solve — closer to a stable sort + prefix-sum than a real
solver — and is exactly analogous to how GNOME mutter *normalizes* a dragged arrangement to remove gaps
and how KDE KScreen *snaps* dragged rectangles to adjacent edges (§6). It is pure and unit-testable.

**Vertical partial overlap = "upper-right" fidelity.** Hyprland tolerates a right-neighbour placed at a
raised y as long as the x-edges stick and the y-ranges overlap (real desks with mismatched monitor
heights work this way). So `XR-side` floating up-and-right of `XR-main` becomes: `XR-side.x =
XR-main.right`, `XR-side.y = XR-main.y − (elevation delta scaled, clamped so y-ranges still overlap by
`min_overlap_px`)`. That captures the diagonal intuition while keeping `movefocus right` working.

### 2.5 Where the XR block attaches to the physical monitors (a real subtlety)

Physical monitors have **no room pose** — they aren't in XR space — so we cannot place XR monitors
*spatially relative to* them. We can only auto-arrange XR monitors *among themselves* and then attach
that block to the physical layout at a **seam**. Options (config `openxr:layout2d:attach`):

- **`right` (default, matches today):** physical monitors keep their configured 2D arrangement; the XR
  block is translated to sit flush to the right of the physical bounding box, vertically centered (or
  aligned to the physical monitor the user's reference forward is "nearest" to). Least surprising.
- **`around`/`replace`:** in a headset-only session (no physical outputs, or all disabled), the XR block
  *is* the whole layout centered at origin. This is the common HypXRland case (headless + XR quads).
- **`follow-primary`:** attach the XR block adjacent to the primary physical monitor on the side that
  matches the reference forward (if you face left of your desk, XR monitors extend leftward). Nice, but
  needs a notion of "the primary physical monitor's facing direction," which we don't have — defer.

Recommend `right` default with `around` auto-selected when there are no enabled physical monitors.

---

## 3. The frame problem (analysis + recommendation)

Monitors live in different anchor frames (`XRAnchor.hpp:19-24`):

- **`LOCAL` (world):** fixed in LOCAL_FLOOR. Its azimuth *relative to the user* changes as the user
  turns/walks. If we computed azimuth from the *instantaneous* head yaw, turning your chair 30° would
  rotate every world monitor's `az` by 30° and could reorder columns — the 2D layout would churn
  constantly and the mouse mapping would become non-deterministic. **Wrong.**
- **`HEAD`:** fixed in VIEW space (`state().anchorPose` is a view-space offset). Its angle relative to
  the head is *constant by construction* — a HUD monitor is "always 20° up-left" no matter where you
  look.
- **`BODY`:** fixed in a yaw-only body frame; angle relative to the torso is constant (modulo the body
  yaw hysteresis).
- **`DEVICE`:** locked to a controller grip; transient, hand-held. **Excluded from 2D layout** (a
  controller palette is not a desktop monitor; matches SteamVR overlay-docking intuition and
  research/08's exclusion of `device`).

### Recommendation: latched reference frame for world monitors + follow frame for head/body, merged; freeze-on-event

**(a) Stable reference for world monitors.** Capture a **reference frame** = the LOCAL_FLOOR forward
(yaw `θ₀`) and eye position `e` at a well-defined moment: session-focused, the last `center`/recenter
verb, or an explicit `sync-layout`. Call it the *desk orientation*. Compute world monitors' `az` relative
to `θ₀`, not to the live head yaw. Turning your head then does **not** move world monitors in the 2D
plane — which is correct, because the mouse doesn't care where you're looking.

**(b) Follow frame for head/body.** Compute `head`/`body` monitors' `az`/`el` directly from their
persistent offset (`state().anchorPose`), which is already expressed in their (stable) follow frame. No
head-motion dependence.

**Merge onto one plane.** Place world monitors (angles in the desk frame) and head/body monitors (angles
in their follow frame) on the *same* azimuth/elevation axis. **Does this hold?** Yes, with a precise
justification: the 2D map represents the arrangement *as seen from the reference pose*. When the user
faces `θ₀` (the desk forward), the desk frame and the view frame **coincide**, so a head-anchored
monitor's follow-frame angle equals its world azimuth *at that instant* — the two sets are drawn in the
same coordinates they'd occupy when you look forward. The user experiences head-anchored monitors as
"always there" and world monitors as "over there when I face forward"; placing both by their
reference-pose angles matches the unified mental picture. When the user turns, the head-anchored monitors
physically swing with them and the world ones stay put — but because we **freeze-on-event** (below), the
*2D layout* (and thus the mouse mapping) does not move. The mouse mapping is a stable contract, not a
live readout of head pose. This is the right behaviour: you don't want your mouse to cross to a different
monitor just because you glanced away.

**(c) Freeze-on-change.** Recompute the projection only on discrete events (§4), never per-frame. This is
what makes (a)+(b) coherent: the layout is a snapshot taken at reference-pose moments, held steady until
a monitor is added/removed/moved. Combine (a) and (b) with (c) — that is the recommendation.

Edge cases to encode:
- **No valid view yet** (`viewValid == false`, tracking not up): fall back to today's auto-append-right,
  or to the last good reference frame if one was ever captured.
- **All monitors head/body** (headset HUD only): reference frame irrelevant; use follow-frame angles
  directly.
- **Mixed with grabbed monitors:** a monitor mid-grab is excluded from the compute set and keeps its
  last layout slot until release (§4).

---

## 4. Update dynamics (when to recompute)

Funnel everything through one debounced main-thread `COpenXRManager::syncLayout2D()` (modeled on
`reconcileDeclaredMonitors`, coalesced through `g_pEventLoopManager->doLater` like
`CMonitorLayoutController::scheduleRecheck`). It reads each layer's `lastWorld()`/`state()` under the
layer mutex, runs the pure projection+compaction (WP-S1), writes each XR monitor's
`m_activeMonitorRule.m_offset`, and calls `scheduleRecheck()` so the existing `arrange()` pipeline places
them (and emits `monitor.layoutChanged` for bars).

**Recompute triggers:**
- **`xrmonitoradded` / `xrmonitorremoved`** — set changed.
- **Grab RELEASE** — the `xrmonitorgrab` socket2 event already fires with a release marker (`",0"`,
  `OpenXRManager.cpp:463-468`). Recompute on the `,0` edge only, **debounced ~300 ms**, and **never on
  the `,1` (grab-begin) edge** (no churn mid-drag).
- **`center` / recenter** — re-latch the reference frame (§3a) and recompute (this is the natural "re-sync
  my layout to how things are now" moment; parallels Virtual Desktop's "auto-arrange on recenter").
- **`distance` / `rotate` / `move` verbs** — they change a monitor's world pose ⇒ recompute (debounced).
- **Explicit `hyprctl openxr sync-layout`** (+ `xrmonitor sync` dispatcher) — force a recompute now.
- **`config.reloaded`** — re-inject offsets after a reload wipes them (§1.2).
- **NEVER per-frame, NEVER mid-grab.**

**Hysteresis (anti-churn).** Keep the previous column/row assignment sticky: only reorder two monitors
when their center azimuths cross by more than a margin (e.g. `> 0.5·min(their angular widths)` or a
config `reorder_hysteresis_deg`), and only re-tier when an elevation crosses a row boundary by more than
`row_merge_deg`. Small nudges (a few degrees) then leave the layout identical, so a slightly bumped quad
doesn't reshuffle the mouse mapping. Combined with the 300 ms debounce, a burst of grab-releases/verb
calls produces exactly one relayout.

**Liveness/graceful-move check (verified §1.3):** a recompute that moves monitors is safe — floating
windows follow, tiled windows relayout, workspaces stay, cursor re-clamps. The only user-visible cost is
a possible small cursor nudge and a relayout flash, which the debounce + hysteresis minimize. Optionally
animate nothing (2D layout is instantaneous); the XR quads don't move (their 3D pose is the *source*, not
the *result*), so there's no visual motion in the headset — only the invisible 2D plane changes.

**Per-monitor opt-out.** A monitor with an explicit user `monitor = XR-1, …, XxY, 1` rule (non-sentinel
offset) or a `layout2d:manual` anchor-spec token is **excluded** — the sync engine treats its box as
fixed and compacts the auto ones around it (same "pinned" idea as research/08 §4.3). This lets a user pin
one monitor and auto-flow the rest.

---

## 5. Config / IPC surface sketch

`openxr:layout2d:*` config vars (declare in `ConfigValues.cpp` per the config-system rule):

| Name | Type | Default | Description |
|---|---|---|---|
| `openxr:layout2d:enabled` | Bool | `true` (open Q) | derive the 2D layout from XR poses (off = today's auto-append-right) |
| `openxr:layout2d:px_per_degree` | Float | `35` | angular→pixel scale for center placement (§2.3) |
| `openxr:layout2d:vertical` | String | `elevation` | `elevation` (angular) \| `world_height` (metric) row placement (§2.2) |
| `openxr:layout2d:attach` | String | `right` | seam vs physical block: `right` \| `around` \| `follow-primary` (§2.5) |
| `openxr:layout2d:min_overlap_px` | Int | `64` | min perpendicular overlap forced between neighbours (§2.4) |
| `openxr:layout2d:row_merge_deg` | Float | `10` | elevation window within which monitors share a row |
| `openxr:layout2d:reorder_hysteresis_deg` | Float | `4` | azimuth cross margin before columns reorder (§4) |
| `openxr:layout2d:debounce_ms` | Int | `300` | coalescing window for recompute triggers |

Anchor-spec opt-out token (extends doc 05 §2.2 grammar): `xrmonitor = XR-pin, 1920x1080, anchor:local …
layout2d:manual` ⇒ excluded from auto-sync.

Dispatcher / `hyprctl openxr` verbs (thin shims → `COpenXRManager`, one impl, two transports; slot into
the `XRIpc.cpp:120-160` verb block):
- `openxr sync-layout` — force `syncLayout2D()` now.
- `openxr sync-layout freeze|thaw` — pause/resume auto-recompute (a user tuning quads without churn).
- `xrmonitor sync` — dispatcher alias.

Events (for bars / external tooling; mirror the existing `xrmonitor*` events):
- `monitor.layoutChanged` already fires from `arrange()` — bars can rely on it as today.
- New `xrlayout2dsync` (`<n> monitors placed`) socket2 event on each recompute, so a bar can re-read
  `hyprctl monitors` after a spatial resync. `hyprctl openxr status -j` gains a `layout2d` block per
  monitor: `{ col, row, x, y, az_deg, el_deg, source: auto|manual|slot }`.

---

## 6. Prior art

- **Immersed** does the *manual inverse* of this feature: on first run it "will try its best to match"
  your OS display settings to the VR arrangement, and exposes **Settings → Arrangement Settings** where
  you *manually* drag numbered boxes so the VR arrangement matches the OS display layout — because "the
  actual movement of the mouse is governed by your display settings," not by where the screens float. So
  Immersed users hand-maintain the very mapping this doc automates. **HypXRland auto-deriving the 2D
  plane from the 3D poses is the differentiator** — no manual Arrangement Settings, it just tracks.
  ([immersed.com/faq](https://immersed.com/faq),
  [inairspace.com](https://inairspace.com/blogs/learn-with-inair/how-to-switch-monitors-in-virtual-desktop-a-complete-guide-to-seamless-multi-monitor-control))
- **Virtual Desktop / general OS multi-monitor:** the universal complaint is the cursor "gets stuck" or
  hits an "invisible wall" between screens because *the virtual layout doesn't match the physical
  placement* — the OS "only lets it cross where the virtual monitor edges touch," and any vertical
  offset creates a wall. The fix everyone gives: "drag the monitor rectangles until they touch along the
  edge where you want the cursor to cross." That is precisely Hyprland's `STICKS` + perpendicular-overlap
  rule (§1.4) surfacing as a product-level truth, and precisely what the compaction pass (§2.4) enforces
  automatically. ([ktcplay](https://us.ktcplay.com/blogs/support-tips/mouse-cursor-stuck-between-monitors),
  [ktcplay fix](https://us.ktcplay.com/blogs/support-tips/fix-mouse-cursor-stuck-between-monitors),
  [ittrip](https://en.ittrip.xyz/windows11/win11-mouse-monitor-fix))
- **GNOME (mutter) / KDE (KScreen):** OS display arrangers **snap dragged monitor rectangles to adjacent
  edges** and **normalize away gaps/overlaps** (mutter rejects gapped configurations; KDE has explicit
  screen-edge snapping in display config). The §2.4 compaction pass is a headless re-implementation of
  the same "preserve the drag intent, but land on a gap-free edge-touching layout" behaviour.
  ([discuss.kde.org](https://discuss.kde.org/t/help-with-precise-monitor-alignment/31718))
- **Meta Horizon Workrooms / Quest Remote Desktop:** desk-anchored primary + up to two satellites
  auto-placed left/right, roughly co-planar (research/08 §2). When monitors are already in a
  left/right/co-planar arrangement, the 2D projection is trivial — reinforces the arc-slot fast path
  (§7).

---

## 7. Intersections with research/08 and /11

- **research/08 (XR-space auto-layout) — the fast path.** If monitors are placed by 08 into arc/grid
  **slots**, the 2D layout falls out of the slot indices with *no projection and no compaction needed*:
  slot `(col,row)` → layout position `(Σ widths of columns left of col, Σ heights of rows above row)`,
  native px, edge-touching by construction. **Spec the fast path:** when a monitor belongs to an 08
  layout, `syncLayout2D()` reads its `(col,row)` directly and skips §2.2–2.4 for that monitor (falls
  back to angular projection only for freeform monitors not in any 08 layout). This makes 08 + 12 a
  clean pair: 08 arranges 3D into slots, 12 mirrors slots to 2D exactly. It also means the *general*
  angular-unwrap path (§2) is really only needed for **freeform** monitors the user hand-placed in 3D —
  which is exactly the case the user described ("as I'd expect if I manually arranged them").
- **research/11 (dynamic monitor naming) — non-blocking.** XR monitor names are stable and unique
  (`createXRMonitor` enforces uniqueness, `OpenXRManager.cpp:1198-1206`), so layout persistence and the
  per-monitor opt-out key off the name with no dependency on 11.

---

## 8. Work-package sketch

**WP-S1 — pure projection + normalization math · M · no deps.**
New `src/openxr/XRLayout2D.hpp/.cpp`, compiled **unconditionally** (no `HAVE_OPENXR`), pure like
`XRAnchor`/`XRMath`: `(monitors: {worldOrViewPose, pxW, pxH, widthMeters, mode, opt-out}, refFrame:
{θ₀, eye, valid}, cfg) → per-monitor {col,row, x, y}`. Implements §2.2 unwrap, §2.4 tier/column/compaction
with `min_overlap_px` and `STICKS`-exact seams, §4 hysteresis (previous-assignment stickiness), the §2.5
attach-seam translate, and the §7 slot fast path. Fully gtest-covered (place two monitors side-by-side →
edges stick; upper-right monitor → right + raised y with overlap; churn test → hysteresis holds order;
slot input → exact indices). **Touches:** `XRLayout2D.*` (new), `tests/xr/`.

**WP-S2 — wiring, reference frame, events, verb · M · dep: WP-S1 (and shares the fast path with
research/08 if that lands).**
Capture/re-latch the reference frame on session-focused / `center` / `sync-layout`; `COpenXRManager::
syncLayout2D()` debounced funnel (doLater) invoked from the §4 triggers (incl. `xrmonitorgrab` `,0`
edge and `config.reloaded`); write `m_activeMonitorRule.m_offset` + `scheduleRecheck()`; `openxr
sync-layout[ freeze|thaw]` verb + `xrmonitor sync`; `openxr:layout2d:*` config vars (with the
`parseKeyword` special-case if any must hot-toggle under the legacy parser, per the config-system note);
per-monitor `layout2d:manual` opt-out; `layout2d` block in `status -j` + `xrlayout2dsync` event.
**Touches:** `OpenXRManager.{hpp,cpp}`, `XRIpc.cpp`, `XRMonitorConfig.{hpp,cpp}` (opt-out token),
`config/legacy/ConfigManager.cpp` (keyword hot-toggle if needed), `config/values/ConfigValues.cpp`,
`hyprtester/src/tests/xr/`.

---

## 9. Open questions for the user

1. **Default-on?** `openxr:layout2d:enabled` default **on** (intuitive, the feature's whole value) or
   **off** (today's auto-append-right; auto is opt-in)? On changes existing behaviour but is what the ask
   describes; recommend **on**, flagged here.
2. **Vertical mapping taste:** **elevation angle** (recommended, clean spherical unwrap, one constant) or
   **world-height** (metric, better when radii differ a lot)?
3. **`px_per_degree` default** — `35` (≈ a 1080p reference monitor at default distance/size). Since
   compaction is authoritative for adjacency, the exact value mostly affects pre-compaction spacing feel.
   Acceptable?
4. **Recompute on grab-release automatically** (debounced, recommended — "I moved it, the mouse mapping
   follows") or **only on explicit `sync-layout`** (fully manual, zero surprise)?
5. **Attach seam** — XR block flush **right** of the physical monitors by default (matches today), or a
   `follow-primary`/`around` policy? And in a headset-only session, confirm `around` (XR block is the
   whole layout).
6. **Physical monitors** — confirm they keep their configured 2D arrangement untouched and only XR
   monitors are auto-placed (we have no room pose for physical outputs, so we can't spatially interleave
   them). Or is interleaving (drag a physical monitor's box into the XR block) ever wanted?
7. **Interaction with research/08** — if 08 lands first, is 12 purely the slot→2D fast path (§7) plus the
   freeform angular fallback, or should the angular unwrap be the primary path regardless?
