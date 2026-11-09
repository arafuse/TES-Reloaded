---
name: mid-draw-renderer-matrices
description: "Anything run inside TrackSetupShaderPrograms (mid-scene shadow apply) must restore the renderer's world/view/proj/invView matrices: the engine builds THAT draw's transform constants from them right after; the state block does not cover them"
metadata:
  node_type: memory
  type: project
  originSessionId: ffe84fbd-fb2a-4713-be61-82503f171389
  modified: 2026-11-09T01:55:00.816Z
---

`TrackSetupShaderPrograms` runs our code BEFORE the engine's original `SetupShaderPrograms`, which
uploads the draw's transform constants from `TheRenderManager->worldMatrix / viewMatrix / projMatrix /
invViewMatrix` (engine NiDX9Renderer fields, GameNi.h). `SetupSceneCamera()` rewrites all four for image
space (world = translate by -camera). The `D3DSBT_ALL` state block restores device state only, not
these CPU-side matrices.

Bug fixed in f06ae92 (2026-11-08): `RenderShadowsMidScene` (fired at the first near-water draw) called
`SetupSceneCamera` without restoring them, so that one water draw landed off screen and the lakebed
showed through. Only the next draw is hit, because the engine resets the world matrix per geometry. The
symptom depended on camera pitch: a distant `Waterplane` in view was drawn first and took the damage
unnoticed. It reproduced with stock water shaders too, because `isNearWater` keys off the shader name.

**How to apply:** any new work inside a draw's setup hook must save and restore these four matrices
around `SetupSceneCamera` (or not call it). The other call sites (shadow-map setup,
RenderEffectsPre/PostHdr) run between draws and are fine. For "one object vanishes", diff two
`[GrassOrderDbg]` captures (`Develop.LogShaders` key) and look at what immediately precedes the object's
draw, not just whether it is drawn.

Related: [[grass-drawn-after-water]], [[near-shell-frame-order]], [[water-reflection-pass-detection]]
