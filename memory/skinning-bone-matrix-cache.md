---
name: skinning-bone-matrix-cache
description: "Game's CalculateBoneMatrixes (0x7655F0) caches BoneMatrixes per BeginFrame via NiSkinInstance::FrameID; the VBmgr+0x3C counter steps ~36 per displayed frame (NOT a frame counter); why ShadowManager resets FrameID after its skinned draws"
metadata:
  type: reference
---

Reverse-engineered (Oblivion 1.2.0.416, disassembly):

- `NiDX9Renderer::CalculateBoneMatrixes` = 0x7655F0, args `(NiSkinInstance*, NiTransform* World, bool, int fmt, bool)`.
- Early-out: compares `SkinInstance->FrameID` (+0x18) against `[renderer+0x8B0 (NiDX9VertexBufferManager)] + 0x3C`; if equal it returns immediately, reusing cached `BoneMatrixes`/`SkinToWorldWorldToSkin` and **ignoring the passed WorldTransform**. Otherwise computes and stamps FrameID.
- The counter at VBmgr+0x3C is incremented only by 0x777A40, whose only caller is the NiDX9Renderer BeginFrame-like 0x762600 (calls device->BeginScene then bumps) → **once per BeginFrame, NOT once per display frame**. MEASURED 2026-11-14: it advances ~36 per displayed frame in this fork (logged 1506, 1560, 1596, 1632, ... at one effect chain per frame), since the shadow, probe and reflection renders each go through 0x762600. Never use it as a display-frame counter: the temporal AO did, and reset its history every frame. Count effect chains (`ShaderManager::EffectChainFrame`) instead.
- Game call sites of 0x7655F0: 0x779737, 0x7C9288, 0x7FB76B, 0x8052ED (per-shader setup paths, gated on `[shader+0x34]==0`).
- 0x718A80 = NiTransform::Invert (no camera input); the bone composite is pure scene-graph + passed WorldTransform, so cross-pass reuse with identical transforms is benign.

`ShadowManager::RenderSkinnedGeo` sets `SkinInstance->FrameID = 0xFFFFFFFF` after its draws as
hygiene, so the game never consumes shadow-pass-stamped state. This cache is not what causes
reflection artifacts — see [[water-reflection-pass-detection]].
