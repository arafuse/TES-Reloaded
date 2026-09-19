---
name: water-reflection-pass-detection
description: "Verified Oblivion frame order — water reflection map (1024x1024) renders AFTER the main pass, NOT via RenderObject(WorldSceneGraph); BeginScene re-fires per off-screen render and resets per-scene latches; mid-scene effects must gate on InMainScenePass"
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
  NOT sufficient for the observed reflection path. The NiSkinInstance::FrameID bone cache is also
  unrelated (see [[skinning-bone-matrix-cache]]).
- Debugging discriminator: log camera ptr + RT dimensions at every WSG render, every apply
  execution, and the water-bind trigger.

Related: [[grass-drawn-after-water]].
