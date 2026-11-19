---
name: point-shadow-light-ratio
description: Point-shadow apply multiplies by (total - lost) / total light, never a per-light 1 - lum*att; what "total" counts, the unverified ambient source, and the sun pass's matching flaw
metadata:
  type: project
---

Since 220ba1a (2026-11-18) `ShadowsPoint.fx.hlsl` darkens a pixel by the fraction of its light the
shadows remove: factor = (total - lost) / total. The old factor, a product of `1 - lum * att` per light,
removed that fraction of ALL light at the pixel. So sunlit areas around exterior torches went dark,
overlapping lights compounded, and faces turned away from a light still got its shadow.

**Why it is valid:** the engine's SLS pixel shaders sum `AmbientColor + Σ PSLightColor * saturate(N.L)`
with no clamp (only `max(..., 0)`, checked in `SLS2002.pso.hlsl`) and multiply by albedo. Removing one
light is therefore an exact ratio and albedo cancels. Brightness only: luma throughout, so no tinting.

**What total counts** (published by `ShadowManager::PublishPointBaseLight`):
- ambient luma and directional luma × N.L, from `Tes->niDirectionalLight` (`Amb`, `Diff`, `Dimmer`;
  `m_direction` is the direction of travel, so to-light = -m_direction)
- each slot's UNFADED luminance (`TESR_ShadowSlotLuminance`); lost uses the faded one, so lost <= total
- the 8 nearest scene lights that hold no slot (`TESR_ShadowFillLights0/1` rows: camera-relative xyz,
  w = radius). Slot lights are excluded by `PointLightSlot::ShadedLight`, an identity token that survives
  eviction (eviction leaves RetiringLight NULL on purpose)
- N is the depth-reconstructed geometric normal (as in the sun pass), so shadow depth can facet slightly
  on low-poly curved meshes; back-facing pixels skip the cube PCF

**UNVERIFIED:** that niDirectionalLight's Amb/Diff are what feed AmbientColor / PSLightColor[0], in
interiors as well as exteriors. With `Develop.ProfileFrame` on, each cell logs
`[ShadowProfile] point base light:`. If ambient reads zero, interior point shadows go back to full depth.
If it reads too large, they vanish.

**How to apply:**
- The total ignores sun visibility deliberately. The sun pass scales the whole sum by Darkness, so a point
  factor taken against the unshadowed total composes with it. Don't add a sun-visibility term.
- The sun pass still has the mirror flaw: `lerp(darkness, 1, lit)` also scales torchlight inside a sun
  shadow. Fixing it would need the same ratio (sun share of total), and is not done yet.
- [[sun-shadow-probe-consumers]] and the Darkness tiers are unaffected; FogStrength still scales the factor.

Related: [[grass-drawn-after-water]] (where the apply fires), [[shader-deployment-workflow]].
