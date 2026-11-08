---
name: shadow-bias-world-units
description: "Exterior sun-shadow depth bias: authored in per-cascade TEXELS under AdaptiveBias (converted to normalized ortho depth = world / 2*FarPlane), why a world-unit bias leaks light at wall contacts, and why there is no far-cascade near-exclusion"
metadata:
  type: project
---

Under `AdaptiveBias = 1`, `deferredConstBias`/`deferredFarConstBias` are shadow-map TEXELS of each
cascade, like `deferred*NormBias`. `ShadowManager::PublishShadowBiasConstants` converts them:
`texels * (2R/Size) / (2 * Selected->ShadowMapFarPlane)` -> normalized depth in
`TESR_ShadowBiasDeferred.z/.w`; the shader and VolumetricLight still see normalized values. The legacy
path (`AdaptiveBias = 0`, also the CODE default) keeps raw normalized values — so code defaults stay
legacy-unit ("0.001"); only the shipped `Shadows.ini` carries texel values. 1 texel = 2 units near,
8 far. Normalized reference: 0.001 = ~16.4 world units at FarPlane 8192.

Why texels: a receiver stays lit wherever its occluder is closer along the sun ray than the bias.
On a floor beside a sun-side wall of thickness t at sun elevation e the lit band is
`x < bias_world * cos(e) - t`; wall tops show slivers. The effective bias is
`value * (1 + min(tan(acos|ndl|), BiasMaxSlope))` — `abs(ndl)`, because sun-away faces otherwise hit
the peak and leak tree shadows through thin walls.

**Actors have their own bias** (`deferredSkinConstBias`, skin-map texels, shipped 8 = ~16 units; missing key
falls back to `deferredConstBias`) → `TESR_ShadowBiasSkin.x`, used only by the skin-map lookup. Actors are
only in MapSkin (static bakes skip them). With the 1-texel near bias, bodies a dozen texels wide
self-shadow along triangle edges near the shadow line, faceting their smooth shading (the regression
after fe8c4e6; play-test confirmed 8 texels fixes it). Don't fold it back into the near bias.

There is deliberately no MapFar "fully inside near frustum" exclusion: it tested far-anchor-relative
centers against near-anchor-relative planes, and the two cached maps re-anchor independently, so far
bakes lost casters that receivers later needed. Culling is a single `SphereInShadowFrustum`; far bakes
draw the near area's >=100-radius casters too. Don't reintroduce the exclusion.

Ruled out as a caster filter: record flag 0x200 ("NotCastShadows" in Game.h, UESP says "Casts
shadows") is set on 1 exterior ref in Oblivion.esm; persistent exterior statics are ~1.9k of 484k,
nearly all markers.

Related: [[shader-deployment-workflow]], [[fxc-verify-shader-edits]]
