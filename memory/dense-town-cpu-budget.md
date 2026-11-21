---
name: dense-town-cpu-budget
description: "MEASURED 2026-11-21 dense-town CPU budget (CPU-bound 23.7 ms, GPU 7.0) - far pass 11.2 ms, water reflection 4.7 ms (3,242 loaded-cell statics), one 143-strip collision mesh took ~20k Havok strip walks/frame (HavokStripCache fixed it, median 23.18 -> 20.95 ms); size bins"
metadata:
  node_type: memory
  type: project
  originSessionId: 5ca85939-0ce7-4885-a0a4-d627c1b78551
  modified: 2026-11-21T15:33:57.003Z
---

Dense mod-town spot (same one as [[occlusion-culling]] and [[reflection-occlusion-not-worth-it]]), DXVK.

**PresentMon (60 s, standing still):** 23.7 ms median frame, CPU busy 23.6, GPU busy 7.0, GPU wait 16.7.
Fully CPU-bound: every main-thread ms is frame time down to ~7 ms.

**Pass spans** (`Pass:*` buckets, added this round; ProfileFrame = 1, census OFF, so ~1.2 ms profiler self-cost inside):
- `Pass:Far` 11.2 ms for ~5,100 draws (~2.2 us/draw all-in, cull + accumulate + draw)
- `Pass:Reflection` **4.73 ms** for 3,258 draws (~1.45 us/draw). The census (reflection table, added
  this round) shows 3,242 of them are loaded-cell **statics** (547 geoData, 1,973 groups); only 13
  distant LOD. The reflection is not occlusion-culled, and the far pass draws 4,313 statics after
  culling.
- `Pass:Shell` 0.23 ms; OR ShadowMaps 1.4 ms CPU.
- Census ON inflates the spans (Far 13.9, Reflection 5.9): read spans from a census-off run.

**Havok strip walk** (`HavokStripProfile.cpp`, vtable hook on hkNiTriStripsShape slot 10 = 0x915D00):
the child-shape lookup walks the strip-length array from strip 0. 20-28k calls/frame, 83 strips walked
per call, all on the main thread, and **100 % on ONE NiTriStripsData (143 strips, 407 tris)**, which is
one mesh the town's NPCs stand on. The sampler put ~1.0 ms/frame in the loop (0x915D60-0x915D9F).
Callers: 0x915CBE (slot 9 0x915C60 calls slot 10 VIRTUALLY, so a vtable hook sees it) 60 %,
0x90542D 15 %, 0x9051A9 11 %, 0x915B6D 7 %. Loop outputs at 0x915DA1: ecx = triangle within
strip, esi = Points + (sum of earlier lengths) * 2; inputs [esp+0x2C] lengths, [esp+0x20] numStrips.
A strip of length < 2 breaks the walk (unsigned compare).

**Fixed (7d02d3b, `[Main] HavokStripCache`, `HavokStripCache.cpp`):** a jump at 0x915D3F to a stub
whose resolver serves an O(1) per-NiTriStripsData table (`HavokStripMath.h`, harness
`Tools\Tests\Run-HavokStripMathTest.ps1`) on the main thread. A Detour on `~NiTriStripsData`
0x719F80 (only caller: the deleting dtor 0x71A110 shared with NiTriStripsDynamicData) queues freed
data for a main-thread drain. Verify mode (`[Develop] HavokStripCacheVerify`): 0 mismatches over
~20 windows of up to 19M lookups, across worldspace changes, an interior transition and a `pcb`
that freed a cached mesh (`freed 1`, then still 0 mismatches). The engine's model cache keeps
collision meshes alive across ordinary cell changes, so `freed` stays 0 without `pcb`.
**PresentMon A/B at the town spot (60 s each):** median 23.18 → **20.95 ms**, mean 23.32 → 21.62,
p95 25.37 → 24.71; GPU busy unchanged (6.85 / 6.94). That is more than the ~1 ms the loop cost: part
is likely the fixed-timestep feedback ([[many-actors-havok-cost]]), part crowd variation (the
cache-on run's 5 s medians ranged 20.2-23.9). The sampler shows the loop rows gone and update phase
6.47 ms/frame; the resolver is below the report's cutoff.

**Main-pass size bins (bound diameter, px of screen height, per frame):** statics <4 px 170,
4-8 px 77, 8-16 px 257 (of 4,313); actors <4 px 214, <8 px 296 of 735, but these are per GEOMETRY
(eyes, teeth, hair pieces), not per actor.

**How to apply:** start any reflection or Havok work from these numbers. The sampler run in this
window had ProfileFrame on, so set it to 0 for sampler runs ([[sampling-profiler]]).
