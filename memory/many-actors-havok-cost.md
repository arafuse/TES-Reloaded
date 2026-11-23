---
name: many-actors-havok-cost
description: "\"Many actors is slow\" is Havok character-proxy collision (NOT AI detection) when actors are piled together; fixed-timestep feedback loop; natural combat is CPU-bound but Havok-light; OR's actor shadow overlay cost and its culls (StitchStrips, ViewCull, MinRadius, point ViewCull); engine vertex packing is the engine's own"
metadata:
  type: project
---

Measured with the in-plugin sampler ([[sampling-profiler]]).

**Piled `placeatme` clones (100 in an exterior):**

| run | FPS | frame | update busy | render |
|---|---|---|---|---|
| baseline (capped) | 90 | 11.1 ms | 1.9 ms | 5.2 ms |
| 100 clones, camera away | 60 | 16.7 ms | 7.8 ms | 8.9 ms |
| 100 clones, facing | 30 | 33.3 ms | 17.1 ms | 16.3 ms |

**The engine half is Havok character proxies, NOT AI detection.** `Calc_DetectionLevel` (0x5463F0)
appeared in no top-40 list. Every hot update address is in 0x8CE000-0x905000, bracketed by
`bhk*`/`hk*` symbols:
- `sub_8D1A30` = `closestPointSegmentSegment` (capsule-collision inner kernel, x87 + SSE1).
- `sub_8CE770` = **nested O(n²) contact-manifold prune** over 48-byte contact records
  (`[esi+0x10]` array, `[esi+0x14]` count), calling `sub_8CE690` per pair.
- `sub_8CED20` = per-step character-proxy contact update; calls `sub_8CE770` at `+0x3B2`.
  Sits between `bhkCharacterPointCollector` (0x8CEA50) and `bhkCharacterStateOnGround` (0x8CFA50).
- Others in the band: `sub_8D1EF0`, `sub_8D0CA0`, `sub_8D0A10`, `sub_8E4590`,
  `sub_9050F0` (high inclusive - near the top of the Havok step).

Cost ≈ **3.2 ms of CPU per simulated second per actor** when piled. Unresolved: linear vs quadratic
(a 50-clone run discriminates: linear ~310 ms/s, quadratic ~200).

**Fixed-timestep feedback loop:** update cost scales with FRAME DURATION, not frame count (~470-510 ms
per simulated second at both 60 and 30 FPS). Dropping frames buys more Havok substeps per frame, which
drops more frames. Normalize FPS comparisons per simulated second, not per frame.

**Natural combat is different (market district):** CPU-bound with no Present wait; update ~5.3 ms,
flat (5900 distinct sites, none above 0.13 ms); the Havok band barely registers. Piled clones are what
make Havok expensive.

**OR's actor shadow overlay** (`ShadowManager::RenderActorOverlay`, `Pass:Skin`) draws actors into the
sun shadow per frame, and the player's torch cube rebakes every frame (`CanCarry`). Current levers,
all on by default in Shadows.ini:
- `[Exteriors] StitchStrips`: one draw per buffer instead of one per strip, and one bone upload per
  partition (shadow draws in combat 33k → ~0.9k/frame; every multi-strip buffer has a CPU `IndexArray`).
- `[ExteriorsSkin] ViewCull`: a caster draws only if its bound, swept 2×radius away from the sun,
  crosses the view's near/left/right/bottom planes or the player's keep sphere (the sun-shadow probe).
  Market combat looking away from the crowd: ActorItems 620 → 202, `Pass:Skin` 1.31 → 0.43 ms,
  `GPU:ActorOverlay` 1.11 → 0.32 ms; facing the crowd it culls only ~18%.
- `[ExteriorsSkin] MinRadius = 4`: skips tiny pieces (`Pass:Skin` 1.36 → 1.18 ms facing the crowd).
  Raising it past 4 buys almost nothing (258 pieces under radius 16 are hands, feet, heads: visible).
- `[Point] ViewCull`: torch-cube casters whose shadow cone (light → bound → far plane) misses the view
  are dropped, and the slot is valid for that frame only (`Point:Bake` 0.95 → 0.69 ms facing the crowd).
- Overlay cost is ~2.1 µs CPU per piece drawn (per skinned partition: Pack 1.0 µs, Bones 0.3, Setup
  0.28, Draw 0.2). `[ExteriorsSkin] Enabled = 0` would save ~1.5 ms CPU / 1.1 ms GPU in unculled combat.
- The cube re-packs per face, and must ([[volatile-geometry-pack-reuse]]). Cube instancing merges only
  ~54 of ~760 cube items/frame in the market: its meshes are mostly unique.
- Actors inside the torch cube reach `RenderSkinnedGeo` through `Render()`, so the `CubeActorGeo`
  counter is stale (always 0).

**Engine CPU vertex packing, ~1.1-1.6 ms, is the engine's own:** a loop in `sub_76E610`
(+0x192..+0x1D0) under `sub_76F590` ← `sub_76FE20`, plus `sub_776D80`, which memcpys into a locked
VB, unlocks it (vtable `+0x30`) and leaves a critical section. It does NOT drop with actor shadows
off. `PackGeometryBuffers` totals ~0.79 ms/frame in combat, only ~0.41 ms of it avoidable
([[dxvk-combat-cpu-headroom]]).

**How to apply:** optimize per-actor physics stepping and our shadow overlay culling, not AI.
Havok is statically linked, so the engine-side lever is hooking the per-actor character-proxy step to
skip or decimate it for actors that don't need full-rate physics. Mind the proxy O(n²) cost before
enabling more actor collision ([[havok-layer-collision-matrix]]). The d3d9.dll "spin-wait" address
in these profiles is DXVK's per-call device lock, i.e. CPU time ([[game-runs-on-dxvk]]).
