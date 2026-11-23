---
name: point-shadow-light-ratio
description: Sun and point shadow applies remove only their own light's share (ratio of an unclamped engine light sum); what the total counts, the unverified ambient source, and the overlap approximation
metadata:
  type: project
---

Both mid-scene shadow applies multiply the scene by the fraction of light that stays, with every light
sum taken from `Shadows/SceneLightTotal.hlsl`:
- **Point** (`ShadowsPoint.fx.hlsl`, 220ba1a): (total - lost) / total. The old per-light product of
  `1 - lum * att` removed that fraction of ALL light, darkening sunlit areas round exterior torches and
  compounding where lights overlapped.
- **Sun** (`ShadowsExteriors.fx.hlsl`, 93f1a26): the existing term `sun` (in [Darkness, 1]) now scales only
  base = ambient + directional × N.L, so factor = (sun × base + points) / (base + points). Without point
  light it is exactly `sun`, so the tuned Darkness tiers look unchanged in daylight. Before this the sun
  shadow also darkened the torchlight inside it.

**Why ratios are valid:** the engine's SLS pixel shaders sum `AmbientColor + Σ PSLightColor * saturate(N.L)`
with no clamp (only `max(..., 0)`, checked in `SLS2002.pso.hlsl`) and multiply by albedo. A light's share is
therefore an exact ratio and albedo cancels. Brightness only: luma throughout, so shadows never tint.

**What the sums count** (published every frame, point shadows on OR off, by
`ShadowManager::PublishPointBaseLight`; `TESR_ShadowBaseLight.z` = 1 when any point light is published):
- ambient luma and directional luma × N.L, from `Tes->niDirectionalLight` (`Amb`, `Diff`, `Dimmer`;
  `m_direction` is the direction of travel, so to-light = -m_direction)
- each slot's UNFADED luminance (`TESR_ShadowSlotLuminance`), over the cube far plane (257 for torches);
  the point pass's lost light uses the faded one, so lost <= total
- the 8 nearest scene lights that hold no slot (`TESR_ShadowFillLights0/1` rows: camera-relative xyz,
  w = radius). Slot lights are excluded by `PointLightSlot::ShadedLight`, an identity token that survives
  eviction (eviction leaves RetiringLight NULL on purpose). With point shadows off, every light is a fill.
- N is the depth-reconstructed geometric normal, so shadow depth can facet slightly on low-poly curved
  meshes; point-pass pixels facing away from a light skip its cube PCF

**Ambient source, logged 2026-11-19:** with `Develop.ProfileFrame` on, each cell logs
`[ShadowProfile] point base light:`. One session across day, night and (by their 8-bit-exact colours)
interiors read: day ambient luma ~0.34, directional ~0.5-0.83; night 0.05 / 0.08; Dimmer always 1; to-light
points up by day and by night. Never zero or oversized, so neither failure mode occurs (zero ambient
would restore full-depth point shadows and erase sun shadows near point lights; too large would erase
point shadows). STILL UNPROVEN: that these exact values are the shader's AmbientColor / PSLightColor[0];
only a constant dump from a lit draw would settle it.

**How to apply:**
- Overlap is approximate: the point pass runs after the sun pass and cannot see `sun`, so where both
  shadows overlap the point shadow is shallower than exact by lost × base × (1 - sun) / total², never
  deeper. Exactness needs `sun` passed between the passes (an MRT side target), which breaks on a
  multisampled scene target. Weigh that before attempting it.
- Any new shadow-like darkening effect should take its share of this same total, not multiply the scene.
- SunShadowStealth still uses the Darkness tier for the player's light level (point light counted in full
  there), so it is consistent with the sun apply. See [[sun-shadow-probe-consumers]].

Related: [[grass-drawn-after-water]] (where the applies fire), [[shader-deployment-workflow]].
