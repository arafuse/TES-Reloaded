---
name: static-draw-instancing-not-worth-it
description: Shelved after a kill gate - render-time instancing of static draws saves <~0.1 ms measured (<0.9 ms loose ceiling) in CPU-bound market combat; groups not adjacent (draws/run 1.10). How to read the DrawStats census and its caveats
metadata:
  type: project
---

**Decision (user agreed): static-draw instancing is shelved.** The draw census (`DrawStats.cpp`,
printed after the counters when `Develop.ProfileFrame` and `Develop.ProfileDrawCensus` are on) stays
as a tool. Measured on DXVK 3.1.1.

**Market combat (CPU-bound; PresentMon mean 8.09 ms, GPU busy 4.30):**
- Main pass ~1949 draws, shell ~172; the `Draws` counter's 2644 also includes off-screen renders.
- Statics: 893 draws, 730 objects, 379 geometry data, 188 materials, **541 groups** (1.65x),
  **draws/run 1.10**, nested engine CPU 0.21 ms (0.23 us/draw).
- Actors: 994 draws (more than statics), 565 groups, 0.31 ms; skinned, so not this kind of instancing.
- Best case ~350 draws removed: 0.08 ms of measured engine time. Even charging all of "Other"
  (6.43 ms / 2644 = 2.4 us/draw) gives a 0.86 ms ceiling, and most of Other is culling and
  traversal that instancing keeps. Getting even that needs draw reordering inside the pass, with the
  ZFUNC EQUAL follow-up constraints ([[par-shader-interpolator-layout]], [[speedtree-shader-variants]]).

**Exterior test scene (GPU-bound):** statics 437 draws, 64 geometry data, 81 groups (5.4x),
draws/run 1.37, 0.10 ms engine CPU. They instance well but the CPU win does not show in a GPU-bound
frame.

**OR's own draw-hook work is not a lever either:** ShaderHook − Hook:Engine ≈ 0.30 ms per frame in
combat, of which ~0.19 ms is the profiler's own QPC pairs; the real ~0.1-0.15 ms is SetCT uploads,
the mid-scene shadow apply and shell-water prep.

**Profiler cost:** ProfileFrame alone costs ~0.25 ms/frame (the report's "profiler self-cost" line);
the census adds ~0.2 us per recorded draw (~0.45 ms/frame in heavy combat), which is why it has its
own switch (default 0).

**Census caveats:**
- Light sets come out as only ~3 per class even in the lit market. If BSShaderLightingProperty::lights
  undercounts, groups are really higher, which only strengthens the kill.
- Actors = skinned or under a `Bip01*` node; land = under `Block (`; trees = `ST*` shader names or a
  SpeedTree* property RTTI; water = `WATER*` PS.
- Materials hash the pass's stage textures read BEFORE the engine call (same read as TextureData).
- `drw/run` counts only draws adjacent in engine order; `groups` is the reorder-everything floor.

**How to apply:** don't revisit static batching unless a scene shows statics' groups << draws AND is
CPU-bound. Re-run the census first; it answers it in one 600-frame window. See
[[dxvk-combat-cpu-headroom]], [[many-actors-havok-cost]].
