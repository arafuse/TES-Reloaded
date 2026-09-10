---
name: shader-bake-defines
description: "INI-derived D3DXMACRO shader bake mechanism (branch feat/shader-bake-experiment) — what's implemented, the measured deltas, and which candidates were measured and rejected"
metadata: 
  node_type: memory
  type: project
  originSessionId: e3572ca8-8108-4517-8ecc-669d245de67f
  modified: 2026-08-30T01:58:34.418Z
---

`ShaderManager::BuildShaderDefines()` (added 2026-08-29, branch `feat/shader-bake-experiment`) feeds INI-derived `D3DXMACRO`s into `CompileShader`/`CompileEffect`, which previously passed `NULL` for `pDefines`, so fxc could never dead-strip a path a setting had switched off. Cache staleness is handled by stamping the macro set to `ShaderDefines.txt` beside the shaders; a mismatch forces a full recompile at startup, independent of `Develop.CompileShaders`.

**Shipped** (each verified byte-identical to the pre-change build when the macro is absent, via `fxc /Fo` + `Get-FileHash` — see [[fxc-verify-shader-edits]]):
- `TESR_TERRAIN_NEARSPECULAR_OFF` — SLS2048/2049 52/55 → 14/17 slots. Free: Adam's `NearSpecular` is already 0.
- `TESR_VOLUMETRICLIGHT_ANIMATEDFOG_OFF` — march pass 393 → 220 slots, 8 in-loop `sincos` → 0. Needs ALL weather profiles at `AnimatedFog=0` (37 of 38 are at 1, one at 4), so it does not fire today.
- `TESR_GRASS_COLLISION_SOURCES` = 1 + `CollisionTrailSlots`, capped 3, or 0 if both collision strengths are 0. GRASS2028/2030/2034 = 190/208/230 at bound 3 (current), 120/140/162 at 1, 85/103/125 at 0. ~39 slots per source.
- `TESR_SHADOWS_ADAPTIVEBIAS_ONLY` — ShadowsExteriors 566 → 519. Pays off now (`AdaptiveBias=1`).

**Traps found while doing this:**
- The grass collision loop is `[unroll]` + `break`, which compiles to MASKING, not flow control — every slot the compile-time bound allows is paid regardless of the live source count. The runtime `break` must stay: slot 0 has a hardcoded weight of `1.0` against a position the plugin `memset`s to 0, so a bound with no runtime guard displaces grass around the worldspace origin when no sources are live.
- `AdaptiveBias` and `BiasTerminatorWidth` are single globals, not per-weather — `PublishShadowBiasConstants` reads the canonical `Exteriors` struct, never the selected tier (`ExteriorsAlt`/`ExteriorsPrecip` differ only in `Darkness`).
- `SettingsVolumetricLight` is private; use `GetVolumetricLightProfiles()`.

**Measured and rejected** (don't re-derive): Skin 95 → 92 (pure multipliers); Water 218 → 208 and loses the Blood/Lava/Bravil/Sewers profiles; SMAA has no INI at all (already `#define`d, already cut to 16/8); Underwater is per-water multipliers and already gated to near/under water; VolumetricFog is ~15 instructions; Rain has an empty settings struct, and Snow's loop count scales with weather intensity so baking it makes light snow cost as much as a blizzard; `Effects.Extra` iterates an empty `ExtraEffects` folder.

**Still open:** ShadowsExteriors terminator ramp (−6 slots, `BiasTerminatorWidth` already 0) and the static-map crossfade (−194 slots and −6 texture, but `FadeTime=1.0` and it is a real `if b1` branch, so the saving is latent register pressure, not ALU). `DistantSpecular`/`MiddleSpecular` are dead INI keys — read into `ShaderConst.Terrain.Data.x/.w`, never referenced by any shader.

Related: [[shader-pipeline-facts]], [[shader-deployment-workflow]], [[fxc-verify-shader-edits]].
