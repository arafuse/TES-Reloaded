---
name: gameh-multigame-blocks
description: Game.h/GameNi.h define each engine struct three times (NEWVEGAS/OBLIVION/SKYRIM); which block is active and pitfalls
metadata: 
  node_type: memory
  type: reference
  originSessionId: 436deae8-f14f-48b2-8562-b12116885928
---

`TESReloaded/Framework/Game.h` and `GameNi.h` define each engine struct THREE times under `#if defined(NEWVEGAS)/#elif defined(OBLIVION)/#elif defined(SKYRIM)`. The build defines OBLIVION.

Gotcha: in Game.h the OBLIVION block is the MIDDLE one, not the first. E.g. `class GridCellArray` appears at ~4132 (NEWVEGAS), ~8123 (OBLIVION, the active one), ~12203 (SKYRIM); `TESObjectCELL` at ~2208/~6310/~10978. Reading the FIRST match gives the NewVegas layout, which can mislead. The compiler resolves names to the OBLIVION block, so trust compiler errors over a first-match read. Concretely: the OBLIVION `GridCellArray` has `worldX/worldY` but NO `gridSize` — use `*SettingGridsToLoad` (SettingManager.h:59, `static const UInt32* = (UInt32*)kSettingGridsToLoad`) for grid loops, as ShadowManager/GrassMode/ShaderManager do.

In GameNi.h the OBLIVION block starts at line ~1822 (`#elif defined(OBLIVION)`); the geometry structs there are correct for Oblivion: NiGeometryBufferData@2108, NiGeometryData@2133, NiGeometry@2226, NiNode@1945, NiAVObject@1881.

To verify a struct's real Oblivion layout, grep for all definitions and read the one inside the OBLIVION `#elif` block (middle range in Game.h), or just let the build tell you. Used by [[mesh-combining-feature]].
