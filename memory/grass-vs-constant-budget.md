---
name: grass-vs-constant-budget
description: The grass vertex shaders have NO constant register free in all three variants; c3 is free in 2028/2030 but taken in 2034
metadata:
  type: project
---

The grass VS constant space (vs_3_0, 256 float4) is completely allocated:
c0-c19 engine, `InstanceData[228]` = c20-c247, c248-c255 ours (GrassScale c248,
ShadowCameraToLightTransformNear c249-252, GrassCollisionParams c253,
GrassCollisionXY0/1 c254-255).

c3 is free in GRASS2028/2030 but holds `LightPosition` in GRASS2034 (the point-lit
variant, used at line ~174). So **there is no register free in all three**, and
InstanceData can't be shrunk (the engine batches to 228).

**Why:** any new per-frame grass VS data has to be packed into the 8 floats of
c254/c255, or the feature has to give something up.

**How to apply:** before designing anything that needs new grass VS constants, check
all three .vso.hlsl files, not just 2028. The 2026-08-28 spring-back work paid for
its two recovery weights by dropping collision sources from 4 to 3 and putting the
weights in c255.zw (source 0 = player at an implicit 1.0). See
[[grass-collision-spring-back]] and [[shader-pipeline-facts]].
