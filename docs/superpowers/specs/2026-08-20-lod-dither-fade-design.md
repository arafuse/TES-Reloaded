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
duration defaulting to 1.0 seconds. During a LOD-to-full handoff the full model dithers in while
the LOD stays fully drawn behind it, and the LOD is dropped only once the full model is opaque.

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
direction, a start time, a pin flag and an optional link to the record it hands off to or from.
Alongside it, an `unordered_map<NiGeometry*, FadeRecord*>` populated lazily at draw time.

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
| Distant slot gained a node | Fade the LOD node in. |
| Cell slot gained a cell | Fade that cell's full models in; pin the paired distant node at alpha 1. On completion, unpin and let the engine drop it. |
| Cell slot lost a cell | Pin the departing cell node; fade the paired LOD node in; release the pin on completion. |
| Distant slot lost a node | Pin and fade out with its own alpha declining 1 to 0. No partner, so no complementary threshold. |
| `LandLOD` child pointer changed | Fade the new quadrant in; cross-dither the old one out against it if it is still holdable. |

**Complementary thresholds.** Where a fade-out has a partner fading in — the `LandLOD` quadrant
swap being the case that needs it — the outgoing record does not run its own alpha. It is published
with the *partner's* rising alpha and the invert flag set, so the two draws test `n < a` and `n > a`
against the same noise value and total coverage stays at exactly 100% for the whole transition. A
lone fade-out with no partner uses the normal test and its own declining alpha.

A LOD node is dropped instantly once its paired fade-in completes rather than fading out itself.
This is deliberate: fading both would halve coverage mid-transition. The cost is that any LOD
silhouette extending beyond the full model's pops, which is strictly smaller than the pop it
replaces.

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

- If it still has an `m_parent`, `AddRef` it and clear `kFlag_AppCulled` (`GameNi.h:519`). Cheap and
  safe.
- If it has already been detached, `AddRef` it and re-attach it to a plugin-owned holder node
  parented under the shadow scene node, so it culls and renders normally.

Which of the two the engine actually does is unknown until measured in game; both paths are
implemented.

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
float4 TESR_GEOM_FadeParams;   // no explicit register - the constant table allocates it

void LODFadeClip(float2 vpos) {
    float a = TESR_GEOM_FadeParams.x;
    float n = frac(sin(dot(vpos + TESR_GEOM_FadeParams.y, float2(12.9898, 78.233))) * 43758.5453);
    float d = TESR_GEOM_FadeParams.z > 0.5 ? (n - a) : (a - n);
    clip(a >= 1.0 ? 1.0 : d);
}
```

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
- **Register collision.** These shaders also receive engine-set constants by fixed register index.
  Existing `TESR_` constants in the same files coexist without trouble, so follow that convention
  and diff the compiled register map to confirm the new constant did not land on an engine-written
  register.
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
