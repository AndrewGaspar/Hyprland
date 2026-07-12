# Research — Monitor Grids (named layout objects)

Design research for adding **grid layouts** to HypXRland: named, anchorable objects that arrange
XR virtual monitors on a flat plane, a cylinder, or a sphere-section, so a user can build a
"virtual monitor stand" once and branch monitors off it with the keyboard, then relocate the whole
stand with a single command.

Status: **research / design only.** Nothing here is implemented. Naming is provisional and defers
to the authoritative docs (`03-anchoring.md`, `05-ipc-config.md`, `02-virtual-monitors.md`) where
it overlaps.

---

## TL;DR

1. A **grid is just a parent transform** with its own anchor. Model it as `CXRGrid` that *owns a
   `CXRAnchor`* for its origin (so a grid can itself be `local` / `head` / `body` / `device`), plus
   pure geometry that maps a cell `(col,row)` to a **grid-local** pose. Each gridded monitor's world
   pose is `poseCompose(gridWorldPose, cellLocalPose)` — one new anchor mode `XR_ANCHOR_GRID` that
   reuses all existing pose math.
2. **Flat / cylinder / sphere are one model**, parameterized by a horizontal and a vertical radius;
   `R → ∞` on an axis makes that axis flat. Plane = both ∞, cylinder = vertical ∞, sphere = both
   finite. Curved cells face the user via the existing `lookAtNoRoll` helper.
3. **Leash the grid as one rigid unit:** solve each grid's anchor *once per frame*, then compose all
   its monitors — so a head/body-anchored monitor stand moves coherently, not per-panel.
4. **Mixed sizes is the hard part**; the recommended default is a tiling-WM-style **uniform cell +
   integer span** (a monitor occupies a `w×h` block), with pixel-aspect-preserving centering inside
   the block so different *resolutions* still look right. Two alternatives (native-size row-packing;
   freeform snap-points) are laid out below.
5. **Keyboard-first surface:** `xrgrid` keyword + dispatcher for the stand; `xrmonitor
   create-adjacent right`, `xrmonitor cell 2,0`, directional `focus`/`cellmove`, and a
   snap-to-nearest-cell on grab release. Recenter is `xrgrid recenter <name> [grab]` — a keyboard
   command with an optional hand fine-tune that reuses the existing grab machinery.

---

## 1. Prior-art survey

| System | Multi-window spatial model | Relevant takeaway |
|---|---|---|
| **Apple visionOS 26** | Windows/volumes float in a Shared Space; users drag freely. visionOS 26 added `SpatialContainer` + `Alignment3D` for explicit 3-axis alignment of content. No user-facing "grid," but the platform grew explicit alignment primitives because pure freeform got unwieldy. ([developer.apple.com](https://developer.apple.com/documentation/visionos/positioning-and-sizing-windows), [createwithswift.com](https://www.createwithswift.com/understanding-spatial-layout-in-visionos-26/)) | Freeform-only ages badly; users want structured alignment. Validates adding a lattice. |
| **Meta Quest / Horizon OS** | Up to 3 windows **docked in a primary curve** + more free-floating; **magnetic snap** when windows approach each other, and resizing a snapped window nudges its neighbor to keep edges flush. Horizon v81 lets you fuse windows into a **curved ultrawide** wrapping the FOV. ([meta.com](https://www.meta.com/help/quest/542427545314119/), [zybervr.com](https://zybervr.com/blogs/news/how-to-use-multiple-apps-simultaneously-on-meta-quest-3)) | The "primary curve" *is* a spherical/cylindrical grid; magnetic snap + neighbor-reflow is exactly the pleasant-mixed-size behavior. Strongest prior art for our target. |
| **Immersed** | Up to 5 virtual monitors, **curved around you**, stacked vertically, above/below FOV; layout persisted locally on the headset. ([immersed.com](https://immersed.com/), [inairspace.com](https://inairspace.com/blogs/learn-with-inair/virtual-reality-multiple-monitor-the-ultimate-productivity-and-immersion-setup)) | Curved-wrap is the expected productivity default; persistence of the arrangement is table-stakes (we have `hyprctl openxr layout`). |
| **SimulaVR** | wlroots-fork VR WM (Godot + Monado); windows become active on gaze, freeform placement. ([github.com/SimulaVR/Simula](https://github.com/SimulaVR/Simula)) | Closest architectural sibling (Wayland+Monado), but freeform — no structured grid to borrow. |
| **Hyprland itself** (target audience) | `dwindle`/`master` tiling; `movefocus`/`movewindow l/r/u/d`; named workspaces. | The idioms our users already know — directional spawn/move and a *named* object (like a named workspace) that you can relocate. This is the model to imitate, not the VR incumbents' pointer-drag UX. |

**Synthesis.** The incumbents converge on a *curved dock + snapping* model but drive it entirely by
hand/gaze pointer. Our differentiator is to give that same curved dock a **named, keyboard-driven,
config-persisted** identity that feels like a Hyprland workspace/tiling layout. No prior system
exposes the grid as a first-class relocatable named object; that is the design's novel core.

---

## 2. Recommended data model

Everything pure-math stays unconditional (no OpenXR headers), matching `XRMath.hpp` /
`XRAnchor.{hpp,cpp}` / `XRMonitorConfig.{hpp,cpp}`.

### 2.1 The grid spec (pure; lives with `XRMonitorConfig` / a new `XRGrid.hpp`)

```cpp
namespace OpenXR {
    enum eXRGridLayout : uint8_t {
        XR_GRID_SPAN = 0,   // uniform cell lattice; monitor occupies an integer w×h block (default)
        XR_GRID_PACK,       // rows auto-pack native-size monitors left→right (alt; §5 Option B)
    };

    struct SXRGridGeometry {
        eXRGridLayout layout   = XR_GRID_SPAN;
        float         radiusH  = 0.f;   // horizontal curve radius (m); 0 / +inf sentinel = flat
        float         radiusV  = 0.f;   // vertical   curve radius (m); 0 / +inf sentinel = flat
        float         pitchH   = 1.7f;  // horizontal cell pitch, meters (center-to-center)
        float         pitchV   = 1.05f; // vertical   cell pitch, meters
        float         gap      = 0.03f; // inter-monitor gap inside a cell/pack, meters
        bool          faceUser = true;  // curved cells yaw/pitch to face the center (else stay planar)
    };

    // A monitor's membership in a grid — lives on CXRMonitorLayer (main thread + frame snapshot),
    // NOT in SXRAnchorState (which stays string-free pure math).
    struct SXRGridCell {
        int col = 0, row = 0;   // SPAN: lattice coords; PACK: (row, index-in-row)
        int spanW = 1, spanH = 1; // SPAN only; PACK ignores
    };
}
```

The grid *itself* (its name, geometry, and its own anchor) is a runtime object:

```cpp
// src/openxr/XRGrid.hpp  (#ifdef HAVE_OPENXR — it owns a CXRAnchor and is manager-side)
class CXRGrid {
  public:
    std::string      m_name;
    SXRGridGeometry  m_geom;
    CXRAnchor        m_originAnchor;    // REUSE: grid origin can be local/head/body/device
    bool             m_declaredByConfig = false;

    // pure geometry: cell (col,row[,span]) -> pose in the grid's LOCAL frame (origin at m_origin)
    SXRPose cellLocalPose(const SXRGridCell& c, uint32_t pxW, uint32_t pxH,
                          float& outWidthMeters, float& outHeightMeters) const;
};
```

The key move: **the grid origin reuses the entire `CXRAnchor` solver.** A `head`-anchored grid
leashes exactly like a head-anchored monitor — the spring, deadzone, and re-anchor math are already
written and unit-tested (`03-anchoring.md` §3.2–3.3, §4.4). We solve it once, get one world pose,
and hang every monitor off it.

### 2.2 The new monitor anchor mode

`SXRAnchorState` gains one enumerant; the *string* grid name lives on the layer, and the parent
pose is injected into `solve()` so the pure solver never learns about grids-by-name:

```cpp
enum eXRAnchorMode : uint8_t { XR_ANCHOR_LOCAL, XR_ANCHOR_HEAD, XR_ANCHOR_BODY,
                               XR_ANCHOR_DEVICE, XR_ANCHOR_GRID /* new */ };

// SXRSolveInput gains:
std::optional<SXRPose> parentWorld;   // grid's solved world pose, supplied for XR_ANCHOR_GRID

// solve() for XR_ANCHOR_GRID (pure): anchorPose holds the cell-local pose (from cellLocalPose)
//   worldPose = poseCompose(*parentWorld, anchorPose);  space = LOCAL_FLOOR
```

So `XR_ANCHOR_GRID` is "`XR_ANCHOR_LOCAL`, but relative to a parent frame the caller hands in."
Grid-name → parent-pose resolution is impure and stays in `COpenXRManager`; the solver stays pure
and testable. Verb math (`move`/`rotate`/`scale`/`distance`) is discussed in §6.

### 2.3 Frame-thread solve ordering (the "solve once per grid" contract)

Today the frame loop snapshots `m_layers` and calls `solve()` on each independently. Grids add a
**two-phase pass** on the snapshot:

```
phase 1: for each grid g in snapshot:  g.worldPose = g.m_originAnchor.solve(view, grips, tune)
phase 2: for each layer l in snapshot:
            if l is gridded:  in.parentWorld = grids[l.m_gridName].worldPose
            result = l.m_anchor.solve(in, tune)      // XR_ANCHOR_GRID composes with parentWorld
```

This gives the rigid-unit leash the prompt asks for: a body-anchored stand yaws as one object; no
per-panel spring fighting. Grid geometry (`cellLocalPose`) is deterministic and cheap; cache it on
the layer and recompute only when the grid geometry or the cell/span changes (a dirty flag under
`m_layersMu`).

---

## 3. Geometry math sketch

Conventions from `03-anchoring.md` §1.1: right-handed, +Y up, −Z forward, quads visible from +Z.
The grid's LOCAL frame has its origin at `CXRGrid` origin; column `col` increases to +X (right), row
`row` increases to +Y (up); the user sits toward the grid's +Z (so cell (0,0) faces them).

Let cell physical width/height be `W×H` meters (from span×pitch or native size, §5). Let cell-center
index offsets be `u = col`, `v = row` (a grid can define whether (0,0) is a corner or the center; use
signed indices so `create-adjacent left` yields `col = −1`).

### 3.1 Flat plane (both radii ∞) — the base case

```
localPos = ( u * pitchH,  v * pitchV,  0 )
localRot = identity                      // every monitor lies in the grid plane, facing +Z
```

`worldPose = poseCompose(gridWorld, {localPos, identity})`. Trivial, deterministic, fully
addressable. A wall of monitors facing the user.

### 3.2 Sphere-section (both radii finite `R`) — wrap-around

The user's eye sits at the grid origin (sphere center). Convert linear pitch to **angular pitch**
via arc length: `θ_h = pitchH / R`, `θ_v = pitchV / R`. Cell direction and position:

```
q_dir  = qMul(qFromYaw(u * θ_h), qFromPitch(v * θ_v))     // yaw about +Y, then pitch about +X
dir    = qRotate(q_dir, (0,0,−1))                          // unit vector center → cell (grid-local)
localPos = dir * R                                         // cell center on the sphere surface
localRot = faceUser ? lookAtNoRoll({0,0,0}, ... ) …        // see below
```

For `localRot`, the monitor is **tangent to the sphere, facing the center** — its +Z points from the
cell back at the origin. That is exactly `lookAtNoRoll(from = localPos, to = origin, fallback)`
(`03-anchoring.md` §1.4): +Z toward the user, roll removed. If `faceUser = false`, keep `identity`
(a flat billboard on a curved rail — rarely wanted).

Sanity: cell (0,0) → `dir = (0,0,−1)`, `localPos = (0,0,−R)` (R in front), `localRot` faces +Z back
at the user. ✔

### 3.3 Cylinder (vertical radius ∞, horizontal finite) — the comfortable middle

Horizontal wraps on a circle; vertical stays a straight stack (most productivity docks are cylinders,
not spheres — vertical curvature is disorienting):

```
θ_h = pitchH / R_h
q_yaw = qFromYaw(u * θ_h)
horiz = qRotate(q_yaw, (0,0,−1)) * R_h + (0,0,R_h)   // arc in XZ, origin tangent at the near point
localPos = horiz + (0, v * pitchV, 0)                 // vertical is linear
localRot = faceUser ? lookAtNoRoll(localPos, {localPos.x, localPos.y, ??}, …)
```

For a cylinder the facing should aim at the **vertical axis line** through the origin (not the point),
so pitch stays 0 and only yaw turns: aim `to = (0, localPos.y, 0)`. That keeps rows level while
columns wrap. (Equivalently `localRot = qFromYaw(u * θ_h)`.)

### 3.4 One model covers all

Treat each axis independently: **finite radius ⇒ angular placement on a circle; infinite ⇒ linear
placement.** `R_h = R_v = ∞` → §3.1 plane; `R_v = ∞` only → §3.3 cylinder; `R_h = R_v = R` → §3.2
sphere. The `R → ∞` limit is continuous (`R·sin(pitch/R) → pitch`, `R·(1−cos(pitch/R)) → 0`), so a
user sliding radius from `1.8 m` up to huge values watches the dock flatten smoothly. Independent
`R_h ≠ R_v` (finite) is a true ellipsoid and mildly ambiguous; **recommend not exposing two distinct
finite radii in v1** — offer `flat` | `cylinder radius:R` | `sphere radius:R`, i.e. at most one finite
radius, mapped onto the general per-axis code.

**Pitch: linear (meters) is the right primary knob.** Because monitor sizes are also meters, linear
pitch composes cleanly with mixed sizes (a 1.6 m monitor at 1.8 m pitch always leaves 0.2 m gaps,
flat or curved). Angular pitch (`apitch:22deg`) is a nice convenience for "6 monitors evenly across
180°" and can be offered as an alternative that the parser converts to linear via `pitch = θ·R`.

---

## 4. Mixed monitor sizes — three options (the acknowledged hard part)

The tension: a lattice wants uniform cells, but monitors differ in pixel resolution *and* desired
physical size. Three coherent ways to resolve it.

### Option A — Uniform cell + integer span (tiling-WM native)  ★ recommended default

The grid has a fixed cell pitch. A monitor occupies an integer **`spanW × spanH`** block; its quad
width is `spanW·pitchH − gap`, height `spanH·pitchV − gap`. Pixel aspect is preserved the way the
existing code already does it (`height = width·pxH/pxW`); if that derived height ≠ the block height,
**center the quad in its block** (letterbox the empty grid space, never distort pixels). Resize =
change span (`scale` and grab-resize operate on integer span, not free meters).

- **Placement:** deterministic `(col,row)` + `(spanW,spanH)`; a multi-cell monitor anchors at the
  block's cell-center (average of occupied cell centers).
- **Collision:** classic rectangle-overlap in cell space. Occupancy is a small set of `(col,row)`.
- **Pleasant?** Very, for keyboard users: everything snaps to a rhythm, `create-adjacent` and
  directional `cellmove` are trivial, layouts align perfectly. Mirrors dwindle/master intuition
  (you don't set a tiled window's pixel size; you change its split/span).
- **Cost:** physical size is quantized to the pitch. A user wanting a genuinely arbitrary size uses
  Option B/pack mode or detaches to freeform. This is the Quest "primary curve" model.

### Option B — Native-size row packing (shelf / flexbox)

The grid defines rows; each monitor keeps its **native** size (`size:` meters, height from aspect).
Each row packs monitors left→right, advancing the cursor by `monitorWidth + gap`; rows stack by
`max(rowHeight) + gap`. No fixed lattice.

- **Placement:** cell = `(row, indexInRow)`; world position is the running pack cursor mapped onto
  the flat/curved rail (arc-length accumulation on a cylinder/sphere).
- **Collision:** none — packing can't overlap; insert/remove/resize **reflows** the row (neighbors
  slide, like tiling). Reflow can be animated for polish.
- **Pleasant?** Best for genuinely mixed sizes and the literal "monitor stand shelf" mental model;
  a 34" ultrawide next to two 24"s just works. Matches Quest's magnetic-snap-and-nudge behavior.
- **Cost:** weaker absolute addressability ("cell 3,2" is meaningless; only "row 1, 3rd" is), and
  reflow-on-change can feel jumpy. Directional spawn/focus still works great.

### Option C — Freeform + snap points (grid as guide, not container)

The grid only publishes **snap targets** (cell centers / edges / a fine sub-lattice). Monitors are
fully freeform (arbitrary size and world pose); on grab release near a snap target they snap. Sizes
unconstrained; grid is a soft magnet.

- **Pleasant?** Maximum flexibility, minimal structure. But it doesn't give a clean keyboard "spawn
  in the next free cell" story (there is no occupancy notion), and no auto-reflow.
- **Best used as a complement, not a grid type:** fold it in as the universal *grab-release snap* and
  an `xrmonitor snap` verb layered over Option A/B, not as its own mode.

### Recommendation

Ship **Option A as the default grid layout** (`XR_GRID_SPAN`) — it is the most Hyprland-native, fully
keyboard-addressable, and deterministic. Offer **Option B as `layout:pack`** for users who want native
sizes and a shelf feel. Treat **Option C as universal snapping** (grab-release + `snap` verb) that
applies on top of either. This yields one coherent story: *span lattice by default, opt into packing,
snapping everywhere.*

---

## 5. Proposed config / dispatcher / IPC surface

Follows `05-ipc-config.md` conventions exactly (comma-separated top-level fields; space-separated
`key:value` sub-tokens; classic-hyprlang v1).

### 5.1 `xrgrid` config keyword (new)

```
xrgrid = <name>, <geometry-spec>, <anchor-spec>

<geometry-spec> ::= ("flat" | "cylinder" | "sphere") [SP "radius:" <m>]
                    [SP "pitch:" <hM> "," <vM>]  [SP "apitch:" <hDeg> "," <vDeg>]
                    [SP "gap:" <m>]  [SP "layout:" ("span"|"pack")]  [SP "curve:" ("on"|"off")]
<anchor-spec>   ::= exactly the xrmonitor anchor-spec grammar (05 §2.2): anchor:local|head|body|device
```

```ini
# world-fixed curved monitor stand: 1.8 m radius sphere-section, 1.7×1.05 m cell pitch
xrgrid = main, sphere radius:1.8 pitch:1.7,1.05 gap:0.04, anchor:local pos:0,1.35,0 yaw:0

# body-leashed HUD shelf that follows your torso, native-size packed
xrgrid = hud, cylinder radius:1.2 layout:pack gap:0.03, anchor:body offset:0,1.3,-1.2
```

### 5.2 Gridded `xrmonitor` (extends 05 §2.2)

A monitor joins a grid by replacing its anchor-spec with a `grid:` spec:

```
<anchor-spec> ::= … existing … | "grid:" <gridName> SP "cell:" <col> "," <row> [SP "span:" <w> "," <h>]
```

```ini
xrmonitor = XR-code,  2560x1440@90, grid:main cell:0,0 span:2,1
xrmonitor = XR-chat,  1280x720,     grid:main cell:2,0
xrmonitor = XR-music, 1920x1080,    grid:main cell:0,1
```

Reconciliation reuses 05 §2.5 verbatim, with grids reconciled *before* monitors (a monitor
referencing an undeclared grid is an error, logged, monitor skipped — same as an unresolved rule).

### 5.3 Dispatchers (thin shims → `COpenXRManager` methods, like every existing verb)

`xrgrid <verb>` (new dispatcher) and additions to the `xrmonitor` dispatcher:

| Dispatcher | Verb | Args | Semantics |
|---|---|---|---|
| `xrgrid` | `create` | `<name> [flat\|cylinder\|sphere] [radius:R] [pitch:h,v] [gap:g] [layout:…] [anchor-spec]` | Create a runtime grid (not reconciled). |
| `xrgrid` | `destroy` | `<name>` | Destroy grid; member monitors **detach to `local`** (freeze current world pose) — see §6. |
| `xrgrid` | `select` | `<name>` | Set the selected grid (target for create-adjacent, recenter). |
| `xrgrid` | `recenter` | `<name\|active> [grab]` | Move grid origin in front of current gaze at `openxr:default_distance`; `grab` starts a hand fine-tune (§6). |
| `xrgrid` | `move` / `rotate` | `<dx dy dz>` / `<dyaw [dpitch]>` | Nudge the grid origin (drives `m_originAnchor.applyMove/applyRotate`). |
| `xrgrid` | `anchor` | `<name> <local\|head\|body\|device:…> [offset:…]` | Re-anchor the **grid origin** (whole stand switches leash mode). |
| `xrgrid` | `reflow` / `set` | `<name> pitch:… \| radius:… \| gap:…` | Retune geometry live; monitors recompute cells. |
| `xrmonitor` | `create-adjacent` | `<right\|left\|up\|down> [name] [WxH]` | Spawn a monitor in the next free cell from the **focused/selected** gridded monitor, on the same grid. |
| `xrmonitor` | `cell` | `<col,row> [span:w,h]` | Move the selected monitor to an explicit cell (swap/reject on collision, §6). |
| `xrmonitor` | `cellmove` | `<right\|left\|up\|down>` | Move the selected monitor one cell (swap with occupant if present — like `movewindow`). |
| `xrmonitor` | `focus` | `<right\|left\|up\|down>` | Move **desktop focus** to the neighbor-cell monitor (maps to Hyprland `movefocus`; routes real focus to that output). |
| `xrmonitor` | `grid` | `<name> [cell:c,r]` | Attach the selected freeform monitor to a grid (snap to given/nearest free cell). |
| `xrmonitor` | `detach` | *(none)* | Convert the selected gridded monitor back to freeform `local` at its current world pose. |
| `xrmonitor` | `snap` | *(none)* | Snap the selected freeform monitor to the nearest cell of the selected grid. |

All also reachable via `hyprctl openxr <same subcommands>` (one implementation, two transports, per
05 §3/§4). `hyprctl openxr layout` gains `xrgrid = …` emission before the gridded `xrmonitor = …`
lines (which serialize `grid:name cell:… span:…` instead of a pose). `hyprctl openxr status`/`-j`
gains a `grids` array (name, geometry, origin anchor, member cells).

New socket2 events (mirroring `xrmonitor*`): `xrgridadded`, `xrgridremoved`, `xrgridrecenter`
(`<name>`), and `xrmonitorcell` (`<name>,<col>,<row>`) on cell changes.

### 5.4 Example binds (Hyprland-idiomatic, directional)

```ini
# build & branch a stand
bind = SUPER,        G,      xrgrid,    create main sphere radius:1.8
bind = SUPER,        N,      xrmonitor, create-adjacent right
bind = SUPER SHIFT,  N,      xrmonitor, create-adjacent down

# navigate & rearrange like a tiling WM
bind = SUPER,        left,   xrmonitor, focus left
bind = SUPER,        right,  xrmonitor, focus right
bind = SUPER SHIFT,  left,   xrmonitor, cellmove left
bind = SUPER SHIFT,  right,  xrmonitor, cellmove right
binde= SUPER,        equal,  xrmonitor, scale +1      # grow span by one cell (gridded)

# relocate the whole stand when you move seats
bind = SUPER,        R,      xrgrid,    recenter main
bind = SUPER SHIFT,  R,      xrgrid,    recenter main grab   # then hand-tune, release to set
bind = SUPER,        H,      xrgrid,    anchor main head     # leash the entire stand to the head
```

---

## 6. Grab / recenter / migration UX

### 6.1 Grabbing a gridded monitor → snap-to-nearest-cell on release

The grab state machine (`04-input.md`) and grab pose math (`03-anchoring.md` §4) already device-lock
a grabbed monitor to the hand. For a gridded monitor we change only **release**: instead of
`endGrab` re-anchoring to the persistent mode, project the release world position into the grid's
local frame, find the **nearest cell** (`col = round(localX/pitchH)`, `row = round(localY/pitchV)`,
or nearest arc index on a curve), and:

- empty target cell → occupy it (`m_cell` updated, pose recomputed; the quad springs to the cell —
  reuse the HEAD spring seed so it glides rather than snaps);
- occupied → **swap** with the occupant (tiling-WM instinct) or fall back to nearest free cell if
  swap is disabled.

This is the pleasant answer and matches Quest's magnetic snap. Freeform monitors ignore this (they
re-anchor to `local` as today).

### 6.2 Grabbing the whole grid

Two viable triggers (open question §7):

1. **Modifier grab:** grab any member monitor while a "grid-grab" bind/held button is active → the
   grab drives `m_originAnchor` (the grid origin) instead of the monitor; all members move rigidly
   because they compose against the origin. Release re-anchors the origin.
2. **Grid handle quad:** render a small always-present "handle" quad at the grid origin (or a thin
   frame around the lattice) that is itself grabbable; grabbing it moves the grid. More discoverable,
   costs a quad + render code.

Either reuses the existing grab machine at the grid level with zero new pose math — the grid origin
*is* a `CXRAnchor`.

### 6.3 Recenter (`xrgrid recenter`) — keyboard, optional hand fine-tune

`recenter <name>` computes the same placement as `applyCenter` (`03-anchoring.md` §5.5: view-forward
at `default_distance`, facing the user) and writes it into `m_originAnchor` — the whole stand jumps
in front of the user. This is the "I moved seats, reset my stand" command. `recenter <name> grab`
does the placement **and** immediately opens a grid-grab (§6.2) so the user hand-tunes the final pose
and releases to commit — keyboard to get it roughly right, hands to perfect it. For a head/body-
anchored grid, recenter re-captures the offset/`bodyHeight` at the current pose so the leash
continues from there.

### 6.4 Migration & coexistence

- Freeform and gridded monitors coexist trivially (different anchor modes on independent layers).
- `xrmonitor grid <name>` converts freeform → gridded (compute nearest/next free cell, seed spring
  from current world pose so it glides in). `xrmonitor detach` converts gridded → freeform `local`
  by freezing `lastWorld()` (exactly the `setMode`→local path, §5.6). `xrmonitor snap` is a one-shot
  align without changing membership.
- Destroying/undeclaring a grid detaches members to `local` at their current world pose (never
  destroys monitors — parallels 05 §2.5 "runtime monitors are never touched"), so a stand can be
  dissolved without losing windows.

---

## 7. Implementation sketch — incremental work packages

Sequenced; each builds on the previous. Effort S/M/L/XL relative to the existing WP1–13 grain.

### WP-G1 — Grid geometry + parser (pure)  · **M**
Pure, unconditional, fully unit-testable — de-risks the math before any runtime.
- New `src/openxr/XRGrid.hpp/.cpp` **or** extend `XRMath`/`XRMonitorConfig`: `SXRGridGeometry`,
  `SXRGridCell`, `cellLocalPose()` (flat/cylinder/sphere per §3), linear↔angular pitch conversion.
- `parseXRGridLine()` (the `xrgrid` keyword) and the `grid:/cell:/span:` extension to
  `parseXRMonitorLine`/`parseXRMonitorCreateArgs` in `XRMonitorConfig.cpp`; add `SXRGridCell` +
  grid-name to `SXRMonitorParams`.
- `serializeXRGridLine()` + gridded `xrmonitor` serialization for `layout` dump.
- Tests: `tests/xr/grid.cpp` — cell→pose for all three geometries, `R→∞` plane limit, round-trip
  parse/serialize, span sizing + aspect centering, error cases.
- **Touches:** `src/openxr/XRGrid.*` (new), `XRMonitorConfig.{hpp,cpp}`, `tests/xr/`. No runtime, no
  `HAVE_OPENXR`. Verifies the no-openxr build.

### WP-G2 — Grid runtime + solve integration  · **L**
Make grids real objects the frame loop honors.
- `CXRGrid` (owns a `CXRAnchor` origin); `COpenXRManager` grid registry (`m_grids`, guarded by
  `m_layersMu` or a sibling mutex) + create/destroy funnel; snapshot grids alongside layers.
- Two-phase frame solve (§2.3): add `XR_ANCHOR_GRID` mode + `parentWorld` to `SXRSolveInput`/`solve`
  in `XRAnchor.{hpp,cpp}` (+ unit tests for the compose); layer carries `m_gridName`/`m_cell` and a
  cached cell-local pose with a dirty flag.
- `xrgrid` keyword registration + declared-set reconciliation (grids before monitors), status/layout
  IPC, `xrgridadded/removed` events.
- **Touches:** `OpenXRManager.{hpp,cpp}`, `XRMonitorLayer.{hpp,cpp}`, `XRAnchor.{hpp,cpp}`,
  `XRIpc.cpp`, `config/legacy/ConfigManager.cpp` (keyword), `ConfigManager` declared-grid list.

### WP-G3 — Keyboard spawn/navigation verbs  · **M**
The productivity payoff.
- `create-adjacent`, `cell`, `cellmove`, `focus` (directional) + occupancy/collision (swap/reject);
  `xrgrid select`; `xrmonitor grid/detach/snap`.
- Dispatcher (`xrgrid` + `xrmonitor` additions) in `DispatcherTranslator.cpp` **and**
  `KeybindManager.cpp` name list (both sites, per 05 §3); `hyprctl openxr` subcommands; example binds
  + `example/openxr.conf` block; `xrmonitorcell` event.
- **Touches:** `OpenXRManager.{hpp,cpp}`, `DispatcherTranslator.cpp`, `KeybindManager.cpp`,
  `XRIpc.cpp`, `example/openxr.conf`.

### WP-G4 — Grab / recenter / migration UX (+ optional PACK)  · **M**
The "pleasant" polish and relocation story.
- Snap-to-nearest-cell on gridded grab release; grab-whole-grid trigger (§6.2 — pick one);
  `xrgrid recenter [grab]`, `xrgrid move/rotate/anchor/set`.
- Optional `XR_GRID_PACK` layout (Option B) — can split into its own **S** if deferred.
- Integration tests via hyprtester `--xr` (grid create, adjacent-spawn, recenter, snap).
- **Touches:** `XRAnchor.{hpp,cpp}` (grid grab release), `OpenXRManager.{hpp,cpp}`, the input grab
  hook (`04-input.md` machinery), `hyprtester/src/tests/xr/`.

Rough total ≈ one XL split four ways; WP-G1→G2 is the critical path, G3/G4 parallelize after G2.

---

## 8. Open questions for the user

1. **Primary mixed-size model.** Ship **Option A (uniform cell + integer span)** as the default and
   Option B (native-size pack) as opt-in `layout:pack`? Or is native-size packing the primary mental
   model you want, with span as the alternative? (This decides the whole default feel.)
2. **Pitch knob.** Linear pitch in meters (recommended; composes with sizes and gives the clean
   `R→∞` flatten) as primary, with angular `apitch:deg` as convenience — agree? Or angular-first?
3. **Geometry set.** Expose **flat / cylinder / sphere** (at most one finite radius)? Cylinder is the
   comfortable default for wrap-around; is a full sphere (curved rows too) actually wanted, or is
   vertical-linear always right?
4. **Grid-grab trigger.** Modifier-grab-any-member vs. a rendered grabbable **grid handle/frame**?
   The latter is more discoverable but adds a quad + render code and a visible object — do you want a
   visible grid frame at all (also useful as an alignment guide)?
5. **Collision policy on `cell`/`cellmove`/span-grow.** Tiling-WM **swap** with the occupant, or
   reject/keep-gaps? And on span-grow into an occupied cell: push neighbors (reflow) or refuse?
6. **Detach-on-destroy.** Confirm: destroying/undeclaring a grid **detaches** members to freeform
   `local` (never destroys monitors)? Same for `xrmonitor detach`?
7. **Focus traversal across grids.** Should directional `focus`/`cellmove` at a grid edge jump to an
   adjacent grid (multi-stand traversal) or stop at the boundary? (Multi-grid is a natural next step —
   "left stand / center stand / right stand.")
8. **Persistence granularity.** `hyprctl openxr layout` currently bakes live poses. For grids, dump
   the `xrgrid` origin as a live world pose + members as `grid:/cell:` (recommended), so a saved
   layout re-materializes the stand exactly — confirm that's the desired persistence shape.
```

