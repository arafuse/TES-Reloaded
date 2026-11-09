---
name: gpu-frame-breakdown
description: MEASURED 2026-11-07 GPU frame attribution (ProfileFrame = 2) - grass ~2.9 ms is dominated by the ~12 batches nearest the camera (overdraw), not distance; tree shadow composites split near/far; how to read the per-shader and per-batch reports and their blind spots
metadata:
  type: project
---

Measured 2026-11-07 in the user's usual exterior test scene (one shadowed point light, near water,
dense grass), after the probe-readback fix ([[d3d9-gpu-readback-stall]]). CPU ~4.7 ms vs GPU ~11 ms:
**the frame is GPU-bound**, so CPU-only wins do not move FPS here.

**Re-measured 2026-11-08 on DXVK 3.1.1, same scene** (ProfileFrame = 1, so no per-shader rows): the
counters match (29,379 grass instances, 160 batches, tree pieces 107 near / 688 far). The user
reported better performance after the upgrade. GPU frame 11.0 → **9.2 ms**, main pass 5.9 → **4.96**,
GPU:MidSun 0.27 (it was 0.43 before commit 196b760, so not all of that drop is DXVK). GPU:ShadowExt
0.73 (TreesNear 0.51, TreesFar 0.57 on the frames that run it), CPU envelope 4.7 → 3.6 ms. The
sampler run showed 96 FPS / 10.41 ms with a 4.03 ms Present wait: still GPU-bound. The per-shader
numbers below are from 2.7.1, so re-run ProfileFrame = 2 before trusting them.

GPU frame ~11.0 ms:
- **Main pass ~5.9 ms**, by pixel shader:
  - **Grass (GRASS2004 + GRASS2030) ~2.7-3.0 ms**, ~50% of the main pass.
  - Mid-scene shadow apply 0.92 ms since commit a466bad: sun 0.43, point ~0.01, plus the pre-water
    depth resolve, the probe and ONE full-screen copy. The applies output a shadow FACTOR that the GPU
    multiplies into the scene (DESTCOLOR blend), so the seed copy and the between-pass copy are gone; the
    last copy stays because near water refracts TESR_RenderedBuffer. A compiled effect without the
    `ShadowOutputsFactor` marker falls back to the old read-and-copy path per pass.
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
  - The static-bake → live-map copy is only ~0.03-0.06 ms; the cost is the tree draws.
  - The pieces come from few meshes: near 107 pieces from 22 buffers, far 688 from 45. Commit f72bc97
    instances them (`InstanceDynamicTrees`, default 1). A/B in one session: GPU:ShadowExt 0.77 → 0.64,
    CPU ShadowMaps 0.89 → 0.72. Most of the remaining ~0.38 ms near composite is per-vertex and
    per-pixel work (overlapping leaf cards MIN-blended into R32F), which batching cannot remove.
  - The user does NOT want a near-map tree interval (visible stutter risk); don't propose it again.
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
- GPU timings jitter 0.1-0.2 ms run to run (GPU:TreesFar swung 0.65-0.85 over 100 samples), so
  settle small differences with an A/B in one session.
- The `SetCT` counter also counts the shadow-map draws' own `CurrentVertex/CurrentPixel->SetCT()`,
  two per non-instanced shadow draw; its swings (1711 → 805 → 196) tracked the tree changes, not a bug.
- The FrameProfile distance-bucket counters fill only on frames that carry a GPU slot (every other
  frame), so their `max` column is the true per-frame count; the average is half of it.

**How to apply:** start the next GPU round from these numbers. Open candidates:
- near grass overdraw (the 12 nearest batches), which needs a visual trade-off or a cheaper
  near-grass pixel path. Ask about MSAA / driver transparency AA first; it multiplies exactly this cost;
- NOT the sun PCF: measured 2026-11-07 by swapping the near 5x5 kernel for one tap, GPU:MidSun went
  only 0.43 -> 0.37 ms, so the filter costs ~0.06 ms and a min/max-tile early-out cannot pay for its
  build pass. The rest of the pass is the always-run setup (~160 instr + 72 adaptive bias per pixel:
  5 view-ray rebuilds for the normal, chained clip->light matrix multiplies). Commit 196b760
  rebuilt the normal from one ray plus preshader steps (always-run 152 -> 123 instructions). Do NOT
  fold the clip->light chain into one CPU matrix: it passes through the TAA-jittered
  InvViewProjMatrix while WorldViewProj is unjittered, so merging would drop that offset and
  change the result.
- `Mid:ProbeRead` is solved: its 2.4-3.0 ms worst frame was DXVK blocking in the sun probe's sysmem
  `LockRect`. Occlusion queries brought it to 0.002 ms avg, 0.004 max ([[d3d9-gpu-readback-stall]]).
