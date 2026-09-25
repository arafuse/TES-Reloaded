# Tree Cover for Sneak Detection — Design

Date: 2026-09-24
Status: Implemented on feat/misc-3; play-tested 2026-09-25

## Goal

When the player is sneaking inside a small SpeedTree tree or shrub, reduce the player's light level
as seen by the sneak detection formula. Because tree collision bends foliage away from the player,
the zone of best concealment moves from the centre of an ellipsoid around the tree (no deformation)
to a ring (asymmetric torus) around the tree centre, with the centre itself fully exposed, in
proportion to how far the foliage is pushed at the player's height.

## Decisions

| Question | Decision |
|---|---|
| What drives "amount of deformation" | The shader's peak sideways push at full bend (the crown), relative to the tree's horizontal size. Revised after play-test 1: at crouched torso height the bend is ~7% of full, giving ρ ≈ 0.04 and no visible centre exposure |
| Low in the shrub | Fully covered vertically from the ground up to the ellipsoid centre; only the upper half falls off. Revised after play-test 1: the symmetric ellipsoid capped a crouched player's cover near 0.66 |
| Detection lever | Scale the target light argument of `Calc_DetectionLevel`: `light' = light × (1 − cover × TreeCoverLightReduction)` |
| Tree source | Main-thread scan of loaded cells' object lists once per frame, only while the player sneaks |
| Hook install | Always installed; gated at runtime on `TreeCover`, so the in-game menu toggle works live |
| Centre exposure | Centre is fully exposed for any non-zero deformation (no small-ρ blend) |

## Components

New module `TESReloaded/Core/TreeCover.h/.cpp` (Oblivion only).

### `UpdateTreeCover()` — per-frame scan, main thread

Called from `ShaderManager::UpdateConstants` immediately after `UpdateGrass`.

- Publishes 0 and returns if `SettingsGrass.TreeCover` is off, `Player`/`Player->process`/
  `Player->parentCell` is NULL, or `Player->process->GetMovementFlags()` lacks `kMovement_Sneak`
  (swimming overrides the sneak flag).
- Walks `objectList` of the player's cell (interior) or every cell in `Tes->gridCellArray`
  (exterior, `*SettingGridsToLoad` square), the same loop shape as `UpdateGrass`.
- Filters, cheapest first: NULL ref, disabled ref, no node, `baseForm->formType != kFormType_Tree`,
  bound radius `> TreeCollisionMaxBound`, horizontal distance from player point to bound centre
  `> R`, player point outside the ellipsoid's vertical range.
- Combines per-tree coverage `cᵢ` as `cover = 1 − ∏(1 − cᵢ)`.
- Stores the result in a file-static `volatile float`.

Before relying on it, verify (disassembly / in-game log) that the tree ref's root node is the
`BSTreeNode` whose bound `TreeCollisionMaxBound` is tuned against (`TreeCollision.cpp` walks up from
geometry to `VFTBSTreeNode`). If the root is a wrapper, read the `BSTreeNode` child's bound instead.

### `TreeCoverAt(...)` — pure coverage function

Header-only in `TESReloaded/Core/TreeCoverMath.h`, free of engine types so a standalone harness can
check it. Inputs: player point `Q`, tree base `P` (ref position), bound centre `C`, bound radius `R`,
and the live push strength (0 when the tree isn't bending). Output: coverage in [0, 1].

**Ellipsoid**

- Vertical: from the ground to the bound top. Centre `z₀ = (P.z + C.z + R) / 2`,
  semi-axis `rᵥ = (C.z + R − P.z) / 2`.
- Horizontal: centred on `C.xy`, semi-axis `rₕ = sqrt(max(R² − (C.z − P.z)², (0.25R)²))`.

**Player point**: `Q = Player->pos + (0, 0, 50)` (crouched torso; code constant).

**Deformation → ring radius** (mirrors `TreeCollisionDisplacement` at full bend)

- `H = TreeCollisionStrength × 0.785` — 0.785 is the peak of `smoothstep(1,0,t) × smoothstep(0,0.3,t)`,
  the shader's sideways push profile. The shader's height falloff (`bend²`) reaches 1 at the crown,
  so `H` is how far the canopy parts; it does not depend on the player's height.
- `ρ = clamp(H / rₕ, 0, 0.85)`. The upper clamp keeps a cover band inside the ellipsoid and avoids
  dividing by `1 − ρ = 0`.
- `ρ = 0` when `TreeCollision` is off or `TheShaderManager->GrassCollisionSourceCount == 0` (the tree
  isn't bending), which reduces the model to the plain ellipsoid.

**Normalised position**: `u = |Q.xy − C.xy| / rₕ`, `v = max(Q.z − z₀, 0) / rᵥ` — the lower half of
the ellipsoid counts as fully inside vertically, since a rooted shrub hides a low body.

**Distance to the ring** (asymmetric torus whose tube fills the ellipsoid)

- `u ≥ ρ` (or `ρ` below a small epsilon): `d = sqrt(((u − ρ) / (1 − ρ))² + v²)`
- `u < ρ`: `d = sqrt(((ρ − u) / ρ)² + v²)`
- `c = 1 − smoothstep(0, 1, d)`

Properties: `ρ = 0` is the lower-flattened ellipsoid (full cover on the axis up to `z₀`, zero at the surface); for any
`ρ > 0` the centre is fully exposed and cover peaks on the ring `u = ρ`.

### `CreateTreeCoverHook()` — detection hook

- `WriteRelCall(0x5F68DB, stub)` — the single call to cdecl `Calc_DetectionLevel` 0x5463F0 (see
  memory `sneak-detection-formula`). `ebp` = target actor, `ebx` = observer at the call site.
- Naked stub, reached by `call`, so arg *n* is at `[esp + 4 + 4n]`. If `TreeCover` is on,
  `ebp == Player`, arg 9 (target sneaking) is non-zero, and the published cover is > 0, it rewrites
  arg 5 (target light) in place, then `jmp 0x5463F0` with all registers preserved.
- Verified by disassembly: arg 5 is an **int** 0–100 (`ftol` of the process vfunc 0x3AC result,
  night-eye scaled and clamped at 0x5F6611–0x5F6629), so the adjuster multiplies and truncates.
  Arg 9 (target sneaking) is a BYTE set from bool `sub_5E0550` (movement flags `& 0x400` and not
  `& 0x800`), forced to 0 when the caller's flag at `[esp+0x128]` is set; the upper three bytes of
  the pushed dword are stale stack, so only the low byte is meaningful and the `(UInt8)` cast is
  required. `sub_5F3B50` is a separate term (arg 7, boot weight). Process vfunc 0x2C0 is
  `GetMovementFlags`.
- Invisibility and chameleon ≥ 100 short-circuit before this call and are unaffected.
- Installed unconditionally from `Main.cpp` beside `CreateTreeCollisionHook()`.

### Threading

`bUseThreadedAI` may run detection off the main thread. The AI side reads only the published
`volatile float`, the `Player` pointer, and aligned settings values; it never walks cells or the
scene graph. Cover lags by at most one frame.

## Settings

In `SettingsGrass` (`Shaders/Grass/Grass.ini`, `[Default]`), each wired through load, save, the
menu map and the set path in `SettingManager.cpp`:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `TreeCover` | int | 0 | Enable tree cover for sneak detection |
| `TreeCoverLightReduction` | float | 0.75 | Light scale removed at full cover |

Because `fDetectionSneakLightMod` is added after the scaled light, full cover dims the player but
does not make them visually undetectable.

Cover eligibility shares `TreeCollisionMaxBound` with bending, so raising it (e.g. to bend young
trees) also grants cover under raised canopies, because the lower half counts as covered.

## Verification

No test framework exists.

1. Release build via MSBuild (PowerShell) succeeds.
2. Temporary ~1 Hz log while sneaking: best tree's `u`, `v`, `ρ`, `c`, combined cover, light
   before/after. Used to spot-check centre / ring / edge and confirm the arg type and root-node
   question. Removed before the feature commit.
3. User in-game check: sneak into a shrub and watch the sneak eye; low and off-centre hides best,
   dead centre with a bent crown exposes.
4. Regenerate `compile_commands.json` for the new files.

## Commits

- `feat(Trees): …` for the code.
- Separate `docs:` commits for this spec and for updating memory `sneak-detection-formula` with the
  verified arg types.

## Out of scope

- NPC targets (only the player is concealed).
- Observer direction (cover is omnidirectional).
- The flatten (downward) component of the bend.
- Caching tree lists across frames.
