---
name: gameh-multigame-blocks
description: Game.h and GameNi.h define each engine struct three times (NEWVEGAS/OBLIVION/SKYRIM); OBLIVION is always the MIDDLE block — reading the first match gives the New Vegas layout
metadata:
  type: reference
---

`TESReloaded/Framework/Game.h` and `GameNi.h` define most engine structs THREE times, under
`#if defined(NEWVEGAS)` / `#elif defined(OBLIVION)` / `#elif defined(SKYRIM)`. The build defines
OBLIVION, so **the active copy is always the MIDDLE one**. Reading the first grep match gives the
New Vegas layout, which silently misleads. Trust compiler errors over a first-match read.

Block boundaries (approximate, they drift with edits):
- Game.h: NEWVEGAS ~419, OBLIVION ~5084, SKYRIM ~8873, `#endif` ~13008. E.g. `GridCellArray` at
  ~4133 / **~8125** / ~12206; `TESObjectCELL` at ~2208 / **~6311** / ~10980.
- GameNi.h: NEWVEGAS ~426, OBLIVION ~1822, SKYRIM ~3611, `#endif` ~4184. OBLIVION geometry structs:
  NiAVObject ~1881, NiNode ~1945, NiVBChip ~2095, NiGeometryBufferData ~2108, NiGeometryData ~2133,
  NiGeometry ~2258.

Layouts genuinely differ between copies. Concrete gotchas:
- `BSShaderProperty`: the NEWVEGAS copy has a `BSShaderType` enum (kType_Default etc.); the OBLIVION
  copy (~3323) has **no type field** and identifies its class via `IsLightingProperty()` (vtable compare).
- OBLIVION `GridCellArray` has `worldX/worldY` and `size` (= uGridsToLoad); existing grid loops use
  `*SettingGridsToLoad` (SettingManager.h) as ShadowManager/GrassMode/ShaderManager do.
- VFT constants in ShadowManager.cpp: the OBLIVION set is the `0x00A7xxxx`/`0x00A3xxxx` block after
  `#elif defined(OBLIVION)`; the `0x010xxxxx` block above it is NEWVEGAS.

**How to apply:** to check a struct's real Oblivion layout, grep all definitions and read the one
inside the OBLIVION `#elif` block, or let the build tell you.
