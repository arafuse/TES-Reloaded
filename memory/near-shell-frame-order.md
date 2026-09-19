---
name: near-shell-frame-order
description: "Load-bearing ordering in TrackRenderObject/TrackSetupShaderPrograms for the near shell (far pass [M,F], then shell [n,M]); read before moving any resolve, clear, flatten or InMainScenePass write"
metadata: 
  node_type: memory
  type: project
  originSessionId: 1b455e64-13f9-4fe7-8d7c-6560a5af7d8a
  modified: 2026-09-19T20:15:18.381Z
---

The near shell renders the main scene twice: far pass (frustum [M, F]) then the shell ([n, M]) after a depth clear to `ShellClearDepth`. The order below used to be spelled out in long inline comments (condensed 2026-09-19); each step breaks something if moved.

In `TrackRenderObject` (RenderHook.cpp), after the far pass:
1. `InMainScenePass = false` BEFORE the shell render. It disarms the near-water mid-scene shadow trigger (else a second apply over the shell's water) and SetCT's depth resolve for the rest of the FRAME. `DepthBufferFilled` can't do this: it is per-scene and every BeginScene (water reflection included) reopens it. Do not "close" `DepthBufferFilled` by hand instead: that would also fire with the shell off, which must stay byte-identical to vanilla.
2. Pre-water fallback (resolve + `RenderShadowsMidScene`) if no near-water draw happened.
3. `ResolveDepthBuffer()` BEFORE the shell's clear: post-processing only ever sees the far pass's [M, F] depth.
4. Shell render. At its first near-water draw `PrepareShellNearWater` captures TESR_RenderedBuffer (masked to shell coverage) and clamps TESR_DepthBufferPreWater. Every shell WATER draw gets pre-water depth swapped in (`BindShellWaterPreWaterDepth`).
5. `RestoreShellWaterDepthSamplers()` right after the shell, before the reflection render can inherit the swap.
6. `FlattenShellDepth()` after the LAST shell draw (shell water needs the unflattened far-pass depth, see fa7f347) and before TrackProcessImageSpaceShaders.
7. First-person node render: must NOT re-resolve depth while the shell is active (the depth buffer now holds only the shell).

SetCT's depth-resolve gate (`!ShellActive || InMainScenePass`) matters most on the NvAPI path, where ResolveDepthInto copies the cached main depth-stencil rather than whatever the off-screen render has bound.

Related: [[water-reflection-pass-detection]], [[grass-drawn-after-water]], [[beginscene-pre-init-window]]
