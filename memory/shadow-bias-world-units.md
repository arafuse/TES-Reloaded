---
name: shadow-bias-world-units
description: "Exterior sun-shadow depth bias: authored in per-cascade TEXELS under AdaptiveBias (converted to normalized ortho depth = world / 2*FarPlane), why a world-unit bias leaks light at wall contacts, and the removed far-cascade exclusion"
metadata:
  node_type: memory
  type: project
  originSessionId: e49fe85e-acf4-4a0f-96dc-f84088b3eff6
  modified: 2026-09-11T18:17:59.291Z
---

Under `AdaptiveBias = 1`, `deferredConstBias`/`deferredFarConstBias` are shadow-map TEXELS of each
cascade (since 2026-09-11), like `deferred*NormBias`. `PublishShadowBiasConstants` converts them:
`texels * (2R/Size) / (2 * Selected->ShadowMapFarPlane)` -> normalized depth in
`TESR_ShadowBiasDeferred.z/.w`; the shader and VolumetricLight still see normalized values. Legacy
path (`AdaptiveBias = 0`, also the CODE default) keeps raw normalized values — so code defaults stay
legacy-unit ("0.001"); only the shipped INI carries texel values. 1 texel = 2 units near, 8 far.
Old normalized reference: 0.001 = ~16.4 world units at FarPlane 8192.

Why texels: a receiver stays lit wherever its occluder is closer along the sun ray than the bias.
On a floor beside a sun-side wall of thickness t at sun elevation e the lit band is
`x < bias_world * cos(e) - t`; wall tops show slivers. The effective bias is
`value * (1 + min(tan(acos|ndl|), BiasMaxSlope))` — `abs(ndl)` since commit cff12bb (sun-away faces
used to get the peak ~82 units and leaked tree shadows through thin walls).

Removed 2026-09-11: the MapFar "fully inside near frustum" exclusion (RootInShadowFrustum and terrain
InShadowFrustum). It tested far-anchor-relative centers against near-anchor-relative planes, and the
two cached maps re-anchor independently, so far bakes lost casters that receivers later needed. Now a
single `SphereInShadowFrustum`; far bakes draw the near area's >=100-radius casters too.

Ruled out (2026-09-10): record flag 0x200 ("NotCastShadows" in Game.h, UESP says "Casts shadows") is
set on 1 exterior ref in Oblivion.esm; persistent exterior statics are ~1.9k of 484k, nearly all markers.

Related: [[shader-deployment-workflow]], [[fxc-verify-shader-edits]]
