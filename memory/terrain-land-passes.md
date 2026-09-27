---
name: terrain-land-passes
description: Near land = SLS2042→2048 opaque base + SLS2043→2049 per-layer blend (vertex weights); both write POM shadow relief (2049 blended by layer weight); engine does NOT upload EyePosition c25 for land — derive the eye from ModelViewProj; land TBN is exact (+u=+X, +v=+Y); SLS2043 is at the vs_3_0 output limit
metadata:
  type: project
---

Near land draws the base texture layer with VS SLS2042 + PS SLS2048 (opaque), then each further
layer with VS SLS2043 + PS SLS2049, alpha-blended: the layer weight is
`dot(PSLightColor[1], COLOR0) + dot(PSLightColor[2], COLOR1)` (per-vertex weights, one-hot selector
constants). Each pass binds its own diffuse (s0) and normal map (s1), so per-pass effects see only
that layer. LOD land is SLS2001/2068 (+ SLS2064 VS). Measured pairing (LogShaders): SLS2042 feeds
only SLS2048 and SLS2043 only SLS2049; stock SLS2046 (reads t6/t7) pairs with SLS2040/2041.

**EyePosition (c25) is garbage in land shaders.** Stock SLS2042/2043 don't declare it, so the engine
never uploads it for land draws and the register holds a stale value from an earlier draw (measured
4096+ units from the real eye, direction swinging across the screen). `TerrainEyePosition()` in
`Terrain/Includes/Parallax.hlsl` solves the model-space eye from `ModelViewProj` (null vector of rows
0, 1, 3). Before trusting any engine constant in an override, check the STOCK shader's constant table.

**Land TBN is exact** (measured with a ddx/ddy cotangent-frame diagnostic): tangent = +u = world +X,
binormal = +v = world +Y. The stock PAR offset `uv += (h*s - s/2) * normalize(viewTS).xy` needs no flip.

**Register budget:** SLS2043 already uses all 11 vs_3_0 outputs (X5622 at 12); terrain parallax freed
TEXCOORD6/7 there (dead shadow outputs) for `ParallaxView : TEXCOORD1` (tangent-space eye, eye distance),
and TEXCOORD8 (never read by the PSO) now carries `ViewDepth` (clip w). SLS2042 dropped its dead
TEXCOORD6/7 too, so it has room to spare.

**Shadow relief:** both land PSOs write the POM side channel on COLOR1 (see [[par-shader-interpolator-layout]]).
SLS2048 is in `POMShadowPixelShaders` (depth-writing, unblended gate). SLS2049 is the one
`isPOMShadowBlender`: RT1 is bound under the blended layer draw and COLOR1.a = the layer weight
(same register as COLOR0.a), so the stored relief is the layer-weighted mix; the geometric depth
lerps with itself and stays within the shadow effects' 0.1% match. That needs FP32 blending on
G32R32F (`ShaderManager::CanBlendPOMDepth`); without it only the base layer writes. Relief scale is
`Terrain.ini ShadowReliefScale` (c8) and fades with the parallax. Play-tested 2026-09-26: looks
right, and the blend-capability check passes (no warning). The layer blend is assumed
SRCALPHA/INVSRCALPHA (the alpha output is a weight; not captured with LogShaders) — if relief ever
looks stacked where layers overlap, check that first.

Landscape diffuse alpha is a heightmap in the installed replacer (116/118 DXT5 maps full-range).

Do not "fix" land eye vectors with `TESR_GEOM_EyePosition` (c128, the `EyePositionShaders` list in
ShaderIOHook.cpp + RenderHook per-geometry upload): it is computed from the MAIN camera, so it is wrong
in the water reflection pass, where the MVP-derived eye is correct. SLS2042/2043 are deliberately not
in that list.

**How to apply:** per-layer effects go in both 2048 and 2049; anything needing all layers at once
(height blending) needs single-pass land. See [[par-shader-interpolator-layout]],
[[shader-deployment-workflow]], [[or-screenshot-dds-diagnostic]].
