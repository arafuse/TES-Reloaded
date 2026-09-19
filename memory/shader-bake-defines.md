---
name: shader-bake-defines
description: "INI-derived D3DXMACRO shader baking is NOT implemented (pDefines is NULL; ShaderDefines.txt is an orphan); measured per-candidate savings and the traps, for if it is revisited"
metadata:
  type: project
---

**Current state:** `ShaderManager::CompileShader`/`CompileEffect` pass `NULL` for `pDefines`, so fxc
can never dead-strip a path an INI setting has switched off. An experimental `BuildShaderDefines()`
that fed INI-derived `D3DXMACRO`s (with a stamp file forcing recompiles on mismatch) was never merged.
`OblivionReloaded/Shaders/ShaderDefines.txt` is tracked but nothing reads it, and no shader
references its macros.

**Measured candidates** (instruction slots, for if this is revisited):
- Terrain near specular off — SLS2048/2049 52/55 → 14/17.
- VolumetricLight animated fog off — march pass 393 → 220, 8 in-loop `sincos` → 0; only pays if ALL
  weather profiles have `AnimatedFog=0`.
- Grass collision source bound — ~39 slots per source (GRASS2028/2030/2034 at 3 sources: 190/208/230).
- ShadowsExteriors adaptive-bias-only — 566 → 519.
- Rejected: Skin (95 → 92), Water (218 → 208, loses Blood/Lava/etc. profiles), SMAA (no INI), Underwater
  (already gated), VolumetricFog (~15 instructions), Rain (empty settings), Snow (baking its loop makes
  light snow cost like a blizzard), `Effects.Extra` (empty folder).

**Traps:**
- The grass collision loop is `[unroll]` + `break`, which compiles to MASKING, not flow control —
  every slot the compile-time bound allows is paid regardless of the live source count. The runtime
  guard must stay: slot 0 has a hardcoded weight of 1.0 against a position that is 0 when no sources
  are live, which would displace grass around the worldspace origin.
- `AdaptiveBias` and `BiasTerminatorWidth` are single globals, not per-weather —
  `PublishShadowBiasConstants` reads the canonical `Exteriors` struct, never the selected tier.
- `SettingManager::SettingsVolumetricLight` is private; only per-weather lookup
  (`GetSettingsVolumetricLight(WeatherName)`) is public, so iterating all profiles needs an accessor.
- Terrain `DistantSpecular`/`MiddleSpecular` INI keys are read but no shader uses them.

Related: [[shader-pipeline-facts]], [[fxc-verify-shader-edits]].
