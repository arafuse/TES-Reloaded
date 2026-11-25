---
name: engine-light-attach-addresses
description: "RE'd + MEASURED ShadowSceneNode light processing: 0x7C78D0 holds the scene cull; the real per-light cost is the per-object light re-link in the cull (0x7C6100 → linear list search 0x7D5F80), quadratic in lights × objects each light reaches"
metadata:
  type: reference
---

From disassembling `Oblivion.exe` (1.2.0.416) and measuring with the sampler ([[sampling-profiler]]); offsets are the OBLIVION block of GameNi.h (NiAVObject world pos +0x88, NiLight Diff +0xEC, Spec.r = radius +0xF8).

- Scene-graph pointer table `0x00B42F54` (`table[index*4]`, getter `0x7B4280`); index 0 = ShadowSceneNode, vtable `0xA904B4`. `lights` NiTList at SSN+0xE4 (start +0xE8).
- **`0x7C78D0`** (vtable slot 31, thiscall 1 arg, `ret 4`, ~3 calls/frame) walks `lights` and then runs the **scene cull** itself (`NiNode::OnVisible` into a BSShaderAccumulator), so its inclusive time is mostly culling (~3.5 ms with no lights in the test scene).
- Per-light dispatch inside it: source light missing or refcount ≤ 1 → `0x7C77C0` (teardown); else `+0x104 != 0` → `0x7C6020` (+ whole-scene walk 0x7C5F60); then unless (`+0xF4` shadow flag and `0x405A80`) → **`0x7D6390` ShadowSceneLight_CullProcess** for every live light. 0x7D6390 is CHEAP: ~0.1 ms/frame for 10 torches.
- **Carried torches**: `+0xF4 = 0`, `+0xFC = 1` (point), `+0x104 = 0`, `CanCarry = 1`. +0x104 is NOT a "dynamic/moving" flag for them.
- **The hotspot**: for every visible object the cull calls **`0x7C6100`** (via sub_7AD270): `0x7C5750` unlinks every non-shadow light from the object's lighting property (prop ID 4) through `0x7D6940`, whose `0x7D5F80` **linear-searches that light's object list** (SSL+0xE4, ~600–770 entries per torch in a town); then it asks **`0x7D2EA0`** ("light reaches object": thiscall(SSL, object, NiLight) `ret 8`, sphere test on world pos and Spec.r) for every scene light and re-links through `0x7D5790`. Cost ∝ lights per object × list length, every frame.
- The update phase also walks each light's scene via `ShadowSceneLight_UpdateLightingProperty 0x7D59E0` (from `ShadowSceneLight_AddToScene`), pruned on the same 0x7D2EA0.
- MEASURED: 10 guard torches in a town after load cost ~24 ms/frame CPU (34.4 vs ~10.5 ms frames; GPU idle). Refusing a light in 0x7D2EA0 drops it from every object within one cull and is the hook [[light-cluster-merge]] uses.
