---
name: discard-illegal-in-chained-effects
description: "`discard` in a post-process .fx effect that writes the chain destination is a stale-buffer bug under EffectChainPingPong — return the source color instead"
metadata:
  type: project
---

`EffectRecord::RenderChained` (used while `[Main] EffectChainPingPong = 1`, the default) rotates
between scratch buffers — `RenderedSurface` / `PingSurface` / `EffectSurface`. The destination does
**not** contain the effect's source image, so a discarded pixel keeps whatever that slot held from an
earlier effect or an earlier frame. In a region that discards every frame the staleness compounds
into a fading camera trail / "grey fuzz". The legacy `EffectRecord::Render` path drew into the live
scene target, which is why `discard` used to be harmless.

**Why:** the chain gives no "leave it alone" semantics; any effect that opts a pixel out must write
the pass-through value explicitly.

**How to apply:** in a `.fx.hlsl` pass whose output is the chain destination, `return
float4(color.rgb, 1.0f)` (or the pass's own input) instead of `discard`/`clip`. Rain, Snow and
SnowAccumulation `BlurNormals` follow this (see their comments). An early `return` also compiles to a
real `if/else` that skips the work, which `texkill` did not. `clip()` still exists in
AmbientOcclusion `BlurPS`, DepthOfField and VolumetricLight — not audited against this rule, so check
where those passes write before relying on them. `discard` is fine in raw `.pso.hlsl` geometry
shaders (ShadowMap, ShadowCubeMap).

Confirm a suspected case without a build: `[Main] EffectChainPingPong = 0` restores the old path.

Related: [[shader-pipeline-facts]], [[fxc-verify-shader-edits]], [[shader-deployment-workflow]]
