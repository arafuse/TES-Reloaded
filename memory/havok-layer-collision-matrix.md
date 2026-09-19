---
name: havok-layer-collision-matrix
description: "RE'd 2026-09-10 - Oblivion's Havok layer matrix (0xBA7DB0), filter function, init and runtime setter; Biped<->CharController is explicitly disabled, which is why actors walk through ragdolls"
metadata: 
  node_type: memory
  type: project
  originSessionId: ac14525b-02b2-42e7-b56d-c2f738f3356a
  modified: 2026-09-10T22:59:17.942Z
---

Static RE (disasm + unicorn emulation of the init), not yet tested in-game.

- **Layer matrix** `UInt32[32]` at `0xBA7DB0`: row = layer, bit = other layer.
  Layer = `filterInfo & 0x3F` (OL_ enum: 8 Biped, 20 CharController, 28/29 CustomPick1/2).
  Biped-part self-collision table `UInt32[32]` at `0xBA7E30` (part = `(info >> 8) & 0x1F`).
- **Init** `sub_8A83C0` (only caller: `call` at `0x88B157` in `sub_88B070`): memsets the
  matrix to 0xFF then clears pairs. It clears **Biped<->CharController** (row 8 bit 20,
  row 20 bit 8). Biped<->Biped, CharController<->CharController, Clutter/Props<->CC are on.
- **Runtime setter** `sub_8A7F20(layerA, layerB, bool enable)` (cdecl, symmetric). ~100
  call sites, but every one toggles row 0x1C/0x1D (CustomPick pick-ray setups) - nothing
  at runtime touches (8, 20), so a patch applied after init sticks.
- **Filter** `sub_8A7F70(infoA, infoB)`: bit 14 = no-collision flag (except layer 29);
  **either system group (`info >> 16`) == 0 -> always collide, matrix ignored**; different
  groups -> matrix; same group -> both Biped uses the part table, both bit 15 ("linked")
  uses matrix + non-adjacent parts, else false.
- bhkCollisionFilter vtables (ctor `0x88A570`): CollidableCollidable `0xA95CDC` ->
  `0x8A8060`; ShapeCollection `0xA95CD0` -> `0x8A8090` (uses ROOT collidable info, ignores
  sub-shape keys); RayShapeCollection `0xA95CC4` -> `0x8A80E0`; RayCollidable `0xA95CBC`
  -> `0x8A8110`. hkCollidable filter info is at `+0x1C`.

**Open question:** whether LIVE actors' bones sit in the world as keyframed Biped bodies.
Oblivion has no Skyrim-style DEADBIP layer, so if they do, flipping the matrix bit also
makes walking actors snag on each other's limbs (and possibly their own, depending on
whether the CC shares the skeleton's system group).

**Implemented 2026-09-10** as option 1: `TESReloaded/Core/RagdollCollision.cpp`, INI
`[Main] RagdollActorCollision` (default 0). Init runs from `TES_constr` (0x4419FF ->
`sub_5350F0` -> `sub_88B070`), i.e. after plugin load, so the call-site hook is in time.
In-game result not yet recorded - update this when tested.

**How to apply:** for "walking actors push ragdolls", either flip the bit after init
(`sub_8A7F20(8, 20, true)` behind a hook on `0x88B157`) or, if live limbs snag, hook the
collidable/shape-collection filter slots and allow the pair only when the Biped body is
non-keyframed. Mind the proxy O(n^2) manifold cost in [[many-actors-havok-cost]].
