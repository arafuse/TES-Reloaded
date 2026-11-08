---
name: grass-manager-generation
description: "Engine grass generation, RE'd 2026-11-02 and CONFIRMED in-game by a probe: DrawGrass 0x4EB3F0 keeps grass within 512 of the camera regardless of view; batches in main-thread map 0xB2CBC4 keyed by GRAS formID; batch +0x00 owning NiGeometry, +0x0C CAPACITY, +0x0E used, +0x10 float4 instances; measured clump density and scan cost"
metadata:
  node_type: memory
  type: project
  originSessionId: fd68ac1b-9420-4460-a9ca-a54b0b68b933
  modified: 2026-11-02T23:34:58.622Z
---

Statically RE'd 2026-11-02 for the grass sneak cover research (capstone + [[oblivion-pdb-symbols]]),
then confirmed the same day by a throwaway in-game probe (2 runs, ~45 samples while sneaking).

**Generation, `DrawGrass` 0x4EB3F0** (cdecl; cell, GrassNode `[0xB36094]`, camera pos, camera
forward XY, …, fade start, end distance, half-FOV). Main thread, called per loaded cell by GrassMode
(`[[grass-drawn-after-water]]` is the render side). Per cell quadrant it walks land-vertex squares
(`128 × [0xB35BF0]` wide). Squares beyond the end distance are skipped. A square closer than
**512 units** (dist² < 262144, `0xA47B18`) to the camera is ALWAYS generated; otherwise it is generated
only if a corner is inside the view cone, and squares wholly outside the cone are REMOVED
(`sub_7C2990` tile key → `sub_7C3980`). In practice the player's grass was present even with the
third-person camera 610–650 units away (the player is in the view cone).

**Per grass type, `sub_4EA8A0`:** places instances (`GetTerrainHeight` for z) into scratch arrays
`[0xB36098]` (float3 pos) / `[0xB3609C]` (float), then `sub_7C4F50` packs them into batches.

**Storage:** NiTMap at `0xB2CBC4` (`+0x04` hash size, `+0x08` bucket table, `+0x0C` count; item =
{next, key, val}), keyed by GRAS formID — every key resolved to a GRAS form via
`TESDataHandler_LookupFormByID` 0x4471A0 (`[0xB33A98]`). An entry's list at `+0x24` holds batches (node:
`[node]` = next, `[node+8]` = batch); `+0x34`/`+0x38` is its tile-key list. Tile key = packed 16:16
ints of the position on a 256-unit grid (`sub_7C2990`). 22–27 types, 240–500 batches, 40k–97k instances
in a loaded exterior grid.

**Batch object** (the same object as TallGrassShaderProperty `+0x9C`, see [[grass-batch-footprints]]):
`+0x00` the owning **NiGeometry** (its geomData `+0xB4` gives the triangle count the draw uses),
`+0x04` per-type model info (`+0x34` UInt16 triangles per instance), `+0x0C` UInt16 **capacity**,
`+0x0E` UInt16 **used count**, `+0x10` float4 instances, `+0x14` UInt32 tile key per instance. Append =
`sub_812510` (`ret 8`). Slots past the used count are zero. Measured: used ≤ capacity always.

**Model height per type** = `GetGrassModelHeight([batch+0])` — stable across all batches of a GRAS
type (hOwner min == max in every sample). The render hook's `Geometry` arg is ALWAYS the batch owner
(0 mismatches in ~1.3M draws), so the groundcover classification pairs correctly. Do NOT cache heights
by batch pointer across frames: batches are freed and reallocated as tiles churn, so a stale pointer
maps to another type's height (that was the probe's first-run noise).

**Instance float4:** xyz = world position, with the fractional bits holding a packed normal (the VS
reads `frac(xyz)`). `w = floor(100 · GRAS[+0xC] · rand[-1,1]) + s`, where `s` in [0, 0.99] is a
brightness term (VS `frac(w)`) and the integer part is the height variation: the VS scales the model by
`0.01 · w · ScaleMask · TESR_GrassScale.xyz + TESR_GrassScale.w` (`.w` = Grass.ini `MinHeight`).
Measured factor `0.01·w + MinHeight` spans 0.18–1.84 (MinHeight 1); per type typically ±0.3.
`GetVertexShaderConstantF(2)` from the instance hook returned garbage, so ScaleMask is unread.

**Measured density (sneaking in a grass field):** within 320 units, 60–150 clumps of the main type
and 100–250 in total; within 80 units (inside the player's own collision ring), 0–14 per type. Mean
clump z within 160 units sits −40…+70 from the player's z (terrain).

**Scan cost:** walking EVERY batch and instance took 0.09–0.21 ms; a real scan should cull batches by
footprint first.

**Logged model heights (this load order):** groundcover 3–8; grass 57, 64, 69, 73, 74, 79, 81, 83, 89,
96, 129, 246 model units. Most types are taller than a crouched torso (`kTreeCoverTorsoHeight` 50),
so a plain "top above torso" test counts nearly all grass as cover.

**How to apply:** a grass cover scan reads batches on the main thread after DrawGrass and publishes a
snapshot for the detection hooks, as TreeCover.cpp does ([[sneak-detection-formula]]). It must share
TreeCover's two call-site patches (each patch site takes only one hook).

**Shipped as GrassCover.cpp (2026-11-03):** the cover scan built on this costs 0.02–0.07 ms per frame while sneaking (≤ 96 clumps kept); play-tested cover is 0.8–1.0 in the 128-unit grass and 0.15–0.33 in the 73–83-unit grass.
