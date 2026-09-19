---
name: gameni-multigame-blocks
description: GameNi.h multi-game block layout — OBLIVION is the SECOND block; class duplication gotchas
metadata: 
  node_type: memory
  type: reference
  originSessionId: 77deca55-e5c9-430d-88ce-9bb65d6a088a
---

In `TESReloaded/Framework/GameNi.h` the big game-struct block is `#if defined(NEWVEGAS)` (line ~426) … `#elif defined(OBLIVION)` (line ~1822) … `#endif` (~4152). So **OBLIVION is the SECOND of two blocks here** — different from Game.h, where OBLIVION is the middle of three (see [[gameh-multigame-blocks]]).

Consequence: many classes are defined TWICE (NEWVEGAS copy first, OBLIVION copy second) with DIFFERENT layouts. Always read the OBLIVION copy (the higher line number, inside the 1822–4152 range). Concrete gotcha that bit me: `BSShaderProperty` — the NEWVEGAS copy (~1557) has a `UInt32 type` field + `BSShaderType` enum (kType_Default etc.); the OBLIVION copy (~3291) has **no `type` field** and identifies its class via `IsLightingProperty()` (vtable compare). Same pattern for NiGeometryBufferData/NiGeometryData/NiVBChip (first set ~643–820 = NEWVEGAS, second set ~2095–2242 = OBLIVION) and the property classes.

Also: VFT constants in ShadowManager.cpp — the OBLIVION set is the `0x00A7xxxx`/`0x00A3xxxx` block at lines ~39-44; the `0x010xxxxx` block at ~21-26 is NEWVEGAS.
