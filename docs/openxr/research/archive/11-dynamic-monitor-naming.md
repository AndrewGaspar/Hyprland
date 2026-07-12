# 11 — Dynamic XR-monitor naming (auto-assigned names)

Research report. **No implementation** — design + prior-art survey so that a later WP can
implement "spawn a monitor with no name argument and get a sensible one." Every claim is cited
to code in this tree (or the installed Aquamarine headers). Companion to the concurrent
`08-auto-layout.md` — see §6 for the seam between the two.

---

## TL;DR

- Today an XR monitor's **name is its identity key everywhere**: layer lookup (`layerByName`,
  `OpenXRManager.cpp:1607`), reload reconciliation (`m_declaredByConfig` diff,
  `OpenXRManager.cpp:1633`), selection cycling, destroy, the `xrmonitoradded`/`xrmonitorremoved`
  IPC events, `monitor=`/`workspace=` targeting by name, and `openxr layout` serialization.
  Every create path (`xrmonitor=` keyword, `xrmonitor create` dispatcher, `hyprctl openxr
  create`) **requires** a non-empty name and errors on collision.
- **Recommendation:** add an auto-name generator that fills `SXRMonitorParams::m_name` when the
  create verb is invoked with no name token. Scheme: **`XR-<n>` monotonic** counter (matches
  Aquamarine's own `HEADLESS-<n>` policy — never reuse a number within a session), with
  `openxr:auto_name_prefix` (default `"XR-"`) to tune the prefix. Reject the anchor-class-aware
  naming idea (`XRH-n` etc.) — names must not change on re-anchor.
- **Grammar:** `create` with a first token matching `^\d+x\d+` (a resolution) is treated as
  **nameless** — the name is auto-generated and the token parses as the mode. `create` with no
  args at all → fully-defaulted auto monitor. This is a small, backward-compatible extension of
  the existing `parseXRMonitorCreateArgs` splitter (`XRMonitorConfig.cpp:344`).
- **Two real blockers for scriptability**, both must be fixed for this to be useful:
  1. `cmdCreate` returns `std::expected<void,std::string>` (`OpenXRManager.hpp:92`) → the socket
     reply is just `"ok"` (`XRIpc.cpp:121-124`). **The assigned name must be returned** or callers
     can't target what they just made. Change the funnel to return the name.
  2. Reconciliation already ignores runtime-created monitors (verified, §3a) — so auto-named
     runtime monitors are safe across `/reload`. Good.
- **Persistence story** (`openxr layout` → paste into config) works unchanged: an auto-named
  monitor serializes to an explicit `xrmonitor = XR-3, ...` line; pasting it makes it *declared*
  under that exact name next boot. Documented caveat: the live session may *also* auto-create
  `XR-3` → collision, resolved by "config-declared wins, skip that number in the counter" (§3b).
- **Nondeterminism warning** for `monitor:XR-3` window/workspace rules against auto names — the
  monitor that gets `XR-3` this session depends on creation order. Mitigation: use semantic
  explicit names for anything you write rules against; auto names are for throwaway/scratch
  monitors and gestures.
- Size: **1 WP** (naming generator + return-value plumbing + grammar + tests), optionally a
  tiny follow-up WP for the config prefix var. See §7.

---

## 1. Prior-art findings (with counter cites)

### 1.1 Aquamarine `HEADLESS-<n>` — the closest precedent, and it's *already under us*

Hyprland's XR monitors are headless outputs created by
`impl->createOutput(params.m_name)` (`OpenXRManager.cpp:1243`). The Aquamarine headless backend
signature is:

```cpp
// /usr/include/aquamarine/backend/Headless.hpp:52
virtual bool createOutput(const std::string& name = "");
```

and it owns a **private, monotonic** counter:

```cpp
// /usr/include/aquamarine/backend/Headless.hpp:66
size_t outputIDCounter = 0;
```

When `createOutput("")` is called with an empty name, Aquamarine synthesizes
`HEADLESS-<outputIDCounter>` (the counter is a `size_t` that is only ever incremented — the
`.cpp` isn't vendored here, but the header shows the field has no decrement path and outputs
are stored in a plain `std::vector` `outputs` with no free-list). **Policy: monotonic, never
reused within a session; resets to 0 only on compositor restart.** This is the behavior we
should mirror for `XR-<n>` so the two naming schemes are mentally consistent for users. Note we
never call `createOutput("")` today — we always pass an explicit name (`OpenXRManager.cpp:1243`),
so Hyprland's XR layer has never leaned on Aquamarine's auto-namer.

### 1.2 Hyprland's `hyprctl output create` — requires a name, no auto-namer at the Hyprland layer

The stock output dispatcher **does not** auto-name; the caller must pass `vars[3]`:

```cpp
// src/debug/HyprCtl.cpp:1753-1762 (dispatchOutput)
if (!vars[3].empty()) { for (... allMonitors()) if (m->m_name == vars[3]) return "Name already taken"; }
if (MODE == "create" || MODE == "add") {
    if (State::monitorState()->query().name(vars[3]).run()) return "A real monitor already uses that name.";
    ... impl->createOutput(vars[3]); ...
```

So Hyprland relies on the user to supply `HEADLESS-2` etc. by hand (the test suite does exactly
this — `hyprtester/src/tests/xr/monitors.cpp:267` `"/output create headless HEADLESS-2"`).
Auto-naming has always been Aquamarine's job. Our design puts the auto-namer in the **XR
manager** (not Aquamarine) so we control the prefix and the collision policy against live XR
layers — see §2.4.

### 1.3 Our own uniqueness + sequencing machinery today

- **Collision check** on every create (`OpenXRManager.cpp:1195-1206`): rejects a name already used
  by any real monitor (`allMonitors()`) *and* any existing layer (`m_monitorName`), with errors
  `"a monitor already uses that name"` / `"an XR monitor with that name already exists"`. An
  auto-namer must consult **both** these sets to pick a free number.
- **`m_seqCounter`** (`OpenXRManager.hpp:253`, `uint64_t`, `++m_seqCounter` at
  `OpenXRManager.cpp:1211`): a **monotonic per-layer creation sequence**, *not* a name source. It
  drives `select next/prev` cycle order (`cmdSelect`, `OpenXRManager.cpp:1781-1803`) and the
  layer-cap recency policy (`recomputeQuadActive`, `:1723`). It is never reused (monotonic) and is
  the natural model for an `XR-<n>` monotonic name counter — but they should stay **separate
  counters**: `m_seq` counts *all* layers (incl. declared ones) and is an internal ordering key;
  the name counter only advances when we mint an auto name, so `XR-<n>` numbers stay dense and
  legible.
- **The parser requires a name.** `parseXRMonitorLine` slices on the first comma and errors
  `"xrmonitor: missing monitor name"` on empty (`XRMonitorConfig.cpp:327-329`);
  `parseXRMonitorCreateArgs` takes `tokens[0]` as the name and errors `"create: missing monitor
  name"` if there are no tokens (`XRMonitorConfig.cpp:347-352`). The struct itself documents the
  invariant: `std::string m_name; // e.g. "XR-1"; must be unique` (`XRMonitorConfig.hpp:71-72`).

### 1.4 The "unique names come from runtime creation" note (WP11 fixtures)

This referred to the **test harness manufacturing collision-free names by hand**, not to any
auto-namer in the product:

```cpp
// hyprtester/src/xr/xr_helpers.cpp:53-55
std::string monitorName(int n) {
    return std::string("XR-t") + std::to_string(getpid()) + "-" + std::to_string(n);
}
```

i.e. `XR-t<pid>-<n>` — PID keys the run, `n` keys the monitor. The tests can't rely on
predictable `XR-1` names because parallel/overlapping runs would collide, so each test author
supplies a unique name at the create call site. This is precisely the ergonomic pain that an
auto-namer removes for interactive users (they should never have to invent `XR-t12345-3`), and it
tells us the *product* auto-namer's dedup must be robust to a populated monitor set.

### 1.5 Other conventions (for the collision-policy decision)

- **WAYLAND-n / DP-n / eDP-n / HEADLESS-n**: all monotonic-ish, backend-assigned, and treated by
  users as *opaque handles* — nobody expects `DP-2` to become `DP-1` after unplugging `DP-1`.
  This is the ecosystem norm and argues for monotonic.
- **Hyprland workspaces**: numeric IDs are user-assigned/explicit; *named* workspaces get negative
  auto-IDs from a decrementing counter (`getNewSpecialID`-style), again monotonic within a
  session. Consistent direction.
- **X11/RandR**: connector names (`DP-1`, `HDMI-A-1`) are hardware-slot-derived and stable, not
  sequential-on-create — not a useful model for virtual monitors.
- **sway headless**: `HEADLESS-<n>` with a monotonic counter (`wlr_headless` / sway's
  `output.c`), same policy as Aquamarine. Cross-checks the monotonic recommendation.

**Verdict:** every comparable system uses a **monotonic, non-reusing** counter. Match it.

---

## 2. Naming-scheme options + recommendation

### 2.1 Option A — `XR-<n>` monotonic (RECOMMENDED)

A single manager counter `m_autoNameCounter` (start 1). To mint: increment, format
`"<prefix><n>"`, and **re-check against the live set** (both `allMonitors()` and `m_layers`, as
`createXRMonitor` already does); if taken (a user explicitly created `XR-5`, or a declared
`XR-5` exists), keep incrementing. Never decrement, never reuse a number after a destroy within
the session. Resets on compositor restart (like Aquamarine).

- **Pro:** matches `HEADLESS-n`/sway; stable identity within a session (a destroyed `XR-3` never
  silently reappears as a different monitor); trivial to reason about; dense-enough numbers.
- **Con:** across restarts the numbers restart at 1 (fine — so does everything else); a
  long-lived session with churn drifts to large numbers (fine — cosmetic).

### 2.2 Option B — `XR-<n>` lowest-free (reuse after destroy) — REJECTED

Pick the smallest `n` not currently in use. Reuses `XR-1` after it's destroyed.

- **Pro:** numbers stay small.
- **Con:** **identity churn** — `monitor:XR-1` mirror rules and `xrmonitoradded XR-1` events now
  refer to a *different* monitor after a destroy/create cycle; scripts that cached `XR-1` get
  silently repointed. Diverges from `HEADLESS-n`. Rejected — the small-number benefit isn't worth
  the identity ambiguity.

### 2.3 Option C — anchor-class-aware names (`XRH-n` head, `XRB-n` body, …) — REJECTED

Encode the anchor mode in the prefix.

- **Fatal flaw:** the `anchor` verb re-anchors a monitor at runtime (`cmdAnchor`, doc 05 §3.1),
  which would demand a **rename** (`XRH-2` → `XRL-2`). Renames are poison: they break
  `layerByName`, the `m_selectedMonitor`/`m_lastHoveredMonitor` string keys
  (`OpenXRManager.cpp:1617-1622`), any user `monitor=`/`workspace=` rule, and every bar that
  cached the name from `xrmonitoradded`. **Names must be identity, orthogonal to placement.**
  Rejected.

### 2.4 Prefix config var — RECOMMENDED (small)

`openxr:auto_name_prefix` (String, default `"XR-"`), declared in
`src/config/values/ConfigValues.cpp` alongside the other `openxr:*` vars
(`ConfigValues.cpp:709-786`; that single `MS<String>(...)` entry is the whole registration per
the config-system convention). Lets a user pick `"disp-"` or `"vscreen-"`. Read at mint time
(hot — affects subsequently minted names only; already-minted names never change, consistent
with §2.3's identity rule). Keep `"XR-"` default so it matches the example config's semantic
names (`XR-code`, `XR-chat`, … in `example/openxr.conf:149-165`).

### 2.5 Reserved-name rules

Users *can* still explicitly create `XR-7` (nothing forbids a name that matches the auto
pattern). Policy: the auto-namer's re-check loop (§2.1) means an explicit `XR-7` simply causes the
counter to **skip 7** when it would next land there. We do **not** parse explicit names to
fast-forward the counter (too clever, and cheap to just probe-and-skip at mint time). Document
that mixing explicit `XR-<n>` names with auto names is allowed but the auto sequence will hop over
occupied numbers.

---

## 3. Identity / lifecycle analysis (the meat)

### 3a. Reload reconciliation — auto-named monitors are safe (verified)

Reconciliation operates **only** on layers with `m_declaredByConfig == true`
(`reconcileDeclaredMonitors`, `OpenXRManager.cpp:1633-1706`): it snapshots `liveDeclared` from
layers where `l->m_declaredByConfig` is set (`:1646-1648`), and the `L \ D` destroy pass only
touches names in that declared set (`:1701-1704`). `cmdCreate` explicitly leaves the flag false
(`OpenXRManager.cpp:1759` comment: *"Runtime-created: NOT declared, so reload reconciliation never
touches it"*; the flag is only set at `:1697` for config-created layers). So an **auto-named
runtime monitor survives `/reload` untouched** — exactly what we want. No change needed here; the
auto-namer inherits this correctness for free because it flows through the same `cmdCreate` →
`createXRMonitor` funnel.

One subtlety to preserve: reconciliation's name-collision branch (`:1686-1690`) already handles
"config declares a name a runtime monitor owns" by **leaving the runtime one alone** and WARNing.
If a user's config declares `XR-3` and the session had auto-minted a runtime `XR-3`, the runtime
one wins and the declaration is ignored with a warning. That's a reasonable default but see §3b
for the paste-persistence interaction.

### 3b. `openxr layout` serialization → persistence, and the next-boot collision

`hyprctl openxr layout` emits a paste-ready `xrmonitor = <name>, <mode>, <anchor>, size:` line
for **every** live monitor including runtime-created ones (doc 05 §4 table, `:386`;
`serializeXRMonitorLine`, `XRMonitorConfig.cpp:286-314`). The name printed is whatever the
monitor currently has — including an auto-minted `XR-3`. **This is the intended v1 persistence
story:** arrange monitors interactively, dump the layout, paste into `hyprland.conf`. Pasting
`xrmonitor = XR-3, ...` makes `XR-3` a **declared** monitor next boot (reconciliation creates it,
`m_declaredByConfig = true`).

**Walk-through of the next-boot collision the task flags:** suppose the user pastes `xrmonitor =
XR-3` *and* leaves a keybind/autostart that also auto-creates a monitor at startup.

1. Config parse → `XR-3` is in the declared set; reconciliation creates it early (STAGE at
   session start / reload).
2. Later, the autostart fires `hyprctl openxr create` (nameless) → auto-namer probes: `XR-1`
   free? … it must **skip `XR-3`** because the declared monitor already owns it (the re-check
   against `allMonitors()`/`m_layers` in §2.1 catches this). It lands on the next free number.

So the collision is **benign given the re-check loop** — the declared `XR-3` wins, the auto-namer
routes around it. The only failure mode is if the auto-create happens to run *before* declared
reconciliation (ordering), in which case §3a's WARN path fires and the declaration is dropped.
**Recommendation:** document that pasted (now-declared) auto-named lines are honored, and that if
you persist auto names you should stop auto-creating them at startup (or give them explicit
semantic names on paste — the better habit). This is a doc note, not code.

### 3c. Workspace / window-rule targeting — nondeterminism warning

`workspace = 1, monitor:XR-main` and `windowrule ... monitor:XR-3` match **by name string**
(doc 02 §"mirror" and the `monitor=` machinery, doc 05 §1.4 `:142-143`: *"come from ordinary
`monitor =` / `monitorv2` rules matched by name"*). If the user writes a rule against an
**auto** name, *which physical monitor gets that name this session depends on creation order* —
nondeterministic across reboots/gesture-order. Mitigations, in preference order:

1. **Discourage rules against auto names** (doc guidance): auto names are for scratch/throwaway
   monitors and gesture-spawned ones. Anything you target with a persistent rule should get an
   explicit semantic name (`XR-code`, `XR-mail`).
2. Monotonic naming (§2.1) at least makes it *stable within a session* — `XR-3` is always the
   same monitor until destroyed.
3. (Future, out of scope) a stable alias/tag independent of the display name.

### 3d. IPC events + bars — name is the key, fine

`xrmonitoradded`/`xrmonitorremoved` carry the name as the data payload (doc 05 §5, `:481-482`;
posted at `OpenXRManager.cpp:1302` for add). A bar consuming these gets the auto name and can key
off it like any other. **Note a naming discrepancy in the task brief:** the events are
`xrmonitoradded` / `xrmonitorremoved` in both docs and code — *not* `xrmonitorcreated`. No action
beyond using the correct names. Once the create funnel returns the assigned name (§4), a bar or
script that issued the create also learns the name synchronously rather than having to correlate
the async `xrmonitoradded` event.

### 3e. select / hover flows — name-agnostic, fine

`select next/prev` cycles by `m_seq` creation order, not by name
(`cmdSelect`, `OpenXRManager.cpp:1781-1803`); hover/selection store the name as an opaque string
(`m_selectedMonitor`, `m_lastHoveredMonitor`, `m_curHoveredMonitor`). Auto names flow through
unchanged. `destroy active` / `select active` don't need a name at all. So the "just give me a
monitor and let me grab/select it" flow already works name-free on the *consumption* side — the
only gap is the *creation* side (this report) and *placement* (auto-layout, §6).

---

## 4. CLI / dispatcher grammar

### 4.1 The parse-ambiguity problem

`parseXRMonitorCreateArgs` (`XRMonitorConfig.cpp:344-372`) currently reads `tokens[0]` as the
name unconditionally, then treats a subsequent token containing `x` and not starting with
`anchor:` as the mode (`:357`). To support a nameless form we must decide whether the **first**
token is a name or a resolution.

**Proposed grammar** (backward compatible — every currently-valid invocation still parses the
same way):

```
create                                  → auto name, default mode, default (gaze) anchor
create <WxH[@Hz]>                        → auto name, given mode, default anchor
create <WxH[@Hz]> <anchor-spec>          → auto name, given mode, given anchor
create <name>                            → given name, default mode, default anchor   (today)
create <name> <WxH[@Hz]>                 → today
create <name> <WxH[@Hz]> <anchor-spec>   → today
create <name> <anchor-spec>              → today
```

**Disambiguation rule:** if the first token matches `^\d+x\d+(@…)?` (i.e. it *is* a resolution)
**or** starts with `anchor:`, treat the invocation as **nameless** and auto-generate the name;
otherwise the first token is the name (current behavior). Concretely: reuse the existing
mode-detector predicate — a token containing `'x'` and not starting with `"anchor:"` — but apply
it to `tokens[0]` as the *first* test. This is a ~10-line change to `parseXRMonitorCreateArgs`:
add a leading branch that, when `tokens[0]` looks like a mode or an anchor (or there are zero
tokens), leaves `m_name` empty and starts mode/anchor parsing at index 0.

Edge case: a user who literally names a monitor `1920x1080` becomes impossible — acceptable
(that was already a terrible name, and `HEADLESS-n`-style names never look like resolutions).

### 4.2 Where the name is actually minted

Keep the **parser pure and name-optional**: `parseXRMonitorCreateArgs` yields
`SXRMonitorParams` with `m_name` possibly empty (it already can't error on empty-first-token
under the new grammar). The **auto-name mint happens in `cmdCreate`** (or `createXRMonitor`),
which is main-thread and can see the live monitor set for dedup. This keeps the mint logic out of
the unconditionally-compiled pure layer (which has no access to `allMonitors()`), consistent with
the existing separation (`XRMonitorConfig.hpp:2-5` — the parser deliberately has no manager deps).

Recommended: a helper `COpenXRManager::mintAutoName()` that reads `openxr:auto_name_prefix`,
advances `m_autoNameCounter`, and probes for freedom. `cmdCreate` calls it when
`parsed->m_name.empty()`.

### 4.3 Return value — MUST return the assigned name (blocker)

Today the whole funnel is `std::expected<void, std::string>` (`OpenXRManager.hpp:92`), and
`XRIpc.cpp:121-124` maps success to the literal string `"ok"`. For nameless create, **the caller
cannot discover what got made.** Fix:

- Change `cmdCreate` (and the underlying `createXRMonitor` already returns the `PXRLAYER`, so its
  name is in hand — `OpenXRManager.cpp:1313`) to surface the assigned name.
- Simplest: `cmdCreate` returns `std::expected<std::string,std::string>` (the name). In
  `XRIpc.cpp`, return the name string on success instead of `"ok"` (for `create` only — other
  verbs keep `"ok"`). Scripts do `NAME=$(hyprctl openxr create 1280x720)`.
- The dispatcher path (`xrmonitorDispatch` → `SDispatchResult`, doc 05 §3) should stuff the name
  into the result's response string too, so `hyprctl dispatch xrmonitor create …` also echoes it.
- Preserve existing behavior for the **named** create: returning the (echoed) name is harmless and
  actually nicer than `"ok"`; if strict back-compat on the `"ok"` string is desired, echo the name
  only when it was auto-assigned. **Recommend always echoing the name** — simpler, and more useful.

`hyprctl openxr create` and the `xrmonitor create` dispatcher share the exact funnel
(`XRIpc.cpp:121` and the keybind path both call `cmdCreate`), so fixing the funnel gives parity
for free.

---

## 5. (folded into §4 and §6)

---

## 6. Intersection with auto-layout (`08-auto-layout.md`) — explicit seam

Dynamic creation-without-name and placement-without-pose are the **two halves of the same
gesture**: "give me a new monitor" (a keybind, a future 'new monitor' pinch, `hyprctl openxr
create` with no args) should produce a monitor that (a) has a sensible **name** (this report) and
(b) lands in a sensible **place** (auto-layout report). The combined default flow:

```
create            (no name, no anchor-spec)
  → mintAutoName()                         [THIS report §4.2]   → "XR-3"
  → auto-placement into the next slot      [08-auto-layout]     → pose/anchor
  → createXRMonitor(params)                [existing funnel]
  → return "XR-3" to the caller            [THIS report §4.3]
```

**Ownership split (what each report must agree on):**

| Concern | Owned by | Contract the other side relies on |
|---|---|---|
| Filling `m_name` when empty | **This report** (§2, §4.2) | 08 must not assume a name is present pre-mint; slot placement keys off identity only *after* mint |
| Filling anchor/pose when `m_anchorProvided == false` | **08-auto-layout** | Today `createXRMonitor` does a naive gaze-center fallback (`OpenXRManager.cpp:1219-1229`); 08 replaces that with real slotting |
| The `create`-with-no-args grammar | **This report** (§4.1) — but the anchor-spec-absent branch is exactly 08's trigger | Both must recognize "nameless *and* anchorless" = the pure "new monitor" gesture |
| Dedup/collision at mint | **This report** (§2.1 probe loop) | 08's reflow never renames (see §2.3) — placement changes pose, never name |
| Return value carries the name | **This report** (§4.3) | 08 may *also* want to return the chosen slot; agree on a combined reply format (e.g. `XR-3` alone, or `XR-3 slot:2`) |

**Flag where they must not diverge:** the current gaze-placement fallback lives *inside*
`createXRMonitor` (`:1219-1229`). If 08 moves placement to a pre-`createXRMonitor` step and this
report moves naming to `cmdCreate`, both are editing the same `cmdCreate`/`createXRMonitor`
seam — **the two WPs touch the same function and must be sequenced or merged** (implement naming
first — it's smaller and 08 can then assume a name is always present by the time it slots). The
`SXRMonitorParams.m_anchorProvided` flag (`XRMonitorConfig.hpp:82`) is the shared signal: naming
keys off `m_name.empty()`, placement keys off `!m_anchorProvided`; they're independent bits, so a
monitor can be nameless+placed, named+unplaced, or both-auto.

---

## 7. WP sketch

**WP-N1 — Auto-assigned XR monitor names (S/M, ~1 sitting).**
- `parseXRMonitorCreateArgs`: allow an empty/absent name; add the `tokens[0]`-is-a-mode /
  is-an-anchor disambiguation (§4.1). Pure-layer change, unit-testable in `tests/xr/parser.cpp`.
- `COpenXRManager::mintAutoName()`: prefix from `openxr:auto_name_prefix`, monotonic
  `m_autoNameCounter`, probe against `allMonitors()` + `m_layers` (mirror the existing dedup at
  `OpenXRManager.cpp:1195-1206`).
- `cmdCreate`: mint when `m_name.empty()`; change return type to carry the name.
- `XRIpc.cpp` `create` branch + `xrmonitorDispatch`: echo the assigned name instead of `"ok"`.
- Docs: update `docs/openxr/05-ipc-config.md` §3.1/§4 create rows for the nameless grammar and the
  name-returning reply; add the nondeterminism note (§3c) and persistence note (§3b).
- Tests: extend `hyprtester/src/tests/xr/monitors.cpp` — `create` with no name returns a name that
  then appears in `j/openxr`/`j/monitors`; two nameless creates get distinct monotonic names;
  destroy+recreate does **not** reuse the number; `create 1280x720` (nameless) parses the res as a
  mode not a name. Parser gtests for the grammar in `tests/xr/parser.cpp`.

**WP-N2 (optional, XS) — `openxr:auto_name_prefix` config var.** One `MS<String>` entry in
`ConfigValues.cpp` (§2.4). Can fold into N1 if trivial; split only if N1 is getting large.

---

## 8. Open questions for the user

1. **Collision policy: monotonic vs lowest-free.** Report recommends **monotonic** (`XR-<n>`
   never reused, matches `HEADLESS-n`/sway) for stable within-session identity. Confirm, or do you
   want small-number reuse despite the identity churn?
2. **Prefix config var** — ship `openxr:auto_name_prefix` (default `"XR-"`), or hardcode `"XR-"`?
3. **Grammar** — accept the `^\d+x\d+`/`anchor:`-first-token ⇒ nameless rule (§4.1)? It forbids
   naming a monitor literally `1920x1080` (acceptable?).
4. **Return value** — always echo the assigned name from `create` (nicer, breaks the literal
   `"ok"` reply), or echo the name only when it was auto-assigned (strict back-compat)?
5. **Persistence habit** — should `openxr layout` rewrite auto names to a placeholder/comment to
   discourage persisting nondeterministic names, or dump them verbatim as today (recommended:
   verbatim, with a doc note)?
6. **Sequencing with auto-layout** — implement WP-N1 before the auto-layout placement WP so the
   latter can assume a name is always present (§6)? Recommended yes.
