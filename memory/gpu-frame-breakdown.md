---
name: gpu-frame-breakdown
description: GPU frame attribution in the usual exterior test scene (ProfileFrame = 2) - frame is GPU-bound (~9.2 ms on DXVK 3.1.1); grass ~half the main pass, dominated by the ~12 batches nearest the camera (distance is not the lever); tree shadow composites; how to read the per-shader and per-batch reports; user's no-go list
metadata:
  type: project
---

Measured in the user's usual exterior test scene (one shadowed point light, near water, dense
grass). **The frame is GPU-bound** (CPU envelope ~3.6 ms vs GPU ~9.2 ms on DXVK 3.1.1; the sampler
shows a ~4 ms Present wait), so CPU-only wins do not move FPS here. Counters: ~29k grass instances,
160 batches, tree pieces 107 near / 688 far.

**GPU frame (~9.2 ms on 3.1.1; per-shader rows below were taken on DXVK 2.7.1 at 11.0 ms, so re-run
ProfileFrame = 2 before trusting them):**
- **Main pass ~5.0 ms** (5.9 on 2.7.1), by pixel shader:
  - **Grass (GRASS2004 + GRASS2030) ~2.7-3.0 ms**, ~50% of the main pass.
  - Mid-scene sun shadow apply (`GPU:MidSun`) ~0.27 ms; point ~0.01. The applies output a shadow
    FACTOR that the GPU multiplies into the scene (DESTCOLOR blend); one full-screen copy remains
    because near water refracts TESR_RenderedBuffer. A compiled effect without the
    `ShadowOutputsFactor` marker falls back to the old read-and-copy path per pass.
  - STLEAF 0.4, near water 0.25-0.4, SLS2000 0.3, land SLS2048/2049 ~0.2 each, SpeedTree branches 0.2.
- Effect chain ~2.1 ms. VolumetricLight and AO are already optimized; the user asked to leave them alone.
- **Shadow maps (`GPU:ShadowExt` ~0.64-0.74 ms).** The cascades are cached, so this is almost all the
  dynamic-tree composites (~1 us of GPU per draw). Near 107 pieces from 22 buffers, far 688 from 45;
  `InstanceDynamicTrees` (default 1) instances them; `DynamicTreesFarInterval` (Shadows.ini ships 3)
  runs the far composite every Nth frame. GPU:TreesFar is averaged only over frames that ran it, so
  read the saving in GPU:ShadowExt. Most of the remaining near composite is per-vertex and per-pixel
  work (overlapping leaf cards MIN-blended into R32F), which batching cannot remove. The static-bake →
  live-map copy is only ~0.03-0.06 ms.
  - The user does NOT want a near-map tree interval (visible stutter risk); don't propose it again.
- Residual ~1.3 ms: water reflection, HDR and the rest of the engine.

**Grass cost is near-camera overdraw.**
- Timed per batch, grouped by nearest blade: within 1k units, 12 batches cost 1.26 ms (~105 us each);
  1-3k, 24 batches 0.66 ms; 3-6k, 75 batches 0.61 ms; beyond 6k, 48 batches 0.37 ms (~8 us each).
- `grass tail+gap` is 0.00 ms, so no engine work hides between the last grass batch and near water.
- About 29k instances and 1.6M triangles per frame; ~55 triangles per instance.
- Halving its vertex work for most batches saved just 0.28 ms. At a lower resolution it fell only
  2.97 → 2.13 ms, while the shadow applies halved.
- Halving Grass.ini Min/MaxDistance (to 4500/6000) cut instances by only 27% (DrawGrass culls whole
  land squares) and saved no measurable time. The far batches are cheap; distance is not the lever.
- Grass.ini `GrassDensity` outside 1-16 is silently ignored: `ApplyGrassDensitySettings` maps only 1-16
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
- GPU timings jitter 0.1-0.2 ms run to run (GPU:TreesFar swung 0.65-0.85 over 100 samples), so
  settle small differences with an A/B in one session.
- The `SetCT` counter also counts the shadow-map draws' own `CurrentVertex/CurrentPixel->SetCT()`,
  two per non-instanced shadow draw, so it swings with tree draws.
- The FrameProfile distance-bucket counters fill only on frames that carry a GPU slot (every other
  frame), so their `max` column is the true per-frame count; the average is half of it.

**Open candidates and dead ends:**
- Near grass overdraw (the 12 nearest batches) needs a visual trade-off or a cheaper near-grass pixel
  path. Ask about MSAA / driver transparency AA first; it multiplies exactly this cost.
- NOT the sun PCF: swapping the near 5x5 kernel for one tap saved only ~0.06 ms, so a min/max-tile
  early-out cannot pay for its build pass. The rest of the sun apply is always-run setup (~123
  instructions + 72 adaptive bias per pixel). Do NOT fold the clip->light matrix chain into one CPU
  matrix: it passes through the TAA-jittered InvViewProjMatrix while WorldViewProj is unjittered, so
  merging would drop that offset and change the result.
