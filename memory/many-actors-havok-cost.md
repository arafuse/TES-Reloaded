---
name: many-actors-havok-cost
description: "MEASURED 2026-09-08 - \"many actors is slow\" is Havok character-proxy collision (NOT AI detection); addresses, per-actor cost, and the fixed-timestep feedback loop"
metadata: 
  node_type: memory
  type: project
  originSessionId: 88be16ed-3132-4290-9a9c-b47a9192e4cc
  modified: 2026-09-09T21:02:45.188Z
---

Profiled with the in-plugin sampler (see [[sampling-profiler]]) on 2026-09-08:
baseline vs 100 `placeatme` clones, exterior, ~35 s per run.

**Per-frame budget** (converted from sample shares; the baseline was frame-capped
at 90 and idling 4.0 ms/frame in `NtWaitForAlertByThreadId`, so its raw percentages
badly understate the real work):

| run | FPS | frame | update busy | render |
|---|---|---|---|---|
| baseline | 90 | 11.1 ms | 1.9 ms | 5.2 ms |
| 100 clones, camera away | 60 | 16.7 ms | 7.8 ms | 8.9 ms |
| 100 clones, facing | 30 | 33.3 ms | 17.1 ms | 16.3 ms |

Cost splits nearly evenly: **+15 ms/frame engine update, +11 ms/frame render.**

**The engine half is Havok character proxies, NOT AI detection.**
`Calc_DetectionLevel` (0x5463F0) appeared in no top-40 list - the usual O(n^2)
detection theory is wrong for this fork. Every hot update address is in
0x8CE000-0x905000, bracketed by `bhk*`/`hk*` symbols:

- `sub_8D1A30` = `closestPointSegmentSegment` (4 vec4 args, dot products, the
  `D1D1*D2D2 - a^2` denominator with degenerate clamps). Capsule-collision inner
  kernel, compiled as x87 + SSE1 `shufps` horizontal dots.
- `sub_8CE770` = **nested O(n^2) contact-manifold prune** over 48-byte contact
  records (`[esi+0x10]` array, `[esi+0x14]` count), calling `sub_8CE690` per pair
  and swap-removing redundant points. All hot offsets are the inner loop.
- `sub_8CED20` = per-step character-proxy contact update; calls `sub_8CE770` at
  `+0x3B2`. Sits between `bhkCharacterPointCollector` (0x8CEA50) and
  `bhkCharacterStateOnGround` (0x8CFA50).
- Others in the band: `sub_8D1EF0`, `sub_8D0CA0`, `sub_8D0A10`, `sub_8E4590`,
  `sub_9050F0` (high inclusive - near the top of the Havok step).

Cost ~= **3.2 ms of CPU per simulated second per actor**.

**Fixed-timestep feedback loop:** update cost scales with FRAME DURATION, not
frame count (7.8 ms/frame at 60 FPS, 17.1 at 30; ~470-510 ms per simulated second
either way). Dropping frames buys more Havok substeps per frame, which drops more
frames. Any FPS comparison must normalize per simulated second, not per frame.

**The render half is partly ours:** off-camera actors still cost +3.65 ms/frame,
with `ShadowManager::RenderActorOverlay` at 41.5% inclusive and
`CollectExteriorGeo`/`DrawGeoArrays` entering the exclusive top-40 only when clones
are present - the shadow actor overlay draws actors that cannot be seen.

**Open questions when this was written:** (1) linear vs quadratic - `placeatme`
piles clones together so every proxy's manifold is full of other proxies, which is
exactly what the O(n^2) prune feeds on; a 50-clone run discriminates (linear ~310
ms/s, quadratic ~200). (2) render half CPU vs GPU - 17.6% of render samples sit on
one d3d9.dll address that looks like a GPU spin-wait; `ProfileFrame=1` GPU
timestamps settle it.

**Why:** it redirects optimization away from AI/detection (where the community
folklore points) and toward per-actor physics stepping plus our own shadow overlay
culling.

**How to apply:** Havok is statically linked and cannot be recompiled, so the
engine-side lever is hooking the per-actor character-proxy step to skip or decimate
it for actors that don't need full-rate physics. See [[oblivion-pdb-symbols]] for
how these addresses were named.
