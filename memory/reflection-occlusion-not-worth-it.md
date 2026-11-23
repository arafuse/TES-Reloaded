---
name: reflection-occlusion-not-worth-it
description: "Shelved after a kill gate: occlusion-culling the water reflection pass saved 0.47 ms in the dense town (under the 1 ms bar); 253 skipped roots removed only 29 of ~3,250 reflection draws"
metadata:
  type: project
---

**Decision (agreed with the user): the water reflection pass is not occlusion-culled.** The far-pass
culler ([[occlusion-culling]]) does not touch it.

**What was tried:** reflection candidates from the far pass's cell walk (bounds above the water plane
and beyond `OcclusionAlwaysDrawDistance`), queried at the reflection pass's `EndScene` call (0x49C81B)
against the reflection's own depth, then skipped one frame late in the reflection's per-node cull
(0x49CBF0). Engine facts it relied on are in [[water-reflection-pass-detection]].

**Measured at the dense town spot** (DXVK, far-pass culler on, PresentMon 60 s standing still):
- Median frame 28.18 → 27.71 ms (-0.47, the size of run-to-run noise); GPU busy unchanged.
- Reflection draws 3,258 → 3,229 (-29); own CPU 0.36 ms (`Occ:ReflBegin` + `Occ:ReflIssue`).
- Queries were healthy: 1,214 queried, 504 hidden (41 %, against 87 % in the far pass: the mirrored
  camera looks up past the walls). But only 253 hidden roots ever reached the reflection's per-node
  cull, and skipping them removed only 29 draws. Likely (not verified): the engine's own reflection
  cull (the size test in `TrackWaterReflectionCull`, or earlier) already drops most of them.

**If revisited:** count first which reflection draws belong to roots that are both hidden and NOT
already rejected by the engine's reflection cull. Without that, culling the reflection can't beat its
~0.36 ms own cost by much. Related: [[dense-town-cpu-budget]], [[static-draw-instancing-not-worth-it]].
