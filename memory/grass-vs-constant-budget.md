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
c254.xyzw + c255.xy, and the two recovery weights sit in c255.zw (source 0 = player at an
implicit weight of 1.0).

**Why:** any new per-frame grass VS data has to displace something already packed here.

**How to apply:** before designing anything that needs new grass VS constants, check
all three `Shaders/Grass/GRASS20xx.vso.hlsl` files, not just 2028. See [[shader-pipeline-facts]].
