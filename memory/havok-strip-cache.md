---
name: havok-strip-cache
description: "HavokStripCache ([Main], on by default) replaces hkNiTriStripsShape's O(strips) child-shape walk (0x915D00, loop 0x915D60) with an O(1) per-NiTriStripsData table; it assumes strips never change after first lookup — new in-place strip writers must invalidate it; never join collision strips"
metadata:
  type: project
---

**Engine fact:** hkNiTriStripsShape's child-shape lookup (vtable slot 10 = 0x915D00) walks the
strip-length array from strip 0 on every call (loop 0x915D60-0x915D9F). Callers: 0x915CBE (slot 9
0x915C60 calls slot 10 VIRTUALLY, so a vtable hook sees it), 0x90542D, 0x9051A9, 0x915B6D. Loop outputs
at 0x915DA1: ecx = triangle within strip, esi = Points + (sum of earlier lengths) * 2; inputs
[esp+0x2C] lengths, [esp+0x20] numStrips. A strip of length < 2 breaks the walk (unsigned compare).
In the dense town ([[dense-town-cpu-budget]]) one 143-strip collision mesh the NPCs stand on took
20-28k lookups per frame (~1 ms/frame in the loop); the cache took the median frame 23.2 → 21.0 ms
(more than the loop cost, likely via the Havok fixed-timestep feedback in [[many-actors-havok-cost]]).

**Implementation (`HavokStripCache.cpp`, `[Main] HavokStripCache`, default 1):** a jump at 0x915D3F to
a stub whose resolver serves an O(1) per-NiTriStripsData table (`HavokStripMath.h`, test harness
`Tools\Tests\Run-HavokStripMathTest.ps1`) on the main thread. A Detour on `~NiTriStripsData` 0x719F80
(only caller: the deleting dtor 0x71A110 shared with NiTriStripsDynamicData) queues freed data for a
main-thread drain. `[Develop] HavokStripCacheVerify` cross-checks every lookup against the engine walk
(0 mismatches across worldspace changes, interiors and a `pcb` that freed a cached mesh). The engine's
model cache keeps collision meshes alive across ordinary cell changes, so `freed` stays 0 without
`pcb`. `HavokStripProfile.cpp` (slot-10 vtable hook) is the diagnostic that counts lookups per mesh.

**The cache assumes a NiTriStripsData's strips never change after it is first looked up.** The
in-place writers of +0x44/+0x48 all run before that:
- `Replace` 0x719F40 (only caller 0x8C6008, on newly built data);
- 0x71A040 via 0x765475 (same strips, clears the index list);
- ProtoPlugin's StripJoin ([[protoplugin-sibling]]), at NiTriStrips link time on the loader thread.
  It touches render data only: collision data is never linked by an NiTriStrips.

**How to apply:** any NEW code that rewrites strips in place after load must also queue the data as
freed (call the destructor hook's push), or the cache will serve a stale table. Never join collision
strips: the NIF's precomputed MOPP tree addresses triangles by index, and a join renumbers them. If a
NIF shares one NiTriStripsData between render and collision, StripJoin would renumber its collision
triangles — a ProtoPlugin-side risk, not seen so far, that verify mode can't catch.
