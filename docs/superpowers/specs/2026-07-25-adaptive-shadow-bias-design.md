# Adaptive sun-facing shadow bias — design

Date: 2026-07-25
Branch: `fix/shadow-acne`
Component: exterior sun shadows (image-space apply pass)

## Problem

Lowering `[Exteriors] deferredConstBias` to remove shadow acne on surfaces facing
away from the sun causes shadows to bleed through surfaces facing the sun. A single
global bias cannot serve both cases, so the two artifacts trade against each other.

## Root cause

`OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl:251-257` already contains a
slope-scaled bias term, but it cannot discriminate sun-facing from sun-away surfaces:

```hlsl
float4 normal = getNormals(IN.UVCoord);        // returns ENCODED (n + 1) / 2, w = 1
float4 lightDir = abs(TESR_ShadowLightDir);    // abs() destroys the sign
float3 n = normalize(normal);                  // normalizes a float4, w = 1 leaks in
float3 l = normalize(lightDir);                // normalizes a float4, w = day-scale leaks in
float cosTheta = clamp(dot(n, l), 0, 1);
float bias = TESR_ShadowBiasDeferred.z * tan(acos(cosTheta));
```

Three compounding defects:

1. `getNormals` returns `(norm + 1) / 2`, so every component lies in `[0,1]` and `n`
   always sits in the positive octant. The true normal direction is gone.
2. `abs()` on the light does the same to `L`.
3. Both `normalize()` calls operate on `float4`s, so `w` contaminates the magnitude —
   `1.0` for the normal, the day-scale for the light.

`cosTheta` is therefore a positive-octant number unrelated to whether the surface faces
the sun. Every pixel receives essentially the same slope factor, which is why one global
constant has to cover both cases.

`tan(acos(cosTheta))` is also unclamped and diverges at grazing angles.

The normal-offset at lines 259-260 shares the root problem: it offsets by the *encoded*
(all-positive) normal, and does so in **clip space** after the WVP multiply, making it a
near-uniform nudge rather than a push along the surface normal.

## Prerequisites confirmed

- `TESR_ShadowLightDir.xyz` points **toward** the sun. `ShaderManager.cpp:1204-1207`
  normalizes the sun node's position; `ShadowManager.cpp:630-632` places the shadow eye
  along `+ShadowLightDir` from the target. A signed `N·L` needs no new CPU plumbing.
- `toWorld()` (`ShadowsExteriors.fx.hlsl:84-90`) reconstructs genuine world-space rays
  from the view matrix columns. The normal reconstruction is therefore correct **up to
  sign only** — a z-only negation is a mirror, not a sign fix.
- `getNormals` has exactly one caller (line 251), so changing its contract is contained.
- `Shadow.Data.x` is used by other code paths (`ShadowManager.cpp:590, 1002, 1644, 1682`)
  and must not be repurposed.

## Design

### 1. Signed world-space normal

The tap work is factored into `getRawNormal(float2 UVCoord)`, which performs the existing
depth taps and the shorter-of-left/right edge-robustness selection and returns the bare
`normalize(cross(dx, dy))` — no encoding, no z-flip. Both the new and legacy paths derive
from this single result, so the tap work is done once and the legacy path stays bit-exact.

New path — camera-facing disambiguation:

```hlsl
// Visible surfaces face the camera; this resolves the cross-product's sign
// ambiguity without assuming a handedness.
float3 N = (dot(raw, viewRay) > 0.0f) ? -raw : raw;
```

`viewRay` is `normalize(toWorld(UVCoord))`, computed once by the caller.

Legacy path — the original mirror and encoding, reconstructed from the same `raw`:

```hlsl
float4 normal = float4((float3(raw.x, raw.y, -raw.z) + 1) / 2, 1);
```

This is exactly what the current `getNormals` produces, since it applies `norm.z *= -1`
and then `(norm + 1) / 2` to the same cross product.

### 2. Real N·L and the terminator ramp

```hlsl
float3 L     = normalize(TESR_ShadowLightDir.xyz);   // no abs(), xyz only
float  ndl   = dot(N, L);
float  facing = smoothstep(0.0f, TESR_ShadowBiasAdaptive.x, ndl);
```

The shadow term becomes:

```hlsl
float Shadow = lerp(darkness, mapShadow, facing);
```

`facing == 0` means the surface points away from the sun. Such surfaces are geometrically
self-shadowed, so they take `darkness` directly and never consult the shadow map. No bias
value can produce acne there because there is no depth compare. This is the core of the
fix: back-face acne stops being a bias problem, which frees `deferredConstBias` to be
tuned solely against bleed-through on sun-facing surfaces.

`darkness` is the correct target because it is exactly what `Lookup` returns on its
shadowed branch, so the forced path and the map path agree.

`smoothstep` rather than a hard cutoff because the reconstructed normals are noisy;
a binary test would speckle near the terminator.

### 3. Clamped slope-scaled bias on both cascades

```hlsl
float ndlSafe  = max(ndl, 0.05f);
float slope    = min(sqrt(1.0f - ndlSafe * ndlSafe) / ndlSafe, TESR_ShadowBiasAdaptive.y);
float biasNear = TESR_ShadowBiasDeferred.z * (1.0f + slope);
float biasFar  = TESR_ShadowBiasDeferred.w * (1.0f + slope);
```

`sqrt(1 - x*x) / x` is `tan(acos(x))` without the transcendentals, now clamped.

`GetLightAmount` takes both biases and forwards `biasFar` to `GetLightAmountFar`.
`LookupFar` gains a `bias` parameter — it currently uses a flat
`TESR_ShadowBiasDeferred.w` and never sees the slope term at all, despite the far cascade
being 4x coarser per texel than near. `GetLightAmountSkin` continues to use `biasNear`,
matching the near-resolution map it samples.

### 4. World-space normal offset

Offset the world position before the transform chain, scaled by `sin(theta)` so it grows
as the surface tilts away from the light:

```hlsl
float  sinT    = sqrt(saturate(1.0f - ndl * ndl));
float4 posNear = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.x * sinT, 1.0f), TESR_WorldViewProjectionTransform);
float4 posFar  = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.y * sinT, 1.0f), TESR_WorldViewProjectionTransform);
```

`ShadowSkin` continues to derive from `posNear`.

Cost: one extra 4x4 multiply per pixel (5 matrix muls to 6), negligible against the 20
depth taps `getNormals` performs plus 36 shadow taps.

### 5. CPU-side texel scaling

`deferredNormBias` and `deferredFarNormBias` are reinterpreted as **shadow-map texel
counts**. `ShadowManager.cpp:1179-1181` already publishes `ShadowData->y/z/w` per-frame
from `SelectExteriorShadowSettings()`; the scaling is added there so it tracks the
selected settings:

```cpp
ShadowBiasDeferred.x = Ext.deferredNormBias    * (2.0f * Radius[MapNear] / Size[MapNear]);
ShadowBiasDeferred.y = Ext.deferredFarNormBias * (2.0f * Radius[MapFar]  / Size[MapFar]);
```

This keeps the offset correct when `ShadowMapSize` or `ShadowMapRadius` is retuned.

Knock-on: the live-edit handlers at `SettingManager.cpp:2972-2986` currently write
directly into `ShadowBiasDeferred.x/.y`, which the per-frame path would overwrite. They
change to write only the raw setting field and let the per-frame path apply the scaling.
`InitShadowBiasConstants` (`ShadowManager.cpp:167-173`) is updated to match so the first
frame before any exterior render is not left with unscaled values.

### 6. New shader constant

One new `float4 TESR_ShadowBiasAdaptive`:

| Component | Meaning |
|-----------|---------|
| `x` | terminator width (smoothstep upper bound on `N·L`) |
| `y` | max slope clamp |
| `z` | enable flag (0 = legacy path) |
| `w` | unused, reserved |

Added to `ShaderConst.ShadowMap` in `ShaderManager.h` and registered by name in
`ShaderManager.cpp` next to `TESR_ShadowBiasDeferred` (line ~349). Registration is by
string lookup through effect reflection, so no register index needs assigning.

### 7. Toggle

`TESR_ShadowBiasAdaptive.z` gates the new math against the existing behavior so the two
can be compared in-game. It is a uniform, so the branch is static and costs nothing at
runtime; both bodies occupy instruction slots but only one executes.

The branch spans everything downstream of `getRawNormal`: normal interpretation (§1),
the bias computation (§3 vs. the current unclamped `tan(acos())`), the offset space
(§4 world-space vs. current clip-space), and the terminator ramp (§2, skipped entirely
when disabled). Disabled, the shader reproduces current behavior bit-exactly. The two
normal-offset INI keys change meaning between the paths — texels when enabled,
clip-space when disabled — so the legacy path is only meaningful with the pre-change
values, which are recorded in §8 for that purpose.

### 8. INI

New and retuned keys in `[Exteriors]` of `OblivionReloaded/Shaders/Shadows/Shadows.ini`.
The existing bias values were calibrated against the broken math and do not carry over.

```ini
AdaptiveBias         = 1       ; 0 = legacy path, for A/B comparison
BiasTerminatorWidth  = 0.15    ; smoothstep width on N·L
BiasMaxSlope         = 4.0     ; clamp on the slope multiplier
deferredNormBias     = 1.5     ; now TEXELS (was -0.03, clip-space)
deferredFarNormBias  = 1.5     ; now TEXELS (was -0.03, clip-space)
deferredConstBias    = 0.00015 ; peak is now const * (1 + BiasMaxSlope); was 0.0004
deferredFarConstBias = 0.0004  ; was 0.001
```

These are starting points for in-game tuning, not final values. The toggle exists so they
can be landed by direct A/B comparison.

Pre-change values, for reference when running the legacy path (`AdaptiveBias = 0`), since
the normal-offset keys change meaning between paths:

```ini
deferredNormBias     = -0.03
deferredFarNormBias  = -0.03
deferredConstBias    = 0.0004
deferredFarConstBias = 0.001
```

## Files touched

| File | Change |
|------|--------|
| `OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl` | normal reconstruction, N·L, terminator ramp, slope bias, world-space offset, toggle |
| `OblivionReloaded/Shaders/Shadows/Shadows.ini` | 3 new keys, 4 retuned defaults |
| `TESReloaded/Core/SettingManager.h` | 3 new fields in `ExteriorsStruct` |
| `TESReloaded/Core/SettingManager.cpp` | read (~1245), write (~1580), JSON (~2228), live-edit (~2972) |
| `TESReloaded/Core/ShaderManager.h` | `ShadowBiasAdaptive` vector in `ShadowMap` const block |
| `TESReloaded/Core/ShaderManager.cpp` | bind `TESR_ShadowBiasAdaptive` by name |
| `TESReloaded/Core/ShadowManager.cpp` | per-frame texel scaling, `InitShadowBiasConstants` |

## Risks

- **Grass and foliage.** The apply pass runs mid-scene right before near water, so grass
  is in `TESR_DepthBufferPreWater` and receives shadows. Depth-derived normals are least
  trustworthy there. The terminator ramp softens but does not eliminate this. First lever
  is widening `BiasTerminatorWidth`; the fallback is a normal-confidence gate that
  suppresses the forced-shadow term where the depth gradient is large, which was
  considered and deliberately deferred as unnecessary complexity until proven needed.
- **Requires a shader recompile** (`Develop.CompileShaders`), since this is a `.fx`
  effect rather than a raw shader.
- **No automated tests exist** in this repo. Validation is in-game A/B against
  `AdaptiveBias`.

## Rejected alternatives

- **Amplify bias for back-faces instead of bypassing the compare.** Keeps every pixel on
  the depth-compare path with a steeply rising bias. Gentler failure mode on noisy
  normals, but does not fully eliminate back-face acne — it remains a tuning problem
  rather than a structural fix.
- **Minimal fix: correct `cosTheta` only.** Leaves the clip-space normal offset and the
  front/back symmetry intact. Smaller change, but back-face acne would still be fought
  with bias.
- **Raw world units for the normal offset.** Simpler, but silently becomes wrong when
  `ShadowMapSize` or `ShadowMapRadius` is retuned.
