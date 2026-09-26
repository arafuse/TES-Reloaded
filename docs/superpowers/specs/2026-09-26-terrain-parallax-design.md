# Terrain Parallax — Design

## Goal

Give near land single-tap parallax (the same technique as the object PAR shaders), driven by the
height already stored in the alpha channel of the landscape diffuse textures. On by default
(ParallaxScale 0.01), tunable from `Terrain.ini` and the in-game menu.

## Findings that shape the design

- **Near land is multipass.** Stock pairs SLS2042.vso → SLS2048.pso draw the base layer opaque;
  SLS2043.vso → SLS2049.pso draw each further texture layer, alpha-blended by per-vertex layer
  weights (`COLOR0`/`COLOR1` dotted with the one-hot `PSLightColor[1..2]`). Each layer pass binds
  its own diffuse on s0 and normal map on s1. All four are already overridden in
  `OblivionReloaded/Shaders/Terrain/` (loaded when `[Shaders] Terrain = 1`).
- **Parallax only moves UVs, never depth**, so the ZFUNC EQUAL follow-up-pass constraints that
  shaped the object POM work (memory `par-shader-interpolator-layout`) do not apply.
- **Height data exists.** Of the 118 DXT5 diffuse maps installed in `Data\Textures\landscape`,
  116 have full-range alpha (0–255, mean ~130); two have flat 255 alpha; 14 diffuse maps are DXT1
  (alpha reads 1). Flat/absent height yields a uniform offset of `+0.5·scale·fade` along the view
  direction, which is invisible on tiled ground.
- **TEXCOORD1 is free.** Stock SLS2042/2043 (vs_2_0) output `oT1` = NormalUV = BaseUV; our
  overrides already dropped it, and our SLS2048/2049 never read it.
- **LOD land** (SLS2001/2068) is out of scope: parallax is invisible at that range.

## Decisions

| Question | Decision |
|---|---|
| Quality | Single-tap offset (stock PAR formula), no ray march |
| Layers | Parallax on **every** layer pass (SLS2048 and SLS2049); each layer uses its own height |
| Settings | Own `Terrain.ini` keys, independent of POM; `ParallaxScale = 0` means off |
| Shadows | No relief shadows on terrain; the POM depth side channel is not extended |
| Height blending between layers | Out of scope (would need single-pass land rendering) |

## Shaders (`OblivionReloaded/Shaders/Terrain/`)

### VSOs SLS2042 and SLS2043

Add one output, keeping every existing output register unchanged:

```hlsl
float4 ParallaxView : TEXCOORD1;   // xyz = tangent-space surface->eye, w = eye distance
```

```hlsl
float3 eyeVec = EyePosition.xyz - IN.position.xyz;
OUT.ParallaxView.xyz = mul(TanSpaceProj, eyeVec);
OUT.ParallaxView.w = length(eyeVec);
```

This follows the stock PAR VSO convention (model-space `EyePosition`, c25, already bound in both
files). The existing `texcoord_5` is not reused: it derives from
`TESR_InvViewProjectionTransform` and feeds fresnel, and it is not verified to be a true
camera-relative vector.

### PSOs SLS2048 and SLS2049

New include `Terrain/Includes/Parallax.hlsl` (not `POM/Includes/PAR.hlsl`, which hard-binds
`TESR_ParallaxData` to c8 and carries the POM-only shadow side channel):

```hlsl
float4 TESR_TerrainParallaxData : register(c7);  // x = scale, y = -0.5*scale, z = fade slope, w = fade bias

// Returns BaseUV shifted by the height in HeightMap's alpha along the tangent-space view vector,
// faded out with eye distance.
float2 TerrainParallaxUV(sampler2D HeightMap, float2 BaseUV, float4 ParallaxView) {
    float height = tex2D(HeightMap, BaseUV).a;
    float fade = saturate(ParallaxView.w * TESR_TerrainParallaxData.z + TESR_TerrainParallaxData.w);
    return BaseUV + (height * TESR_TerrainParallaxData.x + TESR_TerrainParallaxData.y) * fade * normalize(ParallaxView.xyz).xy;
}
```

- c7 is unused in both PSOs (they use c1–c4 and c6).
- Every existing `BaseMap` / `NormalMap` sample in SLS2048 and SLS2049 switches from `IN.BaseUV`
  to the displaced UV. The SLS2049 output alpha (layer weight) is unchanged.
- Cost per layer pass: one extra texture fetch plus ~6 ALU ops.

## CPU side

### Settings (`TESReloaded/Core/SettingManager.h/.cpp`)

- `SettingsTerrainStruct` gains `float ParallaxScale; float ParallaxFadeDistance;`.
- Read from `Terrain\Terrain.ini [Default]` alongside the existing keys (~line 619).
- Written back in the save path (~line 1668).
- Exposed in the menu: list (~line 2329) and set (~line 3144) branches for `"Terrain"`.
- `Terrain.ini` gains `ParallaxScale = 0.01` and `ParallaxFadeDistance = 8000.0`.

### Constants (`TESReloaded/Core/ShaderManager.h/.cpp`)

- `ShaderConstants::TerrainStruct` gains `D3DXVECTOR4 ParallaxData;`.
- `SetConstantTableValue2` maps `"TESR_TerrainParallaxData"` to it.
- `ShaderManager::UpdateTerrain` packs it:
  - `x = ParallaxScale`, `y = -0.5f * ParallaxScale`
  - fade is full strength to half the fade distance, then linear to zero at the fade distance:
    `z = -2 / ParallaxFadeDistance`, `w = 2`
  - `ParallaxFadeDistance <= 0` disables the fade: `z = 0`, `w = 1`

## Verification

1. **Offline compile:** fxc `/T vs_3_0` / `/T ps_3_0 /E main /I OblivionReloaded\Shaders\Terrain`
   on all four `.hlsl` files; no new errors or warnings versus HEAD.
2. **Build:** MSBuild Release/x86 of `OblivionReloaded` succeeds.
3. **Compile in-game:** run the game with `[Develop] CompileShaders = 1` so it compiles the edited
   `.hlsl` (the compiled `.vso`/`.pso` are gitignored and never committed).
4. **Inert when off:** with `ParallaxScale = 0` terrain looks identical to before.
5. **Effect when on (~0.03–0.05):** cobblestone / rocky-dirt layers show relief; bumps read as
   raised consistently along both UV axes. If one axis appears inverted, the land
   tangent/binormal sign differs from the PAR convention and that component of `ParallaxView.xy`
   is negated. This is the one unknown that can only be settled in-game.
6. **Fade:** no visible seam at the near-land / LOD boundary and no distant shimmer.
7. **Pairing:** a `Develop.LogShaders` capture in an exterior confirms that only SLS2048/2049 pair
   with SLS2042/2043 (in particular, that no stock sibling such as SLS2046 reads TEXCOORD1).

## Commits

- `feat(Terrain): ...` — shaders, settings, constants, `Terrain.ini`.
- `docs: ...` — this spec, the plan, and a memory note on the land pass structure and the
  measured tangent-sign result.

## Implementation notes (post-verification)

- **Eye position:** the engine never uploads `EyePosition` (c25) for land draws (stock SLS2042/2043
  don't declare it), so it is stale. The VSOs derive the model-space eye from `ModelViewProj` via
  `TerrainEyePosition()` in `Terrain/Includes/Parallax.hlsl`. The land TBN was measured exact; no
  sign flip.
- **Register budget:** SLS2043 was at the vs_3_0 limit of 11 outputs; its dead TEXCOORD6/7 outputs
  (and the matching unread SLS2049 inputs) were removed to make room for TEXCOORD1.
- **Default:** the user chose `ParallaxScale = 0.01`, on by default (shipped INI and code default).
- **Degenerate projection:** `TerrainEyePosition()` clamps `w` so an orthographic MVP yields a distant eye (fade → 0) instead of NaN. SLS2042/2043 are no longer in `EyePositionShaders` (the unused per-geometry `TESR_GEOM_EyePosition` upload).
