---
name: gpu-frame-breakdown
description: MEASURED 2026-11-07 GPU frame attribution (ProfileFrame = 2) - grass ~2.9 ms is dominated by the ~12 batches nearest the camera (overdraw), not distance; tree shadow composites split near/far; how to read the per-shader and per-batch reports and their blind spots
metadata:
  type: project
---

Measured 2026-11-07 in the user's usual exterior test scene (one shadowed point light, near water,
dense grass), after the probe-readback fix ([[d3d9-gpu-readback-stall]]). CPU ~4.7 ms vs GPU ~11 ms:
**the frame is GPU-bound**, so CPU-only wins do not move FPS here.

GPU frame ~11.0 ms:
- **Main pass ~5.9 ms**, by pixel shader:
  - **Grass (GRASS2004 + GRASS2030) ~2.7-3.0 ms**, ~50% of the main pass.
  - Mid-scene shadow apply 0.98 ms: sun 0.63, point 0.02, plus ~0.3 ms of full-screen copies.
  - STLEAF 0.4, near water 0.25-0.4, SLS2000 0.3, land SLS2048/2049 ~0.2 each, SpeedTree
    branches 0.2.
- Effect chain ~2.1 ms. VolumetricLight and AO were optimized in earlier rounds; the user asked
  to leave them alone.
- **Shadow maps ~1.1-1.3 ms.** The cascades are cached, so this is almost all the dynamic-tree
  composites: near 107 tree pieces 0.42-0.51 ms, far 688 pieces 0.66 ms (~1 us of GPU per draw,
  i.e. per-draw overhead). ShadowMapInstanced.vso cannot apply the tree wind transform. Since commit
  7dd21dc, `DynamicTreesFarInterval` (Shadows.ini ships 3) runs the far composite every Nth frame:
  GPU:ShadowExt 1.09 → 0.74 ms, and the user reported a big FPS win. GPU:TreesFar is averaged only
  over frames that ran it, so read the saving in GPU:ShadowExt.
- Residual ~1.3 ms: water reflection, HDR and the rest of the engine.

**Grass is real and is near-camera overdraw.**
- Timed per batch (ProfileFrame = 2), with batches grouped by nearest blade: within 1k units, 12
  batches cost 1.26 ms (~105 us each); 1-3k, 24 batches 0.66 ms; 3-6k, 75 batches 0.61 ms; beyond 6k,
  48 batches 0.37 ms (~8 us each).
- `grass tail+gap` measured 0.00 ms, so no engine work hides between the last grass batch and near
  water.
- About 29k instances and 1.6M triangles per frame; ~55 triangles per instance.
- Halving its vertex work for most batches saved just 0.28 ms.
- At a lower resolution it fell only 2.97 → 2.13 ms, while the shadow applies halved.
- Halving Grass.ini Min/MaxDistance (to 4500/6000) did take effect, but it only cut instances by
  27% (29.2k → 21.3k; DrawGrass culls whole land squares, so 6-9k still had 6.5k instances) and
  saved no measurable time. The far batches are cheap; distance is not the lever.
- Grass.ini `GrassDensity = 24` is silently ignored: `ApplyGrassDensitySettings` maps only 1-16
  (iMinGrassSize 240..20). Anything else leaves Oblivion.ini's own value in charge.

**Reading ProfileFrame = 2:**
- A segment runs from one pixel shader change to the next and is billed to that shader.
- Segments sum exactly to GPU:MainPass, so the coverage line in the report proves nothing is lost.
- Blind spot 1: draw counts are hook calls. Grass and SpeedTree batches pass through
  SetupShaderPrograms once per batch ([[grass-batched-per-geometry-hook]],
  [[speedtree-shader-variants]]), so they show "1 draw". Their time is still fully counted.
- Blind spot 2: a draw that bypasses the hook is billed to the shader before it.
- Grass rows (`grass b<1k` ... `b>6k`) are one segment per batch from the instance-staging hook,
  grouped by nearest blade; their `draws` column is batches per frame. `GRASS2004.pso` itself then
  shows only the sliver before the first batch.
- GPU timings jitter 0.1-0.2 ms run to run. Compare ProfileFrame = 2 runs only with each other.
- The FrameProfile distance-bucket counters fill only on frames that carry a GPU slot (every other
  frame), so their `max` column is the true per-frame count; the average is half of it.

**How to apply:** start the next GPU round from these numbers. Open candidates:
- near grass overdraw (the 12 nearest batches), which needs a visual trade-off or a cheaper
  near-grass pixel path. Ask about MSAA / driver transparency AA first; it multiplies exactly this cost;
- the near tree composite (0.42-0.51 ms every frame);
- the remaining sun PCF (0.63-0.75 ms);
- merging the mid-scene copies.
