---
name: speedtree-property-not-pp-lighting
description: SpeedTree lighting properties are NOT BSShaderPPLightingProperty, so SetupAlphaTexture's textures[0] read is out of bounds for tree branches
metadata:
  type: project
---

`SpeedTreeShaderLightingProperty` and `BSShaderPPLightingProperty` are **sibling** subclasses of
`BSShaderLightingProperty` (0x9C in Oblivion), not parent/child. Sizes: PP is 0xF0 with
`NiTexture** textures[4]` at **0xBC**; SpeedTree is 0xA8, and `SpeedTreeLeafShaderProperty` 0xB0.
There is no texture array at 0xBC in the SpeedTree branch of the hierarchy.

`ShadowManager::SetupAlphaTexture` does `((BSShaderPPLightingProperty*)LProp)->textures[0]` after
only checking `LProp->IsLightingProperty()` — and that check **accepts**
`VFTSpeedTreeBranchShaderProperty` (0x00A92A94), alongside BSShaderPPLighting, Hair and
Lighting30. So a SpeedTree branch with an alpha property, drawn into a pass with
`AlphaEnabled = 1` (ExteriorsNear, ExteriorsOrtho), reads a bogus pointer at LProp+0xBC and
dereferences it two levels deep (`Texture->rendererData->dTexture`).

**Why:** pre-existing, still unfixed as of the DynamicTrees work (2026-09-20). It has not been
observed to crash, which suggests branch geo usually carries no alpha blend/test flag — but that
is an assumption about content, not a guarantee, and mod-added trees can break it.

**How to apply:** anything that widens which passes draw SpeedTree branches, or that turns
`AlphaEnabled` on for a pass that draws them, walks into this. A fix needs a VFT test before the
cast, not a wider `IsLightingProperty()`. The DynamicTrees overlay dodges it only because
`[ExteriorsSkin] AlphaEnabled = 0`; if that is ever flipped on, fix this first. Leaves are
unaffected — they never reach SetupAlphaTexture, and their alpha test is forced by
`TESR_ShadowData.x == 2` in ShadowMap.pso.

Struct layouts live in the OBLIVION block of GameNi.h (see [[gameh-multigame-blocks]]).
Related: [[shadow-bias-world-units]], [[shader-pipeline-facts]].
