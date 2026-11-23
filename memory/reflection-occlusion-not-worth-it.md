---
name: reflection-occlusion-not-worth-it
description: "MEASURED 2026-11-21 kill gate: occlusion-culling the water reflection pass saved 0.47 ms in the dense town (under the 1 ms bar); its 253 skipped roots removed only 29 of ~3,250 reflection draws; shelved and reverted (c65e614)"
metadata:
  type: project
---

**Decision (agreed with the user before measuring): shelved.** The code was reverted in c65e614, which restores the tree to ef09f63. That commit keeps the profiler's per-pass `reflection` draw row. The design, plan and code are in git: spec `docs/superpowers/specs/2026-11-20-reflection-occlusion-culling-design.md`, code dbba6de..98699c6.

**What was built:**
- Reflection candidates come from the far pass's cell walk: bounds above the water plane and beyond `OcclusionAlwaysDrawDistance`.
- One frame late, the reflection's per-node cull (0x49CBF0) skipped the roots whose query had come back with zero pixels.
- The queries were issued at the reflection pass's `EndScene` call (0x49C81B), against the reflection's own depth.
- Engine facts it relied on are in [[water-reflection-pass-detection]].

**Measured at the dense town spot** (where only a thin strip of water is visible; DXVK; the far-pass culler on; `OcclusionCullDebug = 1`):

| | Off | On |
|---|---|---|
| PresentMon median frame (60 s, standing still) | 28.18 ms | 27.71 ms (**-0.47**) |
| PresentMon CPU busy | 28.02 ms | 27.56 ms |
| PresentMon GPU busy | 7.16 ms | 7.31 ms |
| Reflection draws per frame | 3,258 | 3,229 (**-29**) |
| Own CPU (`Occ:ReflBegin` + `Occ:ReflIssue`) | 0 | 0.20 + 0.16 = 0.36 ms |

- **Queries were healthy:**
  - 1,319 candidates, 1,214 queried, all results ready one frame later.
  - 504 hidden (41 %, against 87 % in the far pass: the mirrored camera looks up past the walls).
- **The lever is small:**
  - Only 253 of the 504 hidden roots ever reached the reflection's per-node cull; the rest never reach it.
  - Those 253 skips removed only 29 draws.
  - Likely cause (not verified): the engine's own reflection cull already drops most of those objects, through the size test in `TrackWaterReflectionCull` or earlier.
  - The ~0.47 ms frame gain is the same size as run-to-run noise. Grass instances differed between the runs (463 vs 481), so the camera was not exactly the same.

**If revisited:** count first which reflection draws belong to roots that are both hidden and NOT already rejected by the engine's reflection cull. Without that, culling the reflection can't beat its ~0.36 ms own cost by much. Related: [[occlusion-culling]], [[static-draw-instancing-not-worth-it]].
