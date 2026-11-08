---
name: grass-batch-footprints
description: "MEASURED grass batch layout - instance XY is absolute world space (identity transform), batches are at most one cell (~4160) wide, unused slots are (0,0,0,0) at the tail"
metadata:
  node_type: memory
  type: project
  originSessionId: fd68ac1b-9420-4460-a9ca-a54b0b68b933
  modified: 2026-11-02T23:06:56.930Z
---

Measured in-game 2026-11-01 by reading each batch's instance array in the `0x7E6A90` hook
(`[[prop+0x9C]+0x10]` = float4 array, UInt16 count at `[prop+0x9C]+0x0C`, 16 bytes each). Static RE
later showed `+0x0C` is the CAPACITY and `+0x0E` the used count ([[grass-manager-generation]]), which
is why the unused slots below sit at the tail:

- Every grass geometry's world transform is identity (pos 0, no rotation, scale 1), so
  `InstanceData.xy` is **absolute world XY**. The "batch-relative" collision sources are really
  world positions minus zero.
- Unused instance slots are all-zero (xy = 0, w = 0) and always at the TAIL of the array. Skip
  them (or stop at the first one) when computing a batch's footprint, or its box stretches to the
  world origin.
- A batch is at most one cell wide: width/height p50 ~2000, max ~4160. ~150-280 batches draw per
  frame, up to 228 instances each, and several batches (grass types) overlap the same ground.
- Collision demand per batch (player + in-view NPCs whose circle touches the box): in a busy area
  with 7-14 candidates, most touched batches had 1-2 sources; 0-9 per frame had 4, rarely 5
  (a group of NPCs walking in a line through one cell).

**Why:** this sizes per-batch collision source selection: 3 slots per batch cover almost every
batch, and overflow only happens when a tight group shares one cell.

**How to apply:** batch footprints come from the instance array, not the geometry bound (that is
one blade model); `GetGrassBatchBounds` in RenderHook.cpp reads it for per-batch source selection. Related: [[grass-batched-per-geometry-hook]], [[grass-vs-constant-budget]].
