---
name: shader-bake-defines
description: "INI-derived D3DXMACRO shader baking is NOT implemented (pDefines is NULL; ShaderDefines.txt is an orphan); measured per-candidate savings and the traps, for if it is revisited"
metadata:
  type: project
---

**Current state:** `ShaderManager::CompileShader`/`CompileEffect` pass `NULL` for `pDefines`, so fxc
can never dead-strip a path an INI setting has switched off. `OblivionReloaded/Shaders/ShaderDefines.txt`
is tracked but nothing reads it, and no shader references its macros.

**Measured candidates** (instruction slots, for if this is revisited):
- Terrain near specular off — SLS2048/2049 52/55 → 14/17.
- VolumetricLight animated fog off — march pass 393 → 220, 8 in-loop `sincos` → 0; only pays if ALL
  weather profiles have `AnimatedFog=0`.
- ShadowsExteriors adaptive-bias-only — 566 → 519.
- Rejected: Skin (95 → 92), Water (218 → 208, loses Blood/Lava/etc. profiles), SMAA (no INI), Underwater
  (already gated), VolumetricFog (~15 instructions), Rain (empty settings), Snow (baking its loop makes
  light snow cost like a blizzard), `Effects.Extra` (empty folder).

**Traps:**
- An `[unroll]` loop with `break` compiles to MASKING, not flow control: every iteration the
  compile-time bound allows is paid regardless of the live count. Baking a loop bound only helps when
  the bound itself shrinks.
- `AdaptiveBias` and `BiasTerminatorWidth` are single globals, not per-weather —
  `PublishShadowBiasConstants` reads the canonical `Exteriors` struct, never the selected tier.
- `SettingManager`'s per-weather VolumetricLight map is private; only per-weather lookup
  (`GetSettingsVolumetricLight(WeatherName)`) is public, so iterating all profiles needs an accessor.
- Terrain `DistantSpecular`/`MiddleSpecular` INI keys are read but no shader uses them.

Related: [[shader-pipeline-facts]], [[fxc-verify-shader-edits]].
