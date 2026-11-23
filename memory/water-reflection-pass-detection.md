---
name: water-reflection-pass-detection
description: "Verified Oblivion frame order — water reflection map (1024x1024) renders AFTER the main pass, NOT via RenderObject(WorldSceneGraph); BeginScene re-fires per off-screen render and resets per-scene latches; mid-scene effects must gate on InMainScenePass; reflection pass flag 0xB42CE8, EndScene site 0x49C81B, main depth, camera-relative view"
metadata:
  type: project
---

Verified by in-game logging + disassembly (Oblivion 1.2.0.416):

- **Frame order:** plugin shadow maps → main WorldSceneGraph render (screen-res RT; the near-water
  trigger and sun-shadow apply fire here) → game calls **BeginScene again** for off-screen renders →
  **water reflection map render (1024×1024 = WaterReflectionMapSize), which does NOT go through
  RenderObject (0x70C0B0)** — `TrackRenderObject` never sees it.
- `ShaderManager::BeginScene()` (hooked at kBeginScene 0x76BE00) resets `RenderedBufferFilled` /
  `DepthBufferFilled` / `PreWaterDepthBufferFilled` on EVERY BeginScene, including the reflection
  render's — so per-scene latches re-arm mid-frame.
- Numbered water surface shaders (e.g. WATER001) DO bind during the reflection render, so an
  ungated mid-scene trigger re-fires there and paints the shadow darkening quad INTO the reflection
  map with main-camera matrices (dark caster silhouettes floating in the water, tracking camera height).
- Guard: `ShaderManager::InMainScenePass` — true only while the main WSG render is on the stack (set
  and cleared in RenderHook's `TrackRenderObject`; also read by `ShaderRecord::SetCT`). **Any
  mid-scene screen-space effect must be gated on it**, not just on per-scene latch flags.
- Engine global 0xB42E86 IS a "rendering water reflections" flag (set around the WSG RenderObject
  call at 0x4D040B, restored before the second call 0x4D04A8, read by many shader-setup funcs) but is
  NOT set on the observed reflection path. The NiSkinInstance::FrameID bone cache is also unrelated
  (see [[skinning-bone-matrix-cache]]).
- Debugging discriminator: log camera ptr + RT dimensions at every WSG render, every apply
  execution, and the water-bind trigger.

**Reflection pass structure:**
- `NiRenderer_ReflectionPass` 0x49BEF0 is called once per frame from 0x49E9BF. Global byte
  **0xB42CE8** is set for its whole duration, so use it to detect the reflection pass (0xB42E86 is
  not set on this path). The profiler's per-pass `reflection` draw row keys on it (about 3,200 draws
  in the dense town); `RenderHook::TrackReflectionPass` wraps 0x49BEF0 for the `Pass:Reflection` timer.
- Its per-node cull is 0x49CBF0, detoured as `RenderHook::TrackWaterReflectionCull`. Its
  `EndScene` is a call at 0x49C81B to 0x7D72D0 (cdecl, no arguments; the caller rewrites
  eax/ecx/edx afterwards). That call site is the last point where the reflection target is bound.
- **Depth:** a 1024x1024 colour target and viewport, but the depth surface is the MAIN one
  (3072x1728 D24S8). The reflection draws only into its top-left 1024x1024 corner.
- **Camera:** the device's `D3DTS_VIEW` is identity. `TheRenderManager->viewMatrix` is rotation
  only, because rendering is camera-relative, and the eye is at 0xB3F92C (`kCameraWorldTranslate`).
  That eye equals the main camera mirrored in the water plane. The projection has near 40
  (`_33` 1.0001, `_43` -40.004), and it matches on the device and the renderer.
- To project a world point into the reflection: Translate(-eye) × viewMatrix × projMatrix, as
  row vectors. Using the device view gives eye (0,0,0) and wrong rects.

Related: [[grass-drawn-after-water]], [[reflection-occlusion-not-worth-it]].
