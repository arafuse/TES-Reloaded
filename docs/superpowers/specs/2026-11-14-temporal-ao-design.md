# Temporal Ambient Occlusion — Design

Date: 2026-11-14
Branch: `feat/gtao`

## Goal

The current AO (`Shaders/AmbientOcclusion/AmbientOcclusion.fx`) looks static and grainy. Adopt
XeGTAO's *temporal* technique — per-frame spatio-temporal noise soaked up by accumulation over
frames — for the existing AO estimator. It must work with the TAA effect disabled.

Out of scope: XeGTAO's horizon-based estimator, depth MIP prefilter, bent normals, applying AO to
ambient light only, half-resolution AO. The estimator, blur weights, compositing and INI look of
the current AO are kept.

## Background

- The legacy noise is `rand(uv)`: a per-pixel hash that never changes between frames and only
  scales the ring radius (uniform in [0, 0.004]); the 5 ring sample angles are identical for every
  pixel. The grain is therefore frozen in screen space and the spatial blur can only smear it.
- XeGTAO relies on the application's TAA to integrate a 64-frame noise cycle
  (`NoiseIndex = frame % 64`, R2 sequence offset). Here TAA is optional (off in the user's INI),
  and the color TAA keeps only ~3 frames (0.7 history weight + color clamp), so a dedicated AO
  accumulation pass is required.
- Effects in the chain render every pass into full-resolution FP16 chain buffers
  (`EffectRecord::Render` / `RenderChained`); there are no per-effect targets. `TextureManager`
  caches each sampler's texture pointer at effect load (`TextureManager.cpp` `LoadTexture`), so
  per-frame ping-pong textures must be bound explicitly with `SetTexture`.
- Reusable from TAA (`Shaders/TAA/TAA.fx.hlsl`): world-position reconstruction (`getWorldPos`,
  `readDepthWorld`) and reprojection (`getReprojectUV`) through
  `TESR_PrevWorldViewProjectionTransform`, which `RenderEffectsPreHdr/PostHdr` set after each
  chain. Not reusable: `TAATexture`, the color neighborhood clamp, the 2-tap projection jitter.

## Architecture

### Entry point

The AO slot in `ShaderManager::RenderEffects`:

- **Temporal off** (`TemporalEnabled = 0`, missing key, effect load failure, or target creation
  failure): the existing `RunEffect(AmbientOcclusionEffect, ...)` runs unchanged. The legacy
  `AmbientOcclusion.fx(.hlsl)` is not modified.
- **Temporal on**: new `ShaderManager::RenderAmbientOcclusionTemporal(Device, RenderTarget)`.

### New effect

`Shaders/AmbientOcclusion/AmbientOcclusionTemporal.fx.hlsl`, loaded as its own `EffectRecord`
alongside the legacy one, with two techniques:

- `Compute` — 4 passes, each driven by C++ into a dedicated target through a new
  `EffectRecord::RenderPass(Device, Pass, Target)` (Begin, BeginPass, draw quad, EndPass, End).
- `Combine` — 1 pass, run through the normal `RunEffect` so it participates in the chain
  rotation like any other effect.

### Per-frame data flow

| # | Pass | Reads | Writes |
|---|---|---|---|
| 1 | AO | depth | `AORaw` (G16R16F: ao, depth) |
| 2 | Accumulate | `AORaw`, `History[prev]` | `History[cur]` (A16B16G16R16F: ao, depth, count) |
| 3 | Blur H | `History[cur]` | `AOBlur` (G16R16F) |
| 4 | Blur V | `AOBlur` | `AORaw` (reused) |
| 5 | Combine (chain) | `AORaw`, scene | chain |

- `History[0..1]` swap roles every frame; no copies.
- History stores the pre-blur accumulated AO, so blur is never fed back into itself.
- Inputs to each Compute pass are bound with `SetTexture` on the sampler ordinal (looked up once
  from the effect's constant table), as `RenderChained` does for `RBRegister`.

### Targets

Four full-resolution targets (`AORaw`, `AOBlur` G16R16F; `History[2]` A16B16G16R16F), created
lazily on the first temporal frame and kept for the session (the game never resets the device).
About 88 MB at 2560×1440. Creation failure logs once, latches the temporal path off for the
session, and the legacy AO runs.

## Shaders

### AO pass (noise)

- `base = IGN(VPOS)` (interleaved gradient noise, Jimenez); second channel from a fixed pixel
  offset.
- `n = frac(base + R2(frame % 64))`, `R2(k) = frac(0.5 + k * (0.7548777, 0.5698403))`, computed on
  the CPU and passed in `TESR_AmbientOcclusionTemporal.xy`.
- Ring radius uses `n.y * 0.004` in place of `rand(uv)`, preserving the legacy distribution;
  `n.x * 2π` rotates the 5-sample ring per pixel per frame.
- `compareDepths`, the fog and 8000-unit fades, strength and `clamp(ao², ClampStrength, 1)` are
  unchanged; the final per-pixel value is what accumulates, so a still frame converges to the
  temporal mean of the legacy look.
- Output `float2(ao, viewDepth / farZ)`, linear view depth decoded the same way as TAA.
- Sky (raw depth ≥ 1) outputs ao = 1.

### Accumulate pass

1. Reconstruct world position; reproject with `TESR_PrevWorldViewProjectionTransform` to
   `prevUV` and `expectedDepth = prevClip.w / farZ`.
2. Sample history bilinearly at `prevUV`.
3. Reject history (use raw, count = 1) when `prevUV` is off-screen, history count is 0, the
   history weight constant is 0 (reset), or
   `|hist.depth - expectedDepth| / expectedDepth > TemporalDepthReject`.
4. Otherwise `count' = min(count + 1, 32)`, `w = max(1 / count', 1 - TemporalBlend)`,
   `ao = lerp(hist.ao, raw, w)`.
5. Write `(ao, currentDepth, count')`. Sky passes ao = 1, count 0.

No neighborhood clamp and no Catmull-Rom history sampling in v1 (the latter is available from
`TAA.fx` if camera motion visibly softens AO).

### Blur and Combine

Blur: the legacy 7-tap weights, offsets and `BlurDropThreshold` test, reading float depth instead
of `unpackDepth`. Combine: the legacy combine verbatim, including the luminance fade and the
`viewao` debug define.

## Settings and constants

Per section (`[Exterior]` and `[Interior]` of `AmbientOcclusion.ini`; a separate section would be
parsed as a third AO profile). Read, saved and exposed in the in-game menu;
`TemporalEnabled` is added to `BoolValues` in `GameMenuManager.cpp`.

| Key | Code default | Shipped INI |
|---|---|---|
| `TemporalEnabled` | 0 | 1 |
| `TemporalBlend` | 0.9 | 0.9 |
| `TemporalDepthReject` | 0.05 | 0.05 |

`TESR_AmbientOcclusionTemporal` (float4), bound in `SetConstantTableValue` next to the AO
constants and filled by `UpdateAmbientOcclusion`:

- `.xy` R2 offset for `frame % 64`
- `.z` `TemporalBlend`, or 0 on a reset frame
- `.w` `TemporalDepthReject`

### Frame index

The engine's display-frame counter at `[NiDX9Renderer + 0x8B0] + 0x3C`, bumped exactly once per
frame by 0x777A40 from the BeginFrame path 0x762600 (memory `skinning-bone-matrix-cache`).
`TESR_Tick.x` is unusable (`FrameRateManager::Time` is never assigned) and `UpdateConstants` has
three call sites.

### History reset

A reset sets `.z = 0` for one frame so the accumulate pass writes raw with count = 1. Triggers:

- first temporal frame after the targets are created;
- the temporal path did not run on the previous engine frame (AO toggled, interior/exterior
  profile switch, menus, loading screens, INI reload);
- camera moved more than 1024 units since the last temporal frame (load doors, fast travel; same
  threshold as `kGrassTeleportDist`).

## Error handling

- Effect load failure: `LoadEffect` returns false and logs; the legacy AO runs.
- Target creation failure: one-time warning, temporal latched off, legacy AO runs.
- Main-menu window (`Player`/`Tes` NULL): covered by the chain's existing guards.

## Verification

No test framework exists; verification is build, compile, then in-game checks.

1. MSBuild Release x86 via PowerShell.
2. fxc compiles `AmbientOcclusionTemporal.fx.hlsl` (fx_2_0) before launch; `CompileShaders = 1`
   produces the shipped `.fx`.
3. `git diff` shows `AmbientOcclusion.fx(.hlsl)` untouched; `TemporalEnabled = 0` matches today
   in-game.
4. Still camera, `viewao` debug build: grain converges within about a second; compare to a legacy
   screenshot (DDS decode, memory `or-screenshot-dds-diagnostic`).
5. Motion (user-driven): disocclusion edges briefly noisy then settle with no dark trails; no
   ground smears behind walking NPCs; no stale AO after load doors or fast travel; same result
   with TAA on and off.
6. `ProfileFrame` AO bucket, temporal vs legacy; expected +0.1–0.2 ms, measured value reported.
