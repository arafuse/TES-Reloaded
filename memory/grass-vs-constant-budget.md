---
name: grass-vs-constant-budget
description: Grass VS constant map; since 2026-11-07 c249-c252 are free in ALL three variants (dead shadow matrix removed), c3 and c16-c19 only in 2028/2030 (2034 uses them for point lights)
metadata:
  type: project
---

The grass VS constant space (vs_3_0, 256 float4): c0-c15 engine, `InstanceData[228]` = c20-c247,
ours `TESR_GrassScale` c248, `TESR_GrassCollisionParams` c253, `TESR_GrassCollisionXY0/1` c254-255.
InstanceData can't be shrunk (the engine batches to 228).

**Free since 2026-11-07** (commit d6d0bd2 dropped the near/far shadow coordinates the VS still computed
for the old in-shader grass shadows; no grass PS read them):
- **c249-c252 in all three** (was `TESR_ShadowCameraToLightTransformNear`).
- c16-c19 in GRASS2028/2030 only (was `...TransformFar`); GRASS2034 holds
  `TESR_PointLightPosition[2]` c16-17 and `TESR_PointLightColor[2]` c18-19 there.
- c3 in GRASS2028/2030 only; GRASS2034 holds `LightPosition`.

Current packing: up to 3 collision sources PER BATCH; their XY positions fill c254.xyzw + c255.xy.
The CPU keeps one prioritised list per frame (`ShaderManager::GrassCollisionSources`, up to 20:
player, visible actors nearest first, footprints newest first, each with its own weight), and
`SetGrassCollisionConstants` (RenderHook.cpp) picks the first 3 whose radius reaches the batch's
instance footprint ([[grass-batch-footprints]]). Source 0 has an implicit weight of 1.0 and its
(actor-size-scaled) radius in c253.x; it is the player whenever the player or a footprint reaches
the batch, otherwise an actor. c255.zw describe sources 1-2 with a tag encoding: a value below
`kGrassCollisionActorTag` (2.0) is a footprint's recovery weight (radius = c253.x); a value at or
above it is tag + that actor's own radius at weight 1. Batches nothing reaches get c253.w = 0.
TreeCollision and the groundcover press pick from the same list (first 3 that reach).

c253-c255 are all re-uploaded per draw in `SetGrassCollisionConstants`, and the values are
mirrored into `ShaderConst.Grass` so a later SetCT re-uploads the same thing; groundcover draws get
c253.w = 0 (no collision sources).

Per-draw data for the grass PIXEL shaders goes through PS constants instead (plenty free): the
groundcover press uses PS c32-c34 (`TESR_GroundcoverPress*`). The VS hands the PS its
batch-relative XY in the spare `TEXCOORD0.zw` (TEXCOORD0 was float2) and the vertex's model-space
height in `TEXCOORD4.w` (the PS only read ambient .rgb; 2034's ambient alpha was dropped), which
avoids a new interpolator — GRASS2034 used 11 of the 12 vs_3_0 outputs before the shadow outputs
went; it now uses 9. PS c32.w is the
drawn model's height, the press's clip line for the squashed texture.

**Why:** any new per-frame grass VS data has to displace something already packed here.

**How to apply:** before designing anything that needs new grass VS constants, check
all three `Shaders/Grass/GRASS20xx.vso.hlsl` files, not just 2028. See [[shader-pipeline-facts]].
