---
name: havok-collection-full-sweep
description: "A collision mesh that is a bare hkNiTriStripsShape (no MOPP/BV tree) is swept triangle-by-triangle on EVERY query: collection agent statics 0x9050F0/0x905370/0x904EB0, castRay 0x915AE0, getAabb 0x9156E0; getNextKey 0x915C60 itself calls getChildShape; the exe has MOPP query code but no MOPP compiler"
metadata:
  node_type: memory
  type: reference
  originSessionId: ba120313-ec60-4df0-b274-b7487b867149
  modified: 2026-11-27T22:24:51.296Z
---

From disassembly of Oblivion.exe 1.2.416 (`petool.py`) and the dense-town strip-lookup census
([[dense-town-cpu-budget]], [[havok-strip-cache]]).

- Every caller of hkNiTriStripsShape slot 10 (`getChildShape` 0x915D00) is a loop over ALL keys,
  `getFirstKey` (slot 8) to `getNextKey` (slot 9) until -1:
  - 0x9050F0, 0x905370, 0x904EB0: the shape-collection agent's three static queries (Havok timer
    string "ShapeCollection" at 0xA9BDC4). This is the agent Havok uses for a collection with NO
    BV tree, so a character proxy touching the mesh's broadphase AABB tests every triangle.
  - 0x915AE0: collection `castRay` (timer "rcShpCollect"); 0x9156E0: `hkShapeCollection::getAabb`.
- `getNextKey` 0x915C60 calls `getChildShape` for each candidate key (call at 0x915CBE) to skip
  degenerate triangles, so a sweep costs ~2 lookups per triangle.
- Dense town: one 143-strip / 407-triangle mesh took ~23.8k lookups/frame = ~30 full sweeps. The
  calls-by-strips-walked histogram matches uniform iteration over the whole mesh, not local queries.
- The exe has the MOPP query VMs (`hkMoppBvTreeShape`, `hkMopp*VirtualMachine`) but no MOPP compiler,
  so a runtime fix needs its own spatial index (e.g. filter the keys that slots 8/9 return while one
  of the five sweeps is running). Content fix: give the mesh packed strips under a MOPP (NifSkope).
- NOT yet measured: the ms/frame inside the sweeps (strip lookup + per-triangle narrowphase).

**How to apply:** "many actors" Havok cost in a natural scene is often actors standing on such a
mesh, not actor count. Time the five sweeps (target vtable 0xA9D010) before building anything. Havok
work belongs in ProtoPlugin ([[protoplugin-sibling]]). See [[many-actors-havok-cost]].
