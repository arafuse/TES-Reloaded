# LOD Dither Fade — Design

Date: 2026-08-20
Status: Approved, ready for implementation planning

## Problem

Distant LOD statics, LOD terrain and full-resolution models all appear and disappear
instantaneously. Three pops are visible during ordinary travel:

1. A distant cell enters distant range and its LOD statics snap into existence.
2. A cell finishes loading, its full models snap in, and the matching LOD statics vanish.
3. The reverse of (2) when a cell unloads, plus LOD statics vanishing at the far edge and
   `LandLOD` terrain quadrants being rebuilt underfoot.

The engine offers no fade for any of these. `DISTLOD2002.vso` has a distance-driven alpha fade
(`AlphaParam`) for the far edge only; it does nothing for load events.

## Goal

Cross-dissolve every one of those transitions using a screen-space dither, over a configurable
duration defaulting to 1.0 seconds. A departing node dithers out on a rising alpha with an inverted
threshold, so it never simply pops.

The exactly-complementary construction -- two draws sharing one `StartTime`, one inverted, covering
100% between them with neither a hole nor a double-draw -- holds only where both halves provably
start in the same `Update()`. In the shipped code that is `PollLandLOD`, whose two `AddFade` calls
are adjacent. It does **not** describe the LOD-to-full handoff on cell load: the arriving full models
are detected by `PollCellGrid` and the departing LOD by `PollDistantRef`, which are independent
detections against different structures with no guaranteed pairing, so the full models dither in from
`a = 0` while the LOD keeps drawing at or near full coverage. That handoff therefore double-draws for
the duration of the fade rather than staying at exactly 100%. This is deliberate: a double-draw is a
far less visible artefact than a hole in the world, and pairing across two pollers would require
reconstructing cell identity that neither tier carries -- the distant tier has none at all, since it
diffs scene-graph children by membership rather than by cell slot.

## Scope

In scope:

- LOD statics and LOD terrain fading in when they stream in.
- LOD to full-model handoff on cell load.
- Full-model to LOD handoff on cell unload.
- LOD statics fading out when they stream out.
- Draw families: statics, trees and terrain.

Out of scope:

- Grass, actors/skin, and LOD water.
- Shadows. The existing shadow-map crossfade already smooths shadow updates, so the shadow passes
  are left untouched.
- Reference-level attach, such as an object created by a script mid-cell. Not a load-boundary pop.

## Chosen approach

Poll engine state once per frame and diff it. Rejected alternatives:

- **Scene-graph diff** — walk `DistantRefLOD` / `LandLOD` / `ObjectLODRoot` children each frame and
  diff pointer sets. Catches everything, but costs 2000+ node visits per frame and loses cell
  identity, forcing LOD-to-full pairing to be reconstructed from world position. *Adopted anyway for
  the distant tier* — see "Engine data used" — because the distant grid array's entry layout turned
  out to be unverified. The cost is one hash-set build of ~2000 pointers per frame; the lost cell
  identity is not needed, since pairing is implicit through shared `StartTime` rather than by key.
- **Engine attach/detach hooks** — exact timing and no polling, but requires reverse-engineering
  addresses this fork does not have, and a mis-hook in cell loading crashes rather than glitches.

Polling wins over attach/detach hooks because it needs no new hook addresses and degrades to a pop
rather than a crash when an assumption is wrong. What it polls differs per tier. The loaded-cell tier
uses `Tes->gridCellArray`, whose layout genuinely is reverse-engineered in `Game.h`, and the
`LandLOD` tier diffs that node's child array by slot index; both are positional diffs. The distant
tier is a **set-membership diff of `DistantRefLOD`'s scene-graph children**, adopted after
`Tes->gridDistantArray`'s entry layout proved to be unverified guesswork (see "Engine data used").

Cell identity is therefore *not* available across tiers and LOD-to-full pairing is **not** a lookup.
Pairing is implicit through a shared `StartTime` where both halves land in one `Update()`, and the
LOD-to-full handoff — which does not — deliberately double-draws instead; see the Goal section.
Detection latency of a few frames is invisible against a 1.0 s fade.

## Architecture

One new manager, `LODFadeManager` (`TESReloaded/Core/LODFadeManager.h` and `.cpp`), exposed as the
singleton `TheLODFadeManager` in `Managers.h`, alongside a shared shader include and one new
per-geometry shader constant. Three parts:

**Poller.** Runs once per frame from the existing per-frame entry point. Diffs the `DistantRefLOD`
node's children, `Tes->gridCellArray` and `Tes->LODRoot->m_children` against shadow copies, and
emits transitions. The two grid-backed tiers diff **by slot index**; the distant tier diffs **by set
membership** (see below), which is why its shadow copy is keyed by node rather than by index.

Every shadow-copy entry carries two things: the remembered node and **the `NiNode*` it was attached
to when the poller last saw it**. The distant tier holds this as an `unordered_map<NiAVObject*,
NiNode*>` and the two positional tiers as a vector of `{Node, Parent, ParentOwned}` entries. Recording
the parent is not optional bookkeeping: a departure is only ever detected one poll *after* the engine
detached the child, and detaching NULLs `m_parent`, so this is the only surviving record of where the
node belonged.

How stale that parent may be, and whether it carries a reference, **differs by tier** — see "Pinning":
the distant and `LandLOD` tiers refresh it on every poll and hold no reference on it, while the cell
tier keeps the last non-NULL answer and therefore owns a reference on it.

**Fade table.** A fixed-capacity array of `FadeRecord`. Each record holds a root `NiAVObject*`, a
start time, a pin flag, an invert flag, a flag recording the cull state a pin found, the holder node
a re-attached pin was hung under (`NULL` for an un-cull pin), and a tier string literal used only to
attribute log lines to the poller that emitted them. The alpha
always rises from 0 to 1 over the fade duration; there is no separate fade-out direction and no
partner-record link. The invert flag flips which side of the alpha the shader's per-pixel threshold
test falls on, which is what lets a departing node's rising-alpha fade-out and an arriving node's
rising-alpha fade-in — sharing a `StartTime` when they land in the same poll — sum to exactly 100%
coverage throughout the handoff; see the state-machine table and "Complementary thresholds" below.
Alongside the table, an `unordered_map<NiGeometry*, FadeRecord*>` populated lazily at draw time.

**Draw hook.** In `RenderHook::TrackSetupShaderPrograms` (`RenderHook.cpp:381`), while any fade is
live, resolve the drawn geometry to a record and publish `TESR_GEOM_FadeParams`.

With an empty fade table the manager costs one cell-grid diff, one `LandLOD` child diff and one
distant membership diff per frame and nothing else: no map probes, no constant sets, no shader work.

### Engine data used

- **`DistantRefLOD`** — the scene-graph node holding every distant static/tree LOD node, roughly
  2075 children. Reached by name: `Tes->LODRoot` is misnamed (it *is* the `LandLOD` node), so its
  `m_parent` is the real container, whose children are `LandLOD`, `DistantRefLOD` and `LODWaterRoot`;
  match `m_pcName` against `"DistantRefLOD"`. Only well-known Gamebryo fields are involved:
  `NiObjectNET::m_pcName` (0x008), `NiAVObject::m_parent`, `NiNode::m_children` and
  `NiRefObject::m_uiRefCount` (0x004), all read from `GameNi.h`'s Oblivion block (lines 1822‑3610).
  The child array is scanned to `m_children.end` rather than `numObjs`, because removing a distant
  cell NULLs its slot and decrements `numObjs` without compacting; NULL slots are skipped.

  **`Tes->gridDistantArray` is NOT used, and must not be re-introduced.** The first implementation
  read it as a `size²` array of 16-byte `DistantGridEntry` and treated `unk04` as the distant cell's
  `NiNode*` (with `unk08` / `unk0C` as signed cell X and Y). That layout was **never verified** —
  `Game.h:8109` labels all four entry fields `unk00`/`unk04`/`unk08`/`unk0C`, and the claim came from
  a reverse-engineering note that had never been exercised. In-game logging showed **all 65×65 = 4225
  slots differing from the previous frame on every frame**, which stable scene-graph pointers cannot
  do: nothing was ever detected, everything was suppressed as a discontinuity. It was also actively
  dangerous — once the shadow copies began owning references, filling a slot did
  `InterlockedIncrement(&Node->m_uiRefCount)`, a write at `Node + 4`, so the poller performed ~4225
  arbitrary memory writes per frame. `Grid->size` reading exactly 65 confirms the *outer* struct is
  right, so the fault is isolated to the entry layout. Anyone wanting the grid array back must first
  re-derive `DistantGridEntry` from disassembly and prove it with a logged diagnostic.

  **Diagnostic.** On the first successful resolve the manager logs once, gated on
  `Develop.LogLODFade`:

  ```
  [LODFade] DistantRefLOD resolved name=%s children=%d
  ```

  Expect `name=DistantRefLOD` and roughly 2075 children. A wildly different count, a different name,
  or no line at all means the traversal is wrong and must be re-derived — the point of the line is
  that a wrong assumption shows up immediately instead of after another silent session.
- `Tes->gridCellArray` (`Game.h:8125`) — the loaded-cell grid; each `GridEntry` carries a
  `TESObjectCELL*` and a `CellInfo` whose `niNode` is the cell's scene-graph node.
- `Tes->LODRoot` (`Game.h:8170`) — misnamed; it points at the `LandLOD` node holding the 12 terrain
  quadrants. Neither grid array covers these, so they get their own 12-entry watcher.

## State machine

Cell and `LandLOD` transitions are keyed by grid slot. Distant transitions are keyed by **set
membership**, not by slot: `DistantRefLOD`'s child array is compacted and its slots reused as cells
stream, so an index-keyed diff reports the whole array changed every frame — exactly the failure the
grid-array poller hit. Arrivals are nodes present now and absent from the previous set; departures
are the reverse. This is immune to reordering and to compaction. Pairing remains implicit through
shared `StartTime` (see "Complementary thresholds"), so losing per-slot cell identity costs nothing.

| Event | Action |
|---|---|
| `DistantRefLOD` gained a child | Fade the LOD node in: rising alpha, not inverted. |
| Cell slot gained a cell | Fade that cell's full models in: rising alpha, not inverted. No pin is taken for the paired distant node here — it keeps rendering at full alpha until it leaves `DistantRefLOD`, which the distant poller detects independently. |
| Cell slot lost a cell | Pin the departing cell node and fade it out on the inverted rising alpha. |
| `DistantRefLOD` lost a child | Pin the departing LOD node and fade it out on the inverted rising alpha. |
| `LandLOD` child pointer changed to a new node | Fade the new quadrant in (rising alpha, not inverted); pin the old quadrant and fade it out on the *same* rising alpha with the invert flag set. |

**Complementary thresholds.** There is no fade-out direction. Every departing node — a child leaving
`DistantRefLOD`, a cell slot losing its cell, or a `LandLOD` quadrant being replaced — is pinned and
faded using the same rising alpha a fade-in uses, with the invert flag set: the departing draw tests
`n > a` while an arriving draw (if any) tests `n < a`, against the same rising `a` and the same
per-frame noise. Pairing is implicit through shared timing rather than an explicit link: when a
departing node has a partner arriving in the same poll, both `AddFade` calls happen inside the same
`Update()` and so share a `StartTime`, which is all that is needed to keep the two draws' thresholds
exactly complementary throughout the transition — no partner field on `FadeRecord`, no cross-poller
lookup by cell coordinate. Two independent, uncoordinated fades — one declining from its own start,
one rising from its own — would instead leave roughly a quarter of the shared pixels uncovered by
either draw through the middle of the transition; the shared-`StartTime`, opposite-threshold
construction is what keeps coverage at exactly 100% instead. A lone departing node with no partner
arriving in the same poll (the common case for the distant and cell grids) still uses the inverted
test; with nothing arriving to complement, it simply presents as a plain 100%-to-0% dissolve.

### Geometry-to-record resolution

Resolution is lazy, not an up-front subtree stamp. Oblivion streams a cell's models in over several
frames, so a set stamped at detection time would miss late arrivals.

The first time a geometry is drawn during a live fade, walk `m_parent` upward to a node registered
as a fade root and cache the result in the map. A miss caches `nullptr` so the walk is never
repeated. Parent-chain depth is under ten, and the walk happens once per geometry per fade episode.

This also makes terrain free: full-resolution `Block (X, Y)` quadrants sit under the cell node and
resolve like any other geometry.

### Pinning

Pinning is the design's principal risk surface, and the only place it touches engine-owned
lifetimes.

**The measurement that settled it.** The first implementation shipped only the un-cull path below
and declined anything already detached, with the decline log line as the diagnostic. An in-game
capture returned **43 declines and 0 successful pins — every single departure declined.** The cause
is structural, not tunable: a departure is only detected one poll *after* the engine removed the
child, and removal NULLs `m_parent`, so the un-cull path can never fire for a departure. The
fade-out half of the feature had never executed once, which also made the cell-load handoff a
visible regression — the LOD popped out instantly while the full model dithered in over a second,
leaving the object semi-transparent for that second where vanilla had it solid throughout. That
measurement is what justified building the re-attach path.

At poll time, when a node departs:

- If it still has an `m_parent`, `AddRef` it, record the cull flag's prior state, and clear
  `kFlag_AppCulled` (`GameNi.h:519`) so it keeps rendering while nothing else in the engine is
  tracking it. Cheap and safe -- no lifetime is touched beyond the refcount. On release, the flag is
  restored to the recorded state rather than unconditionally cleared, since a node the engine had
  already culled must not be forced visible by the pin's release. Logged as `mode=uncull`. In
  practice this path is nearly dead for departures, per the measurement above; it remains as the
  correct answer for the case where the engine does cull in place.
- If it has already been detached -- the normal case -- it is `AddRef`ed, un-culled and **re-attached
  under a plugin-owned holder node hanging off the parent it was last observed under**. Logged as
  `mode=reattach`.

**Holders are keyed by original parent, not global.** A single holder under `WorldSceneGraph` was the
obvious construction and is wrong: render context follows position in the scene graph. A distant LOD
node renders through the `DISTLOD` shaders because it sits under `DistantRefLOD`; a loaded-cell node
renders as an ordinary object because it sits under the loaded-object root. Hanging both off one
global holder would make one of the two draw wrongly, or not at all. Keying holders by remembered
parent puts every departing node back in exactly the context it always had.

Supporting properties of the holder design:

- **Holders are never destroyed.** A pinned node is a child of its holder for the duration of a fade,
  so freeing a holder a live pin still names would fault in `Unpin`. A holder whose parent has been
  torn down is dropped from the parent-keyed map but keeps its reference, so its pointer never
  dangles. The cost is a handful of leaked 0xDC-byte nodes per session; the alternative is a
  use-after-free.
- **An orphaned cached holder declines the pin — it does not rebuild, and the stale entry is kept as
  a tombstone.** Before a cached holder is reused, `Holder->m_parent == Parent` is checked. That
  dereferences only the holder, which we own a reference to, so the test is safe even when the map
  key has since been freed and its address reused by a different node. A mismatch is the *strongest
  evidence this code ever gets* that `Parent` is freed memory — `~NiNode` NULLs its children's
  `m_parent`, so an orphaned holder is precisely the signature of a destructed parent. Building a
  fresh holder there would mean calling `AddObject` on the very pointer just concluded to be dead, so
  `GetHolder` returns NULL. The creation path validates `Parent` not at all, so this branch is the
  only place a dead parent can ever be caught.

  The entry is **retained**, not erased. Erasing made the check single-use: a second departure for
  the same parent in the same poll would miss the map and fall through to the creation path, which is
  reachable — five cell slots change on a boundary crossing and the discontinuity guard only fires
  above ten. Keeping the tombstone makes every subsequent lookup re-hit the mismatch and keep
  declining. The trade is that if the parent's address is later recycled by a live node, that node
  can never get a holder and its departures pop silently — the fail-safe direction.
- **Holders are excluded from the pollers' own diffs.** `PollDistantRef` and `PollLandLOD` walk the
  child arrays of nodes that holders attach to. Without an explicit skip a holder registers as an
  arrival, gets a fade, and eventually gets pinned under a second holder -- a self-sustaining loop.
- **Holders are attached with `FirstAvail = 0` (append), not the `1` used everywhere else in this
  fork.** `1` means "first NULL slot", not "append". Every other `AddObject` call site here attaches
  to a skeleton node, where filling a hole is harmless. A holder is different: for the `LandLOD` tier
  it is created *because* a quadrant departed, and this design's own model of a departure is "a slot
  going non-NULL to NULL", so at that moment `LandLOD->m_children` has a hole at the departed
  quadrant's index and `1` would drop a bare `NiNode` straight into it. If the engine indexes those
  dozen children positionally, or casts them to a terrain-LOD node type, that is type confusion and
  it faults. Nothing else in this fork walks `LODRoot`'s children, so this cannot be settled
  statically; appending costs nothing and removes the collision entirely.
- **The holder is given an identity local transform and a deliberately loose world bound, refreshed
  every frame.** `NiNode::New` leaves a zero world bound, and a zero-radius sphere fails the frustum
  test, so the culler would reject the holder and everything under it -- an invisible fade-out, i.e.
  the same nothing-happens outcome the decline path already produced. The bound radius is `1e6`:
  large enough never to be rejected, and deliberately *not* larger, so that if the engine ever
  recomputes a parent's bound from its children via `UpdateWorldBound` the value cannot poison the
  union up the graph and persist on an emptied holder. Each pinned child still carries its own
  correct bound and is culled on that, so the only cost is one extra node visit per frame. The
  identity local transform is written explicitly rather than trusted from `New`, because a zero scale
  there would collapse every pinned node to a point. The holder's world transform is copied from its
  parent, so a pinned child's already-computed world transform stays correct whether or not the
  engine runs an update pass over that subtree; it is re-copied for every live holder once per
  `Update()`, because this fork's LOD roots are anchor- and camera-relative and a parent that moves
  during the fade would otherwise leave the holder on a stale snapshot. No engine update pass is
  called: `UpdateDownwardPass` has no existing call site in this fork, properties and effects are
  already baked onto the geometries and an empty holder contributes none, so seeding two fields is
  both the smaller assumption and the safer one.

**Why a one-poll-old parent pointer is safe to dereference.** The remembered parent is refreshed on
every poll while the node is attached, so at departure time it is at most one poll old. For it to
dangle, the container itself must have been destructed inside that one-poll window -- and a container
dying takes all of its children with it, which every poller sees as mass churn and suppresses through
the discontinuity guard *before* any pin is attempted. The guard is therefore load-bearing for
pin safety, not only for visual sanity.

**This argument holds only for the two non-sticky tiers.** It rests entirely on "at most one poll
old", and the cell tier below deliberately breaks that. See the next section for what replaces it.

**The cell tier's remembered parent is sticky, and therefore reference-counted.** The `LandLOD` tier
observes its nodes *through* a child array, so a node's presence in the poll and a live `m_parent`
are the same fact, and an unconditional refresh is exactly right. `PollCellGrid` is different: it
observes `Grid->grid[i].info->niNode`, independently of the scene graph. If the engine detaches a
cell node before it clears the grid entry, an unconditional refresh would stamp NULL over a perfectly
good parent while the slot still reads non-NULL, and the departure would then decline with
`no parent remembered` — silently the same nothing-happens outcome the 43 declines were. The cell
tier therefore only ever overwrites its remembered parent with a **non-NULL** value.

Stickiness means that pointer *can* outlive one poll, which is precisely what the no-reference
argument above depends on. So **the sticky tier takes a reference on the remembered parent.** This
reverses the original instruction not to reference the parent — that instruction was correct while
every tier refreshed unconditionally, and stickiness is exactly what invalidated it. Owning the
pointer is the only thing that makes it safe to dereference in `Pin`.

An earlier revision claimed the orphaned-holder check in `GetHolder` backstopped the staleness
instead. **That was wrong, and it is worth recording why**, because the reasoning is easy to
reconstruct and re-adopt:

- The check was single-use — it `erase`d the entry on a mismatch. Departure 1 for a dead parent `P`
  declined correctly; departure 2 *in the same poll* then missed the map entirely and walked into the
  creation path, dereferencing `P` three times, once as a write.
- Five cell slots change on a boundary crossing and the guard fires at `Changed > 2 * gridSize` = 10,
  so five departures in one poll is a **reachable** sequence, not a theoretical one.
- Even made durable, the check cannot help a parent freed *before any holder was ever created under
  it*: there is no entry to mismatch against.

Both halves are now fixed and both are needed. The check is durable (see the tombstone note under
"Holders are keyed by original parent"), and the sticky reference means the pointer cannot be freed
while it is remembered at all. The reference is dropped whenever the slot's node changes, so a
remembered parent never outlives its node, and `ParentOwned` is stored per entry rather than per tier
so that no release site — `AssignSlot`'s own overwrite, `ReleaseParent`, `ReleaseSlots`, the
`!Player || !Tes` guard — has to know which vector it is looking at.

**Unpin ordering.** `Unpin` restores the cull flag, detaches the node from its holder, and only then
decrements. The decrement can hit zero and destruct the node, so every read and write to it has to
precede it.

The detach is **unconditional**, even when the engine has re-attached the node elsewhere in the
meantime. Skipping it in that case is the obvious resurrection guard and it is wrong: it would leave
the node in the holder's child array forever, drawn and referenced every frame. `RemoveObject` NULLs
`m_parent`, so the owner is read before the call and restored after it when it was not our holder.

**`RemoveObject` TRANSFERS the child array's reference — settled by measurement, not argument.** The
question was whether it *releases* the array's reference or *transfers* it into the out parameter,
which is really an `NiAVObjectPtr` slot. The two readings differed by a leak versus a use-after-free,
in opposite directions, so `Unpin` was shipped instrumented rather than guessed:

```
[LODFade] %s unpin root=%08X refcount %u -> %u
```

**35 re-attached pins in game all logged `refcount 2 -> 2`** — unchanged across the call. Transfer,
unambiguously. The arithmetic closes exactly: the two references at the detach are Pin's own and the
holder array's; `RemoveObject` moves the array's into `Removed`, which is discarded, so the count
holds at 2, and the single `InterlockedDecrement` left it at 1 with nothing ever dropping the last
one. Every re-attached pin was leaking its node and entire subtree — 35 of them in seconds of play.

`Unpin` therefore drops that transferred reference explicitly, **inside the holder branch only**: the
un-cull path performs no `AddObject` and no `RemoveObject`, so it never acquires the array reference
and keeps its single decrement. The extra decrement carries no `Destructor` because it provably
cannot reach zero — Pin's own reference is still outstanding — and the final decrement remains the
only one that can destruct.

This confirmed the **signature** argument: `NiAVObject** RemovedChild` is the address of an
`NiAVObjectPtr`'s raw slot, and Gamebryo's `DetachChild` AddRefs into that out-slot before releasing
the array slot, netting zero. It also vindicates discarding `EquipmentManager.cpp:636` as evidence —
its `Object->Destructor(1)` is a direct deleting-destructor call that bypasses refcounting entirely
and fits either reading.

The instrumentation is **kept as the regression test** for this. It brackets `RemoveObject` alone and
not the extra decrement, so the correct reading stays `refcount 2 -> 2` on a re-attach and `0 -> 0` on
an un-cull, which takes neither branch. Those numbers are not a bug to be "fixed" back.

The out parameter is initialised to **NULL** rather than aliasing the node, and that is the *safe*
initialisation, not merely the tidier one: if the slot really is a smart pointer's, assignment through
it releases whatever it previously held, so the aliased `RemoveObject(&Object, Object)` form used
elsewhere in this fork is the risky one. NULL also distinguishes "child not found" for free.

**Viability is tested before a record is created.** `CanPin` runs ahead of `AddFade`. Without it,
every unpinnable departure produced a `start` / `pin declined` / `retire` triple in one call --
a record created and destroyed immediately, churning `FadeSetDirty` and `LiveCount` and flooding the
log. The decline log line is kept; it is still the diagnostic.

**Log lines carry a tier tag.** `start`, `retire`, `pin`, `pin declined` and `unpin` are all prefixed
with `distant`, `cell` or `landlod`, because the previous capture could not attribute a line to a
poller and so could not confirm the LOD-to-full handoff at all.

Every pin carries a hard timeout of twice the fade time, after which it is force-released
regardless of state. The entire pin path sits behind the `PinDeparting` INI toggle, so it can be
disabled without losing the fade-in half of the feature.

### Discontinuity guard

If a single poll sees arrivals plus departures exceed a quarter of `DistantRefLOD`'s current child
count, or more than `2 * gridSize` cell slots change, or a cell purge is detected, the manager treats
it as a teleport: it resyncs the shadow copies and emits no transitions. Without this, arriving
anywhere by fast travel would dissolve the entire world in at once. The first population — the
previous distant set still empty — takes the same silent-resync path, so entering a worldspace never
dissolves in all ~2075 distant nodes at once.

The cell-slot figure is derived, not guessed. Crossing one cell boundary shifts a single row or
column of the `gridSize x gridSize` loaded grid, which is `gridSize` slots; crossing a corner shifts
a row and a column at once, `2 * gridSize - 1` slots. `2 * gridSize` therefore sits one slot above
the worst legitimate case and well below a full reload of `gridSize²`.

## Draw-time plumbing

Reuse the existing `TESR_GEOM_*` per-geometry constant channel rather than inventing one.

Add `TESR_GEOM_FadeParams` to the name map in `ShaderProgram::SetPerGeomConstantTableValue`
(`ShaderManager.cpp:514`), pointing at a new `ShaderConst.LODFade.Params` float4:

- `.x` — fade alpha
- `.y` — per-frame seed that animates the dither
- `.z` — invert flag, selecting the complementary threshold used by cross-dithered out-fades
- `.w` — unused

Coverage is derived rather than listed. `CreateCT` (`ShaderManager.cpp:671`) already enumerates the
constant table looking for the `TESR_GEOM_` prefix, so it sets a `HasFadeParams` flag on
`ShaderProgram` when it finds ours. There is no shader-name list to keep in sync.

Per draw, while the fade table is live: resolve geometry to record, write `Params`, call
`SetPerGeomCT()`. Covered draws that are *not* fading are given `1.0` explicitly, so a stale value
cannot leak from a fading draw into the next one. One final pass after the table empties resets
everything to `1.0`, after which the block goes quiet until the next fade.

## Shader side

A new shared include, `OblivionReloaded/Shaders/Includes/LODFade.hlsl`, exporting one function:

```hlsl
float4 TESR_GEOM_FadeParams : register(c110);   // c100 is TESR_GEOM_Toggles

void LODFadeClip(float2 vpos) {
    float a = TESR_GEOM_FadeParams.x;
    float z = TESR_GEOM_FadeParams.z;
    [branch]
    if (a < 1.0 || z > 0.5) {
        float n = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + TESR_GEOM_FadeParams.y);
        clip(z > 0.5 ? (n - a) : (a - n));
    }
}
```

The `[branch]` wraps the hash and `clip()` so a fully-settled draw (`a >= 1.0` and not
inverted — the common case, most pixels most of the time) skips the ~15-instruction hash body
entirely at runtime, rather than always computing it and throwing the result away. `TESR_GEOM_FadeParams`
is a per-draw constant, so the branch is uniform across the whole draw call, which is exactly what
dynamic branching is for. Verified in the compiled `ps_3_0` assembly as a real `if_lt`/`endif` pair
around the hash, not flattened into `cmp` selects.

The hash is interleaved gradient noise (`frac(52.9829189 * frac(dot(vpos, float2(0.06711056,
0.00583715))) + seed)`), not the more common `sin`-based hash. This is an instruction-budget
constraint, not a style choice: ps_3_0 has no native `sin`, so it expands into a large
range-reduction sequence, while IGN is four ALU ops. The `SM3*`/`SM3LL*` multi-light shaders in the
covered set are already large enough (up to ~480 baseline instruction slots) that the `sin` hash
pushed the largest of them (`SM3000.pso.hlsl`) over the ps_3_0 512-instruction-slot limit —
`fxc` compiles past that limit without error, so it would have failed silently at
`CreatePixelShader` time on real hardware instead of at compile time. IGN was chosen over a
cheaper 2-op R2 low-discrepancy alternative because IGN is purpose-built for dithering and
distributes better spatially; R2 produces a visible diagonal lattice. Even with IGN, `SM3000`
lands at ~509/512 — three slots of margin, the best available without dropping shaders from
coverage. Do not reintroduce the `sin` hash, and be aware that the residual cost driving the
largest files close to the limit is the `[branch]` and `clip()` scaffolding and register
pressure, not the hash itself — no cheaper hash meaningfully changes that margin.

Each covered pixel shader gains `float2 vpos : VPOS` on its input struct and one
`LODFadeClip(IN.vpos)` call at the top of `main`. `VPOS` requires ps_3_0, which is what this
pipeline compiles to.

The dither is animated per frame rather than a fixed ordered matrix. Under the mod's TAA this
resolves to a smooth cross-dissolve; with TAA disabled it reads as crawling noise for the fade
duration, which is the accepted trade.

Covered set, roughly 45 files:

- `Shaders/ExtraShaders` — `DISTLOD2001`, the `SLS1xxx` and `SLS2xxx` statics, `SM3*`, `SM3LL*`,
  `STLEAF*`
- `Shaders/Terrain` — `SLS2001`, `SLS2048`, `SLS2049`, `SLS2068`
- `Shaders/POM` — `PAR*.pso`
- `Shaders/POMExterior`

### Points to verify, not assume

- **Include resolution.** D3DX resolves nested relative includes from the top-level `.pso`
  directory while `fxc` resolves from the including file. A new top-level `Includes` directory must
  be confirmed working in both, not just under `fxc`.
- **Register choice, resolved.** These shaders receive engine-set constants by fixed register
  index, and the existing per-geometry constants sidestep that by declaring explicit high registers:
  `TESR_GEOM_Toggles : register(c100)` in the pixel shaders and
  `TESR_GEOM_EyePosition : register(c128)` in the vertex shaders. The obvious next slot, `c101`, was
  the initial choice but turned out to already be taken: `TESR_SpecularData : register(c101)` in five
  shaders this feature covers (`SLS2003`, `SLS2012`, `SLS2018`, `SLS2033`, `SLS2039`), and `c102` is
  also taken (`TESR_TerrainData` in `SLS2033`). An audit of every covered file (`ExtraShaders`,
  `Terrain`, `POM`, `POMExterior`) found `c103`+ clear, so `TESR_GEOM_FadeParams` uses `c110` —
  chosen with headroom rather than the next free slot, so a future addition doesn't repeat this
  exercise. `ShaderRecord::CreateCT` reads `RegisterIndex` back out of the constant table, so the
  explicit register is honoured end to end.
- **LOD trees.** `DistantRefLOD[0]`, the "LOD Trees" node, may not bind `DISTLOD2001`. Capture the
  bound shader name in game to confirm which shader needs covering.

## Configuration

A new `[LODFade]` section in `OblivionReloaded.ini`:

| Key | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | Master toggle for the feature. |
| `FadeTime` | 1.0 | Fade duration in seconds. |
| `PinDeparting` | 1 | Enables the fade-out half. Off leaves fade-in working and touches no engine lifetimes. |
| `MaxFades` | 256 | Fade table capacity. |

Plus `Develop.LogLODFade` to dump every emitted transition. Every per-transition line carries a tier
tag naming the poller that emitted it, so a line can be attributed without guessing:

```
[LODFade] distant start root=%08X live=%d
[LODFade] distant pin root=%08X mode=reattach
[LODFade] cell pin root=%08X mode=uncull
[LODFade] landlod pin declined, no parent remembered root=%08X
[LODFade] distant pin declined, holder unavailable root=%08X
[LODFade] cell unpin root=%08X refcount %u -> %u
[LODFade] distant retire root=%08X pinned=%d
[LODFade] holder created under parent=%08X name=%s
```

## Failure handling

Every failure mode degrades to a pop, never a crash:

- Fade table full — no fade for the overflowing transition.
- Parent-walk miss — no fade for that geometry.
- Departing node detached with no parent remembered — decline before the record is created, logged
  as `pin declined, no parent remembered`. No fade for that departure; it pops as it did before.
- Holder allocation fails — decline, logged as `pin declined, holder unavailable`.
- Pin timeout exceeded — force release.
- Teleport or cell purge — silent resync, no transitions emitted.
- `Player` or `Tes` null, as at the main menu — poller does nothing.

## Performance

Steady state, with an empty fade table, is one cell-grid diff, one `LandLOD` child diff and one
`DistantRefLOD` membership diff per frame -- the last a hash-set build of ~2075 pointers -- and no
per-draw work at all.
During a transition, both the LOD and full-model versions of the affected cell are drawn for up to
`FadeTime`, so overdraw rises for that window. Cell loads are already the most expensive moment in
exterior travel, so the added draws land where there is already a hitch rather than creating a new
one.

## Verification

This repository has no test framework, so verification is a build gate plus an in-game checklist.

Build gate:

- `fxc` compiles cleanly for every touched shader:
  `fxc /T ps_3_0 /E main /I <shaderDir> <file>`, using the SDK at
  `C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x64\fxc.exe`.
- MSBuild succeeds via the solution file, per `CLAUDE.md`.
- Shaders are recompiled in game with `[Develop] CompileShaders=1`, since edited `.hlsl` is
  otherwise ignored in favour of the cached extensionless binary.

In-game checklist:

- Walk across a cell boundary in Tamriel and confirm the LOD-to-full handoff dissolves.
- Ride away from a loaded cell and confirm the full-to-LOD handoff dissolves.
- Cross a `LandLOD` quadrant boundary and confirm the terrain quadrant dissolves.
- Watch a LOD-to-full handoff specifically for coplanar z-fighting. For the length of the fade the
  full-detail model and its LOD stand-in occupy the same space and both draw, which is a situation
  vanilla never creates; shimmering on flat faces, roof planes and terrain seams is the symptom.
- **Acceptance test for the re-attach path.** With `Develop.LogLODFade=1`, ride away from a loaded
  area and confirm the log now shows `mode=reattach` pins where it previously showed only
  `pin declined`, each followed by a matching `unpin` on the same root. A `[LODFade] holder created`
  line should appear once per container (expect roughly one for `DistantRefLOD`, one for the loaded
  object root, one for `LandLOD`) and then never again. Many `holder created` lines mean the reuse
  validation keeps failing — each failure declines a pin and drops the entry, so holders are being
  rebuilt and departures are being lost.
- **Regression-check the `refcount %u -> %u` field** on the `unpin` lines. It must read `2 -> 2` on a
  re-attached pin and `0 -> 0` on an un-cull. Anything else means the reference balance has moved and
  the extra decrement in the holder branch needs re-deriving; see "Unpin ordering".
- **Exercise the `cell` and `landlod` tiers.** The run that settled the above contained *only*
  `distant` lines — neither of the other two pollers emitted a single transition, so both remain
  entirely unverified in game despite sharing the pin, holder and refcount machinery. Cross a cell
  boundary on foot and cross a `LandLOD` quadrant boundary, watching for `cell` and `landlod` tags.
- **Read the one-shot `cell grid` and `landlod` population lines** before trusting either tier's
  silence. `PollCellGrid` logs `[LODFade] cell grid size=%d slots=%d populated=%d` and `PollLandLOD`
  logs `[LODFade] landlod children=%d populated=%d`, each once per session. Standing in a loaded
  exterior, `populated` should sit close to `slots` for the cell line (25 at the default
  `uGridsToLoad=5`) and near 12 for the `landlod` line. A `populated=0` on the cell line means
  `CellInfo::niNode` (`Game.h`) is at the wrong offset and the cell tier's silence is a bad read, not
  proof that no boundary was crossed; a low `landlod` count would point the same way at the
  `LandLOD` child-array walk instead. A healthy `populated` count alongside continued silence would
  instead mean the author simply never crossed that tier's boundary this run.
- **Read the `cell diff` lines if `cell` still emits nothing.** `PollCellGrid` also logs
  `[LODFade] cell diff: cellptr=%d niNode=%d info=%d (of %d)` (throttled to once a second, plus
  always the first non-zero poll) whenever at least one of the three candidate keys changes slot
  count against the previous poll, and a `[LODFade] cell diff sample: slot=%d cell %08X -> %08X,
  niNode %08X -> %08X` line alongside it when `cellptr` is non-zero. `niNode` is the key the poller
  already detects transitions on, so it should agree with the silence being investigated. `cellptr`
  non-zero while `niNode` stays zero confirms the persistent-container hypothesis: the engine is
  swapping which `TESObjectCELL` occupies a grid slot without ever changing the `CellInfo::niNode`
  pointer the poller diffs, so the cell tier is keyed on the wrong field and needs to switch to
  diffing `GridEntry::cell` instead. `info` distinguishes the two ways that could happen -- non-zero
  means the `CellInfo*` container itself is being replaced (and `niNode`'s silence would then be the
  real anomaly), zero means the same container is being reused and refilled underneath a stable
  `CellInfo*` (matching the hypothesis as written above).
- Fast-travel and confirm the discontinuity guard suppresses a world-wide dissolve.
- Set `PinDeparting=0` and confirm the fade-in half still works alone. This is the fallback: if the
  re-attach path crashes, shipping with `PinDeparting=0` is an acceptable outcome and forcing the
  re-attach path is not.
- Toggle TAA off and confirm the dither is noisy but not broken.
- Walk a cell boundary watching for cells that are still loaded and visible dithering out. The
  slot-keyed pollers (cell and `LandLOD`) treat any slot going from one non-NULL node to a different
  one as a departure, which is required so a direct swap is not missed — but if the engine re-indexes
  the loaded grid on a boundary crossing rather than rotating it, still-present cells produce that
  same pattern. This failure mode cannot affect the distant tier, which diffs by membership. The
  `Changed > 2 * size` discontinuity guard should mask it; this checks that it does.
- Watch a `LandLOD` quadrant that is replaced by nothing rather than by a new quadrant. The
  complementary construction only reaches exactly 100% coverage when an arrival accompanies the
  departure; a quadrant replaced by NULL dissolves into a hole rather than into its successor.
- Confirm covered geometry is visible at all with `[LODFade] Enabled=0`. The clip is compiled into
  the shaders and cannot be switched off from the INI, so opacity depends on the one forced opaque
  publish; if that regressed, statics, trees and terrain render invisible rather than unfaded.

## Risks

**Pinning a node the engine has decided to unload** is the one that can crash rather than glitch.
Refcounting should keep the node tree and its properties valid, but if the engine tears down
backing data independently of the node — `NiGeometryData::BuffData` is created lazily and managed
separately, for instance — the pinned draw dereferences freed memory. Mitigations: the hard
timeout, the `PinDeparting` toggle, and implementing the fade-in half first so the feature is
useful even if pinning proves unsafe.

**Mutating the live scene graph** is the risk the re-attach path adds on top of that. Attaching a
holder to an engine-owned container and re-parenting a detached node under it is a write into
structures the engine owns, so a mistake faults rather than glitches. The mitigations are the ones
listed under "Pinning": holders are never freed, an orphaned holder declines the pin rather than
writing into a parent it has just concluded is dead *and keeps its entry as a tombstone so that check
is durable*, holders append rather than filling a freed slot, the two non-sticky tiers' remembered
parent is never more than one poll old and a container's death is always accompanied by a suppressing
discontinuity, the sticky cell tier holds a reference so its parent cannot be freed while remembered,
and the whole path stays behind `PinDeparting`.

**Resurrection** -- the engine re-attaching a node it dropped while we still hold it under a holder
-- is handled by detaching unconditionally in `Unpin` and restoring the owner `RemoveObject` NULLs;
see "Unpin ordering". The obvious guard, skipping the detach when `m_parent` is not our holder, would
have been worse than the bug: it leaves the node in the holder's array forever, drawn and referenced
every frame.

**`RemoveObject`'s reference accounting** was the one live unknown; it is now settled by measurement
and the leak it would otherwise have caused is fixed. See "Unpin ordering". The instrumentation stays
in place as the regression test.

**The `cell` and `landlod` tiers are unverified in game.** The run that exercised the re-attach path
produced `distant` transitions only. Both other tiers share the pin, holder, refcount and sticky-parent
machinery, so the risk is that a tier-specific assumption — the cell tier's sticky parent, or the
`LandLOD` array's fixed arity — is wrong in a way `distant` traffic cannot reveal.

**TAA history rejection.** Dithered pixels change every frame by construction. If TAA's history
rejection treats the changing coverage as disocclusion, the fade may flicker rather than resolve.
Needs an in-game look before the dither pattern is considered settled.
