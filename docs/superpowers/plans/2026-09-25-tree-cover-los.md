# Tree Cover Line-of-Sight Blocking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the sneaking player is inside a small SpeedTree shrub, an observer whose eye→torso sight line crosses enough of that shrub's foliage loses line of sight for sneak detection.

**Architecture:** `TreeCoverMath.h` gains a shared ellipsoid helper and a pure `TreeCoverRayDepth()` that integrates the existing coverage field along a segment. `UpdateTreeCover()` (main thread, per frame) publishes the shrubs containing the player's torso in a seqlock-guarded snapshot. A `__fastcall` wrapper patched over the detection call to the engine LOS test (0x5F6647) calls the original and, for the player target, returns "no LOS" when the summed foliage depth reaches a threshold.

**Tech Stack:** C++ (MSVC v145, x86), OBSE plugin, `WriteRelCall` memory patching, `ThisCall` helper (`TESReloaded/Framework/Types.h`).

**Spec:** `docs/superpowers/specs/2026-09-25-tree-cover-los-design.md`

## Global Constraints

- Oblivion only. All code lives in the existing `TESReloaded/Core/TreeCover.h/.cpp` and `TreeCoverMath.h`; no new source files (no vcxproj or `compile_commands.json` changes).
- Coding style (AGENTS.md): public symbols get `///` doc comments; code is self-documenting; inline comments only when necessary and at most 1–3 lines.
- Build ONLY through the PowerShell tool (Bash's TEMP breaks MSBuild with a fake MSB3073):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  The post-build step copies the DLL into the game's Plugins folder; the game must not be running.
- Verified engine facts (do not re-derive): the detection LOS call is `call 0x5F2820` at **0x5F6647**; `sub_5F2820` is thiscall (`ecx` = observer Actor*), 5 dword stack args `(1, target, 1, &reason, 0)`, returns a bool in `al`, callee cleans (`ret 0x14`). LOS = 0 zeroes the visual term of `Calc_DetectionLevel` and scales its sound term by `fSneakSoundLosMult`. `TESObjectREFR::pos` +0x2C, `scale` +0x38.
- Constants: eye height `kTreeCoverEyeHeight = 110` × observer `scale`; torso = `Player->pos.z + 50` (existing `kTreeCoverTorsoHeight`); `kTreeCoverRaySamples = 16`; `kTreeCoverMaxShrubs = 8`.
- Settings in `SettingsGrass` / `Shaders\Grass\Grass.ini` `[Default]`: `TreeCoverBlockLOS` (bool via `GetPrivateProfileIntA`, code default 0), `TreeCoverLOSDepth` (float, code default 64.0). Shipped INI: `TreeCoverBlockLOS = 1`, `TreeCoverLOSDepth = 64`.
- The hook may run on the threaded-AI thread: it reads only the snapshot, `Player`, the observer's `pos`/`scale` and settings — never cells or the scene graph.
- Commits: code as `feat(Trees): …` / `fix(Trees): …`; spec/plan/memory changes only in separate `docs:` commits.

**User decisions (already made):**
- Mechanism: wrap the detection call to the engine LOS test; no invisible Havok geometry.
- Directional rule: integrate foliage density along observer-eye → player-torso; block at a depth threshold (world units).
- Only shrubs the player is inside can block; hiding behind a bush is out of scope.
- Light reduction stays unchanged and still applies to observers whose LOS isn't blocked.

---

## File Structure

| File | Responsibility |
|---|---|
| `TESReloaded/Core/TreeCoverMath.h` (modify) | Shared ellipsoid shape, `TreeCoverAt`, new `TreeCoverRayDepth` — no engine types |
| `TESReloaded/Core/TreeCover.cpp` (modify) | Snapshot publishing in the per-frame scan; LOS call-site wrapper; install patch |
| `TESReloaded/Core/TreeCover.h` (modify) | Doc comments updated for the new behaviour |
| `TESReloaded/Core/SettingManager.h/.cpp` (modify) | Two new `SettingsGrass` settings |
| `OblivionReloaded/Shaders/Grass/Grass.ini` (modify) | Ship values |

---

### Task 1: LOS settings

**Goal:** Add `TreeCoverBlockLOS` and `TreeCoverLOSDepth` to `SettingsGrass`, wired through load, save, menu map and set, with ship values in `Grass.ini`.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h:433` (after `TreeCoverLightReduction`)
- Modify: `TESReloaded/Core/SettingManager.cpp:604`, `:1575`, `:2246`, `:2988` (after each `TreeCoverLightReduction` line)
- Modify: `OblivionReloaded/Shaders/Grass/Grass.ini:23`

**Acceptance Criteria:**
- [ ] `grep -c "TreeCoverBlockLOS\|TreeCoverLOSDepth" TESReloaded/Core/SettingManager.cpp` prints 11 (load 3 lines, save 2, map 2, set 4)
- [ ] Both fields have `///<` doc comments in `SettingsGrassStruct`
- [ ] `Grass.ini` `[Default]` contains `TreeCoverBlockLOS = 1` and `TreeCoverLOSDepth = 64`
- [ ] Release build succeeds

**Verify:** the grep above → `11`; MSBuild command (Global Constraints) → `Build succeeded`, 0 errors.

**Steps:**

- [ ] **Step 1: Add the fields** — in `TESReloaded/Core/SettingManager.h`, after `float TreeCoverLightReduction;` (line 433):

```cpp
	bool TreeCoverBlockLOS;				///< Shrubs the sneaking player is inside block observers' line of sight
	float TreeCoverLOSDepth;			///< Effective foliage depth, world units, along a sight line that blocks it
```

- [ ] **Step 2: Load** — in `SettingManager.cpp` after `SettingsGrass.TreeCoverLightReduction = atof(value);` (line 604):

```cpp
	SettingsGrass.TreeCoverBlockLOS = GetPrivateProfileIntA("Default", "TreeCoverBlockLOS", 0, Filename);
	GetPrivateProfileStringA("Default", "TreeCoverLOSDepth", "64.0", value, SettingStringBuffer, Filename);
	SettingsGrass.TreeCoverLOSDepth = atof(value);
```

- [ ] **Step 3: Save** — after the `WritePrivateProfileStringA("Default", "TreeCoverLightReduction", …)` line (~1575):

```cpp
			WritePrivateProfileStringA("Default", "TreeCoverBlockLOS", ToString(SettingsGrass.TreeCoverBlockLOS).c_str(), Filename);
			WritePrivateProfileStringA("Default", "TreeCoverLOSDepth", ToString(SettingsGrass.TreeCoverLOSDepth).c_str(), Filename);
```

- [ ] **Step 4: Menu map** — after `Settings["TreeCoverLightReduction"] = SettingsGrass.TreeCoverLightReduction;` (~2246):

```cpp
			Settings["TreeCoverBlockLOS"] = SettingsGrass.TreeCoverBlockLOS;
			Settings["TreeCoverLOSDepth"] = SettingsGrass.TreeCoverLOSDepth;
```

- [ ] **Step 5: Set path** — after the `TreeCoverLightReduction` branch (~2988):

```cpp
			else if (!strcmp(Setting, "TreeCoverBlockLOS"))
				SettingsGrass.TreeCoverBlockLOS = Value;
			else if (!strcmp(Setting, "TreeCoverLOSDepth"))
				SettingsGrass.TreeCoverLOSDepth = Value;
```

- [ ] **Step 6: Ship values** — in `OblivionReloaded/Shaders/Grass/Grass.ini`, after `TreeCoverLightReduction = 0.75`:

```ini
TreeCoverBlockLOS = 1
TreeCoverLOSDepth = 64
```

- [ ] **Step 7: Verify** — run the grep (expect `11`) and the Release build (PowerShell tool). Expected: `Build succeeded`.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp OblivionReloaded/Shaders/Grass/Grass.ini
git commit -m "feat(Trees): Add tree cover line-of-sight settings"
```

---

### Task 2: Foliage ray depth math

**Goal:** Factor the ellipsoid out of `TreeCoverAt` into `TreeCoverShapeOf`/`TreeCoverShapeAt` (behaviour unchanged) and add `TreeCoverRayDepth()`, proven by a standalone harness.

**Files:**
- Modify: `TESReloaded/Core/TreeCoverMath.h`
- Test: `%TEMP%\treecover-los-check\TreeCoverLosCheck.cpp` (harness, NOT committed)

**Acceptance Criteria:**
- [ ] Harness prints 13 `ok` lines then `ALL PASS`, exit code 0, compiled with `/W4` and no warnings from `TreeCoverMath.h`
- [ ] `TreeCoverAt` regression values unchanged (centre 1, half-radius 0.5, half-height 0.5, low 1, ring 1, bent centre 0)
- [ ] Ray depths: side 40, miss 0, bent vertical 0, flat vertical 70, inside 26.57, bent side-to-ring 6 (±0.05)
- [ ] Header contains no engine types and every new public symbol has a `///` comment

**Verify:** harness command in Step 2 → 13 `ok` lines, `ALL PASS`, `$LASTEXITCODE` 0.

**Steps:**

- [ ] **Step 1: Write the harness** at `%TEMP%\treecover-los-check\TreeCoverLosCheck.cpp` (create the directory). Reference shrub: base z 0, bound centre (0,0,60), R 100 → top 160, ellipsoid centre z 80, rᵥ 80, rₕ = √(100²−60²) = 80. Push 0 → ring 0; push 40 → ring 40·0.785/80 = 0.3925; push 120 → ring clamped to 0.85. Expected depths were computed analytically (∫(1−smoothstep)=½ per unit) and cross-checked numerically.

```cpp
#include <cstdio>
#include <cmath>
#include "TreeCoverMath.h"

static int Failures = 0;

static void Check(const char* Name, float Got, float Want, float Tolerance) {
	bool Ok = fabsf(Got - Want) <= Tolerance;
	if (!Ok) Failures++;
	printf("%-28s got %9.4f want %9.4f  %s\n", Name, Got, Want, Ok ? "ok" : "FAIL");
}

static TreeCoverInput Shrub(float Push) {
	TreeCoverInput In = {};
	In.BaseZ = 0.0f;
	In.CX = 0.0f; In.CY = 0.0f; In.CZ = 60.0f;
	In.R = 100.0f;
	In.PushStrength = Push;
	return In;
}

static float At(TreeCoverInput In, float X, float Y, float Z) {
	In.QX = X; In.QY = Y; In.QZ = Z;
	return TreeCoverAt(In);
}

int main() {
	TreeCoverInput Flat = Shrub(0.0f);
	TreeCoverInput Mid = Shrub(40.0f);
	TreeCoverInput Bent = Shrub(120.0f);

	Check("at centre", At(Flat, 0, 0, 80), 1.0f, 1e-4f);
	Check("at half radius", At(Flat, 40, 0, 80), 0.5f, 1e-4f);
	Check("at half height", At(Flat, 0, 0, 120), 0.5f, 1e-4f);
	Check("at low", At(Flat, 0, 0, 20), 1.0f, 1e-4f);
	Check("at ring", At(Mid, 31.4f, 0, 80), 1.0f, 1e-3f);
	Check("at bent centre", At(Mid, 0, 0, 80), 0.0f, 1e-4f);

	Check("ray side", TreeCoverRayDepth(Flat, -1000, 0, 50, 0, 0, 50), 40.0f, 0.05f);
	Check("ray miss", TreeCoverRayDepth(Flat, -1000, 200, 50, 1000, 200, 50), 0.0f, 1e-4f);
	Check("ray bent vertical", TreeCoverRayDepth(Bent, 0, 0, 1000, 0, 0, 50), 0.0f, 1e-3f);
	Check("ray flat vertical", TreeCoverRayDepth(Flat, 0, 0, 1000, 0, 0, 50), 70.0f, 0.05f);
	Check("ray inside", TreeCoverRayDepth(Flat, 30, 0, 50, 0, 0, 50), 26.575f, 0.05f);
	Check("ray bent side to ring", TreeCoverRayDepth(Bent, -1000, 0, 50, -68, 0, 50), 6.0f, 0.05f);

	TreeCoverInput Empty = Shrub(0.0f);
	Empty.R = 0.0f;
	Check("ray degenerate", TreeCoverRayDepth(Empty, -1000, 0, 50, 0, 0, 50), 0.0f, 0.0f);

	printf(Failures ? "%d FAILED\n" : "ALL PASS\n", Failures);
	return Failures ? 1 : 0;
}
```

- [ ] **Step 2: Run it to verify it fails** (PowerShell tool):

```powershell
$vc = 'C:\Development\Microsoft\Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
$inc = 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded\Core'
$dir = "$env:TEMP\treecover-los-check"
cmd /c "call `"$vc`" >nul && cd /d `"$dir`" && cl /nologo /EHsc /W4 /I `"$inc`" TreeCoverLosCheck.cpp && TreeCoverLosCheck.exe"
```

Expected: compile error `'TreeCoverRayDepth': identifier not found`.

- [ ] **Step 3: Rewrite `TESReloaded/Core/TreeCoverMath.h`** — keep `TreeCoverInput`, the three constants, `TreeCoverSaturate` and `TreeCoverSmoothstep` exactly as they are; replace the `TreeCoverAt` definition (from its `///` comment to the end of the file) with:

```cpp
/// Samples TreeCoverRayDepth takes along the part of a segment inside a tree's ellipsoid bounds.
static const int kTreeCoverRaySamples = 16;

/// One tree's concealment ellipsoid and ring, derived from a TreeCoverInput.
struct TreeCoverShape {
	float CX, CY;			///< Horizontal centre (bound centre)
	float BaseZ;			///< Ground z (tree ref position)
	float TopZ;				///< Bound top z
	float CentreZ;			///< Ellipsoid centre z, midway from ground to top
	float VerticalAxis;		///< Vertical semi-axis
	float HorizontalAxis;	///< Horizontal semi-axis
	float Ring;				///< Normalised ring radius of best cover; 0 when the tree isn't bending
};

/// Builds the ellipsoid running from the ground to the bound top, and the ring the live push opens
/// (the shader's full-bend push over the horizontal semi-axis). Returns false when the tree has no volume.
inline bool TreeCoverShapeOf(const TreeCoverInput& In, TreeCoverShape& Out) {
	float Top = In.CZ + In.R;
	float Height = Top - In.BaseZ;
	if (In.R <= 0.0f || Height <= 0.0f) return false;

	float Rise = In.CZ - In.BaseZ;
	float MinAxis = 0.25f * In.R;
	float HorizontalSq = In.R * In.R - Rise * Rise;
	Out.CX = In.CX;
	Out.CY = In.CY;
	Out.BaseZ = In.BaseZ;
	Out.TopZ = Top;
	Out.CentreZ = (In.BaseZ + Top) * 0.5f;
	Out.VerticalAxis = Height * 0.5f;
	Out.HorizontalAxis = sqrtf(HorizontalSq > MinAxis * MinAxis ? HorizontalSq : MinAxis * MinAxis);
	Out.Ring = TreeCoverSaturate(In.PushStrength * kTreeCoverPushPeak / Out.HorizontalAxis);
	if (Out.Ring > kTreeCoverMaxRing) Out.Ring = kTreeCoverMaxRing;
	return true;
}

/// Returns how concealed the point (X, Y, Z) is by one tree's shape, in [0, 1]. Cover peaks on the ring
/// (the centre when Ring is 0) and falls to 0 at the ellipsoid surface; the lower half counts as fully
/// inside vertically. Callers keep Z at or above the ground.
inline float TreeCoverShapeAt(const TreeCoverShape& Shape, float X, float Y, float Z) {
	float DX = X - Shape.CX;
	float DY = Y - Shape.CY;
	float U = sqrtf(DX * DX + DY * DY) / Shape.HorizontalAxis;
	float Above = Z - Shape.CentreZ;
	float V = (Above > 0.0f ? Above : 0.0f) / Shape.VerticalAxis;
	float Ring = Shape.Ring;
	float Radial = (Ring < kTreeCoverMinRing || U >= Ring) ? (U - Ring) / (1.0f - Ring) : (Ring - U) / Ring;
	return 1.0f - TreeCoverSmoothstep(sqrtf(Radial * Radial + V * V));
}

/// Returns how concealed the player point is by one tree, in [0, 1].
/// Cover peaks at the centre of an ellipsoid running from the ground to the bound top. As the tree bends,
/// the peak moves out to a ring of normalised radius Ring (the shader's full-bend push over the horizontal
/// semi-axis) and the centre is exposed. The ellipsoid's lower half counts as fully inside vertically.
/// OutRing, when given, receives that ring radius.
inline float TreeCoverAt(const TreeCoverInput& In, float* OutRing = 0) {
	if (OutRing) *OutRing = 0.0f;
	TreeCoverShape Shape;
	if (!TreeCoverShapeOf(In, Shape)) return 0.0f;
	if (OutRing) *OutRing = Shape.Ring;
	return TreeCoverShapeAt(Shape, In.QX, In.QY, In.QZ);
}

/// Narrows [T0, T1] to where Start + t·Delta lies within [Lo, Hi] on one axis. Returns false when empty.
inline bool TreeCoverClipSlab(float Start, float Delta, float Lo, float Hi, float& T0, float& T1) {
	if (fabsf(Delta) < 1e-6f) return Start >= Lo && Start <= Hi;
	float TA = (Lo - Start) / Delta;
	float TB = (Hi - Start) / Delta;
	if (TA > TB) { float Swap = TA; TA = TB; TB = Swap; }
	if (TA > T0) T0 = TA;
	if (TB < T1) T1 = TB;
	return T0 < T1;
}

/// Returns the effective foliage depth, in world units, that one tree puts on the segment A → B: its
/// coverage integrated along the part of the segment inside the ellipsoid's bounding box. The Q fields of
/// Tree are ignored.
inline float TreeCoverRayDepth(const TreeCoverInput& Tree, float AX, float AY, float AZ, float BX, float BY, float BZ) {
	TreeCoverShape Shape;
	if (!TreeCoverShapeOf(Tree, Shape)) return 0.0f;

	float DX = BX - AX;
	float DY = BY - AY;
	float DZ = BZ - AZ;
	float T0 = 0.0f;
	float T1 = 1.0f;
	float Axis = Shape.HorizontalAxis;
	if (!TreeCoverClipSlab(AX, DX, Shape.CX - Axis, Shape.CX + Axis, T0, T1) ||
		!TreeCoverClipSlab(AY, DY, Shape.CY - Axis, Shape.CY + Axis, T0, T1) ||
		!TreeCoverClipSlab(AZ, DZ, Shape.BaseZ, Shape.TopZ, T0, T1)) return 0.0f;

	float Step = (T1 - T0) / kTreeCoverRaySamples;
	float Sum = 0.0f;
	for (int i = 0; i < kTreeCoverRaySamples; i++) {
		float T = T0 + (i + 0.5f) * Step;
		Sum += TreeCoverShapeAt(Shape, AX + DX * T, AY + DY * T, AZ + DZ * T);
	}
	return Sum * Step * sqrtf(DX * DX + DY * DY + DZ * DZ);
}
```

- [ ] **Step 4: Run the harness to verify it passes** — same command as Step 2. Expected: 13 lines ending `ok`, then `ALL PASS`; `$LASTEXITCODE` 0. If `/W4` warns, fix the header (not the harness).

- [ ] **Step 5: Release build** (PowerShell tool, MSBuild command from Global Constraints). Expected: `Build succeeded` — `TreeCover.cpp` still compiles against the refactored header.

- [ ] **Step 6: Commit** (the harness stays in `%TEMP%`, uncommitted)

```bash
git add TESReloaded/Core/TreeCoverMath.h
git commit -m "feat(Trees): Add foliage ray depth to tree cover math"
```

---

### Task 3: Shrub snapshot and LOS hook

**Goal:** Publish the shrubs containing the sneaking player's torso each frame in a seqlock-guarded snapshot, and patch the detection LOS call (0x5F6647) with a wrapper that returns "no LOS" for the player when the summed foliage depth reaches `TreeCoverLOSDepth`.

**Files:**
- Modify: `TESReloaded/Core/TreeCover.cpp`
- Modify: `TESReloaded/Core/TreeCover.h`

**Acceptance Criteria:**
- [ ] `UpdateTreeCover()` publishes the snapshot on every path, including all early-outs (empty snapshot)
- [ ] `CreateTreeCoverHook()` writes `WriteRelCall(0x005F6647, …)` in addition to the existing 0x5F68DB patch
- [ ] The wrapper always calls the original `0x005F2820` with the caller's five arguments and returns its result unchanged unless: result true, `TreeCoverBlockLOS` on, target is `Player`, snapshot read succeeds and is non-empty
- [ ] The wrapper touches only the snapshot, `Player`, `Observer->pos`/`scale` and settings
- [ ] Release build succeeds with no new warnings in `TreeCover.cpp`

**Verify:** MSBuild command (Global Constraints) → `Build succeeded`; `grep -n "0x005F6647\|0x005F2820" TESReloaded/Core/TreeCover.cpp` shows both constants.

**Steps:**

- [ ] **Step 1: Constants and snapshot** — in `TreeCover.cpp`, after the existing `kDetectionArgSneaking` constant, add:

```cpp
static const UInt32	kDetectionLosCall		= 0x005F6647; // the detection call to the actor LOS test
static const UInt32	kActorHasLineOfSight	= 0x005F2820;
static const float	kTreeCoverEyeHeight		= 110.0f;
static const int	kTreeCoverMaxShrubs		= 8;
static const int	kSnapshotReadTries		= 4;

/// The shrubs containing the sneaking player's torso this frame, for the line-of-sight hook.
struct TreeCoverSnapshot {
	int				Count;
	float			TorsoX, TorsoY, TorsoZ;
	TreeCoverInput	Shrubs[kTreeCoverMaxShrubs];
};

static volatile LONG		SnapshotSequence = 0;
static TreeCoverSnapshot	Snapshot = {};
```

- [ ] **Step 2: Seqlock helpers** — after the declarations above:

```cpp
// Seqlock: odd while writing. Detection may run on the threaded-AI thread.
static void PublishSnapshot(const TreeCoverSnapshot& Next) {

	InterlockedIncrement(&SnapshotSequence);
	Snapshot = Next;
	InterlockedIncrement(&SnapshotSequence);

}

static bool ReadSnapshot(TreeCoverSnapshot& Out) {

	for (int Try = 0; Try < kSnapshotReadTries; Try++) {
		LONG Before = SnapshotSequence;
		_ReadWriteBarrier();
		if (Before & 1) {
			YieldProcessor();
			continue;
		}
		Out = Snapshot;
		_ReadWriteBarrier();
		if (SnapshotSequence == Before) return true;
	}
	return false;

}
```

- [ ] **Step 3: Record shrubs in the scan** — change `AccumulateTreeCover` to take the snapshot and record each shrub with non-zero cover. New signature and loop tail:

```cpp
static void AccumulateTreeCover(TList<TESObjectREFR>::Entry* Entry, TreeCoverInput& In, float MaxBound, float& Exposure, TreeCoverSnapshot& Shrubs) {
```

Replace the line `Exposure *= 1.0f - TreeCoverAt(In);` with:

```cpp
		float Cover = TreeCoverAt(In);
		Exposure *= 1.0f - Cover;
		if (Cover > 0.0f && Shrubs.Count < kTreeCoverMaxShrubs) Shrubs.Shrubs[Shrubs.Count++] = In;
```

- [ ] **Step 4: Publish from `UpdateTreeCover()`** — rename the existing `UpdateTreeCover()` body into `static float ScanTreeCover(TreeCoverSnapshot& Shrubs)` that returns the cover instead of storing it (each `PlayerTreeCover = 0.0f; return;` becomes `return 0.0f;`, and the end becomes `return 1.0f - Exposure;`). After `In.QZ` is set, record the torso:

```cpp
	Shrubs.TorsoX = In.QX;
	Shrubs.TorsoY = In.QY;
	Shrubs.TorsoZ = In.QZ;
```

Pass `Shrubs` as the new last argument to both `AccumulateTreeCover` calls. Then add the new public function:

```cpp
void UpdateTreeCover() {

	TreeCoverSnapshot Shrubs = {};
	PlayerTreeCover = ScanTreeCover(Shrubs);
	PublishSnapshot(Shrubs);

}
```

- [ ] **Step 5: The wrapper** — after `AdjustDetectionLight` (before `DetectionLevelHook`):

```cpp
// Replaces the detection call to the actor LOS test; thiscall with callee cleanup, so __fastcall fits.
static bool __fastcall DetectionLineOfSightHook(Actor* Observer, void* Edx, UInt32 Arg1, TESObjectREFR* Target, UInt32 Arg3, UInt32* Reason, UInt32 Arg5) {

	bool Visible = (UInt8)ThisCall(kActorHasLineOfSight, Observer, Arg1, Target, Arg3, Reason, Arg5);
	if (!Visible || Target != Player || !TheSettingManager->SettingsGrass.TreeCoverBlockLOS) return Visible;

	TreeCoverSnapshot Shrubs;
	if (!ReadSnapshot(Shrubs) || !Shrubs.Count) return Visible;

	float EyeX = Observer->pos.x;
	float EyeY = Observer->pos.y;
	float EyeZ = Observer->pos.z + kTreeCoverEyeHeight * Observer->scale;
	float Depth = 0.0f;
	for (int i = 0; i < Shrubs.Count; i++)
		Depth += TreeCoverRayDepth(Shrubs.Shrubs[i], EyeX, EyeY, EyeZ, Shrubs.TorsoX, Shrubs.TorsoY, Shrubs.TorsoZ);
	return Depth < TheSettingManager->SettingsGrass.TreeCoverLOSDepth;

}
```

If `Target != Player` does not compile (pointer types unrelated in the OBLIVION block), use `Target != (TESObjectREFR*)Player`.

- [ ] **Step 6: Install** — in `CreateTreeCoverHook()` add after the existing `WriteRelCall`:

```cpp
	WriteRelCall(kDetectionLosCall, (UInt32)DetectionLineOfSightHook);
```

- [ ] **Step 7: Header docs** — in `TreeCover.h`, replace the file comment and the two function comments with:

```cpp
/// Tree cover for sneak detection: while the player sneaks inside small SpeedTree trees and shrubs,
/// the light level fed to Calc_DetectionLevel is scaled down, and observers whose sight line crosses
/// enough of those shrubs' foliage lose line of sight. Oblivion only.

/// Patches the only call to Calc_DetectionLevel and the detection call to the actor LOS test. Always
/// installed; gated at runtime on SettingsGrass.TreeCover and TreeCoverBlockLOS.
void CreateTreeCoverHook();

/// Rescans nearby small trees and publishes the player's cover and the shrubs containing them for the
/// detection hooks. Main thread, once per frame.
void UpdateTreeCover();
```

- [ ] **Step 8: Build** (PowerShell tool, MSBuild command). Expected: `Build succeeded`, no warnings from `TreeCover.cpp`.

- [ ] **Step 9: Commit**

```bash
git add TESReloaded/Core/TreeCover.cpp TESReloaded/Core/TreeCover.h
git commit -m "feat(Trees): Block detection line of sight through shrubs the player is in"
```

---

### Task 4: Play-test and tune LOS depth

**Goal:** With a temporary ~1 Hz log, have the user confirm in game that shrubs block side sight lines but not the parted centre or an observer inside the bush, and tune the `TreeCoverLOSDepth` default from the logged depths.

**Files:**
- Modify (temporarily, then revert): `TESReloaded/Core/TreeCover.cpp`
- Modify (if retuned): `TESReloaded/Core/SettingManager.cpp` (code default), `OblivionReloaded/Shaders/Grass/Grass.ini`

**Acceptance Criteria:**
- [ ] Log lines `TreeCoverLOS: observer %08X depth %.1f blocked %d` appear in `OblivionReloaded.log` while sneaking in a shrub near an NPC
- [ ] User reports: sneak eye stays closed with an NPC looking through the bush from the side; eye can open when the NPC looks down into the parted centre or stands in the bush
- [ ] The temporary log is removed; `git diff` of `TreeCover.cpp` against Task 3's commit is empty before the tuning commit
- [ ] If the default changed, code default and `Grass.ini` agree

**Verify:** `grep -c "TreeCoverLOS:" TESReloaded/Core/TreeCover.cpp` → `0` after cleanup; MSBuild → `Build succeeded`.

**Steps:**

- [ ] **Step 1: Add the temporary log** — in `DetectionLineOfSightHook`, replace the final `return` with:

```cpp
	bool Blocked = Depth >= TheSettingManager->SettingsGrass.TreeCoverLOSDepth;
	static DWORD LastLog = 0;
	DWORD Now = GetTickCount();
	if (Now - LastLog > 1000) {
		LastLog = Now;
		Logger::Log("TreeCoverLOS: observer %08X depth %.1f blocked %d", Observer->refID, Depth, Blocked);
	}
	return !Blocked;
```


- [ ] **Step 2: Build** (PowerShell tool). Expected: `Build succeeded`.

- [ ] **Step 3: Hand to the user** — ask them to sneak into a shrub near an NPC and try: NPC looking from the side, NPC above/looking down into the bent centre, NPC walking into the bush. Ask them to report the sneak eye behaviour and paste the `TreeCoverLOS:` lines from `C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log`. Wait for their report.

- [ ] **Step 4: Tune** — pick a `TreeCoverLOSDepth` that separates the logged "should block" depths from the "should see" ones. If it differs from 64, update the code default in `SettingManager.cpp` (`"64.0"` string) and `Grass.ini`.

- [ ] **Step 5: Remove the log** — restore the `return Depth < TheSettingManager->SettingsGrass.TreeCoverLOSDepth;` line. Run `git diff TESReloaded/Core/TreeCover.cpp` → empty. Build → `Build succeeded`.

- [ ] **Step 6: Commit** (only if the default changed)

```bash
git add TESReloaded/Core/SettingManager.cpp OblivionReloaded/Shaders/Grass/Grass.ini
git commit -m "fix(Trees): Tune tree cover line-of-sight depth after play-test"
```

---

### Task 5: Record play-test outcome in docs

**Goal:** Mark the spec implemented with the tuned depth and add the play-test result to memory `sneak-detection-formula`, in a `docs:` commit.

**Files:**
- Modify: `docs/superpowers/specs/2026-09-25-tree-cover-los-design.md` (Status line; Settings default if tuned)
- Modify: `memory/sneak-detection-formula.md` (one line in the LOS paragraph: hook live-tested, tuned depth)

**Acceptance Criteria:**
- [ ] Spec `Status:` reads `Implemented on feat/misc-3; play-tested <date>`
- [ ] Spec settings table default for `TreeCoverLOSDepth` matches the code default
- [ ] Memory LOS paragraph states the wrapper at 0x5F6647 is implemented in `TreeCover.cpp` and live-tested
- [ ] Commit touches only `docs/` and `memory/`

**Verify:** `git show --stat HEAD` lists only `docs/superpowers/specs/2026-09-25-tree-cover-los-design.md` and `memory/sneak-detection-formula.md`.

**Steps:**

- [ ] **Step 1: Update the spec** — set `Status: Implemented on feat/misc-3; play-tested YYYY-MM-DD` (today's date) and, if Task 4 retuned it, the `TreeCoverLOSDepth` default in the Settings table.

- [ ] **Step 2: Update memory** — append to the `**LOS (verified 2026-09-25 …)**` paragraph in `memory/sneak-detection-formula.md`: `Wrapped in TreeCover.cpp (DetectionLineOfSightHook, __fastcall + ThisCall to the original); live-tested YYYY-MM-DD, TreeCoverLOSDepth = N.`

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-09-25-tree-cover-los-design.md memory/sneak-detection-formula.md
git commit -m "docs: Record tree cover line-of-sight play-test"
```
