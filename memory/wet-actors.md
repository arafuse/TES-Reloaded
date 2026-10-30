---
name: wet-actors
description: Wet actors feature map (WetActors.cpp, RainProbe NPC points, c102/c103, shader list) and its traps — WetAlbedo takes the RAW normal alpha, early-return [branch] gets flattened, interiors not covered
metadata:
  type: project
---

Shipped on `feat/misc-5` (2026-10-30), play-tested. Spec: `docs/superpowers/specs/2026-10-29-wet-actors-design.md`.

**Data flow:** `WetActors.cpp` keeps wetness 0..1 per refID (real-time soak/dry, game-hour skips dry it, swimming = 1, saved in the co-save `'WETA'` record). `RenderHook::TrackSetupShaderPrograms` uploads it per draw as `TESR_GEOM_Wetness` (c102: x = wetness, y = 1 keeps the legacy base-pass highlight) for pixel shaders flagged `isWetActor` in ShaderIOHook. `TESR_WetActorData` (c103) = Darkening / Sheen / Glossiness. Draw → actor goes by walking `m_parent` (see [[actor-shader-passes]]).

**Rain probe:** 8 player points + 2 per NPC for up to 24 NPCs = 56 points in a 56×1 R32F target; `GetRainProbeActorExposure(refID)`. Until the probe has answered once, everyone uses the cheap outdoors-and-raining test.

**Traps:**
- `WetAlbedo(Albedo, SpecMask)` already applies `1 − SpecMask`; pass the raw normal-map alpha. Passing `1 − alpha` darkened shiny skin instead of pores (fixed in c7b84c8).
- An early-return `[branch]` got flattened by fxc, running the sheen on every pixel. Use `x = 0; [branch] if (Wet > 0) { … }` and check the asm for `if_lt` (see [[shader-pipeline-facts]]).
- VS SLS2005/2010 got a new half-vector output; `TESR_GEOM_Wetness.y` gates OR's old highlight so dry actors are unchanged. The `hasNewHalfVector` flag is only set when the override actually loaded.
- Outdoor hair uses a new `ExtraShadersExterior/SM3LL001` (stock copy plus the wet look); dialogue copies in `ExtraShadersDialog`/`SkinDialog` carry the wet look too.

**Not done:** interiors (interior-shadow shader variants have no wet look, so hair stays dry indoors).

**How to apply:** adding a new actor shader variant means adding it to `WetActorPixelShaders` and calling `WetAlbedo`/`WetSheen` in it.
