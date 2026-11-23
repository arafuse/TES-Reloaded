---
name: mid-draw-renderer-matrices
description: "Anything run inside TrackSetupShaderPrograms (mid-scene shadow apply) must restore the renderer's world/view/proj/invView matrices: the engine builds THAT draw's transform constants from them right after; the state block does not cover them"
metadata:
  type: project
---

`TrackSetupShaderPrograms` runs our code BEFORE the engine's original `SetupShaderPrograms`, which
uploads the draw's transform constants from `TheRenderManager->worldMatrix / viewMatrix / projMatrix /
invViewMatrix` (engine NiDX9Renderer fields, GameNi.h). `SetupSceneCamera()` rewrites all four for image
space (world = translate by -camera). The `D3DSBT_ALL` state block restores device state only, not
these CPU-side matrices. `RenderShadowsMidScene` (fired at the first near-water draw) saves and
restores them.

**Why:** when they are not restored, only the draw being set up is hit (the engine resets the world
matrix per geometry), so the symptom is one object vanishing, and which object depends on draw order
(e.g. near water vanishing at some camera pitches, with the lakebed showing through).

**How to apply:** any new work inside a draw's setup hook must save and restore these four matrices
around `SetupSceneCamera` (or not call it). The other call sites (shadow-map setup,
RenderEffectsPre/PostHdr) run between draws and are fine. For "one object vanishes", diff two
`[GrassOrderDbg]` captures (`Develop.LogShaders` key) and look at what immediately precedes the object's
draw, not just whether it is drawn.

Related: [[grass-drawn-after-water]], [[near-shell-frame-order]], [[water-reflection-pass-detection]]
