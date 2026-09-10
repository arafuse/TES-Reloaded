---
name: shadow-bias-world-units
description: "Exterior sun-shadow depth bias in WORLD units (normalized x 2*ShadowMapFarPlane = x16384), the contact light-leak band it causes at wall bases, and a latent far-cascade exclusion bug"
metadata: 
  node_type: memory
  type: project
  originSessionId: e49fe85e-acf4-4a0f-96dc-f84088b3eff6
  modified: 2026-09-11T00:34:54.195Z
---

`deferredConstBias`/`deferredFarConstBias` are normalized ortho depth; the ortho spans
`2 * ShadowMapFarPlane` (16384 at 8192), so 0.001 = ~16.4 world units, and the adaptive path
multiplies by `(1 + min(tan(acos|ndl|), BiasMaxSlope))` -> up to ~82 units. The SAME normalized
constant is used for near (2 units/texel) and far (8 units/texel) — it is not texel-scaled, unlike
the normal offset (`deferredNormBias`, scaled by 2R/Size in PublishShadowBiasConstants).

Consequence: a receiver is unoccluded when the occluder is closer along the sun ray than the bias.
On a floor beside a sun-side wall of thickness t at sun elevation e, the lit band is
`x < bias_world * cos(e) - t` (~58 - t units at e=20°, ~81 - t at e=10°) — distant tree shadows show
through it. Fixed 2026-09 for sun-AWAY faces (abs(ndl) in the slope term, commit 4a04e86); the
sun-facing contact band remains a tuning trade vs acne.

Ruled out while investigating (2026-09-10): record flag 0x200 ("NotCastShadows" in Game.h, UESP
says "Casts shadows") is set on exactly 1 exterior ref in Oblivion.esm, so the naming is moot;
persistent exterior statics are ~1.9k of 484k and nearly all markers.

Latent bug (unfixed): RootInShadowFrustum(MapFar) excludes casters "fully inside near" by testing
FAR-anchor-relative centers against ShadowMapFrustum[MapNear], which is NEAR-anchor-relative, and the
two cached regions rebake independently — so after near re-anchors, receivers that fall back to the
far map lose static casters from the old near box (holes beyond ~1-2k units, behind the player).

Related: [[shader-deployment-workflow]], [[fxc-verify-shader-edits]]
