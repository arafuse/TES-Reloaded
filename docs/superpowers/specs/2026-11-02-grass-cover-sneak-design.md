# Grass cover for sneaking — design

Date: 2026-11-02
Status: implemented (play-tested 2026-11-03)
Builds on: tree cover (`TESReloaded/Core/TreeCover.cpp`), memory `sneak-detection-formula`,
`grass-manager-generation`, `grass-batch-footprints`

## Goal

When the player sneaks in grass tall enough to hide a crouched figure, they are harder to detect, the
same way tree cover already works for small SpeedTree shrubs:

1. **Light reduction:** the light level fed to `Calc_DetectionLevel` is scaled down by how fully tall
   grass surrounds the player.
2. **Line-of-sight blocking:** an observer whose sight line to the player passes through enough tall
   grass loses line of sight.

Only grass that rises well above a crouched figure counts. In the probed load order, the 128-unit grass
type hides the player well, the 73–83-unit types give a little cover in their tallest clumps, and
short grass and groundcover give none.

### Success criteria

- Sneaking in the middle of a field of the 128-unit grass gives grass cover ≥ 0.8; at the field's edge
  it gives roughly half that.
- Sneaking in the 73–83-unit grass gives a small cover (≤ 0.3); short grass, groundcover and bare
  ground give 0.
- A hostile NPC across a tall-grass field loses the sneaking player; the same NPC on higher ground
  looking down into the field keeps them.
- The per-frame scan costs ≤ 0.05 ms in a dense field (the full-scan probe measured 0.09–0.21 ms).
- With the feature off, behaviour is identical to today.

### Non-goals

- NPCs hiding in grass (tree cover is also player-only).
- Grass between the observer and the player far from the player: only grass near the player counts.
- Reading the engine's `ScaleMask` grass constant (the probe could not read it back; `ScaleZ`/`ScaleX`
  from Grass.ini stand in for it).

## Engine facts this relies on (confirmed in-game 2026-11-02)

- Grass lives in the main-thread NiTMap at `0xB2CBC4` (`+0x04` hash size, `+0x08` buckets, item =
  {next, key, val}), keyed by GRAS formID. An entry's batch list is at `val+0x24` (node: next at
  `+0`, batch at `+8`).
- Batch: `+0x00` owning `NiGeometry`, `+0x0C` capacity, `+0x0E` used count, `+0x10` float4 instances.
  A batch is at most one cell (~4160 units) wide.
- Instance: xyz = world position; `w` gives the height factor `F = 0.01·w·ScaleZ + MinHeight`
  (0.18–1.84 measured).
- The model height (`GetGrassModelHeight` on the owning geometry) is stable per grass type. Batches are
  freed and reallocated as tiles churn, so nothing may be cached by batch pointer across frames.
- Grass within 512 units of the camera is always generated, and the player's grass was present with
  the camera 650 units away.

## Design

### Components

| Unit | Role |
|------|------|
| `GrassCoverMath.h` (new) | Pure math, no engine types: clump top after collision flattening, tallness weight, sector coverage, sight-line depth through a clump. |
| `GrassCover.h/.cpp` (new) | Main-thread scan of the grass map into a clump list; computes the player's grass cover; publishes a seqlocked snapshot; answers sight-depth queries from the snapshot. |
| `TreeCover.cpp` (changed) | Keeps the two detection patches; combines tree and grass cover in the light hook and in the LOS hook. |
| `RenderHook.cpp/.h` (changed) | Exposes the cached grass model metrics: height, plus a new horizontal radius. |
| `SettingManager` (changed) | Eight new Grass.ini keys, loaded, saved, and exposed in the game menu like the TreeCover keys. |
| `ShaderManager.cpp` (changed) | Calls `UpdateGrassCover()` next to `UpdateTreeCover()`. |

`CreateTreeCoverHook` stays the only installer of the two call-site patches (`0x5F68DB`, `0x5F6647`),
because each site takes one hook.

### Settings (Grass.ini `[Default]`)

| Key | Default | Meaning |
|-----|---------|---------|
| `GrassCover` | 1 (code default 0) | Scale the sneaking player's detection light by grass cover |
| `GrassCoverLightReduction` | 0.6 | Fraction of the light removed at full grass cover |
| `GrassCoverMinHeight` | 90 | Clump top above the player's feet, world units, where cover begins |
| `GrassCoverFullHeight` | 120 | Clump top above the player's feet where a clump counts fully |
| `GrassCoverRadius` | 200 | World units around the player that are scanned for clumps |
| `GrassCoverOpacity` | 0.5 | How much one full-height clump blocks, 0–1; overlapping clumps compound |
| `GrassCoverBlockLOS` | 1 (code default 0) | Tall grass blocks observers' line of sight |
| `GrassCoverLOSDepth` | 150 | Effective grass depth, world units, along a sight line that blocks it |

All defaults are starting points for in-game tuning.

### Data flow per frame (main thread)

`UpdateGrassCover()` returns early, and publishes an empty snapshot, unless `GrassCover` or
`GrassCoverBlockLOS` is on and the player is sneaking, not swimming, and in a worldspace.

1. **Scan.** Walk the map's entries and their batch lists. For each batch:
   - Read the owning geometry's model metrics (height `H`, horizontal radius `W`). Skip the batch when
     the geometry or its data is null, or when `H ≤ GroundcoverMaxHeight` (groundcover).
   - Skip the batch when `used > capacity` or the instance array is null.
   - Cull: skip the batch when instance 0 is farther than 6000 + `GrassCoverRadius` (one cell's
     diagonal plus the radius) from the player.
   - For each used instance within `GrassCoverRadius + W` horizontally of the player, build a clump:
     - Height factor `F` from `w`; top `T = z + H·F`.
     - **Collision flattening**, mirroring `GrassCollisionDisplacement` for the player source only
       (radius `Rc` = CollisionRadius × the player's radiusScale): `influence = smoothstep(1, 0, d/Rc)`,
       `T -= influence · CollisionFlattenStrength`. The clump moves outward by
       `influence · smoothstep(0, 0.3, d/Rc) · CollisionStrength`. Both apply only while grass
       collision is active (`TheShaderManager->GrassCollisionSourceCount > 0`).
     - Tallness `τ = smoothstep(GrassCoverMinHeight, GrassCoverFullHeight, T − player z)`. Keep the clump
       only if `τ > 0`.
     - Clump = {x, y, base z, top T, half-width `W·ScaleX`, weight `Opacity·τ`}.
   - Keep at most 96 clumps, the nearest ones.
2. **Light cover.** Split the circle around the player into 16 sectors, each starting fully exposed.
   - A clump at distance `d` covers the angles within `atan2(halfWidth, d)` of its bearing; a clump
     containing the player covers every sector.
   - Each covered sector's exposure is multiplied by `1 − weight`.
   - Grass cover = mean over sectors of `1 − exposure`. A player surrounded on all sides gets near 1;
     a player at a field's edge gets about half.
3. **Publish** {cover, torso point, clumps} through a seqlock, the same pattern as
   `TreeCoverSnapshot`.

### Detection hooks (any thread)

- **Light (`AdjustDetectionLight`):** gate on (TreeCover or GrassCover), target is the player, and
  sneaking. `Scale = (1 − treeCover·TreeCoverLightReduction) · (1 − grassCover·GrassCoverLightReduction)`,
  where each term is 1 when its feature is off. Clamp to [0, 1].
- **LOS (`DetectionLineOfSightHook`):** after the engine says visible, compute
  `blocked = treeDepth/TreeCoverLOSDepth + grassDepth/GrassCoverLOSDepth ≥ 1`. Each term counts only
  when its BlockLOS switch is on and its threshold is > 0.
  - `grassDepth` = Σ over snapshot clumps of `weight ×` the length of the eye → torso segment inside
    the clump's vertical cylinder (circle of half-width in XY, base z to top T). It is a 2D
    segment/circle chord clipped against the z-range.
  - A low observer's sight line runs through many clumps; one looking down from a hill crosses only
    the few beside the player.

### Error handling

- The scan only reads engine memory, and only on the main thread, which also owns grass generation.
  Every pointer is null-checked, the hash size is sanity-checked (≤ 65536), and `used ≤ capacity` is
  required. No SEH: the probe's guarded scan never faulted over ~45 samples.
- If the snapshot read fails (writer mid-update after the retries), the LOS hook treats grass depth
  as 0, as tree cover does.
- An empty snapshot (feature off, not sneaking, interior) gives cover 0 and depth 0.

### Cost

- One read per batch for the cull, then a distance test for the instances of the few batches within
  reach.
- The model metrics are cached by geometry data, computed once per new batch geometry (the existing
  `GrassModelHeights` cache, extended with the horizontal radius).
- Sight-depth queries loop over ≤ 96 clumps per observer check.

## Testing

There is no test harness in this repo. Verification is in-game, with temporary instrumentation
(removed before commit) that logs grass cover, clump count, scan time, the light before/after, and
the LOS depths:

1. Sneak in the 128-unit grass field centre, at its edge, in the 73–83-unit grass, in short grass, and
   on bare ground. Check the cover against the success criteria.
2. Check the scan time stays ≤ 0.05 ms in the densest spot.
3. Line of sight: a hostile NPC on level ground across a tall field should lose the player; an NPC on
   a rise should keep them; an NPC adjacent in the grass should keep them.
4. With `GrassCover = 0` and `GrassCoverBlockLOS = 0`, the light and LOS values match today's for the
   same spots, with tree cover on and off.
5. Tree and grass together: a shrub inside tall grass compounds both reductions, and the scale never
   drops below 0.

## Open points for tuning (not blockers)

- Defaults for `GrassCoverOpacity`, `GrassCoverLOSDepth`, `GrassCoverMinHeight` and
  `GrassCoverFullHeight`, after step 1 of testing.
- Whether the half-width should also scale by `F`. That depends on the unknown `ScaleMask`; start
  without it, and compare against what is on screen.

## Play-test result (2026-11-03)

One unscripted sneak through the probed fields, shipped defaults:

- Tall grass: cover 0.82–1.00 (15–90 clumps); field edges 0.54–0.71; the shorter grass 0.15–0.33.
- Light scale 0.40–0.67 in grass alone, 0.12–0.24 with a shrub on top.
- LOS: blocked when the grass depth along the sight line passed 150 (165–1966 logged), kept below it.
- Scan 0.023–0.071 ms; the peak (85–90 clumps) is a little over the 0.05 ms target, accepted.
- No crashes. The switch-off and shader-off checks were not run separately; tuning left the defaults as they are.
