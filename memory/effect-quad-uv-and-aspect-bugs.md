---
name: effect-quad-uv-and-aspect-bugs
description: "The shared EffectQuad's +0.5 texel UV offset is REQUIRED (D3D9 samples at integer coords) - removing it blurs everything; plus the integer-divided aspect ratio in ReciprocalResolution.z."
metadata:
  type: reference
---

Two things about the shared post-processing quad setup in `ShaderManager.cpp`. The first is a trap
that has already been fallen into once — do not repeat it.

## 1. The `+0.5` texel UV offset on EffectQuad is CORRECT. Do NOT "fix" it.

The shared `EffectVertex` quad is `D3DFVF_XYZ` (`EFFECTQUADFORMAT`) at NDC -1..1 with
`UAdj = 0.5/width`, `VAdj = 0.5/height` added to the UVs, and `FrameVS` passes POSITION through
untransformed.

It LOOKS wrong - the half-texel correction is usually described as an XYZRHW-only requirement.
It is not wrong. **D3D9 samples pixel attributes at INTEGER screen coordinates, not at pixel
centres** (that changed in D3D10+). So for a quad spanning NDC -1..1 over a viewport of width W,
pixel i interpolates `u = i/W`, while texel i's centre is `(i+0.5)/W`. The offset closes exactly
that gap. Removing it makes every fullscreen pass sample on a texel boundary; `LINEAR` then returns a
2x2 box blur that compounds across the ~15 fullscreen passes per frame, and the whole scene goes soft.

Falsification test for any theory like this: if the offset were wrong, the long-standing pass chain
would already have been visibly blurry. It never was.

Independent corroboration in-tree: `convToImageSpace()` in `Shaders/Water/WaterLens.fx.hlsl`
builds texcoords from an identical NDC ±1 quad and adds the same half-texel offset.

Consequence for any HALF-RES pass driven by this shared quad: `UAdj` is computed from the FULL
width, so a half-res pass gets half the offset it would want (0.25 texel short). Harmless with POINT
sampling; slightly softening with LINEAR.

## 2. `ReciprocalResolution.z` (aspect ratio) IS integer-divided - do NOT fix it alone.

`ShaderConst.ReciprocalResolution.z = TheRenderManager->width / TheRenderManager->height;` — both are
`UInt32` (OBLIVION `NiDX9Renderer` in GameNi.h, offsets A58/A5C), so aspect == **1** at 1920x1080,
1280x720, 4:3 and 16:10; == 2 at 2560x1080.

Consumers of `.z`: AmbientOcclusion (`g_InvFocalLen` and the sample spread), Cinema, GodRays,
MasserRays, SecundaRays (`raspect = 1 / .z`). AO's `g_InvFocalLen = { tan(0.5*radians(FoV)) / aspect,
tan(0.5*radians(FoV)) }` DIVIDES by aspect where a correct projection would multiply; the two errors
cancel at aspect 1, which is why it looks right. Changing `.z` changes every consumer at once, so audit
them all first.

**For new code**, derive focal length from the projection matrix, which is exact and aspect-correct by
construction:
```hlsl
float2 InvFocalLen = float2(1.0 / TESR_ProjectionTransform._11, 1.0 / TESR_ProjectionTransform._22);
float  ProjScale   = 0.5 * targetHeight * TESR_ProjectionTransform._22;  // world units -> pixels at unit depth
```
`ReciprocalResolution.w` holds FoV, set in `RenderManager::SetSceneGraph` from
`SettingsMain.Main.FoV` / `Player->worldFoV`.
