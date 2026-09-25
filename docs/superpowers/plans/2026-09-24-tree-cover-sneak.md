# Tree Cover for Sneak Detection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the player sneaks inside small SpeedTree trees/shrubs, scale down the light level fed to Oblivion's sneak detection formula, with the best-concealment zone moving from an ellipsoid centre to a ring as tree collision bends the foliage.

**Architecture:** A header-only, engine-free coverage function (`TreeCoverMath.h`) is evaluated once per frame on the main thread by `UpdateTreeCover()` over the loaded cells' small tree refs; the combined cover is published in a `volatile float`. A naked stub patched over the single call to `Calc_DetectionLevel` (0x5F68DB) reads that float and scales the int light argument for the sneaking player before jumping to the original function.

**Tech Stack:** C++ (MSVC v145, x86), OBSE plugin, `WriteRelCall` memory patching, MSVC inline asm.

**Spec:** `docs/superpowers/specs/2026-09-24-tree-cover-sneak-design.md`

## Global Constraints

- Oblivion only. New files live in `TESReloaded/Core/` and are compiled only by `OblivionReloaded.vcxproj`.
- Coding style (AGENTS.md): public symbols get `///` doc comments; code is self-documenting; inline comments only when necessary and at most 1–3 lines.
- Build ONLY through the PowerShell tool (Bash's TEMP breaks MSBuild with a fake MSB3073):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  The post-build step copies the DLL into the game's Plugins folder; the game must not be running.
- Verified engine facts (do not re-derive): `Calc_DetectionLevel` = 0x5463F0, cdecl, 16 dword args, only call at 0x5F68DB (`add esp, 0x40` follows); `ebp` = target Actor* at the call; arg 5 = target light, **int** 0–100; arg 9 = target sneaking, only the low byte is meaningful; process vfunc 0x2C0 = `BaseProcess::GetMovementFlags` (0x400 sneak, 0x800 swim overrides sneak); `BSTreeNode` vtable = 0x00A65854.
- Coverage constants: player torso = `pos.z + 50`; push-profile peak 0.785; ring clamp 0.85; ellipsoid horizontal floor `0.25R`.
- Settings live in `SettingsGrass` / `Shaders\Grass\Grass.ini` `[Default]`: `TreeCover` (int, code default 0), `TreeCoverLightReduction` (float, code default 0.75).
- Commits: code as `feat(Trees): …`; memory/spec/plan changes only in separate `docs:` commits.

**User decisions (already made):**
- Deformation amount = the shader's displacement curve evaluated at the player's torso height relative to the tree's horizontal size.
- Lever = scale the light argument: `light' = light × (1 − cover × TreeCoverLightReduction)`.
- Tree source = main-thread scan of loaded cells once per frame, only while sneaking.
- Hook always installed; runtime-gated on `TreeCover`.
- Tree centre is fully exposed for any non-zero deformation (no small-ring blend).

---

## File Structure

| File | Responsibility |
|---|---|
| `TESReloaded/Core/TreeCoverMath.h` (create) | Pure ellipsoid/torus coverage function, no engine types |
| `TESReloaded/Core/TreeCover.h` (create) | Public API: `CreateTreeCoverHook()`, `UpdateTreeCover()` |
| `TESReloaded/Core/TreeCover.cpp` (create) | Per-frame tree scan, published cover, detection stub |
| `TESReloaded/Core/SettingManager.h/.cpp` (modify) | Two new `SettingsGrass` settings |
| `OblivionReloaded/Shaders/Grass/Grass.ini` (modify) | Ship values for the new settings |
| `TESReloaded/Core/ShaderManager.cpp` (modify) | Call `UpdateTreeCover()` each frame |
| `OblivionReloaded/Main.cpp` (modify) | Install the hook |
| `OblivionReloaded/OblivionReloaded.vcxproj(.filters)` (modify) | Register the new files |

---

### Task 1: TreeCover settings

**Goal:** Add `TreeCover` and `TreeCoverLightReduction` to `SettingsGrass`, wired through load, save, menu map and set, with ship values in `Grass.ini`.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h:431` (after `TreeCollisionFlattenStrength`)
- Modify: `TESReloaded/Core/SettingManager.cpp:601`, `:1570`, `:2239`, `:2977` (after each `TreeCollisionFlattenStrength` line)
- Modify: `OblivionReloaded/Shaders/Grass/Grass.ini`

**Acceptance Criteria:**
- [ ] `grep -c "TreeCover\b\|TreeCoverLightReduction" TESReloaded/Core/SettingManager.cpp` counts 8 lines (4 touch points × 2 settings)
- [ ] Both fields have `///<` doc comments in `SettingsGrassStruct`
- [ ] Release build succeeds with 0 errors

**Verify:** Release build (Global Constraints command) → `OblivionReloaded.vcxproj -> ...\OblivionReloaded.dll`, 0 errors.

**Steps:**

- [ ] **Step 1: Add the fields** to `SettingsGrassStruct` in `SettingManager.h`, directly after `float TreeCollisionFlattenStrength;	///< Maximum downward push, world units`:

```cpp
	bool TreeCover;						///< Scale the sneaking player's detection light level by small-tree cover
	float TreeCoverLightReduction;		///< Fraction of the light level removed at full cover
```

- [ ] **Step 2: Load** — in `SettingManager.cpp`, directly after `SettingsGrass.TreeCollisionFlattenStrength = atof(value);` (≈ line 601):

```cpp
	SettingsGrass.TreeCover = GetPrivateProfileIntA("Default", "TreeCover", 0, Filename);
	GetPrivateProfileStringA("Default", "TreeCoverLightReduction", "0.75", value, SettingStringBuffer, Filename);
	SettingsGrass.TreeCoverLightReduction = atof(value);
```

- [ ] **Step 3: Save** — directly after the `WritePrivateProfileStringA("Default", "TreeCollisionFlattenStrength", ...)` line (≈ 1570):

```cpp
			WritePrivateProfileStringA("Default", "TreeCover", ToString(SettingsGrass.TreeCover).c_str(), Filename);
			WritePrivateProfileStringA("Default", "TreeCoverLightReduction", ToString(SettingsGrass.TreeCoverLightReduction).c_str(), Filename);
```

- [ ] **Step 4: Menu map** — directly after `Settings["TreeCollisionFlattenStrength"] = SettingsGrass.TreeCollisionFlattenStrength;` (≈ 2239):

```cpp
			Settings["TreeCover"] = SettingsGrass.TreeCover;
			Settings["TreeCoverLightReduction"] = SettingsGrass.TreeCoverLightReduction;
```

- [ ] **Step 5: Set** — directly after the `TreeCollisionFlattenStrength` branch (≈ 2977, `SettingsGrass.TreeCollisionFlattenStrength = Value;`):

```cpp
			else if (!strcmp(Setting, "TreeCover"))
				SettingsGrass.TreeCover = Value;
			else if (!strcmp(Setting, "TreeCoverLightReduction"))
				SettingsGrass.TreeCoverLightReduction = Value;
```

- [ ] **Step 6: Ship values** — append to `OblivionReloaded/Shaders/Grass/Grass.ini` after `TreeCollisionFlattenStrength = 40` (the game folder's Shaders dir is a symlink to this repo file):

```ini
TreeCover = 1
TreeCoverLightReduction = 0.75
```

- [ ] **Step 7: Build** with the Global Constraints PowerShell command. Expected: 0 errors.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp OblivionReloaded/Shaders/Grass/Grass.ini
git commit -m "feat(Trees): Add tree cover sneak settings"
```

---

### Task 2: Coverage math with standalone check

**Goal:** Header-only `TreeCoverAt()` implementing the spec's ellipsoid → asymmetric-torus coverage, proven by a standalone harness.

**Files:**
- Create: `TESReloaded/Core/TreeCoverMath.h`
- Test (not committed): `%TEMP%\treecover-check\TreeCoverCheck.cpp`

**Acceptance Criteria:**
- [ ] Harness prints `ok` for all 13 checks and exits 0
- [ ] `TreeCoverMath.h` includes only `<cmath>` (no engine types, no `min`/`max` macros)
- [ ] With push strength 0 the result is the plain ellipsoid; with push > 0 the tree centre returns 0

**Verify:** harness command in Step 4 → 13 lines ending `ok`, `ALL PASS`, exit code 0.

**Steps:**

- [ ] **Step 1: Write the harness** at `%TEMP%\treecover-check\TreeCoverCheck.cpp` (create the directory). Reference shrub: base z 0, bound centre (0,0,60), R 100 → top 160, ellipsoid centre z 80, rᵥ 80, rₕ = √(100²−60²) = 80. At torso z 80: bend 0.8, so push 120 gives ring = 120·0.785·0.64/80 = 0.7536.

```cpp
#include <cstdio>
#include <cmath>
#include "TreeCoverMath.h"

static int Failures = 0;

static void Check(const char* Name, float Got, float Want) {
	bool Ok = fabsf(Got - Want) < 1e-3f;
	printf("%-26s got %.4f want %.4f %s\n", Name, Got, Want, Ok ? "ok" : "FAIL");
	if (!Ok) Failures++;
}

static TreeCoverInput Shrub(float QX, float QZ, float Push) {
	TreeCoverInput In = { QX, 0.0f, QZ, 0.0f, 0.0f, 0.0f, 60.0f, 100.0f, Push };
	return In;
}

int main() {
	const float Ring = 120.0f * 0.785f * 0.64f / 80.0f;
	Check("ellipsoid centre", TreeCoverAt(Shrub(0, 80, 0)), 1.0f);
	Check("ellipsoid half radius", TreeCoverAt(Shrub(40, 80, 0)), 0.5f);
	Check("ellipsoid edge", TreeCoverAt(Shrub(80, 80, 0)), 0.0f);
	Check("above top", TreeCoverAt(Shrub(0, 170, 0)), 0.0f);
	Check("bent centre exposed", TreeCoverAt(Shrub(0, 80, 120)), 0.0f);
	Check("bent on ring", TreeCoverAt(Shrub(Ring * 80.0f, 80, 120)), 1.0f);
	Check("bent inner half", TreeCoverAt(Shrub(Ring * 40.0f, 80, 120)), 0.5f);
	Check("bent outer half", TreeCoverAt(Shrub((Ring + (1.0f - Ring) * 0.5f) * 80.0f, 80, 120)), 0.5f);
	Check("low centre exposed", TreeCoverAt(Shrub(0, 20, 120)), 0.0f);
	float OutRing = -1.0f;
	Check("clamped ring cover", TreeCoverAt(Shrub(68, 80, 1000), &OutRing), 1.0f);
	Check("clamped ring value", OutRing, 0.85f);
	TreeCoverInput Degenerate = Shrub(0, 80, 0);
	Degenerate.R = 0.0f;
	Check("zero bound", TreeCoverAt(Degenerate), 0.0f);
	Check("ring output unbent", (TreeCoverAt(Shrub(0, 80, 0), &OutRing), OutRing), 0.0f);
	printf(Failures ? "%d FAILED\n" : "ALL PASS\n", Failures);
	return Failures ? 1 : 0;
}
```

- [ ] **Step 2: Run it to verify it fails** (PowerShell tool):

```powershell
$vc = 'C:\Development\Microsoft\Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
$inc = 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded\Core'
$dir = "$env:TEMP\treecover-check"
cmd /c "call `"$vc`" >nul && cd /d `"$dir`" && cl /nologo /EHsc /W4 /I `"$inc`" TreeCoverCheck.cpp && TreeCoverCheck.exe"
```

Expected: `fatal error C1083: Cannot open include file: 'TreeCoverMath.h'`.

- [ ] **Step 3: Write `TESReloaded/Core/TreeCoverMath.h`**

```cpp
#pragma once

#include <cmath>

/// One tree's geometry and the player's torso point, in world units (z up), for TreeCoverAt.
struct TreeCoverInput {
	float QX, QY, QZ;		///< Player torso point
	float BaseZ;			///< Tree ref position z, taken as the ground
	float CX, CY, CZ;		///< BSTreeNode world bound centre
	float R;				///< BSTreeNode world bound radius
	float PushStrength;		///< Live sideways push (TreeCollisionStrength); 0 when the tree isn't bending
};

static const float kTreeCoverPushPeak	= 0.785f;	// peak of smoothstep(1,0,t) * smoothstep(0,0.3,t) in TreeCollision.hlsl
static const float kTreeCoverMaxRing	= 0.85f;
static const float kTreeCoverMinRing	= 0.001f;

inline float TreeCoverSaturate(float X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); }

inline float TreeCoverSmoothstep(float X) { X = TreeCoverSaturate(X); return X * X * (3.0f - 2.0f * X); }

/// Returns how concealed the player point is by one tree, in [0, 1].
/// Unbent, cover peaks at the centre of an ellipsoid running from the ground to the bound top. As
/// the tree bends away from the player, the peak moves out to a ring of normalised radius Ring
/// (the shader's push at torso height over the horizontal semi-axis) and the centre is exposed.
/// OutRing, when given, receives that ring radius.
inline float TreeCoverAt(const TreeCoverInput& In, float* OutRing = 0) {
	if (OutRing) *OutRing = 0.0f;
	float Top = In.CZ + In.R;
	float Height = Top - In.BaseZ;
	if (In.R <= 0.0f || Height <= 0.0f) return 0.0f;

	float CentreZ = (In.BaseZ + Top) * 0.5f;
	float VerticalAxis = Height * 0.5f;
	float Rise = In.CZ - In.BaseZ;
	float MinAxis = 0.25f * In.R;
	float HorizontalSq = In.R * In.R - Rise * Rise;
	float HorizontalAxis = sqrtf(HorizontalSq > MinAxis * MinAxis ? HorizontalSq : MinAxis * MinAxis);

	float Bend = TreeCoverSaturate((In.QZ - In.BaseZ) / In.R);
	float Ring = TreeCoverSaturate(In.PushStrength * kTreeCoverPushPeak * Bend * Bend / HorizontalAxis);
	if (Ring > kTreeCoverMaxRing) Ring = kTreeCoverMaxRing;
	if (OutRing) *OutRing = Ring;

	float DX = In.QX - In.CX;
	float DY = In.QY - In.CY;
	float U = sqrtf(DX * DX + DY * DY) / HorizontalAxis;
	float V = (In.QZ - CentreZ) / VerticalAxis;
	float Radial = (Ring < kTreeCoverMinRing || U >= Ring) ? (U - Ring) / (1.0f - Ring) : (Ring - U) / Ring;
	return 1.0f - TreeCoverSmoothstep(sqrtf(Radial * Radial + V * V));
}
```

- [ ] **Step 4: Run the harness to verify it passes** — same command as Step 2. Expected: 13 lines ending `ok`, then `ALL PASS`; `$LASTEXITCODE` 0. If `/W4` warns, fix the header (not the harness).

- [ ] **Step 5: Commit** (the harness stays in `%TEMP%`, uncommitted)

```bash
git add TESReloaded/Core/TreeCoverMath.h
git commit -m "feat(Trees): Add tree cover coverage math"
```

---

### Task 3: Per-frame tree cover scan

**Goal:** `UpdateTreeCover()` scans loaded small tree refs once per frame on the main thread while the player sneaks and publishes the combined cover.

**Files:**
- Create: `TESReloaded/Core/TreeCover.h`, `TESReloaded/Core/TreeCover.cpp`
- Modify: `TESReloaded/Core/ShaderManager.cpp` (include + call after the `if (currentCell) { ... }` block that ends with `UpdateGrass(...)`, ≈ line 2657)
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj` (after lines 148 and 189), `OblivionReloaded/OblivionReloaded.vcxproj.filters` (after the `TreeCollision.h` / `TreeCollision.cpp` entries)

**Acceptance Criteria:**
- [ ] Cover is published as 0 when `TreeCover` is off, `Player->process`/`parentCell` is NULL, or movement flags lack 0x400 or have 0x800
- [ ] Exterior walks every `Tes->gridCellArray` cell (`*SettingGridsToLoad` square); interior walks `Player->parentCell`
- [ ] Refs are skipped when NULL, disabled/deleted, not `kFormType_Tree`, have no `BSTreeNode` (root or direct child), bound radius ≤ 0 or > `TreeCollisionMaxBound`, horizontally beyond R, or with torso z outside [ref z, bound top]
- [ ] Trees combine as `1 − ∏(1 − cᵢ)`; `PushStrength` is 0 unless `TreeCollision` is on and `GrassCollisionSourceCount > 0`
- [ ] Release build succeeds; `compile_commands.json` regenerated

**Verify:** Release build → 0 errors; `powershell -File Tools\GenerateCompileCommands.ps1` completes and `grep -c TreeCover.cpp compile_commands.json` ≥ 1.

**Steps:**

- [ ] **Step 1: Create `TESReloaded/Core/TreeCover.h`**

```cpp
#pragma once

/// Tree cover for sneak detection: while the player sneaks inside small SpeedTree trees and shrubs,
/// the light level fed to Calc_DetectionLevel is scaled down. Oblivion only.

/// Patches the only call to Calc_DetectionLevel. Always installed; gated at runtime on
/// SettingsGrass.TreeCover.
void CreateTreeCoverHook();

/// Rescans nearby small trees and publishes the player's cover for the detection hook. Main thread,
/// once per frame.
void UpdateTreeCover();
```

- [ ] **Step 2: Create `TESReloaded/Core/TreeCover.cpp`** (the hook is added in Task 4; `CreateTreeCoverHook` is not defined yet and nothing calls it yet)

```cpp
#include "TreeCover.h"
#include "TreeCoverMath.h"

static const void*	VFTBSTreeNode			= (void*)0x00A65854;
static const float	kTreeCoverTorsoHeight	= 50.0f;
static const UInt32	kMovementSneak			= 0x400;
static const UInt32	kMovementSwim			= 0x800;

static volatile float PlayerTreeCover = 0.0f;

static NiNode* FindRefTreeNode(NiNode* Root) {

	if (*(void**)Root == VFTBSTreeNode) return Root;
	for (int i = 0; i < Root->m_children.end; i++) {
		NiAVObject* Child = Root->m_children.data[i];
		if (Child && *(void**)Child == VFTBSTreeNode) return (NiNode*)Child;
	}
	return NULL;

}

static void AccumulateTreeCover(TList<TESObjectREFR>::Entry* Entry, TreeCoverInput& In, float MaxBound, float& Exposure) {

	for (; Entry; Entry = Entry->next) {
		TESObjectREFR* Ref = Entry->item;
		if (!Ref || (Ref->flags & (TESObjectREFR::kFlags_Disabled | TESObjectREFR::kFlags_Deleted))) continue;
		if (!Ref->baseForm || Ref->baseForm->formType != TESForm::FormType::kFormType_Tree) continue;
		NiNode* Root = Ref->GetNode();
		NiNode* Tree = Root ? FindRefTreeNode(Root) : NULL;
		if (!Tree) continue;
		NiBound* Bound = Tree->GetWorldBound();
		if (Bound->Radius <= 0.0f || Bound->Radius > MaxBound) continue;
		float DX = In.QX - Bound->Center.x;
		float DY = In.QY - Bound->Center.y;
		if (DX * DX + DY * DY > Bound->Radius * Bound->Radius) continue;
		if (In.QZ < Ref->pos.z || In.QZ > Bound->Center.z + Bound->Radius) continue;
		In.BaseZ = Ref->pos.z;
		In.CX = Bound->Center.x;
		In.CY = Bound->Center.y;
		In.CZ = Bound->Center.z;
		In.R = Bound->Radius;
		Exposure *= 1.0f - TreeCoverAt(In);
	}

}

void UpdateTreeCover() {

	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	if (!Settings->TreeCover || !Player || !Player->process || !Player->parentCell) {
		PlayerTreeCover = 0.0f;
		return;
	}
	UInt32 Movement = Player->process->GetMovementFlags();
	if (!(Movement & kMovementSneak) || (Movement & kMovementSwim)) {
		PlayerTreeCover = 0.0f;
		return;
	}

	TreeCoverInput In = {};
	In.QX = Player->pos.x;
	In.QY = Player->pos.y;
	In.QZ = Player->pos.z + kTreeCoverTorsoHeight;
	bool Bending = Settings->TreeCollision && TheShaderManager->GrassCollisionSourceCount > 0;
	In.PushStrength = Bending ? Settings->TreeCollisionStrength : 0.0f;

	float Exposure = 1.0f;
	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++) {
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
				TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y);
				if (Cell) AccumulateTreeCover(&Cell->objectList.First, In, Settings->TreeCollisionMaxBound, Exposure);
			}
		}
	}
	else {
		AccumulateTreeCover(&Player->parentCell->objectList.First, In, Settings->TreeCollisionMaxBound, Exposure);
	}
	PlayerTreeCover = 1.0f - Exposure;

}
```

If the build rejects a field or type name, check the OBLIVION (middle) block of `Game.h`/`GameNi.h` (memory `gameh-multigame-blocks`): `TESObjectREFR` at ≈ Game.h:6369 (`flags`, `baseForm` 0x1C, `pos` 0x2C, `GetNode()`), `Actor::process` 0x58, `NiAVObject::GetWorldBound()` GameNi.h:1931, `NiNode::m_children` GameNi.h:1957, `TList<T>::Entry` usage as in `ShadowManager.cpp:905`.

- [ ] **Step 3: Call it each frame** — in `ShaderManager.cpp` add `#include "TreeCover.h"` after `#include <filesystem>` (line 8), then in `ShaderManager::UpdateConstants` insert directly after the closing brace of the `if (currentCell) { ... }` block whose last statement is `UpdateGrass(ShaderConst, GrassCollisionSources, GrassCollisionWeights, GrassCollisionSourceCount);` and before `if (TheSettingManager->SettingsMain.Shaders.POM)`:

```cpp

	UpdateTreeCover();
```

It must run after `UpdateGrass` (it reads `GrassCollisionSourceCount`) and outside the `currentCell` block so cover resets when the player has no cell.

- [ ] **Step 4: Register the files.** In `OblivionReloaded.vcxproj`, after `<ClInclude Include="..\TESReloaded\Core\TreeCollision.h" />` add:

```xml
    <ClInclude Include="..\TESReloaded\Core\TreeCover.h" />
    <ClInclude Include="..\TESReloaded\Core\TreeCoverMath.h" />
```

and after `<ClCompile Include="..\TESReloaded\Core\TreeCollision.cpp" />` add:

```xml
    <ClCompile Include="..\TESReloaded\Core\TreeCover.cpp" />
```

In `OblivionReloaded.vcxproj.filters`, after the `TreeCollision.h` `ClInclude` element add:

```xml
    <ClInclude Include="..\TESReloaded\Core\TreeCover.h">
      <Filter>Core</Filter>
    </ClInclude>
    <ClInclude Include="..\TESReloaded\Core\TreeCoverMath.h">
      <Filter>Core</Filter>
    </ClInclude>
```

and after the `TreeCollision.cpp` `ClCompile` element add:

```xml
    <ClCompile Include="..\TESReloaded\Core\TreeCover.cpp">
      <Filter>Core</Filter>
    </ClCompile>
```

- [ ] **Step 5: Build** (Global Constraints command). Expected: 0 errors.

- [ ] **Step 6: Regenerate clangd database** (PowerShell): `powershell -File Tools\GenerateCompileCommands.ps1`. Expected: completes; `compile_commands.json` (gitignored) mentions `TreeCover.cpp`.

- [ ] **Step 7: Commit**

```bash
git add TESReloaded/Core/TreeCover.h TESReloaded/Core/TreeCover.cpp TESReloaded/Core/ShaderManager.cpp OblivionReloaded/OblivionReloaded.vcxproj OblivionReloaded/OblivionReloaded.vcxproj.filters
git commit -m "feat(Trees): Scan small trees for sneaking player cover"
```

---

### Task 4: Detection light hook

**Goal:** Patch the call at 0x5F68DB so the sneaking player's light argument is scaled by `1 − cover × TreeCoverLightReduction` before `Calc_DetectionLevel` runs.

**Files:**
- Modify: `TESReloaded/Core/TreeCover.cpp`
- Modify: `OblivionReloaded/Main.cpp:17` (include) and `:58` (install)

**Acceptance Criteria:**
- [ ] `WriteRelCall(0x005F68DB, DetectionLevelHook)` installed unconditionally after `CreateTreeCollisionHook()`
- [ ] Stub preserves all registers (`pushad`/`popad`) and tail-jumps to 0x005463F0 with the original stack
- [ ] Light is changed only when `TreeCover` is on, cover > 0, target is `Player`, and arg 9's low byte is non-zero; scale is clamped at ≥ 0
- [ ] Release build succeeds

**Verify:** Release build → 0 errors; `grep -n "CreateTreeCoverHook" OblivionReloaded/Main.cpp` shows the call on the line after `CreateTreeCollisionHook();`.

**Steps:**

- [ ] **Step 1: Add the constants** at the top of `TreeCover.cpp`, after `static const UInt32 kMovementSwim = 0x800;`:

```cpp
static const UInt32	kDetectionLevelCall		= 0x005F68DB; // the only call to Calc_DetectionLevel
static const UInt32	kCalcDetectionLevel		= 0x005463F0;
static const int	kDetectionArgLight		= 5;
static const int	kDetectionArgSneaking	= 9;
```

- [ ] **Step 2: Add the adjuster, stub and installer** at the end of `TreeCover.cpp`:

```cpp
static void __stdcall AdjustDetectionLight(Actor* Target, SInt32* Args) {

	float Cover = PlayerTreeCover;
	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	if (Cover <= 0.0f || !Settings->TreeCover || Target != Player || !(UInt8)Args[kDetectionArgSneaking]) return;
	float Scale = 1.0f - Cover * Settings->TreeCoverLightReduction;
	if (Scale < 0.0f) Scale = 0.0f;
	Args[kDetectionArgLight] = (SInt32)(Args[kDetectionArgLight] * Scale);

}

// Reached by the original call, so [esp] is its return address and the cdecl args follow it.
static __declspec(naked) void DetectionLevelHook() {

	__asm {
		pushad
		lea		eax, [esp + 0x24]
		push	eax
		push	ebp
		call	AdjustDetectionLight
		popad
		jmp		kCalcDetectionLevel
	}

}

void CreateTreeCoverHook() {

	WriteRelCall(kDetectionLevelCall, (UInt32)DetectionLevelHook);

}
```

`0x24` = 0x20 (`pushad`) + 4 (return address) → arg 0. `ebp` is the target Actor* at the call site. `AdjustDetectionLight` runs on whichever thread evaluates detection; it reads only the published float and settings.

- [ ] **Step 3: Install it** — in `OblivionReloaded/Main.cpp` add `#include "TreeCover.h"` after `#include "TreeCollision.h"` (line 17), and add `CreateTreeCoverHook();` on the line after `CreateTreeCollisionHook();` (line 58), same indentation.

- [ ] **Step 4: Build** (Global Constraints command). Expected: 0 errors. If `Target != Player` fails to compile, compare against `(Actor*)Player`.

- [ ] **Step 5: Commit**

```bash
git add TESReloaded/Core/TreeCover.cpp OblivionReloaded/Main.cpp
git commit -m "feat(Trees): Scale sneak detection light by tree cover"
```

---

### Task 5: In-game verification and memory update

**Goal:** Confirm in-game that cover is computed from real tree refs and lowers the player's detection light, then record the verified facts in memory.

**Files:**
- Modify temporarily (never committed): `TESReloaded/Core/TreeCover.cpp`
- Modify: `memory/sneak-detection-formula.md`

**Acceptance Criteria:**
- [ ] Log shows `[TreeCover]` lines with `root + child > 0` while sneaking in a shrub (the `BSTreeNode` lookup works)
- [ ] Logged `light A -> B` has `B < A` whenever cover > 0, and cover is 0 when not inside a small tree
- [ ] Logged cover near the centre of a bent shrub is lower than at mid-radius
- [ ] Temporary logging removed: `git diff --quiet TESReloaded/Core/TreeCover.cpp` exits 0
- [ ] Memory updated and committed as a separate `docs:` commit

**Verify:** `grep "\[TreeCover\]" "C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log"` shows the lines above; `git status --short` clean except the docs commit.

**Steps:**

- [ ] **Step 1: Add temporary logging** to `TreeCover.cpp` (do NOT commit). After `static volatile float PlayerTreeCover = 0.0f;` add:

```cpp
static volatile SInt32 DebugLightBefore = -1, DebugLightAfter = -1;
static int DebugFrame = 0, DebugRoot = 0, DebugChild = 0, DebugTrees = 0;
static float DebugBestCover = 0.0f, DebugBestRing = 0.0f;
```

In `FindRefTreeNode`, change `if (*(void**)Root == VFTBSTreeNode) return Root;` to `if (*(void**)Root == VFTBSTreeNode) { DebugRoot++; return Root; }` and the child return to `{ DebugChild++; return (NiNode*)Child; }`.

In `AccumulateTreeCover`, replace `Exposure *= 1.0f - TreeCoverAt(In);` with:

```cpp
		float Ring = 0.0f;
		float Cover = TreeCoverAt(In, &Ring);
		DebugTrees++;
		if (Cover > DebugBestCover) { DebugBestCover = Cover; DebugBestRing = Ring; }
		Exposure *= 1.0f - Cover;
```

In `UpdateTreeCover`, just before `float Exposure = 1.0f;` add `DebugRoot = DebugChild = DebugTrees = 0; DebugBestCover = DebugBestRing = 0.0f;` and after `PlayerTreeCover = 1.0f - Exposure;` add:

```cpp
	if (++DebugFrame % 60 == 0)
		Logger::Log("[TreeCover] cover %.3f trees %d root %d child %d best %.3f ring %.3f light %d -> %d", PlayerTreeCover, DebugTrees, DebugRoot, DebugChild, DebugBestCover, DebugBestRing, DebugLightBefore, DebugLightAfter);
```

In `AdjustDetectionLight`, replace the last line with:

```cpp
	DebugLightBefore = Args[kDetectionArgLight];
	Args[kDetectionArgLight] = (SInt32)(Args[kDetectionArgLight] * Scale);
	DebugLightAfter = Args[kDetectionArgLight];
```

- [ ] **Step 2: Build** (Global Constraints command; game closed). Expected: 0 errors.

- [ ] **Step 3: Ask the user to play-test** — exterior, near NPCs so detection runs, with `TreeCover = 1`: (a) stand outside any shrub while sneaking, (b) sneak to mid-radius of a small shrub, (c) sneak to its dead centre, (d) stand up (not sneaking). Hold each ~5 s. Then read `C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log` and check the acceptance criteria. If `root` and `child` are both 0 for trees in range, stop and investigate the tree node hierarchy (systematic-debugging) before continuing.

- [ ] **Step 4: Remove the logging**: `git checkout -- TESReloaded/Core/TreeCover.cpp`, rebuild, confirm 0 errors and `git diff --quiet TESReloaded/Core/TreeCover.cpp`.

- [ ] **Step 5: Update memory** — in `memory/sneak-detection-formula.md` replace the line `Not yet capture-verified.` with a note of which of root/child held the `BSTreeNode` (from Step 3), and append:

```markdown
**Verified 2026-09-24 (tree cover feature, `TreeCover.cpp`):** arg 5 (light) is an INT 0–100 — `ftol`
of process vfunc 0x3AC, night-eye scaled and clamped at 0x5F6611–0x5F6629, stored at frame
`[esp+0x24]`. Arg 9 is the dword result of bool `sub_5F3B50` (movement flags `& 0x400`, not `& 0x800`;
low byte only). Process vfunc 0x2C0 = `GetMovementFlags` (matches Game.h). The hook at 0x5F68DB is
installed: `pushad`, `lea eax,[esp+0x24]` = arg 0, `ebp` = target, then `jmp 0x5463F0`.
```

- [ ] **Step 6: Commit memory separately**

```bash
git add memory/sneak-detection-formula.md
git commit -m "docs: Record verified detection arg types for tree cover"
```
