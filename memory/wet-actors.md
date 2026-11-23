---
name: wet-actors
description: Wet actors feature map (WetActors.cpp, RainProbe NPC points, c102/c103, shader list, multipass overrides) and its traps — WetAlbedo/WetLightScale take the RAW normal alpha, early-return [branch] gets flattened, multipass VS must keep stock position math
metadata:
  type: project
---

Covers the outdoor path and the multipass path (flagged inns, interiors, actors near several lights).
Play-tested.

**Data flow:** `WetActors.cpp` keeps wetness 0..1 per refID (real-time soak/dry, game-hour skips dry
it, swimming = 1, saved in the co-save `'WETA'` record). `RenderHook::TrackSetupShaderPrograms` uploads
it per draw as `TESR_GEOM_Wetness` (c102: x = wetness, y = 1 keeps the legacy base-pass highlight) for
pixel shaders flagged `isWetActor` (`WetActorPixelShaders` in ShaderIOHook). `TESR_WetActorData` (c103)
= Darkening / Sheen / Glossiness. Draw → actor goes by walking `m_parent` (see [[actor-shader-passes]]).

**Rain probe:** 8 player points + 2 per NPC for up to 24 NPCs = 56 points (`RainProbe.cpp`, occlusion
query readback per [[d3d9-gpu-readback-stall]]); `GetRainProbeActorExposure(refID)`. Until the probe
has answered once, everyone uses the cheap outdoors-and-raining test.

**Traps:**
- `WetAlbedo(Albedo, SpecMask)` already applies `1 − SpecMask`; pass the raw normal-map alpha. Passing
  `1 − alpha` darkens shiny skin instead of pores.
- An early-return `[branch]` gets flattened by fxc, running the sheen on every pixel. Use `x = 0;
  [branch] if (Wet > 0) { … }` and check the asm for `if_lt` (see [[shader-pipeline-facts]]).
- VS SLS2005/2010 have a new half-vector output; `TESR_GEOM_Wetness.y` gates OR's old highlight so dry
  actors are unchanged. The `hasNewHalfVector` flag is only set when the override actually loaded.
- Outdoor hair uses `ExtraShadersExterior/SM3LL001` (stock copy plus the wet look); dialogue copies in
  `ExtraShadersDialog`/`SkinDialog` carry the wet look too.

**Multipass path** (pass structure in [[actor-shader-passes]]): stock SLS2031/2032/2035 (lighting) are
rewritten in `ExtraShaders/` to multiply the light by `WetLightScale(normal alpha)` before the texture
pass multiplies it in (= darkening the albedo; exactly 1 when dry); SLS2037/2041 (highlights) add
`WetSheenFromHalf` before saturate/alpha; OR's SKIN2006/2008 multiply by `WetLightScale`. Because OR
compiles model 3 only, every VS feeding them is rewritten too (SLS2023/2024/2027/2028/2031/2032/2035/2036),
plus stock-equivalent SLS2038/2042 so the rewritten VS never pair with a model-2 PS. These overrides
load in EVERY cell (not interior-shadow variants), so dry output must stay identical to stock.
- **Position rule:** follow-up passes are depth-tested against the first pass, and the texture pass
  keeps the stock VS. Rigid VS use `mul(ModelViewProj, IN.Position)`, skinned use `SkinPosition`
  (`ExtraShaders/Includes/Skinning.hlsl`) with stock mul/mad order, or actors z-fight.
- **Drying indoors:** `InteriorDryTime` (default 90 s) replaces `DryTime` for actors in a cell with no
  worldspace (flagged inns included); `WetState::Indoors` keeps the rate for unloaded actors.

**Open:** regular (unflagged) interiors passed a visual play-test, but their pass structure was never
captured. If some actor part looks dry indoors, capture it with the `Develop.LogShaders` key before
guessing which shader draws it.

**How to apply:** adding a new actor shader variant means adding it to `WetActorPixelShaders` and
calling `WetAlbedo`/`WetSheen` in it (multipass: `WetLightScale`/`WetSheenFromHalf`, and rewrite every
VS it pairs with).
