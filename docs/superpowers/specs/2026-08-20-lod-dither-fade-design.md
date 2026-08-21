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
duration defaulting to 1.0 seconds. During a LOD-to-full handoff the full model dithers in while the
departing LOD dithers out concurrently on the same rising alpha with an inverted threshold, so the
two draws are always exactly complementary and total coverage stays at 100% for the whole handoff --
neither a hole nor a double-draw at any point during the transition.

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

Poll the engine's grid arrays and diff them. Rejected alternatives:

- **Scene-graph diff** — walk `DistantRefLOD` / `LandLOD` / `ObjectLODRoot` children each frame and
  diff pointer sets. Catches everything, but costs 2000+ node visits per frame and loses cell
  identity, forcing LOD-to-full pairing to be reconstructed from world position.
- **Engine attach/detach hooks** — exact timing and no polling, but requires reverse-engineering
  addresses this fork does not have, and a mis-hook in cell loading crashes rather than glitches.

Polling wins because the grid arrays are already reverse-engineered in `Game.h` and carry cell
identity, which turns LOD-to-full pairing into a lookup. Detection latency of a few frames is
invisible against a 1.0 s fade.

## Architecture

One new manager, `LODFadeManager` (`TESReloaded/Core/LODFadeManager.h` and `.cpp`), exposed as the
singleton `TheLODFadeManager` in `Managers.h`, alongside a shared shader include and one new
per-geometry shader constant. Three parts:

**Poller.** Runs once per frame from the existing per-frame entry point. Diffs
`Tes->gridDistantArray`, `Tes->gridCellArray` and `Tes->LODRoot->m_children` against shadow copies
of their slot-to-`NiNode*` mappings, and emits transitions.

**Fade table.** A fixed-capacity array of `FadeRecord`. Each record holds a root `NiAVObject*`, a
start time, a pin flag, an invert flag and a flag recording the cull state a pin found. The alpha
always rises from 0 to 1 over the fade duration; there is no separate fade-out direction and no
partner-record link. The invert flag flips which side of the alpha the shader's per-pixel threshold
test falls on, which is what lets a departing node's rising-alpha fade-out and an arriving node's
rising-alpha fade-in — sharing a `StartTime` when they land in the same poll — sum to exactly 100%
coverage throughout the handoff; see the state-machine table and "Complementary thresholds" below.
Alongside the table, an `unordered_map<NiGeometry*, FadeRecord*>` populated lazily at draw time.

**Draw hook.** In `RenderHook::TrackSetupShaderPrograms` (`RenderHook.cpp:381`), while any fade is
live, resolve the drawn geometry to a record and publish `TESR_GEOM_FadeParams`.

With an empty fade table the manager costs one grid diff per frame and nothing else: no map probes,
no constant sets, no shader work.

### Engine data used

- `Tes->gridDistantArray` (`Game.h:8109`) — a `size²` array of 16-byte `DistantGridEntry`, where
  `unk04` is the distant cell's `NiNode*` and `unk08` / `unk0C` are its signed cell X and Y.
  `size = uGridsToLoad + 2 * uGridDistantCount`, 65 in practice.
- `Tes->gridCellArray` (`Game.h:8125`) — the loaded-cell grid; each `GridEntry` carries a
  `TESObjectCELL*` and a `CellInfo` whose `niNode` is the cell's scene-graph node.
- `Tes->LODRoot` (`Game.h:8170`) — misnamed; it points at the `LandLOD` node holding the 12 terrain
  quadrants. Neither grid array covers these, so they get their own 12-entry watcher.

## State machine

All transitions are keyed by grid slot, so pairing is a lookup rather than a positional heuristic.

| Event | Action |
|---|---|
| Distant slot gained a node | Fade the LOD node in: rising alpha, not inverted. |
| Cell slot gained a cell | Fade that cell's full models in: rising alpha, not inverted. No pin is taken for the paired distant node here — it keeps rendering at full alpha until its own distant slot changes, which the distant poller detects independently. |
| Cell slot lost a cell | Pin the departing cell node and fade it out on the inverted rising alpha. |
| Distant slot lost a node | Pin the departing LOD node and fade it out on the inverted rising alpha. |
| `LandLOD` child pointer changed to a new node | Fade the new quadrant in (rising alpha, not inverted); pin the old quadrant and fade it out on the *same* rising alpha with the invert flag set. |

**Complementary thresholds.** There is no fade-out direction. Every departing node — a distant slot
losing its node, a cell slot losing its cell, or a `LandLOD` quadrant being replaced — is pinned and
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

At poll time, when a node departs:

- If it still has an `m_parent`, `AddRef` it, record the cull flag's prior state, and clear
  `kFlag_AppCulled` (`GameNi.h:519`) so it keeps rendering while nothing else in the engine is
  tracking it. Cheap and safe -- no lifetime is touched beyond the refcount. On release, the flag is
  restored to the recorded state rather than unconditionally cleared, since a node the engine had
  already culled must not be forced visible by the pin's release.
- If it has already been detached, the pin is declined and logged rather than attempted. Re-attaching
  a genuinely detached node means creating or reusing a plugin-owned holder node and manipulating the
  live scene graph, which is materially riskier than the un-cull path above, and is a separate,
  conditional task gated on measuring in game how often this decline path is actually hit.

Only the un-cull path above is implemented. Which of the two the engine actually does -- cull in
place, or detach outright -- is unknown until measured in game; the decline log line is that
measurement.

Every pin carries a hard timeout of twice the fade time, after which it is force-released
regardless of state. The entire pin path sits behind the `PinDeparting` INI toggle, so it can be
disabled without losing the fade-in half of the feature.

### Discontinuity guard

If a single poll sees more than a quarter of the distant slots change, or more than `2 * gridSize`
cell slots change, or a cell purge is detected, the manager treats it as a teleport: it resyncs the
shadow copies and emits no transitions. Without this, arriving anywhere by fast travel would
dissolve the entire world in at once.

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

Plus `Develop.LogLODFade` to dump every emitted transition.

## Failure handling

Every failure mode degrades to a pop, never a crash:

- Fade table full — no fade for the overflowing transition.
- Parent-walk miss — no fade for that geometry.
- Pin timeout exceeded — force release.
- Teleport or cell purge — silent resync, no transitions emitted.
- `Player` or `Tes` null, as at the main menu — poller does nothing.

## Performance

Steady state is one grid diff per frame with an empty fade table, and no per-draw work at all.
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
- Fast-travel and confirm the discontinuity guard suppresses a world-wide dissolve.
- Set `PinDeparting=0` and confirm the fade-in half still works alone.
- Toggle TAA off and confirm the dither is noisy but not broken.

## Risks

**Pinning a node the engine has decided to unload** is the one that can crash rather than glitch.
Refcounting should keep the node tree and its properties valid, but if the engine tears down
backing data independently of the node — `NiGeometryData::BuffData` is created lazily and managed
separately, for instance — the pinned draw dereferences freed memory. Mitigations: the hard
timeout, the `PinDeparting` toggle, and implementing the fade-in half first so the feature is
useful even if pinning proves unsafe.

**TAA history rejection.** Dithered pixels change every frame by construction. If TAA's history
rejection treats the changing coverage as disocclusion, the fade may flicker rather than resolve.
Needs an in-game look before the dither pattern is considered settled.
