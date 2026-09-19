---
name: water-reflection-pass-detection
description: "Verified Oblivion frame order — water reflection map (1024x1024) renders AFTER the main pass, NOT via RenderObject(WorldSceneGraph); BeginScene fires per off-screen render and resets per-scene flags"
metadata: 
  node_type: memory
  type: project
  originSessionId: d2bf5c63-769e-4543-904d-65fed09533d1
---

Verified by in-game [ReflDbg] logging + disassembly (Oblivion 1.2.0.416, 2026-07-17):

- **Frame order** (contradicts old code comments claiming reflections render first): mod ShadowMaps hook → main WorldSceneGraph render (screen-res RT; water-bind trigger + sun-shadow apply fire here, correctly) → game calls **BeginScene again** for off-screen renders → **water reflection map render (1024×1024 = WaterReflectionMapSize), which does NOT go through RenderObject (0x70C0B0)** — TrackRenderObject never sees it.
- `ShaderManager::BeginScene()` (hooked at kBeginScene 0x76BE00) resets `RenderedBufferFilled` / `DepthBufferFilled` / `PreWaterDepthBufferFilled` on EVERY BeginScene, including the reflection render's — so per-scene latches re-arm mid-frame.
- Numbered water surface shaders (e.g. WATER001) DO bind during the reflection render. Bug this caused: the pre-water/mid-scene sun-shadow apply trigger (RenderHook `TrackSetupShaderPrograms`) re-fired there and painted the darkening quad INTO the reflection map with main-camera matrices → dark caster silhouettes floating in the water, tracking camera height ("actor reflections not anchored", fixed 2026-07-17).
- Fix: `InMainScenePass` static in RenderHook.cpp — true only while the main WSG render is on the stack (set/cleared in TrackRenderObject); the water-bind trigger requires it. Any mid-scene screen-space effect must be gated this way, not just by per-scene latch flags.
- Dead ends worth remembering: engine global 0xB42E86 IS a "rendering water reflections" flag (set around the WSG RenderObject call at 0x4D040B, restored before the second call 0x4D04A8, read by many shader-setup funcs) but is NOT sufficient/relevant for the observed reflection path; NiSkinInstance::FrameID bone cache was also not the cause (see [[skinning-bone-matrix-cache]]).
- Debugging discriminator that cracked it: log camera ptr + RT dimensions at every WSG render, every apply execution, and the water-bind trigger.
