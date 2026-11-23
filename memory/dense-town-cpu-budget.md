---
name: dense-town-cpu-budget
description: "Dense mod-town CPU budget (CPU-bound ~21 ms median with HavokStripCache, GPU ~7 ms): far pass 11.2 ms, water reflection 4.7 ms (3,242 loaded-cell statics), OR shadow maps 1.4 ms; main-pass size bins"
metadata:
  type: project
---

Dense mod-town spot (same one as [[occlusion-culling]] and [[reflection-occlusion-not-worth-it]]),
DXVK, occlusion culling on.

**PresentMon (60 s, standing still):** ~21 ms median frame with `HavokStripCache` (23.2 without it),
GPU busy ~7.0, the rest GPU wait. Fully CPU-bound: every main-thread ms is frame time down to ~7 ms.
The sampler's update phase is ~6.5 ms/frame.

**Pass spans** (`Pass:*` buckets; ProfileFrame = 1, census OFF, so ~1.2 ms profiler self-cost inside):
- `Pass:Far` 11.2 ms for ~5,100 draws (~2.2 us/draw all-in, cull + accumulate + draw). The far pass
  draws 4,313 statics after culling.
- `Pass:Reflection` **4.73 ms** for 3,258 draws (~1.45 us/draw). The census's reflection table shows
  3,242 of them are loaded-cell **statics** (547 geoData, 1,973 groups); only 13 distant LOD.
- `Pass:Shell` 0.23 ms; OR ShadowMaps 1.4 ms CPU.
- Census ON inflates the spans (Far 13.9, Reflection 5.9): read spans from a census-off run.

**Main-pass size bins (bound diameter, px of screen height, per frame):** statics <4 px 170,
4-8 px 77, 8-16 px 257 (of 4,313); actors <4 px 214, <8 px 296 of 735, but these are per GEOMETRY
(eyes, teeth, hair pieces), not per actor.

**How to apply:** start any far-pass, reflection or Havok work from these numbers. Set ProfileFrame
to 0 for sampler runs ([[sampling-profiler]]). Havok strip lookups here were dominated by one mesh;
see [[havok-strip-cache]].
