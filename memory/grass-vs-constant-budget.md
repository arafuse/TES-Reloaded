---
name: grass-vs-constant-budget
description: "Grass VS constant map after the trail map; c249-c250 and c253 trail, c251-c252 and c254-c255 free, c3 and c16-c19 only in 2028/2030 (2034 uses them for point lights); groundcover PS c32-c34 and s4"
metadata:
  node_type: memory
  type: project
  originSessionId: a18156c9-a87a-4c79-98a4-0454b8edb452
  modified: 2026-11-18T00:34:17.872Z
---

The grass VS constant space (vs_3_0, 256 float4): c0-c15 engine, `InstanceData[228]` = c20-c247,
ours `TESR_GrassScale` c248, `TESR_GrassTrailWindow` c249, `TESR_GrassTrailTime` c250,
`TESR_GrassCollisionParams` c253 (x unused, y push, z flatten, w trail-reaches flag). c251-c252 and
c254-c255 are free. The old `TESR_GrassCollisionXY0/1` (c254-c255) are gone with the per-batch packing.
InstanceData can't be shrunk (the engine batches to 228).

The vertex shaders also read the trail map through vertex sampler 0 (`TESR_GrassTrailTall`), so this
is a sampler slot, not a constant. Memory [[grass-trail-map]] has the layout and encodings.

**Free since 2026-11-07** (commit d6d0bd2 dropped the near/far shadow coordinates the VS still computed
for the old in-shader grass shadows; no grass PS read them):
- **c251-c252 in all three**; c254-c255 in all three (freed by the trail map).
- c16-c19 in GRASS2028/2030 only (was `...TransformFar`); GRASS2034 holds
  `TESR_PointLightPosition[2]` c16-17 and `TESR_PointLightColor[2]` c18-19 there.
- c3 in GRASS2028/2030 only; GRASS2034 holds `LightPosition`.

Current packing: there is no per-batch collision source list any more. Tall grass takes one bilinear
`tex2Dlod` of the tall trail map at the clump's world XY (`GrassTrail.hlsl`), so no source count or
per-batch slot limit applies. The CPU only sets c253 per geometry in `SetGrassCollisionConstants`
(RenderHook.cpp): y push, z flatten, and w = 1 when `GrassTrail::TallReaches` says the batch's instance
footprint overlaps live tall-grass trail; w = 0 otherwise, so the `[branch]` skips far batches. c253 is
re-uploaded per draw and mirrored into `ShaderConst.Grass` so a later SetCT re-uploads the same values.
`ShaderManager::GrassCollisionSources` (the per-frame source list) still exists for TreeCollision and
GrassCover, and footprint stamps (`kGrassStampCount`) feed only TreeCollision.

Per-draw data for the grass PIXEL shaders goes through PS constants instead (plenty free). The
groundcover press uses PS c32-c34: c32 `TESR_GroundcoverPressParams` (x unused, y strength, z
darkening, w model height), c33 `TESR_GrassTrailWindow`, c34 `TESR_GrassTrailTime`, and the ground
trail map is sampler s4 (`TESR_GrassTrailGround`). Groundcover draws get strength 0 when
`GroundReaches` says the batch misses the ground trail. The VS hands the PS its batch-relative XY in the
spare `TEXCOORD0.zw` (TEXCOORD0 was float2) and the vertex's model-space height in `TEXCOORD4.w` (the PS
only read ambient .rgb; 2034's ambient alpha was dropped), which avoids a new interpolator. GRASS2034
used 11 of the 12 vs_3_0 outputs before the shadow outputs went; it now uses 9.

**Why:** any new per-frame grass VS data has to displace something already packed here, or use a
sampler or PS constant instead.

**How to apply:** before designing anything that needs new grass VS constants, check
all three `Shaders/Grass/GRASS20xx.vso.hlsl` files, not just 2028. Prefer a trail-map style texture or a
PS constant over a new VS constant. See [[shader-pipeline-facts]] and [[grass-trail-map]].
