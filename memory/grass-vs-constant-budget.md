---
name: grass-vs-constant-budget
description: "Grass VS constant map: c248 scale, c249-c250 and c253 trail, c251-c252 and c254-c255 free, c3 and c16-c19 free only in 2028/2030 (2034 uses them for point lights); groundcover PS c32-c34 and s4"
metadata:
  type: project
---

The grass VS constant space (vs_3_0, 256 float4): c0-c15 engine, `InstanceData[228]` = c20-c247,
ours `TESR_GrassScale` c248, `TESR_GrassTrailWindow` c249, `TESR_GrassTrailTime` c250,
`TESR_GrassCollisionParams` c253 (x unused, y push, z flatten, w trail-reaches flag). InstanceData
can't be shrunk (the engine batches to 228).

**Free:**
- c251-c252 and c254-c255 in all three (GRASS2028/2030/2034).
- c16-c19 in GRASS2028/2030 only; GRASS2034 holds `TESR_PointLightPosition[2]` c16-17 and
  `TESR_PointLightColor[2]` c18-19 there.
- c3 in GRASS2028/2030 only; GRASS2034 holds `LightPosition`.

The vertex shaders also read the trail map through vertex sampler 0 (`TESR_GrassTrailTall`), a sampler
slot, not a constant. [[grass-trail-map]] has the layout and encodings.

**Collision packing:** there is no per-batch collision source list. Tall grass takes one bilinear
`tex2Dlod` of the tall trail map at the clump's world XY (`GrassTrail.hlsl`), so no source count or
per-batch slot limit applies. The CPU only sets c253 per geometry in `SetGrassCollisionConstants`
(RenderHook.cpp, from the instance-staging hook): y push, z flatten, and w = 1 when
`GrassTrail::TallReaches` says the batch's instance footprint overlaps live tall-grass trail; w = 0
otherwise, so the `[branch]` skips far batches. c253 is mirrored into `ShaderConst.Grass` so a later
SetCT re-uploads the same values.

Per-draw data for the grass PIXEL shaders goes through PS constants instead (plenty free). The
groundcover press uses PS c32-c34: c32 `TESR_GroundcoverPressParams` (x unused, y strength, z
darkening, w model height), c33 `TESR_GrassTrailWindow`, c34 `TESR_GrassTrailTime`, and the ground
trail map is sampler s4 (`TESR_GrassTrailGround`). Groundcover draws get strength 0 when
`GroundReaches` says the batch misses the ground trail. The VS hands the PS its world XY in the
spare `TEXCOORD0.zw` and the vertex's model-space height in `TEXCOORD4.w`, which avoids a new
interpolator. GRASS2034 uses 9 of the 12 vs_3_0 outputs.

**Why:** any new per-frame grass VS data has to displace something already packed here, or use a
sampler or PS constant instead.

**How to apply:** before designing anything that needs new grass VS constants, check
all three `Shaders/Grass/GRASS20xx.vso.hlsl` files, not just 2028. Prefer a trail-map style texture or a
PS constant over a new VS constant. See [[shader-pipeline-facts]], [[grass-batched-per-geometry-hook]].
