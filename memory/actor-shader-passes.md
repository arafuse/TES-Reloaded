---
name: actor-shader-passes
description: MEASURED actor draw passes outdoors (base, sun spec SLS2039, rim SLS2075, hair SM3LL*); spec pass only within fSpecularLOD2; NiSkinInstance::RootParent is NOT the actor root; which actor shaders are stock outdoors / swapped for dialog
metadata:
  type: project
---

Captured 2026-10-29 with the `[ActorDraws]` diagnostic (Develop.LogShaders key, RenderHook.cpp), outdoors in rain, ~40 loaded actors.

**Pass order per actor part (main scene), each pass a separate draw:**
- Base (opaque): PS `SLS2003` (VS SLS2003 rigid / SLS2005 skinned), `SLS2012`/`SLS2013` (VS 2009/2010, when point lights touch it), `SLS2018`/`SLS2019` (VS 2013/2014: horses, creature eyes), exposed skin `SKIN2001`/`SKIN2003`, hair `SM3LL003` (OR) or `SM3LL001` (stock outdoors).
- Sun specular, additive: VS SLS2033/2034 → PS `SLS2039` (OR). Stock math: `pow(N·H, Toggles.z) * normalAlpha * PSLightColor[0]` × shadow. Point-light spec is `SLS2041` (stock, attenuation map).
- Rim/fill, additive, EVERY actor at every distance: VS SLS2071/2072 → stock `SLS2075` (hair: SM3028/29 → SM3035/36). Not lighting from lights; leave alone.
- Others: `SLS2043` = env-cube reflection (env-mapped parts only); `SLS1030`/`SLS1031` = vertex-color output passes.

**The spec pass is distance-LOD'd** by Oblivion.ini `fSpecularLOD2` (800 on this setup): only the player and near NPCs get SLS2039 at all. Anything that must show at range (e.g. wet darkening) has to live in the base pass.

**`NiSkinInstance::RootParent` never equals the actor's `GetNode()` root** (0 of ~800 draws). Map a draw to its actor by walking `m_parent` up to the ref root (or the player's `firstPersonNiNode`).

**Outdoor swaps:** `InteriorShadowShaders` (ShaderManager.cpp) revert to stock outdoors, including `SLS2041` and `SM3LL001`; `SM3LL003` loads from `ExtraShadersExterior`. In exterior dialogue, `ExteriorDialogShaders` (SLS2003/2018/2039, SKIN2001/2003/2007) load from `ExtraShadersDialog`/`SkinDialog` — any actor material change must be made in those copies too. Interiors use the interior-shadow variants, which were not captured.

**How to apply:** for per-actor material effects (the planned wet-actors feature: per-draw `TESR_GEOM_*` constant, player gated by RainProbe, NPCs switchable between cheap "outdoors + raining" and accurate per-NPC rain probe points within a configurable radius), edit the base and spec shaders above in every folder variant. Built as [[wet-actors]]. See [[sun-shadow-probe-consumers]], [[shader-pipeline-facts]].
