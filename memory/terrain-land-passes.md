---
name: terrain-land-passes
description: Near land = SLS2042→2048 opaque base + SLS2043→2049 per-layer blend (vertex weights); engine does NOT upload EyePosition c25 for land — derive the eye from ModelViewProj; land TBN is exact (+u=+X, +v=+Y); SLS2043 is at the vs_3_0 output limit
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
TEXCOORD6/7 there (dead shadow outputs) for `ParallaxView : TEXCOORD1` (tangent-space eye, eye distance).
SLS2042 has one output left.

Landscape diffuse alpha is a heightmap in the installed replacer (116/118 DXT5 maps full-range).

**How to apply:** per-layer effects go in both 2048 and 2049; anything needing all layers at once
(height blending) needs single-pass land. See [[par-shader-interpolator-layout]],
[[shader-deployment-workflow]], [[or-screenshot-dds-diagnostic]].
