---
name: effect-quad-uv-and-aspect-bugs
description: "The shared EffectQuad's +0.5 texel UV offset is REQUIRED (D3D9 samples at integer coords) - removing it blurs everything; plus the integer-divided aspect ratio in ReciprocalResolution.z."
metadata:
  type: reference
---

Two things about the shared post-processing quad setup. The FIRST one is a trap I already fell
into once (2026-08-17, branch feat/ao-performance-and-fixes) - do not repeat it.

## 1. The `+0.5` texel UV offset on EffectQuad is CORRECT. Do NOT "fix" it.

`ShaderManager.cpp` builds the shared `EffectVertex` quad as `D3DFVF_XYZ` (`EFFECTQUADFORMAT`)
at NDC -1..1 with `UAdj = 0.5/width`, `VAdj = 0.5/height` added to the UVs, and `FrameVS`
passes POSITION through untransformed.

It LOOKS wrong - the half-texel correction is usually described as an XYZRHW-only requirement.
It is not wrong. **D3D9 samples pixel attributes at INTEGER screen coordinates, not at pixel
centres** (that changed in D3D10+). So for a quad spanning NDC -1..1 over a viewport of width W,
pixel i interpolates `u = i/W`, while texel i's centre is `(i+0.5)/W`. The offset closes exactly
that gap.

**I removed it and the entire scene went blurry** - every fullscreen pass then samples precisely
on a texel boundary, `LINEAR` returns a 50/50 blend of two texels, and that 2x2 box blur compounds
across the ~15 fullscreen passes per frame. Reverted.

**The falsification test I should have run first, and that settles this in one step:** if the
offset were wrong, the pre-existing 15-pass chain would ALREADY have been catastrophically blurry.
It never was. Any theory that implies long-standing working output is broken is wrong.

Independent corroboration in-tree: `convToImageSpace()` in `Shaders/Water/WaterLens.fx.hlsl`
builds texcoords from an identical NDC +/-1 quad and adds the same `+0.5 * TESR_ReciprocalResolution`.
Two independent places agreeing is the signal.

Consequence for any HALF-RES pass driven by this shared quad: `UAdj` is computed from the FULL
width, so a half-res pass gets half the offset it would want (0.25 texel short). With POINT
sampling that rounds into the right texel and is harmless; with LINEAR it would soften slightly.

## 2. `ReciprocalResolution.z` (aspect ratio) IS integer-divided - but do NOT fix it alone.

`ShaderManager.cpp`: `ShaderConst.ReciprocalResolution.z = TheRenderManager->width / TheRenderManager->height;`
Both are `UInt32` (`GameNi.h:3175`, NiDX9Renderer offsets A58/A5C), so aspect == **1** at 1920x1080,
1280x720, 4:3 and 16:10; == 2 at 2560x1080.

The `g_InvFocalLen` idiom copied into DepthOfField, GodRays, KhajiitRays (Masser/Secunda), Rain,
Snow and SnowAccumulation is `{ tan(0.5*radians(FoV)) / aspect, tan(0.5*radians(FoV)) }` - it
DIVIDES by aspect where a correct projection would multiply. The two errors cancel, which is why
those shaders look right. Fixing either half alone makes output worse.

**The robust workaround** (used by the rewritten AO shader): derive from the projection matrix,
which is exact and aspect-correct by construction and needs no FoV convention:
```hlsl
float2 InvFocalLen = float2(1.0 / TESR_ProjectionTransform._11, 1.0 / TESR_ProjectionTransform._22);
float  ProjScale   = 0.5 * targetHeight * TESR_ProjectionTransform._22;  // world units -> pixels at unit depth
```
`ReciprocalResolution.w` holds FoV, set in `RenderManager::SetSceneGraph` from
`SettingsMain.Main.FoV` / `Player->worldFoV`.
