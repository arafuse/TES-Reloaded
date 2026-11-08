---
name: many-actors-havok-cost
description: "MEASURED - \"many actors is slow\" is Havok character-proxy collision (NOT AI detection); addresses, per-actor cost, and the fixed-timestep feedback loop"
metadata:
  type: project
---

Measured with the in-plugin sampler (see [[sampling-profiler]]), baseline vs 100 `placeatme`
clones in an exterior:

| run | FPS | frame | update busy | render |
|---|---|---|---|---|
| baseline (capped) | 90 | 11.1 ms | 1.9 ms | 5.2 ms |
| 100 clones, camera away | 60 | 16.7 ms | 7.8 ms | 8.9 ms |
| 100 clones, facing | 30 | 33.3 ms | 17.1 ms | 16.3 ms |

Cost splits nearly evenly: **+15 ms/frame engine update, +11 ms/frame render.**

**The engine half is Havok character proxies, NOT AI detection.**
`Calc_DetectionLevel` (0x5463F0) appeared in no top-40 list - the usual O(n²) detection theory is
wrong. Every hot update address is in 0x8CE000-0x905000, bracketed by `bhk*`/`hk*` symbols:

- `sub_8D1A30` = `closestPointSegmentSegment` (capsule-collision inner kernel, x87 + SSE1).
- `sub_8CE770` = **nested O(n²) contact-manifold prune** over 48-byte contact records
  (`[esi+0x10]` array, `[esi+0x14]` count), calling `sub_8CE690` per pair.
- `sub_8CED20` = per-step character-proxy contact update; calls `sub_8CE770` at `+0x3B2`.
  Sits between `bhkCharacterPointCollector` (0x8CEA50) and `bhkCharacterStateOnGround` (0x8CFA50).
- Others in the band: `sub_8D1EF0`, `sub_8D0CA0`, `sub_8D0A10`, `sub_8E4590`,
  `sub_9050F0` (high inclusive - near the top of the Havok step).

Cost ≈ **3.2 ms of CPU per simulated second per actor**.

**Fixed-timestep feedback loop:** update cost scales with FRAME DURATION, not frame count (~470-510 ms
per simulated second at both 60 and 30 FPS). Dropping frames buys more Havok substeps per frame, which
drops more frames. Normalize FPS comparisons per simulated second, not per frame.

**Part of the render half is ours:** off-camera actors still cost +3.65 ms/frame, with
`ShadowManager::RenderActorOverlay` at 41.5% inclusive and `CollectExteriorGeo`/`DrawGeoArrays`
entering the exclusive top-40 only when clones are present - the shadow actor overlay draws actors
that cannot be seen.

**Unresolved:** linear vs quadratic. `placeatme` piles clones together, which is exactly what the O(n²)
prune feeds on. A 50-clone run discriminates (linear ~310 ms/s, quadratic ~200).

**Resolved (2026-11-07):** the "GPU spin-wait" d3d9.dll address (`Direct3DCreate9On12Ex+0x145535`) is
CPU time: DXVK's per-call device lock, see [[game-runs-on-dxvk]].

**Natural combat is different (market district, 2026-11-07, 73.8 FPS):**
- **CPU-bound:** 13.55 ms frame, with no Present wait. Update 5.3 ms, render 8.2 ms, GPU 11.4 ms.
- **The Havok band barely registers.** Its only top-40 site is `sub_8E1200` at 0.012 ms. The update
  phase is flat: 5900 distinct sites, none above 0.13 ms. Piled clones are what make Havok expensive.
- **Our shadows are 3.0 ms of main-thread CPU:** `Pass:Skin` 1.89 ms (545 actor items) and a point
  cube rebaked every frame at 0.96 ms. They issue **33,300 DrawIndexedPrimitive calls per frame**:
  `DrawGeoArrays` makes one draw per strip, about 43 per actor item and 16 per cube geometry.
  `RenderSkinnedGeo` adds one `SetVertexShaderConstantF` per bone and two full `SetCT` calls per
  partition. Every one of those calls also pays DXVK's device lock.
  **Fixed in b6d8ac8** (`[Exteriors] StitchStrips`, plus one bone upload per partition):
  - draws: 33,306 → 936/frame, with 0 fallbacks (every multi-strip buffer has a CPU `IndexArray`);
  - shadow CPU: 3.00 → 1.79 ms;
  - shadow GPU: 2.98 → 1.89 ms (the tiny draws cost GPU time too).
  What remains is `Pass:Skin` at 1.05 ms (540 pieces) and the player's torch cube rebaked every frame
  (`CanCarry`): `Point:Bake` 0.59 ms CPU, `GPU:ShadowPoint` 0.87 ms.
- **Actor overlay split (1f0cc02 timers, 2026-11-08, 620 items):** `[ExteriorsSkin] Enabled = 0` saves
  1.47 ms CPU and 1.10 ms GPU (`GPU:ActorOverlay`). Per skinned partition: Pack 1.0 µs, Bones 0.3,
  Setup 0.28, Draw 0.2. `Rigid:Pack` is the biggest timer (0.95 ms/frame): ~0.6 µs for a clean static,
  ~4 µs for actor rigid pieces (likely morphing FaceGen heads, unverified).
  **Fixed in 3e5d5f8** (`[ExteriorsSkin] ViewCull`): a caster draws only if its bound, swept 2×radius
  away from the sun, crosses the view's near/left/right/bottom planes or the player's keep sphere
  (the sun-shadow probe). Market combat: 130 of 190 refs culled, ActorItems 620 → 202,
  `Pass:Skin` 1.31 → 0.43 ms, `GPU:ActorOverlay` 1.11 → 0.32 ms, shadow CPU 2.46 → 1.50 ms. No
  visible change reported. The cull ratio depends on where the camera points.
- **The cube re-packs per face, and must:** dd07726 packed each item once per bake and was
  reverted (b8cf27d) for drawing stray triangles, see [[volatile-geometry-pack-reuse]].
- **Carried-light cube view cull (9010770, `[Point] ViewCull`):** a torch cube rebakes every frame,
  so casters whose shadow cone (light → bound → far plane) misses the view are dropped, and the
  slot is marked valid for that frame only. Visually confirmed in both views. Facing the market
  crowd: 57 of 342 casters culled (17%), `Point:Bake` 0.95 → 0.69 ms; the overlay cull kept 236 of
  288 refs there (18% culled, `Pass:Skin` back at 1.36 ms). Both culls pay off when looking away
  from the crowd and very little when facing it.
- **Tiny-piece cutoff (95929f9, `[ExteriorsSkin] MinRadius = 4`):** facing the crowd, 76 of 626
  overlay pieces are under radius 4 (29 under 2), only 18 more under 8, but 258 under 16 (hands,
  feet, heads: visible). `Pass:Skin` 1.36 → 1.18 ms, `GPU:ActorOverlay` 1.12 → 1.06 ms, no visible
  change. Raising it past 4 buys almost nothing. Overlay cost is ~2.1 µs CPU per piece drawn. Actors inside the torch cube reach `RenderSkinnedGeo` through `Render()`, so the
  `CubeActorGeo` counter is stale (always 0). Cube instancing (a80f8a0) merges only ~54 of ~760
  cube items/frame in the market: its meshes are mostly unique.
- **Engine CPU vertex packing, ~1.1-1.6 ms, is the engine's own:** a loop in `sub_76E610`
  (+0x192..+0x1D0) under `sub_76F590` ← `sub_76FE20`, plus `sub_776D80`, which memcpys into a locked
  VB, unlocks it (vtable `+0x30`) and leaves a critical section. It did NOT drop with actor shadows
  off. Attributed callers: `NiDX9Renderer::PackGeometryBuffers+0x28F` ~45%,
  `PackSkinnedGeometryBuffer` ~2%; the rest are stack-residue chains. Shadow packs of dirty
  geometry most likely just come first and spare the main pass its pack.

**How to apply:** optimize per-actor physics stepping and our shadow overlay culling, not AI.
Havok is statically linked, so the engine-side lever is hooking the per-actor character-proxy step to
skip or decimate it for actors that don't need full-rate physics. See [[oblivion-pdb-symbols]] for how
these addresses were named.
