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

**Unresolved:** (1) linear vs quadratic — `placeatme` piles clones together, which is exactly what
the O(n²) prune feeds on; a 50-clone run discriminates (linear ~310 ms/s, quadratic ~200). (2) render
half CPU vs GPU — 17.6% of render samples sit on one d3d9.dll address that looks like a GPU
spin-wait; `ProfileFrame=1` GPU timestamps would settle it.

**How to apply:** optimize per-actor physics stepping and our shadow overlay culling, not AI.
Havok is statically linked, so the engine-side lever is hooking the per-actor character-proxy step to
skip or decimate it for actors that don't need full-rate physics. See [[oblivion-pdb-symbols]] for how
these addresses were named.
