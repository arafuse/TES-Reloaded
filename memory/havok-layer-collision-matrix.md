---
name: havok-layer-collision-matrix
description: "Oblivion's Havok layer matrix (0xBA7DB0) clears Biped<->CharController, so actors walk through ragdolls; ProtoPlugin's RagdollCollision (moved out of OR) re-enables it; full RE in ProtoPlugin's module-ragdollcollision.md"
metadata:
  type: project
---

**Engine fact:** the layer matrix `UInt32[32]` at `0xBA7DB0` (row = layer, bit = other layer; layer =
`filterInfo & 0x3F`, 8 Biped, 20 CharController) is built once by `sub_8A83C0` from the `TES`
constructor, before the main loop. It clears Biped<->CharController, which is why actors walk
through ragdolls. The setter `sub_8A7F20(a, b, enable)` is only called at runtime for the
CustomPick rows 0x1C/0x1D.

**The fix lives in ProtoPlugin** ([[protoplugin-sibling]]): `ProtoPlugin/RagdollCollision.cpp`,
`[Ragdoll] bActorsPushRagdolls` (default 1). It sets the pair at GameInitialized; no code is patched.
Its full write-up (filter function, collision-filter vtables, the open live-limb snagging question)
is ProtoPlugin's `memory/module-ragdollcollision.md`. OR's `[Main] RagdollActorCollision` key is
gone; a leftover line in an old INI is ignored.

**How to apply:** OR must not change rows 8 or 20 of the matrix, or hook `0x88B157`. Mind the proxy
O(n²) manifold cost in [[many-actors-havok-cost]] if actor collision grows.
