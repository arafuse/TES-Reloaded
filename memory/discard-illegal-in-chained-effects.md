---
name: discard-illegal-in-chained-effects
description: "`discard` in any post-process .fx effect is a stale-buffer bug once EffectChainPingPong is on — return the source color instead"
metadata: 
  node_type: memory
  type: project
  originSessionId: 74aa57b5-a845-4148-8cf6-c6c9e981e0f0
  modified: 2026-09-04T23:41:10.233Z
---

2026-09-04 (branch feat/misc-4). Adam reported "a wall of grey fuzz" with a fading camera trail
whenever he stood in an occluded spot in the rain. Root cause: `discard` in a full-screen post
effect.

The legacy `EffectRecord::Render` path drew into the live scene render target, which already held
the effect's own input, so a discarded pixel harmlessly kept the scene. `ShaderManager::RenderChained`
(added in `feat: Cleanup and optimizations pt 5`, default-on since `EffectChainPingPong` = 1) rotates
between three scratch buffers instead — `RenderedSurface` / `PingSurface` / `EffectSurface`. The
destination does **not** contain the source image, so a discarded pixel keeps whatever that slot held
from an earlier effect or an earlier frame. In a region that discards every frame the staleness
compounds, which is the fading trail.

**Why:** any effect that opts a pixel out must write the pass-through value explicitly; the chain
gives no "leave it alone" semantics.

**How to apply:** never use `discard`/`clip` in `OblivionReloaded/Shaders/**/*.fx.hlsl` post effects —
`return float4(color.rgb, 1.0f)` (or the pass's own input) instead. Fixed in `Rain.fx.hlsl`,
`Snow.fx.hlsl` (`ortho < 0.01`) and `SnowAccumulation.fx.hlsl` `BlurNormals` (sky depth).
Bonus: the asm shows HEAD's `texkill` did *not* skip the 24-layer streak loop, while the early
`return` compiles to a real `if_lt/else/endif` around it — so this is also a small win on occluded
pixels. `discard` stays fine in raw `.pso.hlsl` shaders (ShadowMap, ShadowCubeMap): those are real
geometry passes, not chain effects.

Repro/confirm without a build: `[Main] EffectChainPingPong=0` restores the old path and the artifact
disappears.

Related: [[shader-pipeline-facts]], [[fxc-verify-shader-edits]], [[shader-deployment-workflow]]
