---
name: gpu-frame-breakdown
description: MEASURED 2026-11-07 GPU frame attribution (ProfileFrame = 2) - grass is ~half the main pass and only partly fill-bound, shadow maps are mostly per-frame dynamic trees, the shadow-apply block after fixes; how to read the per-shader report and its two blind spots
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
- **Shadow maps ~1.1-1.3 ms.** The cascades are cached, so this is almost all `DynamicTrees` /
  `DynamicTreesFar` redrawing ~800 non-instanced tree draws into the near and far maps every frame.
  ShadowMapInstanced.vso cannot apply the tree wind transform.
- Residual ~1.3 ms: water reflection, HDR and the rest of the engine.

**Grass is not vertex-ALU bound and only partly fill-bound.**
- Halving its vertex work for most batches saved just 0.28 ms.
- At a lower resolution it fell only 2.97 → 2.13 ms, while the shadow applies halved.
- Halving Grass.ini Min/MaxDistance changed nothing. That is unexplained, since DrawGrass skips
  squares beyond the end distance ([[grass-manager-generation]]). Verify the change actually took
  effect, for example with an instance counter, before concluding distance doesn't matter.
- Grass.ini `GrassDensity = 24` is silently ignored: `ApplyGrassDensitySettings` maps only 1-16
  (iMinGrassSize 240..20). Anything else leaves Oblivion.ini's own value in charge.

**Reading ProfileFrame = 2:**
- A segment runs from one pixel shader change to the next and is billed to that shader.
- Segments sum exactly to GPU:MainPass, so the coverage line in the report proves nothing is lost.
- Blind spot 1: draw counts are hook calls. Grass and SpeedTree batches pass through
  SetupShaderPrograms once per batch ([[grass-batched-per-geometry-hook]],
  [[speedtree-shader-variants]]), so they show "1 draw". Their time is still fully counted.
- Blind spot 2: a draw that bypasses the hook is billed to the shader before it.
- GPU timings jitter 0.1-0.2 ms run to run. Compare ProfileFrame = 2 runs only with each other.

**How to apply:** start the next GPU round from these numbers. Open candidates:
- grass, which needs the distance test redone with a counter;
- tree shadow redraws: refresh the far map's trees less often, or instance them with wind;
- the remaining sun PCF (0.63 ms);
- merging the mid-scene copies.
