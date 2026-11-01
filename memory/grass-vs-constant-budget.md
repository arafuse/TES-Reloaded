---
name: grass-vs-constant-budget
description: The grass vertex shaders have NO constant register free in all three variants; c3 is free in 2028/2030 but taken in 2034
metadata:
  type: project
---

The grass VS constant space (vs_3_0, 256 float4) is completely allocated:
c0-c19 engine, `InstanceData[228]` = c20-c247, c248-c255 ours (`TESR_GrassScale` c248,
`TESR_ShadowCameraToLightTransformNear` c249-252, `TESR_GrassCollisionParams` c253,
`TESR_GrassCollisionXY0/1` c254-255).

c3 is free in GRASS2028/2030 but holds `LightPosition` in GRASS2034 (the point-lit
variant). So **there is no register free in all three**, and InstanceData can't be
shrunk (the engine batches to 228).

Current packing: up to 3 collision sources (player + 2 trail slots); their XY positions fill
c254.xyzw + c255.xy. Source 0 = player at an implicit weight of 1.0, with the player's
(actor-size-scaled) radius in c253.x. c255.zw describe sources 1-2 with a tag encoding: a value
below `kGrassCollisionActorTag` (2.0) is a footprint's recovery weight (radius = c253.x); a value
at or above it is tag + that actor's own radius at weight 1. The CPU keeps the true weights in
`GrassCollisionWeights` (TreeCollision reads them) and the packed values in `GrassCollisionSlotData`.

c253-c255 are all re-uploaded per draw in `SetGrassCollisionConstants` (RenderHook.cpp), and the
values are mirrored into `ShaderConst.Grass` so a later SetCT re-uploads the same thing; groundcover
draws get c253.w = 0 (no collision sources).

Per-draw data for the grass PIXEL shaders goes through PS constants instead (plenty free): the
groundcover press uses PS c32-c34 (`TESR_GroundcoverPress*`). The VS hands the PS its
batch-relative XY in the spare `TEXCOORD0.zw` (TEXCOORD0 was float2) and the vertex's model-space
height in `TEXCOORD4.w` (the PS only read ambient .rgb; 2034's ambient alpha was dropped), which
avoids a new interpolator — GRASS2034 already uses 11 of the 12 vs_3_0 outputs. PS c32.w is the
drawn model's height, the press's clip line for the squashed texture.

**Why:** any new per-frame grass VS data has to displace something already packed here.

**How to apply:** before designing anything that needs new grass VS constants, check
all three `Shaders/Grass/GRASS20xx.vso.hlsl` files, not just 2028. See [[shader-pipeline-facts]].
