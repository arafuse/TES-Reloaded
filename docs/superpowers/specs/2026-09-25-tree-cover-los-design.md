# Tree Cover Line-of-Sight Blocking — Design

Date: 2026-09-25
Status: Implemented on feat/misc-3; play-tested 2026-09-25

## Goal

Complement tree cover light reduction ([2026-09-24-tree-cover-sneak-design.md](2026-09-24-tree-cover-sneak-design.md)):
while the player sneaks inside a small SpeedTree tree or shrub, an observer whose sight line to the
player passes through enough of that shrub's foliage loses line of sight for sneak detection.

## Decisions

| Question | Decision |
|---|---|
| Mechanism | Wrap the detection call to the engine LOS test instead of adding invisible Havok geometry. No bodies, layers or cell lifetimes; arrows, camera, movement and the other 11 LOS callers are unaffected |
| Blocking rule | Directional: integrate foliage density along the observer-eye → player-torso segment; block when the depth reaches a threshold |
| Shrub scope | Only shrubs the player is inside (the ones the cover scan already finds). Hiding *behind* a bush is out of scope |
| Light reduction | Unchanged; still applies to observers whose LOS is not blocked |

## Engine facts (disassembly-verified 2026-09-25)

- `Actor_GetDetectionLevel` 0x5F6540 calls the general actor LOS test `sub_5F2820` exactly once, at
  **0x5F6647**: thiscall, `ecx` = observer, stack args `(1, target, 1, &reason, 0)`, returns `al`,
  callee cleans (`ret 0x14`). `sub_5F2820` has 12 callers in total (combat, dialogue, etc.).
- The returned byte is stored at frame `[esp+0x2c]` (0x5F664E) and reaches three consumers:
  `Calc_DetectionLevel` arg 2, the caller's out byte `[esi]` (0x5F6693), and the observer's
  detection-list update, process vfunc 0xA8 (0x5F6943–0x5F6950).
- In `Calc_DetectionLevel` 0x5463F0 the visual term is
  `(light + fDetectionSneakLightMod) × LOS × distanceFactor × …`, so LOS = 0 makes it **zero**. The
  sound term is multiplied by `fSneakSoundLosMult` (value at 0xB36718) when LOS = 0, 1.0 otherwise.
  Blocked LOS means the player can only be heard, and more quietly.
- `TESObjectREFR::pos` is at +0x2C and `scale` at +0x38 (Oblivion block of Game.h).

## Components

All in the existing module `TESReloaded/Core/TreeCover.h/.cpp` and `TreeCoverMath.h` (Oblivion only).

### `TreeCoverRayDepth(...)` — pure ray math (`TreeCoverMath.h`)

Free of engine types, like `TreeCoverAt`.

- Inputs: one shrub's `TreeCoverInput` (its Q fields ignored), segment endpoints `A` (observer eye)
  and `B` (player torso).
- Clips `A→B` to the shrub's ellipsoid bounding box (horizontal semi-axis `rₕ` around `C.xy`,
  vertical from `BaseZ` to `C.z + R`), using the same `rₕ` and vertical extent as `TreeCoverAt`.
  To keep them consistent, factor the ellipsoid-axis computation out of `TreeCoverAt` into a shared
  helper.
- Takes `kTreeCoverRaySamples = 16` midpoint samples on the clipped part, evaluates the shared shape's
  coverage (`TreeCoverShapeAt`, the same field `TreeCoverAt` uses) at each sample, and returns
  `Σ c × step length`: effective foliage depth in world units.
- Samples below `BaseZ` count as 0. An empty clip returns 0.

Properties that fall out of the density field: a ray down the bent-open centre sees ~0 density; a
short ray from an observer inside the same shrub accumulates little depth; an observer with a small
ref scale (lower eye) crosses the dense lower half; small creatures at scale 1.0 use the human eye
height.

### Published shrub snapshot

- `UpdateTreeCover()` already visits every shrub whose ellipsoid contains the torso point. For each
  one with non-zero coverage it also appends the shrub's `TreeCoverInput` to a snapshot of at most
  `kTreeCoverMaxShrubs = 8` entries (extra shrubs are dropped). It publishes an empty snapshot on
  every early-out path (feature off, no player, not sneaking, swimming).
- Snapshot also carries the torso point `B` used this frame.
- Detection may run on the threaded-AI thread (`bUseThreadedAI=1`). The snapshot is guarded by a
  seqlock: the writer increments a `volatile LONG` sequence (odd = writing), writes, then increments
  again, with `MemoryBarrier()` / `_ReadWriteBarrier()` fencing; the reader copies the snapshot and
  retries while the sequence is odd or changed (bounded retries; on failure it treats the snapshot as
  empty, i.e. no override).

### `DetectionLineOfSightHook` — the call-site wrapper

- `WriteRelCall(0x5F6647, (UInt32)DetectionLineOfSightHook)`, installed in `CreateTreeCoverHook()`
  beside the existing Calc_DetectionLevel patch.
- Declared `static bool __fastcall DetectionLineOfSightHook(Actor* Observer, void* Edx, UInt32 Arg1,
  TESObjectREFR* Target, UInt32 Arg3, UInt32* Reason, UInt32 Arg5)`, matching the thiscall
  convention with callee cleanup. It first calls the original `0x5F2820` via `ThisCall` with the same
  arguments.
- It returns the original result unchanged unless all hold: result is true, `TreeCoverBlockLOS` is
  on, `Target == Player`, the snapshot is non-empty.
- Eye point `A = Observer->pos + (0, 0, kTreeCoverEyeHeight × Observer->scale)`, with
  `kTreeCoverEyeHeight = 110`.
- Sums `TreeCoverRayDepth` over the snapshot's shrubs; returns false when the sum is
  `≥ TreeCoverLOSDepth`, but only for a positive `TreeCoverLOSDepth` (a non-positive threshold or NaN
  depth leaves the original result).
- `*Reason` is left as the original wrote it.
- Reads only the snapshot, `Player`, the observer's `pos`/`scale` and settings; never walks cells or
  the scene graph.

## Settings

In `SettingsGrass` (`Shaders/Grass/Grass.ini`, `[Default]`), each wired through load, save, the menu
map and the set path in `SettingManager.cpp`, like `TreeCoverLightReduction`:

| Key | Type | Default (code) | Meaning |
|---|---|---|---|
| `TreeCoverBlockLOS` | int | 0 | Shrubs the sneaking player is inside block observers' line of sight |
| `TreeCoverLOSDepth` | float | 64 | Effective foliage depth (world units) along the sight line that blocks it |

`TreeCoverBlockLOS` only has an effect while `TreeCover` is on, since the snapshot comes from the
cover scan. The shipped `Grass.ini` enables it (`TreeCoverBlockLOS = 1`) alongside `TreeCover = 1`.

## Verification

No test framework exists.

1. Standalone harness (scratchpad, not committed) compiling `TreeCoverMath.h`: a horizontal ray
   through the ring gives large depth; a vertical ray down the centre of a bent shrub gives ~0; a
   short ray inside the shrub gives small depth; a ray missing the shrub gives 0; `ρ = 0` behaves as
   the plain ellipsoid.
2. Release build via MSBuild (PowerShell) succeeds.
3. Temporary ~1 Hz log from the hook: observer form ID, depth, blocked or not. Used to tune the
   `TreeCoverLOSDepth` default. Removed before the feature commit.
4. User in-game check: sneak into a shrub; the sneak eye stays closed while an NPC looks through the
   bush from the side, and opens when they look down into the parted centre or stand in the bush.
5. Regenerate `compile_commands.json` only if files are added (none planned).

## Commits

- `feat(Trees): …` for the code.
- Separate `docs:` commits for this spec and for memory `sneak-detection-formula` (LOS call site,
  consumers, `fSneakSoundLosMult`).

## Out of scope

- Shrubs the player is behind but not inside.
- NPC targets (only the player is concealed).
- The other `sub_5F2820` callers (combat targeting, dialogue, spells).
- Graded / probabilistic LOS; the result is a hard threshold.
