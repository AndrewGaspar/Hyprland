# Research — Auto Monitor Layout in XR Space

Design research for **automatic spatial placement** of HypXRland virtual monitors: instead of
requiring an explicit pose for every `xrmonitor`, let the compositor *flow* monitors into a sensible
arrangement — a tiling layout manager, but for XR space — with **distinct policies per anchor class**
(world/`local`-anchored monitors arrange around a room-fixed focal set; `head`/`body`-anchored ones
arrange within the follow-frame without colliding with each other).

Status: **research / design only. Nothing here is implemented.** No live runs, no code changes. Naming
is provisional and defers to the authoritative docs (`03-anchoring.md`, `05-ipc-config.md`,
`02-virtual-monitors.md`) and to the untriaged grids research (`research/03-monitor-grids.md`) where it
overlaps.

---

## TL;DR

1. **Auto-layout is a placement *policy* that runs on top of the existing anchor engine and (mostly)
   on top of the grids design.** A "layout" owns a shape (arc/cylinder row, tier stack, focal cluster,
   desk plane, HUD corners) and an *assignment rule* (which monitor lands in which slot); the anchor
   engine still does the per-frame pose math, and the grids' **shared parent transform** is what makes
   a follow-*cluster* move as one rigid unit. Auto-layout ≈ "grids/03 + an auto-assignment planner +
   per-anchor-class defaults." It **subsumes** the manual `xrgrid ... cell:c,r` addressing (you can
   still pin a cell) and **composes with** it (auto is the default assignment; manual cell is a pin).

2. **The head/body follow-cluster is the piece that does *not* exist yet.** Today each monitor leashes
   independently (`CXRAnchor` per `CXRMonitorLayer`, `03-anchoring.md` §3.2–3.3) — three head-anchored
   monitors are three springs that overlap and fight. A follow-cluster needs **one shared leash parent**
   solved once per frame, with monitors hung off it at fixed angular slots — this is *exactly*
   research/03's "grid owns a `CXRAnchor`, solve once, compose children" (§2.3). **Make this dependency
   explicit: the head/body auto-layout is blocked on the grids parent-transform work (WP-G2).**

3. **Recommended shape: per-anchor-class layout engines with one shared "slotting" core.** World/`local`
   monitors flow onto a **named world arc/cylinder** (arc row is the productivity default — every VR
   incumbent converges on a curved dock). Head/body monitors flow into a **follow-cluster** with named
   angular slots (`center` / `left-wing` / `right-wing` / `above` / `below`) that maintain angular
   separation in the view/body frame. `device`-anchored monitors are **never auto-laid-out** (a
   controller palette is intentionally hand-placed).

4. **Placement-on-create = "nearest gap to gaze, else next free slot."** A new monitor with no pose
   goes to the free slot closest to where the user is currently looking (matches visionOS gaze-driven
   placement and "don't overlap"); reflow-on-destroy is **compact by default** (tiling instinct) with a
   `keep-gaps` opt-out. **Grabbing a monitor out of auto-layout pins it** (a per-monitor `auto|pinned`
   mode flag — exactly "floating a tiled window"); a `snap`/`retile` verb re-adopts it.

5. **Serialization is honest about auto vs pinned.** `hyprctl openxr layout` dumps auto-placed monitors
   as `layout:<name> slot:auto` (or their resolved `slot:<name>`/`cell:c,r` if pinned to one) plus the
   `xrlayout` definition line, **not** baked world poses — so a saved arrangement re-flows identically
   on the next machine/seat rather than freezing one session's spring positions. (Pinned-to-pose
   monitors still bake a pose, as today.)

---

## 1. Relationship to `research/03-monitor-grids.md` (read that first)

The grids research already designed the load-bearing runtime primitive this feature needs:

- **`CXRGrid` owns a `CXRAnchor`** for its origin, so a whole arrangement can itself be
  `local`/`head`/`body`/`device` (research/03 §2.1).
- **Two-phase frame solve** — solve each grid's origin *once*, then compose every child monitor against
  it (research/03 §2.3). This is the "rigid unit leash" that a follow-*cluster* fundamentally requires.
- **`XR_ANCHOR_GRID` mode** — "`XR_ANCHOR_LOCAL`, but relative to a parent frame the caller hands in"
  (research/03 §2.2), keeping `CXRAnchor::solve()` pure.
- **Flat/cylinder/sphere geometry** unified by per-axis radius (research/03 §3), with cell→pose math.

**Auto-layout is the assignment/flow layer that grids/03 deliberately left as manual `cell:c,r`.**
Grids/03 gives you *addressable slots you place monitors into by hand*; auto-layout adds *a planner that
picks the slot for you*, plus *per-anchor-class defaults* and *reflow on create/destroy/resize*. Two
ways to frame the relationship, pick one (Open Question 1):

- **(A) Auto-layout is a thin planner over grids.** Implement grids/03 first (WP-G1..G4); auto-layout is
  "grids + `slot:auto` assignment + a couple of curated presets + follow-cluster defaults." Least new
  concept, most code reuse. **Recommended.**
- **(B) Auto-layout is its own `xrlayout` object that *contains* a grid.** A layout is a higher-level
  thing (a named policy + preset) that *materializes* a grid under the hood. More surface, but lets the
  layout expose ergonomic presets (`arc`, `stack`, `focal`) without users thinking in cells.

This doc designs the **union**: an `xrlayout` object (ergonomic presets + auto-assignment + per-class
defaults) that is implemented as a grid (research/03's geometry + parent transform) with a planner on
top. Where a term here (cell, span, `XR_ANCHOR_GRID`, two-phase solve) is already defined in
research/03, this doc reuses it verbatim rather than redefining it.

---

## 2. Prior-art survey

Extends research/03 §1 (which surveyed the *multi-window spatial models*); this table focuses on the
**auto-placement / auto-arrange heuristics** specifically.

| System | Auto-placement heuristic | Takeaway for HypXRland |
|---|---|---|
| **Apple visionOS** | New windows spawn **in front of / slightly offset from** the opener, gaze-anchored; the system cascades/stacks additional windows to avoid full overlap; `defaultWindowPlacement` lets an app hint a relative slot (e.g. "below" a leader window) and pass a size so the system can **push to avoid overlap**. visionOS 26 added `SpatialContainer`/`Alignment3D` because pure freeform aged badly. ([developer.apple.com](https://developer.apple.com/documentation/visionos/positioning-and-sizing-windows), [stepinto.vision](https://stepinto.vision/example-code/how-to-use-default-placement-to-position-new-windows/), [eduardodevelops.com](https://eduardodevelops.com/how-place-a-second-window-under-other-window-with-visionos)) | **Gaze-relative spawn + overlap-avoidance + relative-slot hints** is the validated new-window model. Our "nearest gap to gaze, else next slot" (§4) is this. Freeform-only loses; add structure. |
| **Virtual Desktop** | Explicit **"auto-arrange monitors on recenter"** checkbox (on by default): recentering re-flows all monitors into the arrangement in front of you; a global **curve** slider; arrange in a curved arc / vertical stack / anywhere in 360°. ([inairspace.com](https://inairspace.com/blogs/learn-with-inair/how-to-move-vr-screen-the-ultimate-guide-to-customizing-your-virtual-space), [uploadvr.com](https://www.uploadvr.com/virtual-desktop-multiple-monitors-update/)) | **"Recenter = re-flow the whole arrangement in front of me"** is the killer verb (maps to `xrlayout recenter`, §5.4). Curvature is a first-class global knob, not per-monitor. |
| **Immersed** | Up to 5 monitors, **curved around you**, stacked vertically; per-display size/curvature/distance; a manual "Arrangement Settings" to match desktop topology; layout persisted. ([immersed.com/faq](https://immersed.com/faq), [inairspace.com](https://inairspace.com/blogs/learn-with-inair/virtual-reality-multiple-monitor-the-ultimate-productivity-and-immersion-setup)) | Curved-wrap + persistence is table-stakes (we have `hyprctl openxr layout`). Distance/curvature/size are the three arc knobs users expect. |
| **Meta Horizon Workrooms / Quest Remote Desktop** | **Desk-anchored** primary + up to 2 virtual monitors auto-placed to the **left/right of the primary**, roughly co-planar at desk height; magnetic snap + neighbor-nudge (research/03 §1). (Workrooms EOL Feb 2026; Quest Remote Desktop continues.) ([uploadvr.com](https://www.uploadvr.com/horizon-workrooms-multi-screen-office/), [meta.com](https://www.meta.com/help/quest/articles/horizon/getting-started-in-horizon-workrooms/adjust-virtual-desk-workrooms/)) | The **focal-primary + left/right satellites** archetype, anchored to a desk plane. Our `focal` preset and desk-plane class. |
| **SteamVR overlays / dashboard** | Overlays dock relative to the dashboard or a tracked device; `IVROverlay` transforms are absolute or device-relative; no auto-flow, but a strong **device-relative dock** notion. | Reinforces: `device`-class monitors are **hand-placed, not auto-flowed**. Auto-layout should skip them. |
| **HCI research** (DuoZone, Handows, "arrangement zones") | VR window managers adopt **default layouts constrained by angular + distance thresholds** to cut head motion and fatigue; "arrangement zones" are translucent tiled-window templates (6 OS-style layout templates) that distribute space within a zone; head-gaze selects the active window. ([arxiv 2511.15676](https://arxiv.org/pdf/2511.15676), [arxiv 2508.09469](https://arxiv.org/pdf/2508.09469), [arxiv 2511.17516](https://arxiv.org/pdf/2511.17516)) | Validates **angular-separation constraints** as the core comfort metric and **named layout templates** as the interaction model — precisely the "master/stack on a sphere" question. |
| **Hyprland itself** (target audience) | `dwindle`/`master` tiling; `movefocus`/`movewindow`; `workspace` as a named relocatable container. | The idioms to imitate: a **named layout** (like a workspace) + **directional move/focus** + **master/stack semantics**. This is the mental model, not the incumbents' pointer-drag UX. |

**Synthesis.** Every incumbent converges on **a curved dock, arranged in front of you, re-flowed on
recenter, with gaze-relative new-window placement and overlap avoidance** — but all drive it by
hand/gaze pointer and none expose it as a *named, keyboard-driven, config-persisted, per-anchor-class*
object. That named + keyboard + per-class identity is HypXRland's differentiator (same thesis as
research/03, extended to auto-assignment). The academic work adds the one hard constraint everyone
respects: **maintain angular separation; minimize head motion.**

### What "master/stack" / "dwindle" mean on a sphere (the tiling analogy)

The user's phrasing invites a direct analogy. Mapping tiling layouts onto an arc/sphere:

- **Arc-row (≈ `master` with all-in-one-row, or i3 default split-h):** monitors laid left→right along a
  horizontal arc at a fixed radius; the "master" is the center slot (largest angular width / at gaze
  center), stack fans out to the wings. Growing the master = widening its angular span, pushing wings
  outward. This is the **recommended default** — it is what Immersed/VD/Workrooms all are.
- **Tier-stack (≈ vertical splits / `master` with stacked slaves):** rows of monitors at different
  pitch tiers (eye level, below for reference material, above for monitoring). Comfort research says
  **vertical curvature is disorienting** (research/03 §3.3) — keep tiers as flat pitch offsets on a
  cylinder, not a full sphere.
- **Focal-primary + satellites (≈ `master`/dwindle with a dominant leader):** one large primary at
  gaze center + smaller satellites orbiting at wider angles / shorter radius. The Workrooms model.
- **Dwindle on a sphere:** recursive binary angular subdivision of the arc (first monitor takes the
  whole arc, second splits it, third splits the larger half…). Cute, but **angular dwindle produces
  uncomfortable tiny wedges fast** and has no natural "resize" gesture in XR. *Not recommended as a
  default*; mention as a possible `layout:dwindle` novelty only.

The clean takeaway: **"master/stack" maps to angular width allocation along an arc; "the master is the
gaze-center slot."** That is the tiling idiom worth shipping.

---

## 3. Design options

Four options along an **"how much structure / how automatic"** axis. All four reuse the anchor engine;
the later ones reuse progressively more of the grids/03 runtime.

### Option 1 — "Presets only" (no runtime layout object) · smallest

Add a handful of **one-shot placement verbs** that compute poses *at call time* and write them into each
monitor's existing `local`/`head`/`body` anchor — no persistent layout object, no parent transform.

- `xrmonitor arrange arc [radius:R] [count-driven spacing]` walks the current XR monitors, computes an
  arc of poses in front of the current gaze, and calls the existing `applyCenter`-style math per monitor.
- Per-anchor-class handled trivially: `arrange` for `local` monitors writes world poses; for `head`
  monitors it writes view-space offsets spread across an angular fan (they still leash *independently*,
  so they can drift into each other — accepted limitation of this option).
- **Pros:** tiny; no grids dependency; ships in one WP; immediately useful ("tidy my monitors").
- **Cons:** *not* auto — it's a manual "re-tidy now" button. No reflow-on-create/destroy. Head/body
  monitors still fight (no shared parent). No persistence beyond baked poses. Doesn't satisfy the "like
  a tiling layout manager" ask; it's the "auto-arrange on recenter *button*" only.
- **Verdict:** ship a *subset* of this early as the `arrange`/`recenter` verb regardless of which option
  wins — it's the low-risk down payment — but it is not the whole feature.

### Option 2 — World-class auto-layout via grids; head/body stays manual · medium

Implement the grids/03 runtime (parent transform + `XR_ANCHOR_GRID` + two-phase solve) and a **planner
for `local`/world monitors only**: a named world arc/cylinder that auto-assigns new monitors to the
next free slot and reflows on destroy. Head/body monitors keep today's independent per-monitor leash
(explicit offsets), no cluster.

- **Pros:** delivers the highest-value case (the productivity monitor wall) on top of a runtime we
  already want (grids). Bounded — head/body cluster (the genuinely new math) is deferred.
- **Cons:** head/body monitors — the ones the user explicitly called out — don't get auto-layout. The
  per-class-policy ask is only half-answered.
- **Verdict:** a good *milestone*, not the endpoint. Effectively "auto-layout = grids/03 + a planner."

### Option 3 — Per-anchor-class layout engines over a shared slotting core · **recommended**

A first-class **`xrlayout` object** (name + preset shape + anchor class + auto-assignment) with **two
concrete layout engines that share one "slotting" core**:

- **World layout** (`anchor:local`): the layout's origin is a fixed `local` anchor (a room-fixed focal
  point); monitors flow onto an **arc/cylinder row** (or tier stack / focal preset) around it. This is
  grids/03 with `slot:auto` assignment.
- **Follow-cluster layout** (`anchor:head` or `anchor:body`): the layout's origin is a `head`/`body`
  anchor (**one shared leash**, solved once per frame — the grids parent transform); monitors flow into
  **named angular slots** within the follow-frame (`center`/`left-wing`/`right-wing`/`above`/`below`,
  or a continuous arc fan) that **guarantee minimum angular separation** so they never overlap and the
  whole cluster moves rigidly with the head/torso. `device` class is excluded (hand-placed).

The **shared slotting core** is the planner: given N monitors and a shape (arc geometry from grids/03 or
a slot list), assign each to a slot (§4 rules), compute each child's grid-local pose (grids/03 §3
`cellLocalPose`), and let the two-phase solve compose against the layout origin.

- **Pros:** directly answers the ask — *distinct policies per anchor class*, both automatic, both
  collision-free-by-construction (slots are pre-separated). Reuses grids/03 wholesale; the only genuinely
  new thing is the follow-cluster slot model and the auto-assignment planner. Named + keyboard +
  persisted (the differentiator). Composes with manual pins and grabs (§6).
- **Cons:** biggest surface of the four; depends on the grids/03 parent-transform work landing first.
  Two engines to tune (world arc vs follow slots have different comfort feels).
- **Verdict:** **the recommendation.** It is the smallest design that actually delivers "per-class
  automatic layout," and it's mostly *assembly* of grids/03 + a planner rather than new pose math.

### Option 4 — Fully automatic "spatial WM" (constraint solver / packing) · largest, not recommended now

Treat XR space as a continuous canvas and run a real layout solver: pack monitors by angular width,
push-to-avoid-overlap (visionOS-style), auto-choose curvature from count, golden-angle spiral for
overflow, animate reflow. No fixed slots — positions are solved each time the set changes.

- **Pros:** most "magical"; handles arbitrary counts/sizes gracefully; the true tiling-WM-on-a-sphere.
- **Cons:** hardest to make *comfortable* and *predictable* (users hate windows that move themselves);
  hardest to make *addressable* ("focus the left one" is ill-defined mid-reflow); most code; most
  tuning; reflow animation is a whole subsystem. Overkill for v1.
- **Verdict:** a north star. Option 3's slot model is the 80/20; keep the continuous-packing planner
  (research/03 Option B "pack") as an opt-in `assign:pack` inside Option 3, not a separate mode.

### Recommendation

**Ship Option 3, staged behind Option 1's down payment and Option 2's milestone:**

1. **Down payment (Option 1 subset):** an `arrange`/`recenter` verb that re-tidies existing monitors —
   immediately useful, no grids dependency.
2. **Milestone (Option 2):** world arc auto-layout once grids/03 runtime exists.
3. **Full (Option 3):** add the follow-cluster engine + per-class defaults + auto-assignment planner.

This yields one coherent story: *a named layout per anchor class; monitors auto-flow into angular slots;
grab to pin, snap to re-adopt; recenter to re-flow in front of you; save/restore via `openxr layout`.*

---

## 4. Placement-on-create & reflow semantics

### 4.1 Where a new pose-less monitor goes

When a monitor is created with **no anchor-spec** (dispatcher `xrmonitor create NAME` with no anchor;
today doc 05 §3.1 places it `default_distance` along gaze) **and an auto-layout is active for the
default class**, the planner picks a slot:

- **Primary rule — nearest gap to gaze.** Compute the angular slot whose direction is closest to the
  current view forward *and* is unoccupied. Matches visionOS "spawn where you're looking, don't
  overlap." Requires `viewValid` (the `SXRVerbContext`, `03-anchoring.md` §5); if invalid, fall back to:
- **Fallback — next free slot in traversal order.** Center-out for arc rows (fill center, then
  alternate right/left wings), top-down for tier stacks. Deterministic, gaze-independent.
- **Overflow — grow the shape, don't overlap.** When all slots are full: widen the arc by adding a slot
  (re-spacing to keep angular separation), or (opt-in `assign:pack`) start a **golden-angle spiral** /
  second tier. Never place two monitors in the same slot.

### 4.2 Reflow on destroy / resize

- **Destroy → compact by default** (tiling instinct): downstream slots shift up to close the gap,
  animated via the existing spring seed so they glide (reuse the HEAD spring, research/03 §6.1). A
  `reflow:keep-gaps` layout flag leaves holes (matches "my monitors stay where I put them").
- **Resize (span/scale) → re-space neighbors.** In a `span` lattice, growing a monitor's angular width
  pushes neighbors outward (research/03 Option A). In `pack`, neighbors slide (research/03 Option B).
- **Recompute triggers:** monitor add/remove, mode/size change, `span`/`cell` change, layout geometry
  change (`radius`/`spacing`/`curve`), and `recenter`. Cheap + deterministic (grids/03 §2.3 caches
  cell-local poses behind a dirty flag) — recompute only the dirty layer set under the layer mutex.

### 4.3 The `auto` vs `pinned` per-monitor mode flag (the "float a tiled window" concept)

Each monitor that belongs to a layout carries a **slotting mode**, analogous to floating vs tiled:

```
enum eXRSlotMode { XR_SLOT_AUTO,   // planner owns this monitor's slot; it reflows with the set
                   XR_SLOT_PINNED   // user fixed it to an explicit cell/slot; planner routes around it
                 };
```

- **`AUTO`** — the planner assigns and reflows the monitor freely.
- **`PINNED`** — the monitor holds a specific `cell:c,r`/`slot:name`; the planner treats that slot as
  occupied and flows other `AUTO` monitors around it. (This is research/03's explicit `cell:` addressing,
  reframed as "pinned within an auto layout.")
- **Grabbing an `AUTO` monitor promotes it to a third state — detached-to-manual** (leaves the layout,
  becomes freeform `local` at its grabbed pose), exactly like floating a tiled window by dragging it
  out. `xrmonitor snap`/`retile` re-adopts it into the nearest free slot as `AUTO` (or `PINNED` if it
  landed on a specific cell). See §6.

---

## 5. Coexistence with grabs & manual poses; recompute triggers

### 5.1 Three per-monitor placement states

1. **Auto-laid-out** (`AUTO` in a layout) — planner owns the pose.
2. **Pinned-in-layout** (`PINNED` slot/cell) — user owns the slot; planner routes around it.
3. **Freeform** (no layout; today's `local`/`head`/`body`/`device` with an explicit pose/offset) — fully
   manual, the current behavior. Grabbing an auto/pinned monitor *out* transitions it here.

Transitions (verbs, §6): `AUTO ⇄ PINNED` (`pin`/`unpin` or `cell`), `AUTO/PINNED → FREEFORM` (grab-out or
`detach`), `FREEFORM → AUTO/PINNED` (`snap`/`retile`/`join`).

### 5.2 Grab interplay

The grab machine (`04-input.md`, research/04-grabbable-borders) already device-locks a grabbed monitor
and re-anchors on release. Two release policies, chosen by the grabbed monitor's state:

- **Auto/pinned monitor grabbed → on release, snap to nearest free slot** (research/03 §6.1's
  snap-to-nearest-cell), *staying in the layout*, unless the release pose is far outside the layout
  volume (past a threshold) → **detach to freeform** (float it out). This gives the pleasant "nudge it
  and it clicks back / yank it out and it stays out" feel (Quest magnetic snap).
- **Freeform monitor grabbed → re-anchor to its persistent mode** (today's behavior, unchanged).

Grabbing the **whole layout** reuses research/03 §6.2 (grab any member with a modifier, or a rendered
grab handle) driving the layout origin's `CXRAnchor` — the entire arc/cluster moves rigidly.

### 5.3 Recompute triggers (single funnel)

All of: monitor add/remove; mode/size change; slot-mode change (`pin`/`unpin`/`detach`/`snap`); layout
geometry change; `recenter`; anchor-class change of the layout origin. Funnel through one
`COpenXRManager::reflowLayout(name)` on the main thread under the layer mutex (like
`reconcileDeclaredMonitors`, doc 05 §2.5), which recomputes the dirty children's grid-local poses and
seeds their springs to glide.

### 5.4 `recenter` — the "auto-arrange on recenter" verb (Virtual Desktop parity)

`xrlayout recenter <name|active>` moves the layout origin to `default_distance` along current gaze (arc)
or captures the current head/body frame (cluster), then reflows all `AUTO` members in front of the user.
This is the single most-requested VR verb (Virtual Desktop ships it on by default) and maps cleanly to
research/03 §6.3's `xrgrid recenter [grab]` (keyboard place + optional hand fine-tune).

---

## 6. Config / dispatcher / IPC surface sketch

Follows doc 05 conventions (comma-separated top-level fields; space-separated `key:value` sub-tokens;
classic-hyprlang v1) and extends research/03 §5. Two namespaces to decide between (Open Question 7):
an `xrlayout` keyword (recommended — ergonomic presets) vs. reusing research/03's `xrgrid` keyword with
an added `assign:auto`. This sketch uses `xrlayout` as the ergonomic front and treats it as
"an `xrgrid` with a preset + auto-assignment."

### 6.1 `openxr:layout:*` config vars (per-class defaults; declare in `ConfigValues.cpp` per doc 05 §1)

| Name | Type | Default | Description |
|---|---|---|---|
| `openxr:layout:auto` | Bool | `false` | auto-place new pose-less XR monitors into the default layout for their anchor class (off = today's single-gaze placement) |
| `openxr:layout:default_shape` | String | `arc` | default preset for auto layouts: `arc` \| `cylinder` \| `stack` \| `focal` \| `cluster` |
| `openxr:layout:arc_radius` | Float | `1.8` | arc/cylinder radius (m) for world layouts |
| `openxr:layout:arc_spacing` | Float | `0.05` | inter-monitor gap (m) along the arc |
| `openxr:layout:curve` | Float | `1.0` | 0 = flat wall, 1 = follow the arc radius (Immersed/VD "curve" slider); interpolates between plane and cylinder facing |
| `openxr:layout:cluster_separation` | Float | `18.0` | minimum angular separation (deg) between follow-cluster monitors |
| `openxr:layout:reflow` | String | `compact` | `compact` (close gaps on destroy) \| `keep-gaps` |
| `openxr:layout:recenter_on_start` | Bool | `false` | reflow all layouts in front of the user when the session reaches FOCUSED |

(Reload behavior per doc 05 §1.2: geometry vars are "hot-live" re-read on reflow; `auto`/`default_shape`
affect subsequently created monitors.)

### 6.2 `xrlayout` keyword (new) — one line defines an auto layout

```
xrlayout = <name>, <preset-spec>, <anchor-spec>

<preset-spec> ::= ("arc"|"cylinder"|"stack"|"focal"|"cluster")
                  [SP "radius:" <m>] [SP "spacing:" <m>] [SP "curve:" <0..1>]
                  [SP "separation:" <deg>] [SP "assign:" ("slot"|"pack"|"span")]
                  [SP "reflow:" ("compact"|"keep-gaps")]
<anchor-spec> ::= exactly the xrmonitor anchor-spec grammar (doc 05 §2.2): anchor:local|head|body
```

```ini
# world-fixed curved productivity wall: 1.8 m arc, auto-flow new monitors into it
xrlayout = wall, arc radius:1.8 spacing:0.05 curve:1.0, anchor:local pos:0,1.35,-0.2 yaw:0

# head-leashed HUD cluster: 5 angular slots, 18° apart, follows gaze as one rigid unit
xrlayout = hud, cluster separation:18 radius:0.9, anchor:head offset:0,0,0

# body-leashed shelf that follows your torso, native-size packed
xrlayout = shelf, cylinder radius:1.2 assign:pack, anchor:body offset:0,1.3,0
```

A monitor joins a layout (auto or pinned) via its anchor-spec (extends doc 05 §2.2 / research/03 §5.2):

```
<anchor-spec> ::= … existing … | "layout:" <name> [SP "slot:" ("auto"|<slotName>|<col>","<row>)]
```

```ini
xrmonitor = XR-code,  2560x1440@90, layout:wall slot:auto          # planner picks the slot
xrmonitor = XR-term,  1920x1080,    layout:wall slot:1,0           # pinned to a cell
xrmonitor = XR-chat,  1280x720,     layout:hud  slot:right-wing    # pinned to a named cluster slot
```

### 6.3 Dispatchers / `hyprctl openxr` verbs (thin shims → `COpenXRManager`, per doc 05 §3/§4)

| Dispatcher | Verb | Args | Semantics |
|---|---|---|---|
| `xrlayout` | `create` | `<name> [preset] [radius:…] [spacing:…] [curve:…] [separation:…] [anchor-spec]` | Create a runtime layout (not reconciled). |
| `xrlayout` | `destroy` | `<name>` | Destroy layout; members **detach to freeform `local`** at current world pose (never destroy monitors — parallels doc 05 §2.5). |
| `xrlayout` | `recenter` | `<name\|active> [grab]` | §5.4 re-flow in front of gaze; `grab` opens a hand fine-tune (research/03 §6.3). |
| `xrlayout` | `set` | `<name> radius:… \| spacing:… \| curve:… \| separation:… \| reflow:…` | Retune geometry live; reflow members. |
| `xrlayout` | `anchor` | `<name> <local\|head\|body> [offset:…]` | Re-anchor the whole layout (switch its leash class). |
| `xrlayout` | `arrange` | `<name\|all>` | Force a reflow now (Option 1 down-payment; also the manual "tidy"). |
| `xrmonitor` | `join` | `<layout> [slot:…]` | Attach the selected freeform monitor to a layout (auto or pinned slot). |
| `xrmonitor` | `detach` | *(none)* | Leave the layout → freeform `local` at current world pose. |
| `xrmonitor` | `pin` / `unpin` | *(none)* | Freeze the selected monitor into its current slot (`PINNED`) / release it back to `AUTO`. |
| `xrmonitor` | `snap` / `retile` | *(none)* | Re-adopt a freeform/grabbed monitor into the nearest free slot of the selected layout. |
| `xrmonitor` | `focus` | `<right\|left\|up\|down>` | Move desktop focus to the neighbor-slot monitor (Hyprland `movefocus` idiom, research/03 §5.3). |
| `xrmonitor` | `cellmove`/`swap` | `<right\|left\|up\|down>` | Move/swap the selected monitor one slot (tiling `movewindow`). |

All also under `hyprctl openxr <same>` (one implementation, two transports).

### 6.4 IPC / events / serialization

- `hyprctl openxr status -j` gains a `layouts` array (name, preset, origin anchor, `members:[{name,
  slot, slotMode}]`) and each monitor's `slotMode: auto|pinned|freeform`.
- New socket2 events (mirroring `xrmonitor*`, doc 05 §5): `xrlayoutadded`, `xrlayoutremoved`,
  `xrlayoutreflow` (`<name>`), `xrmonitorslot` (`<name>,<slot>,<mode>`).
- **`hyprctl openxr layout` serialization (Open Question 6):** for a layout member, emit
  `layout:<name> slot:auto` (or `slot:<resolved>` if pinned) plus the `xrlayout = …` definition line
  **before** the member `xrmonitor` lines — **not** a baked world pose. Rationale: an auto layout's
  whole value is that it re-flows; baking one session's spring-settled poses would freeze it and defeat
  the point. Freeform monitors serialize a pose exactly as today (doc 03 §7). This is the one place
  auto-layout deliberately diverges from the current "bake live poses" persistence.

### 6.5 Example binds (Hyprland-idiomatic)

```ini
bind = SUPER,        G,     xrlayout,  create wall arc radius:1.8
bind = SUPER,        N,     xrmonitor, create              # auto-flows into the default layout
bind = SUPER,        R,     xrlayout,  recenter active     # re-flow in front of me (VD parity)
bind = SUPER SHIFT,  R,     xrlayout,  recenter active grab
bind = SUPER,        left,  xrmonitor, focus left
bind = SUPER SHIFT,  left,  xrmonitor, swap left
bind = SUPER,        P,     xrmonitor, pin                 # float-out equivalent: pin in place
bind = SUPER SHIFT,  P,     xrmonitor, detach              # yank out of the layout
bind = SUPER,        T,     xrmonitor, snap                # re-tile into nearest slot
bind = SUPER,        C,     xrlayout,  anchor active head  # leash the whole wall to my head
```

---

## 7. Feasibility against the current code

### What the anchor engine already gives (verified in `src/openxr/XRAnchor.{hpp,cpp}`)

- **Per-monitor `head`/`body` leash** with spring + deadzone + hysteresis is done and unit-tested
  (`CXRAnchor::solve`, `03-anchoring.md` §3.2–3.3). A follow-*cluster* needs the *same math applied once
  to a shared parent*, not new spring code — the spring is reusable as-is.
- **Re-anchor / re-express-into-mode** (`reanchorFromWorld`, `endGrab`, `setMode`, `03-anchoring.md`
  §4.4/§5.6) is exactly what `detach`/`join`/`snap`/`pin` need to move a monitor between freeform and
  slotted representations without a visual pop.
- **Grab machinery + release-latch ring** (`SXRGrabRing`, `pickReleasePose`, research/04) already exists;
  snap-to-slot-on-release is a *release policy branch*, not new grab code.
- **`applyCenter`/`applyDistance`/`applyScale`** give the arc-placement and per-slot sizing primitives;
  the Option 1 down-payment `arrange` verb is essentially "call `applyCenter`-style math per monitor."
- **Verb context capture** (`SXRVerbContext`, main-thread copy of last frame's poses) means the planner
  can read `view` for gaze-relative placement without touching the frame thread.

### What does NOT exist yet — the explicit dependencies

- **No shared parent transform / named layout object.** This is **the** gap. `CXRAnchor` is per-layer;
  there is no object that solves an origin once and composes children. **The head/body follow-cluster is
  blocked on research/03's `CXRGrid` + `XR_ANCHOR_GRID` + two-phase frame solve (research/03 §2.1–2.3,
  WP-G1/WP-G2).** World arc auto-layout is *also* cleanest on that runtime, though a degenerate
  Option-1 version can bypass it by writing independent `local` poses.
- **No auto-assignment planner.** New. Pure, testable: `(monitors, shape, occupancy, gazeDir) → slot
  assignment`. Belongs next to `XRGrid`/`XRMonitorConfig` as unconditional pure math (gtest-covered like
  `anchor_math`/`parser`).
- **No slot-mode flag on the layer.** `CXRMonitorLayer` needs `eXRSlotMode` + slot id (main thread +
  frame snapshot), living next to research/03's `SXRGridCell` (research/03 §2.1). Per the refcount rule
  (MEMORY: frame thread does zero hyprutils refcount ops), keep it plain-value in the snapshot.
- **Reflow funnel.** New `COpenXRManager::reflowLayout()` on the main thread under the layer mutex,
  modeled on `reconcileDeclaredMonitors()` (doc 05 §2.5).

### Threading & safety notes (from MEMORY)

- Planner + reflow run **main-thread** under the layer mutex; the two-phase solve runs **frame-thread**
  reading a snapshot — never allocate/refcount across the boundary (the SP/WP refcount race, MEMORY
  2026-07-07). Slot ids and geometry cross as plain values, matching research/03 §2.3.
- `openxr:layout:*` hot-toggles under the **legacy hyprlang parser** need the same `parseKeyword`
  special-case as `openxr:enabled`/`openxr:inhibit_idle` (MEMORY; doc 05 §1.3) if any must apply without
  a full reload — or rely on the `xrlayout set`/`arrange` verbs (which don't go through the config path)
  for live tuning.

---

## 8. Implementation sketch — work packages (one-subagent-sized)

Sequenced; effort S/M/L relative to the WP1–13 / WP-G grain. **Auto-layout sits on top of the grids WPs
(research/03 §7); those are prerequisites for WP-L2+.**

### WP-L1 — `arrange`/`recenter` down-payment (Option 1 subset) · **S–M**
No grids dependency. `xrlayout arrange`/`xrmonitor arrange` verb that walks live XR monitors and writes
arc poses via existing `applyCenter`/`applyDistance` math; `openxr:layout:arc_radius`/`arc_spacing`
config. Pure arc-pose helper (`arcSlotPose(index, count, radius, spacing, gazeFwd)`) in a new
`XRLayout.hpp` + gtests. Ships an immediate "tidy my monitors" button. **Touches:** `XRLayout.*` (new,
pure), `OpenXRManager.{hpp,cpp}`, `DispatcherTranslator.cpp`, `KeybindManager.cpp`, `XRIpc.cpp`,
`tests/xr/`. *(Explicitly a stopgap that Option 3 subsumes.)*

### WP-L2 — Auto-assignment planner (pure) · **M** · dep: research/03 WP-G1
Pure planner: `assignSlots(members, shape, occupancy, gazeDir, mode) → per-member slot` with the §4
rules (nearest-gap-to-gaze, next-free-slot fallback, overflow grow, compact vs keep-gaps reflow). Reuses
research/03 `cellLocalPose`. `eXRSlotMode` + slot id added to `SXRMonitorParams`/layer. Fully
unit-testable without a runtime. **Touches:** `XRLayout.*`, `XRMonitorConfig.{hpp,cpp}`, `tests/xr/`.

### WP-L3 — World auto-layout runtime (Option 2) · **L** · dep: research/03 WP-G2
`xrlayout` keyword + runtime object (an `xrgrid` with a preset + `assign:auto`); wire the planner into a
main-thread `reflowLayout()`; reflow on create/destroy/resize; status/layout IPC + `xrlayout*` events;
`hyprctl openxr layout` serializes `layout:/slot:` not poses (§6.4). **Touches:** `OpenXRManager.*`,
`XRMonitorLayer.*`, `XRIpc.cpp`, `config/legacy/ConfigManager.cpp` (keyword), `ConfigValues.cpp`.

### WP-L4 — Follow-cluster engine + per-class defaults (Option 3 core) · **M** · dep: WP-L3, research/03 WP-G2
The head/body cluster: named angular slots (`center`/`left-wing`/`right-wing`/`above`/`below`) or
continuous fan within the follow-frame, `cluster_separation` enforcement, seeded off the shared
parent's single leash solve. `openxr:layout:*` per-class defaults + `openxr:layout:auto` new-monitor
routing. **Touches:** `XRLayout.*`, `OpenXRManager.*`, `ConfigValues.cpp`, `tests/xr/`.

### WP-L5 — Grab/pin/detach/snap coexistence + reflow polish · **M** · dep: WP-L4, research/04
Slot-mode transitions (`pin`/`unpin`/`detach`/`join`/`snap`/`retile`); grab-release snap-to-slot vs
detach-past-threshold policy; `swap`/`focus` directional verbs; animated compact reflow (spring seed);
hyprtester `--xr` integration tests (create layout, auto-spawn, recenter, snap, detach). **Touches:**
`XRAnchor.{hpp,cpp}` (release policy), `OpenXRManager.*`, input grab hook, `hyprtester/src/tests/xr/`.

Rough total ≈ one L split five ways; **WP-L1 ships standalone immediately**; WP-L2→L3 is the critical
path (both gated on grids WP-G1/WP-G2); WP-L4/L5 follow.

---

## 9. Open questions for the user (taste + scope)

1. **Grids relationship.** Adopt framing (A) *auto-layout = thin planner over research/03 grids* (build
   grids first, recommended), or (B) *`xrlayout` is its own object that materializes a grid*? This
   decides whether research/03 is a hard prerequisite or a shared subsystem.
2. **Default arrangement.** Is **arc-row (curved productivity wall)** the right default shape, matching
   every incumbent? Or focal-primary+satellites (Workrooms), or tier-stack?
3. **Auto default-on?** Should `openxr:layout:auto` default **off** (new monitors keep today's
   single-gaze placement; auto is opt-in) or **on** (new monitors flow into a default layout)? Off is
   safer/less surprising; on is more "magical."
4. **Curvature.** Expose Immersed/VD-style **curve** as a 0–1 slider interpolating flat↔arc (recommended)
   with a single `radius`? Or per-axis radii (research/03 warns against two finite radii)? Is a full
   sphere (curved rows) ever wanted, or is vertical-linear always right?
5. **Reflow-on-destroy default.** **Compact** (close gaps, tiling instinct) or **keep-gaps** (monitors
   stay put)? And on recenter — auto-reflow the whole set (Virtual Desktop default) or leave placed
   monitors alone?
6. **Persistence shape.** Confirm auto-placed monitors serialize as `layout:/slot:auto` + an `xrlayout`
   line (re-flows on load), **not** baked poses — accepting that a restored layout may settle slightly
   differently. Pinned/freeform monitors still bake poses.
7. **Keyword namespace.** New `xrlayout` keyword (ergonomic presets, recommended) vs. reuse research/03's
   `xrgrid` with `assign:auto` (fewer concepts, but "grid" is a lower-level word)? Or ship both (xrlayout
   as sugar over xrgrid)?
8. **`device` class.** Confirm `device`-anchored (controller-locked) monitors are **excluded** from
   auto-layout (hand-placed palettes only), matching SteamVR overlay docking intuition.
9. **Dwindle novelty.** Worth a `layout:dwindle` (recursive angular subdivision) as a fun option, or is
   arc/stack/focal/cluster enough? (Recommendation: skip — angular dwindle gets uncomfortable fast.)
```
