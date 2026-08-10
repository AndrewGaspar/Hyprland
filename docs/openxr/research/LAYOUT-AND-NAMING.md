# LAYOUT & NAMING — grids, auto-layout, 2D-plane sync, and auto-names

**Live planning surface.** Consolidates the spatial-arrangement research into one backlog.
These four reports are tightly coupled: grids provide the shared parent transform that
auto-layout plans over, naming assigns identity to auto-placed monitors, and 2D-plane sync
keeps pointer/focus adjacency matching the 3D arrangement. Deep detail is in the archived
originals.

Sources coalesced here:
- [`archive/03-monitor-grids.md`](archive/03-monitor-grids.md) — named layout objects (grid parent transform + `XR_ANCHOR_GRID`).
- [`archive/08-auto-layout.md`](archive/08-auto-layout.md) — automatic spatial placement (a planner over grids).
- [`archive/11-dynamic-monitor-naming.md`](archive/11-dynamic-monitor-naming.md) — auto-assigned monitor names.
- [`archive/12-spatial-2d-layout.md`](archive/12-spatial-2d-layout.md) — derive Hyprland's 2D layout plane from the 3D pose.

---

## Capability vision

Build a virtual monitor arrangement once and let the compositor manage it spatially:

- **Grids** — a named parent transform (`CXRGrid`) owning its own anchor, so a "virtual
  monitor stand" can be `local`/`head`/`body`/`device`-anchored and relocated as one rigid
  unit; monitors branch off cells with the keyboard.
- **Auto-layout** — instead of an explicit pose per `xrmonitor`, *flow* monitors into a
  sensible arrangement (arc/cylinder row, tier stack, focal cluster, HUD corners) with
  distinct policies per anchor class. Auto-layout ≈ grids + an auto-assignment planner +
  per-anchor-class defaults.
- **Auto-names** — spawn a monitor with no name and get `XR-<n>`, so auto-placement and
  quick scratch monitors don't force the user to invent identifiers.
- **2D-plane sync** — project the 3D arrangement down into Hyprland's native 2D monitor
  layout so mouse-crossing and directional focus match spatial intuition (a monitor to your
  upper-right in the headset sits upper-right in the 2D plane).

The unifying architectural insight (reports 03 & 08): **the one missing primitive is a
shared parent transform / named layout object.** `CXRAnchor` is per-layer today; there is no
object that solves an origin once and composes children. Everything else here is a planner,
a name generator, or a projection — all pure, testable, and built on that transform.

---

## Consolidated design decisions

### Grids (from 03) — the foundation
- **A grid is just a parent transform** modeled as `CXRGrid` owning a `CXRAnchor` origin,
  plus pure geometry mapping a cell `(col,row)` to a grid-local pose. Each gridded monitor's
  world pose is `poseCompose(gridWorldPose, cellLocalPose)` — one new anchor mode
  `XR_ANCHOR_GRID` reusing all existing pose math.
- **Flat / cylinder / sphere are one model**, parameterized by a horizontal and vertical
  radius; `R → ∞` on an axis flattens it. Plane = both ∞, cylinder = vertical ∞, sphere =
  both finite. Curved cells face the user via `lookAtNoRoll`.
- **Solve each grid's anchor once per frame** (two-phase solve), then compose all its
  monitors — so a head/body-anchored stand moves coherently, not per-panel.
- **Mixed sizes:** ship **Option A (uniform cell + integer span)** as the default (tiling-WM
  native), with Option B (native-size row packing) as opt-in `layout:pack`. Reject freeform
  Option C as the primary model.
- Pitch knob: **linear pitch in meters** as primary (composes with sizes, gives the clean
  `R→∞` flatten), angular `apitch:deg` as convenience.
- Grabbing a gridded monitor → snap-to-nearest-cell on release. Destroying/undeclaring a grid
  **detaches** members to freeform `local` (never destroys monitors).

### Auto-layout (from 08) — a planner over grids
- **Recommended shape: per-anchor-class layout engines over a shared slotting core**
  (Option 3). World/`local` monitors arrange around a room-fixed focal set; `head`/`body`
  ones arrange within the follow-frame without colliding.
- **Explicit dependency: the head/body follow-cluster is blocked on grids' `CXRGrid` +
  `XR_ANCHOR_GRID` + two-phase solve (03 WP-G1/WP-G2).** World arc auto-layout is *also*
  cleanest on that runtime, though a degenerate Option-1 `arrange` verb can bypass it by
  writing independent `local` poses — that's the down-payment stopgap.
- Three per-monitor placement states: `auto` (planner owns the pose), `pinned` (fixed cell),
  `float` (manual pose, like a floated tiled window). Auto is the default assignment; a manual
  cell is a pin; a grab floats it.
- The auto-assignment planner is pure/testable: `(members, shape, occupancy, gazeDir, mode)
  → per-member slot`, with nearest-gap-to-gaze placement, next-free-slot fallback, overflow
  grow, and compact-vs-keep-gaps reflow. Reflow runs main-thread under the layer mutex
  (`reflowLayout()`, modeled on `reconcileDeclaredMonitors()`).

### Naming (from 11)
- **`XR-<n>` monotonic** counter (matches Aquamarine's own `HEADLESS-<n>` policy — never
  reuse a number within a session), with `openxr:auto_name_prefix` (default `"XR-"`). Reject
  lowest-free reuse (identity churn) and anchor-class-aware names (`XRH-n` — names must not
  change on re-anchor).
- **Grammar:** `create` with a first token matching `^\d+x\d+` (a resolution) or `anchor:` is
  **nameless** (auto-generate, parse token as mode); `create` with no args → fully-defaulted
  auto monitor. Backward-compatible extension; forbids literally naming a monitor `1920x1080`.
- **`create` MUST return the assigned name** (blocker) — echo it instead of `"ok"`.
- Sequencing: implement naming (WP-N1) **before** the auto-layout placement WP so the latter
  can assume a name is always present.

### 2D-plane sync (from 12) — **SHIPPED** (WP-S1 + WP-S2)
- XR monitors used to land in a dumb right-appended row (default monitor rule → auto-offset →
  `CMonitorPositionController::arrange` appends flush-right in creation order); the 2D position
  had nothing to do with the 3D pose.
- **Design: viewer-centric unwrap** — project each quad's pose to angular (yaw/pitch)
  coordinates around the viewer, scale by `PX_PER_DEG`, then normalize/compact to a valid
  Hyprland layout. Latch a **reference frame** for world monitors + a follow frame for
  head/body, merged; **freeze-on-event** recompute (debounced) rather than per-frame.
- Implemented in `src/openxr/XRLayout2D.{hpp,cpp}` (pure, unconditional, `tests/xr/layout2d.cpp`)
  + `COpenXRManager::syncLayout2D()`. User surface: `openxr:layout2d:*`, `hyprctl openxr
  sync-layout [freeze|thaw]` / `xrmonitor sync`, an `xrlayout2dsync` event, and a `layout2d` block
  in `hyprctl openxr status`. Documented in [`../05-configuration.md`](../05-configuration.md)
  §2 "2D-plane sync".
- Open questions from report 12 §9, as answered by the implementation: default **on**; vertical
  **elevation** (with `world_height` available); `px_per_degree` **35**; recompute on grab release
  **automatically** (debounced, never mid-carry); attach seam **right**, auto-`around` in a
  headset-only session; physical monitors **untouched** (they have no room pose); the arc-slot
  fast path (§7) waits on research/08 landing — until then every monitor takes the angular path.

#### Follow-up: ray-cast edge crossing (task #139) — **SHIPPED**
Live validation of WP-S2 (2026-08-09) landed on "roughly correct, though it feels slightly hinky …
I almost feel like if we treated the mouse movement as logically a ray casting through 3d space
*when pushing past the boundary of the monitor* would feel closer to expected behavior." The gap is
real and structural, not a tuning miss: the unwrap gives **topological** adjacency, the eye wants
**visual** adjacency, and compaction discards depth (§2.4 projects centres by angle alone), so two
quads at the same bearing and wildly different distances become the same column. Elevation and yaw
diverge the same way.

So the crossing DECISION — and only that decision — is now taken from the room instead of the grid.
At the instant the cursor pushes past an edge, the overshoot becomes a point on the source quad's
extended plane, a ray is cast from the live head pose (the WP-V1 pose ring) through it, and the
nearest quad it meets wins; the cursor is warped to the 2D coordinates of the 3D hit point.
Implemented in `src/openxr/XRCursorCross.hpp` (pure, unconditional, `tests/xr/cursor_cross.cpp`,
29 cases) + `COpenXRManager::redirectCursorCrossing()`, hooked at `CPointerManager::move()` — the
one place the pre-clamp overshoot still exists before `closestValid()` discards it. Config
`openxr:cursor_crossing = raycast | layout` (default `raycast`, hot via a `parseKeyword`
special-case so the two feels can be A/B'd from inside the headset).

**The 2D sync is unchanged and remains authoritative** for everything else: it still owns the layout
for `movefocus`, adjacency, the warp verbs, `hyprctl openxr status`, and the pointer's clamp union —
and it is the FALLBACK for every case the ray cannot answer (no session, head pose older than
200 ms, a non-XR or `device`-anchored source, a ray that meets nothing). Nothing writes a monitor
offset, so there is no path by which the two can fight. Open taste question after living with it:
is 4° of angular forgiveness (the second, tolerated pass) the right amount for a real desk gap?

### Shared threading & config contract
Planner, reflow, projection, and name-minting all run **main-thread** under `m_layersMu`; the
two-phase solve runs **frame-thread** over a snapshot. Slot ids, cell geometry, grid origins
cross as **plain values** (frame thread does zero hyprutils refcount ops — MEMORY 2026-07-07).
`openxr:layout:*` and `openxr:auto_name_prefix` are legacy-hyprlang vars; any that must
hot-apply without a full reload need the `parseKeyword` special-case (like `openxr:enabled`),
or rely on the `xrlayout set`/`arrange` verbs which bypass the config path.

---

## Merged work-package backlog

**Critical path: grids first.** 03's `CXRGrid` + `XR_ANCHOR_GRID` + two-phase solve is the
prerequisite for auto-layout's head/body cluster (08 WP-L2+). Naming (11) is small and should
land *before* auto-layout placement. 2D-sync (12) is largely independent and can run in
parallel.

### Phase 1 — Grids (report 03)
| WP | What | Effort |
|----|------|--------|
| G1 | Grid geometry + parser (pure): `SXRGridGeometry`, `cellLocalPose()` flat/cyl/sphere, `xrgrid` keyword, `grid:/cell:/span:` monitor extension, serialize, `tests/xr/grid.cpp` | M |
| G2 | Grid runtime + solve integration: `CXRGrid` registry, `XR_ANCHOR_GRID` mode + `parentWorld` in `solve`, two-phase frame solve, declared-set reconciliation, `xrgridadded/removed` events | L |
| G3 | Keyboard spawn/navigation verbs: `create-adjacent`, `cell`, `cellmove`, directional `focus`, `xrgrid select`, `xrmonitor grid/detach/snap`; dispatchers + binds | M |
| G4 | Grab/recenter/migration UX: snap-to-cell on grid-member release, grab-whole-grid, `xrgrid recenter/move/rotate/anchor/set`, optional PACK layout | M |

*(These reuse the WP-G* namespace from report 03; do not confuse with the shipped grabbable-chrome WP-G1…G6 in report 04.)*

### Phase 2 — Naming (report 11) — land before auto-layout placement
| WP | What | Effort |
|----|------|--------|
| N1 | Auto-assigned names: nameless-`create` grammar + disambiguation in `parseXRMonitorCreateArgs`, `mintAutoName()` monotonic counter, `create` returns the name, IPC echoes it, hyprtester coverage | S/M |
| N2 | `openxr:auto_name_prefix` config var (fold into N1 if trivial) | XS |

### Phase 3 — Auto-layout (report 08)
| WP | What | Effort / dep |
|----|------|--------------|
| L1 | `arrange`/`recenter` down-payment (Option 1 subset): `arcSlotPose` helper + `xrlayout arrange` verb writing arc poses via `applyCenter`/`applyDistance`; ships an immediate "tidy my monitors" button | S–M · no grids dep |
| L2 | Auto-assignment planner (pure): `assignSlots(...)` with §4 rules | M · dep G1 |
| L3+ | Per-anchor-class engines, slot-mode flag on the layer, `reflowLayout()` funnel, `openxr:layout:*` config, `xrlayout` keyword, dispatchers/events | M–L · dep G2, N1 |

### Phase 4 — 2D-plane sync (report 12) — **DONE**
| WP | What | Effort |
|----|------|--------|
| ~~S1~~ | ~~Pure projection + normalization math (viewer-centric unwrap, `PX_PER_DEG`, compaction), gtests, no deps~~ **shipped** — `XRLayout2D.{hpp,cpp}`, `tests/xr/layout2d.cpp` (24 cases incl. a randomized structural sweep) | M |
| ~~S2~~ | ~~Reference-frame capture, debounced recompute funnel, offset injection into the monitor layout, `hyprctl openxr` verb~~ **shipped** — `syncLayout2D()`, `openxr:layout2d:*`, `sync-layout`/`xrmonitor sync`, `xrlayout2dsync`, `hyprtester/src/tests/xr/layout2d.cpp` | M · dep S1 |
| ~~S4~~ | ~~Ray-cast edge crossing: cast from the head through the exit point instead of trusting grid adjacency, so depth/elevation/yaw are respected at the crossing moment (task #139, from WP-S2 live validation)~~ **shipped** — `XRCursorCross.hpp`, `redirectCursorCrossing()`, `openxr:cursor_crossing`, `tests/xr/cursor_cross.cpp` | S · dep S2 |
| S3 | *(follow-up)* slot fast path — when research/08 places monitors into arc/grid slots, mirror `(col,row)` straight to 2D and skip the angular path for those monitors (report 12 §7) | S · dep 08 |

### Seams between the four
- **Naming ↔ auto-layout:** N1 first so placement can assume a name always exists (11 §6).
- **Grids ↔ auto-layout:** auto-layout's cluster is the *reason* grids exist; L1 is the only
  auto-layout piece that ships without grids.
- **Layout ↔ 2D-sync:** 08 decides where quads go in 3D; 12 keeps the 2D plane in sync with
  wherever they land (recompute on the same dock/undock/reflow events). 12 has shipped, so 08's
  reflow only needs to call `requestLayout2DSync()` when it moves a quad — the funnel already
  exists and is already wired to dock/undock.
- **Adaptive anchoring (shipped, `archive/13`)** already fires dock/undock events that 12's
  recompute funnel and 08's reflow should subscribe to.

---

## Open questions carried forward (for user triage)
- Primary mixed-size model: Option A (uniform cell + span) default vs Option B (native pack)?
- Geometry set: expose full sphere (curved rows) or is vertical-linear always right (cylinder
  default)?
- Grid-grab trigger: modifier-grab-any-member vs a rendered grabbable grid handle/frame?
- Collision policy on `cell`/`cellmove`/span-grow: tiling-WM swap vs reject/keep-gaps?
- Naming: confirm monotonic (never-reuse) over lowest-free; ship the prefix var vs hardcode?
- ~~2D-sync: recompute triggers and whether world monitors freeze on a latched reference frame
  (recommended) vs live-track.~~ **Answered by the shipped WP-S2:** latched reference frame,
  event-driven debounced recompute, never mid-carry. Remaining taste question after living with
  it: is `px_per_degree = 35` / `row_merge_deg = 10` the right feel for a real desk arrangement?
