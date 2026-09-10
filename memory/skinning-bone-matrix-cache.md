---
name: skinning-bone-matrix-cache
description: "Game's CalculateBoneMatrixes (0x7655F0) caches BoneMatrixes per display frame via NiSkinInstance::FrameID; mod shadow pass can poison the water-reflection pass's skinning"
metadata: 
  node_type: memory
  type: project
  originSessionId: d2bf5c63-769e-4543-904d-65fed09533d1
---

Reverse-engineered facts (Oblivion 1.2.0.416, verified by disassembly 2026-07-17):

- `NiDX9Renderer::CalculateBoneMatrixes` = 0x7655F0, args `(NiSkinInstance*, NiTransform* World, bool, int fmt, bool)`.
- Early-out: compares `SkinInstance->FrameID` (+0x18) against `[renderer+0x8B0 (NiDX9VertexBufferManager)] + 0x3C`; if equal it returns immediately, reusing cached `BoneMatrixes`/`SkinToWorldWorldToSkin` and **ignoring the passed WorldTransform**. Otherwise computes and stamps FrameID.
- The counter at VBmgr+0x3C is incremented only by 0x777A40, whose only caller is NiDX9Renderer::BeginFrame-ish 0x762600 (calls device->BeginScene then bumps) → **once per display frame**, shared by shadow, water-reflection, and main passes.
- Game call sites of 0x7655F0: 0x779737, 0x7C9288, 0x7FB76B, 0x8052ED (per-shader setup paths, gated on `[shader+0x34]==0`).
- 0x718A80 = NiTransform::Invert (no camera input); bone composite is pure scene-graph + passed WorldTransform.

NOTE (2026-07-17): this cache was NOT the cause of the floating-actor-reflection bug — that was the sun-shadow apply firing inside the water-reflection render (see [[water-reflection-pass-detection]]). The bone-matrix math is pure scene-graph + passed WorldTransform (no camera input found), so cross-pass reuse with identical transforms is benign. ShadowManager::RenderSkinnedGeo still sets `SkinInstance->FrameID = 0xFFFFFFFF` after its draws as hygiene so the game never consumes shadow-pass-stamped state.
