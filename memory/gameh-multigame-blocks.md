---
name: gameh-multigame-blocks
description: Game.h and GameNi.h define each engine struct three times (NEWVEGAS/OBLIVION/SKYRIM); OBLIVION is always the MIDDLE block — reading the first match gives the New Vegas layout
metadata:
  type: reference
---

`TESReloaded/Framework/Game.h` and `GameNi.h` define most engine structs THREE times, under
`#if defined(NEWVEGAS)` / `#elif defined(OBLIVION)` / `#elif defined(SKYRIM)`. The build defines
OBLIVION, so **the active copy is always the MIDDLE one**. Reading the first grep match gives the
New Vegas layout, which silently misleads. Trust compiler errors over a first-match read. Find the
block boundaries by grepping for `^#elif defined(OBLIVION)` / `^#elif defined(SKYRIM)` (Game.h's
OBLIVION block starts near line 5100, GameNi.h's near 1800).

Layouts genuinely differ between copies. Concrete gotchas:
- `BSShaderProperty`: the NEWVEGAS copy has a `BSShaderType` enum (kType_Default etc.); the OBLIVION
  copy has **no type field** and identifies its class via `IsLightingProperty()` (vtable compare).
- OBLIVION `GridCellArray` has `worldX/worldY` and `size` (= uGridsToLoad); existing grid loops use
  `*SettingGridsToLoad` (SettingManager.h) as ShadowManager/GrassMode/ShaderManager do.
- VFT constants in ShadowManager.cpp: the OBLIVION set is the `0x00A7xxxx`/`0x00A3xxxx` block after
  `#elif defined(OBLIVION)`; the `0x010xxxxx` block above it is NEWVEGAS.

**How to apply:** to check a struct's real Oblivion layout, grep all definitions and read the one
inside the OBLIVION `#elif` block, or let the build tell you.
