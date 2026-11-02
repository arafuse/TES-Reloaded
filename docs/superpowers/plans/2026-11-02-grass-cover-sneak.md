# Grass Cover for Sneaking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While the sneaking player is in grass tall enough to hide a crouched figure, the detection light level is scaled down and observers whose sight line crosses enough tall grass lose line of sight, alongside the existing tree cover.

**Architecture:** A main-thread per-frame scan (`GrassCover.cpp`) walks the engine's grass map (`0xB2CBC4`), turns nearby tall instances into clumps (mirroring the player's grass collision push and flattening), computes a 16-sector surround cover and publishes the clumps in a seqlocked snapshot. All math lives in an engine-free header (`GrassCoverMath.h`), proven by a standalone harness. The two existing detection patches in `TreeCover.cpp` combine tree and grass cover: light scale = product of each feature's clamped reduction; line of sight is blocked when the tree and grass depth ratios sum to ≥ 1.

**Tech Stack:** C++ (MSVC v145, x86), OBSE plugin, direct engine-memory reads, `WriteRelCall` patches already installed by `CreateTreeCoverHook`.

**Spec:** `docs/superpowers/specs/2026-11-02-grass-cover-sneak-design.md`

## Global Constraints

- Oblivion only. Coding style (AGENTS.md): public symbols get `///` doc comments; code is self-documenting; inline comments only when necessary and at most 1–3 lines.
- Build ONLY through the PowerShell tool (Bash's TEMP breaks MSBuild with a fake MSB3073):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  Expected tail: `OblivionReloaded.vcxproj -> ...\OblivionReloaded\Release\OblivionReloaded.dll` and no ` error ` lines. The post-build step copies the DLL to the game's Plugins folder; the game must not be running.
- Verified engine facts (confirmed in-game 2026-11-02, memory `grass-manager-generation`; do not re-derive): grass NiTMap at `0xB2CBC4` — `+0x04` UInt32 bucket count, `+0x08` bucket table; item = {next `+0`, GRAS formID `+4`, type `+8`}; type `+0x24` = batch list head, node = {next `+0`, prev `+4`, batch `+8`}. Batch: `+0x00` owning `NiGeometry*`, `+0x0C` UInt16 capacity, `+0x0E` UInt16 used, `+0x10` `float*` instances (4 floats each: world x, y, z, w). Height factor `F = 0.01·w·ScaleZ + MinHeight` (Grass.ini `ScaleZ`, `MinHeight`). Batches are freed/reallocated as tiles churn: NEVER keep a batch pointer across frames. Groundcover = model height ≤ `GroundcoverMaxHeight`.
- Torso point = `Player->pos.z + 50` (same as tree cover's `kTreeCoverTorsoHeight`). Sneak = movement flags `& 0x400`, swim = `& 0x800`.
- Threading: the scan runs on the main thread only (it owns grass generation). The detection hooks may run on the threaded-AI thread: they read only the snapshot, `Player`, the observer's `pos`/`scale` and settings.
- `OblivionReloaded/Shaders/Grass/Grass.ini` carries the user's uncommitted edit `CollisionTrailSlots = 8`. Never commit that line, and never revert it. Stage Grass.ini only through the hunk filter in Task 1, Step 6.
- Commits: code as `feat(Grass): …` / `refactor(Trees): …` / `fix(Grass): …`; plan/spec/memory changes only in separate `docs:` commits.

**User decisions (already made):**
- Grass cover extends tree cover through the same two detection hooks; no new patches.
- Only grass rising well above a crouched figure counts: cover begins at a clump top 90 above the player's feet and is full at 120 (the 128-unit grass hides well; the 73–83-unit grass gives a little at most; short grass and groundcover give none).
- Cover strength comes from how fully tall clumps surround the player, counting grass just outside their own collision ring.
- Light: `(1 − tree·TreeLR)·(1 − grass·GrassLR)`. LOS: `treeDepth/TreeLOSDepth + grassDepth/GrassLOSDepth ≥ 1`.
- No test harness exists in the repo; math is proven by an uncommitted standalone harness, behaviour in-game with temporary instrumentation removed before commit.

## Review Focus

- **Dense fields overflow the clump list:** more than 96 tall clumps in reach must keep the 96 nearest, not the first 96 found. Pinned by the `keep nearest` harness checks (Task 3).
- **Out-of-range INI values:** a light reduction > 1 or < 0, `GrassCoverOpacity` > 1, or `GrassCoverFullHeight ≤ GrassCoverMinHeight` must keep the light scale in [0, 1] and the weights ≤ 1. Pinned by the `light over-reduction`, `light negative reduction`, `clump opacity clamped` and `clump step edge weight` checks (Task 3).
- **Grass switches off = today's behaviour:** with `GrassCover = 0` and `GrassCoverBlockLOS = 0`, light and LOS must match the tree-only formulas exactly. Pinned by `light tree only` and `sight tree at/below threshold` (Task 3) and the A/B step in Task 7.
- **Grass shader off or stale collision sources:** with `Shaders.Grass` off, the scan must not apply a push or flattening from a stale source list, and must not crash. Pinned by `PlayerCollisionRadius` (Task 5) and the shader-off step in Task 7.
- **Not sneaking / swimming / interior / main menu:** cover 0 and an empty snapshot; LOS untouched; no reads while `Player` is NULL (memory `beginscene-pre-init-window`). Pinned by the gates in `ScanGrassCover` (Task 5) and the gate steps in Task 7.

---

## File Structure

| File | Responsibility |
|---|---|
| `TESReloaded/Core/Seqlock.h` (create) | Header-only single-writer snapshot template, shared by tree and grass cover |
| `TESReloaded/Core/GrassCoverMath.h` (create) | Engine-free math: clump construction, nearest-N, sector cover, sight depth, tree+grass combinators |
| `TESReloaded/Core/GrassCover.h/.cpp` (create) | Per-frame grass map scan, snapshot, cover and sight-depth API |
| `TESReloaded/Core/TreeCover.h/.cpp` (modify) | Use `Seqlock.h`; combine tree and grass in the two detection hooks |
| `TESReloaded/Core/RenderHook.h/.cpp` (modify) | Public cached grass model metrics (height + horizontal half-width) |
| `TESReloaded/Core/SettingManager.h/.cpp` (modify) | Eight `GrassCover*` settings |
| `TESReloaded/Core/ShaderManager.cpp` (modify) | Call `UpdateGrassCover()` each frame |
| `OblivionReloaded/OblivionReloaded.vcxproj(.filters)` (modify) | New files |
| `OblivionReloaded/Shaders/Grass/Grass.ini` (modify) | Ship values |

---

### Task 1: Grass cover settings

**Goal:** Add the eight `GrassCover*` settings to `SettingsGrass`, wired through load, save, menu map and menu set, with ship values in `Grass.ini`.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h:449` (after `TreeCoverLOSDepth`)
- Modify: `TESReloaded/Core/SettingManager.cpp:623`, `:1638`, `:2342`, `:3126` (after each `TreeCoverLOSDepth` site)
- Modify: `OblivionReloaded/Shaders/Grass/Grass.ini` (after `TreeCoverLOSDepth = 64`)

**Acceptance Criteria:**
- [ ] `grep -c "GrassCover" TESReloaded/Core/SettingManager.cpp` prints `46` (load 14, save 8, map 8, set 16)
- [ ] All eight fields have `///<` doc comments in `SettingsGrassStruct`
- [ ] `git diff --cached -- OblivionReloaded/Shaders/Grass/Grass.ini` at commit time shows only the eight `GrassCover` lines, and the working tree still has `CollisionTrailSlots = 8`
- [ ] Release build succeeds

**Verify:** the grep → `46`; MSBuild (Global Constraints) → DLL line, no errors.

**Steps:**

- [ ] **Step 1: Add the fields** — in `TESReloaded/Core/SettingManager.h`, after `float TreeCoverLOSDepth;` (line 449):

```cpp
	bool GrassCover;					///< Scale the sneaking player's detection light level by tall-grass cover
	float GrassCoverLightReduction;		///< Fraction of the light level removed at full grass cover
	float GrassCoverMinHeight;			///< Clump top above the player's feet, world units, where cover begins
	float GrassCoverFullHeight;			///< Clump top above the player's feet where a clump counts fully
	float GrassCoverRadius;				///< World units around the player scanned for grass clumps
	float GrassCoverOpacity;			///< How much one full-height clump blocks, 0-1; overlapping clumps compound
	bool GrassCoverBlockLOS;			///< Tall grass around the sneaking player blocks observers' line of sight
	float GrassCoverLOSDepth;			///< Effective grass depth, world units, along a sight line that blocks it
```

- [ ] **Step 2: Load** — in `SettingManager.cpp`, after `SettingsGrass.TreeCoverLOSDepth = atof(value);` (line 623):

```cpp
	SettingsGrass.GrassCover = GetPrivateProfileIntA("Default", "GrassCover", 0, Filename);
	GetPrivateProfileStringA("Default", "GrassCoverLightReduction", "0.6", value, SettingStringBuffer, Filename);
	SettingsGrass.GrassCoverLightReduction = atof(value);
	GetPrivateProfileStringA("Default", "GrassCoverMinHeight", "90.0", value, SettingStringBuffer, Filename);
	SettingsGrass.GrassCoverMinHeight = atof(value);
	GetPrivateProfileStringA("Default", "GrassCoverFullHeight", "120.0", value, SettingStringBuffer, Filename);
	SettingsGrass.GrassCoverFullHeight = atof(value);
	GetPrivateProfileStringA("Default", "GrassCoverRadius", "200.0", value, SettingStringBuffer, Filename);
	SettingsGrass.GrassCoverRadius = atof(value);
	GetPrivateProfileStringA("Default", "GrassCoverOpacity", "0.5", value, SettingStringBuffer, Filename);
	SettingsGrass.GrassCoverOpacity = atof(value);
	SettingsGrass.GrassCoverBlockLOS = GetPrivateProfileIntA("Default", "GrassCoverBlockLOS", 0, Filename);
	GetPrivateProfileStringA("Default", "GrassCoverLOSDepth", "150.0", value, SettingStringBuffer, Filename);
	SettingsGrass.GrassCoverLOSDepth = atof(value);
```

- [ ] **Step 3: Save** — after the `WritePrivateProfileStringA("Default", "TreeCoverLOSDepth", …)` line (~1638):

```cpp
			WritePrivateProfileStringA("Default", "GrassCover", ToString(SettingsGrass.GrassCover).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverLightReduction", ToString(SettingsGrass.GrassCoverLightReduction).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverMinHeight", ToString(SettingsGrass.GrassCoverMinHeight).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverFullHeight", ToString(SettingsGrass.GrassCoverFullHeight).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverRadius", ToString(SettingsGrass.GrassCoverRadius).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverOpacity", ToString(SettingsGrass.GrassCoverOpacity).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverBlockLOS", ToString(SettingsGrass.GrassCoverBlockLOS).c_str(), Filename);
			WritePrivateProfileStringA("Default", "GrassCoverLOSDepth", ToString(SettingsGrass.GrassCoverLOSDepth).c_str(), Filename);
```

- [ ] **Step 4: Menu map** — after `Settings["TreeCoverLOSDepth"] = SettingsGrass.TreeCoverLOSDepth;` (~2342):

```cpp
			Settings["GrassCover"] = SettingsGrass.GrassCover;
			Settings["GrassCoverLightReduction"] = SettingsGrass.GrassCoverLightReduction;
			Settings["GrassCoverMinHeight"] = SettingsGrass.GrassCoverMinHeight;
			Settings["GrassCoverFullHeight"] = SettingsGrass.GrassCoverFullHeight;
			Settings["GrassCoverRadius"] = SettingsGrass.GrassCoverRadius;
			Settings["GrassCoverOpacity"] = SettingsGrass.GrassCoverOpacity;
			Settings["GrassCoverBlockLOS"] = SettingsGrass.GrassCoverBlockLOS;
			Settings["GrassCoverLOSDepth"] = SettingsGrass.GrassCoverLOSDepth;
```

- [ ] **Step 5: Menu set** — after the `TreeCoverLOSDepth` branch (`SettingsGrass.TreeCoverLOSDepth = Value;`, ~3126):

```cpp
			else if (!strcmp(Setting, "GrassCover"))
				SettingsGrass.GrassCover = Value;
			else if (!strcmp(Setting, "GrassCoverLightReduction"))
				SettingsGrass.GrassCoverLightReduction = Value;
			else if (!strcmp(Setting, "GrassCoverMinHeight"))
				SettingsGrass.GrassCoverMinHeight = Value;
			else if (!strcmp(Setting, "GrassCoverFullHeight"))
				SettingsGrass.GrassCoverFullHeight = Value;
			else if (!strcmp(Setting, "GrassCoverRadius"))
				SettingsGrass.GrassCoverRadius = Value;
			else if (!strcmp(Setting, "GrassCoverOpacity"))
				SettingsGrass.GrassCoverOpacity = Value;
			else if (!strcmp(Setting, "GrassCoverBlockLOS"))
				SettingsGrass.GrassCoverBlockLOS = Value;
			else if (!strcmp(Setting, "GrassCoverLOSDepth"))
				SettingsGrass.GrassCoverLOSDepth = Value;
```

- [ ] **Step 6: Ship values** — in `OblivionReloaded/Shaders/Grass/Grass.ini`, after `TreeCoverLOSDepth = 64` (leave `CollisionTrailSlots = 8` untouched):

```ini
GrassCover = 1
GrassCoverLightReduction = 0.6
GrassCoverMinHeight = 90
GrassCoverFullHeight = 120
GrassCoverRadius = 200
GrassCoverOpacity = 0.5
GrassCoverBlockLOS = 1
GrassCoverLOSDepth = 150
```

- [ ] **Step 7: Verify** — `grep -c "GrassCover" TESReloaded/Core/SettingManager.cpp` → `46`; Release build (PowerShell tool) → DLL line, no errors.

- [ ] **Step 8: Commit** (Bash tool). Grass.ini (CRLF) is staged through a byte-safe hunk filter that drops the user's `CollisionTrailSlots` hunk:

```bash
F=OblivionReloaded/Shaders/Grass/Grass.ini
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp
git diff -U0 -- "$F" | python -c "import sys,re; t=sys.stdin.buffer.read(); p=re.split(rb'(?m)^(?=@@)',t); sys.stdout.buffer.write(p[0]+b''.join(h for h in p[1:] if b'CollisionTrailSlots' not in h))" | git apply --cached --unidiff-zero
git diff --cached -- "$F"
grep -n "CollisionTrailSlots" "$F"
git commit -m "feat(Grass): Add grass cover settings"
```

Expected: `git apply` may warn `trailing whitespace` (the CRs; harmless); the cached Grass.ini diff shows only the eight `+GrassCover…` lines; the grep still shows `CollisionTrailSlots = 8`.

---

### Task 2: Shared seqlock

**Goal:** Move tree cover's seqlock into a reusable `Seqlock.h` template and use it in `TreeCover.cpp`, with no behaviour change.

**Files:**
- Create: `TESReloaded/Core/Seqlock.h`
- Modify: `TESReloaded/Core/TreeCover.cpp:15,24-55,149-155,165`
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj:150`, `OblivionReloaded/OblivionReloaded.vcxproj.filters:76`

**Acceptance Criteria:**
- [ ] `grep -c "SnapshotSequence\|PublishSnapshot\|ReadSnapshot\|kSnapshotReadTries" TESReloaded/Core/TreeCover.cpp` prints `0`
- [ ] `Seqlock.h` holds the same algorithm (odd while writing, `InterlockedIncrement` twice, 4 read tries with `_ReadWriteBarrier` and `YieldProcessor`), with `///` comments on the class and both methods
- [ ] Release build succeeds

**Verify:** the grep → `0`; MSBuild → DLL line, no errors.

**Steps:**

- [ ] **Step 1: Create `TESReloaded/Core/Seqlock.h`:**

```cpp
#pragma once

#include <intrin.h>

/// A snapshot one thread writes and other threads copy out without locking. The sequence is odd while
/// the writer copies; a reader keeps its copy only if it saw the same even sequence before and after.
template <typename T>
class Seqlock {
public:
	/// Replaces the snapshot. Call from one writer thread only.
	void Publish(const T& Next) {
		InterlockedIncrement(&Sequence);
		Value = Next;
		InterlockedIncrement(&Sequence);
	}

	/// Copies the snapshot into Out. Returns false when writes kept overlapping the copy.
	bool Read(T& Out) const {
		for (int Try = 0; Try < kReadTries; Try++) {
			LONG Before = Sequence;
			_ReadWriteBarrier();
			if (Before & 1) {
				YieldProcessor();
				continue;
			}
			Out = Value;
			_ReadWriteBarrier();
			if (Sequence == Before) return true;
		}
		return false;
	}

private:
	static const int	kReadTries = 4;
	volatile LONG		Sequence = 0;
	T					Value = {};
};
```

- [ ] **Step 2: Use it in `TreeCover.cpp`.**
  - Add `#include "Seqlock.h"` after `#include "TreeCoverMath.h"`.
  - Delete the line `static const int	kSnapshotReadTries		= 4;`.
  - Replace everything from `static volatile LONG		SnapshotSequence = 0;` through the closing `}` of `ReadSnapshot` (keep the `PlayerTreeCover` line) with:

```cpp
static Seqlock<TreeCoverSnapshot>	Snapshot;

static volatile float PlayerTreeCover = 0.0f;
```

  - In `UpdateTreeCover`, replace `PublishSnapshot(Shrubs);` with `Snapshot.Publish(Shrubs);`.
  - In `DetectionLineOfSightHook`, replace `if (!ReadSnapshot(Shrubs) || !Shrubs.Count) return Visible;` with `if (!Snapshot.Read(Shrubs) || !Shrubs.Count) return Visible;`.

- [ ] **Step 3: Project files.** In `OblivionReloaded.vcxproj`, after `<ClInclude Include="..\TESReloaded\Core\TreeCoverMath.h" />` add `<ClInclude Include="..\TESReloaded\Core\Seqlock.h" />`. In `OblivionReloaded.vcxproj.filters`, after the `TreeCoverMath.h` `</ClInclude>`, add:

```xml
    <ClInclude Include="..\TESReloaded\Core\Seqlock.h">
      <Filter>Core</Filter>
    </ClInclude>
```

- [ ] **Step 4: Verify** — the grep → `0`; Release build → DLL line, no errors.

- [ ] **Step 5: Commit**

```bash
git add TESReloaded/Core/Seqlock.h TESReloaded/Core/TreeCover.cpp OblivionReloaded/OblivionReloaded.vcxproj OblivionReloaded/OblivionReloaded.vcxproj.filters
git commit -m "refactor(Trees): Move the tree cover seqlock into Seqlock.h"
```

---

### Task 3: Grass cover math

**Goal:** Add the engine-free `GrassCoverMath.h` (clump construction, nearest-N list, sector cover, sight depth, tree+grass combinators), proven by a standalone harness.

**Files:**
- Create: `TESReloaded/Core/GrassCoverMath.h`
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj`, `OblivionReloaded/OblivionReloaded.vcxproj.filters` (header entry)
- Test: `%TEMP%\grasscover-check\GrassCoverCheck.cpp` (harness, NOT committed)

**Acceptance Criteria:**
- [ ] Harness prints 44 `ok` lines then `ALL PASS`, exit code 0, compiled with `/W4` and no warnings from `GrassCoverMath.h` or `TreeCoverMath.h`
- [ ] The header contains no engine types; every public symbol has a `///` comment
- [ ] Release build succeeds

**Verify:** the harness command in Step 2 → 44 `ok` lines, `ALL PASS`, `$LASTEXITCODE` 0.

**Steps:**

- [ ] **Step 1: Write the harness** at `%TEMP%\grasscover-check\GrassCoverCheck.cpp` (create the directory). The expected values were computed independently (Python model of the spec).

```cpp
#include <cstdio>
#include <cmath>
#include "GrassCoverMath.h"

static int Failures = 0;

static void Check(const char* Name, float Got, float Want, float Tolerance = 0.001f) {
	bool Ok = fabsf(Got - Want) <= Tolerance;
	if (!Ok) Failures++;
	printf("%s %s: got %.4f want %.4f\n", Ok ? "ok" : "FAIL", Name, Got, Want);
}

static void CheckBool(const char* Name, bool Got, bool Want) {
	bool Ok = Got == Want;
	if (!Ok) Failures++;
	printf("%s %s: got %d want %d\n", Ok ? "ok" : "FAIL", Name, (int)Got, (int)Want);
}

static GrassClump MakeClump(float X, float Y, float BaseZ, float TopZ, float HalfWidth, float Weight) {
	GrassClump Clump = { X, Y, BaseZ, TopZ, HalfWidth, Weight };
	return Clump;
}

int main() {
	GrassCoverParams Off = { 90.0f, 120.0f, 0.5f, 0.0f, 60.0f, 40.0f };
	GrassCoverParams On = Off;
	On.CollisionRadius = 80.0f;
	GrassClump C = {};

	CheckBool("clump tall kept", GrassCoverClumpOf(0, 0, 0, 150, 0, 0, 128, 1, 60, Off, C), true);
	Check("clump tall x", C.X, 150);
	Check("clump tall top", C.TopZ, 128);
	Check("clump tall weight", C.Weight, 0.5f);
	CheckBool("clump short dropped", GrassCoverClumpOf(0, 0, 0, 150, 0, 0, 79, 1, 60, Off, C), false);
	GrassCoverClumpOf(0, 0, 0, 150, 0, 0, 100, 1.05f, 60, Off, C);
	Check("clump mid weight", C.Weight, 0.25f);
	GrassCoverClumpOf(0, 0, 0, 150, 0, -30, 128, 1, 60, Off, C);
	Check("clump downslope weight", C.Weight, 0.0877f);
	GrassCoverClumpOf(0, 0, 0, 40, 0, 0, 128, 1, 60, On, C);
	Check("clump pushed x", C.X, 70);
	Check("clump flattened top", C.TopZ, 108);
	Check("clump pushed weight", C.Weight, 0.324f);
	GrassCoverClumpOf(0, 0, 0, 100, 0, 0, 128, 1, 60, On, C);
	Check("clump outside ring x", C.X, 100);
	Check("clump outside ring weight", C.Weight, 0.5f);
	CheckBool("clump under player dropped", GrassCoverClumpOf(0, 0, 0, 0, 0, 0, 128, 1, 60, On, C), false);
	GrassCoverParams Step = Off;
	Step.MinHeight = Step.FullHeight = 100.0f;
	GrassCoverClumpOf(0, 0, 0, 150, 0, 0, 128, 1, 60, Step, C);
	Check("clump step edge weight", C.Weight, 0.5f);
	GrassCoverParams Opaque = Off;
	Opaque.Opacity = 1.5f;
	GrassCoverClumpOf(0, 0, 0, 150, 0, 0, 128, 1, 60, Opaque, C);
	Check("clump opacity clamped", C.Weight, 1.0f);

	GrassClump Kept[3];
	float DistSqs[3];
	int Count = 0;
	float Inserts[5] = { 5, 1, 4, 2, 9 };
	for (int i = 0; i < 5; i++) GrassCoverKeepNearest(Kept, DistSqs, Count, 3, MakeClump(Inserts[i], 0, 0, 0, 0, 0), Inserts[i]);
	Check("keep nearest count", (float)Count, 3);
	Check("keep nearest sum", DistSqs[0] + DistSqs[1] + DistSqs[2], 7);
	Check("keep nearest clump sum", Kept[0].X + Kept[1].X + Kept[2].X, 7);

	Check("cover empty", GrassCoverAt(Kept, 0, 0, 0), 0);
	GrassClump Around = MakeClump(0, 0, 0, 128, 60, 0.5f);
	Check("cover inside clump", GrassCoverAt(&Around, 1, 0, 0), 0.5f);
	GrassClump Ring[kGrassCoverSectors];
	GrassClump HalfRing[kGrassCoverSectors];
	int Half = 0;
	for (int s = 0; s < kGrassCoverSectors; s++) {
		float Angle = (s + 0.5f) * 2.0f * kGrassCoverPi / kGrassCoverSectors - kGrassCoverPi;
		Ring[s] = MakeClump(150 * cosf(Angle), 150 * sinf(Angle), 0, 128, 60, 1);
		if (Ring[s].X > 0) HalfRing[Half++] = Ring[s];
	}
	Check("cover full ring", GrassCoverAt(Ring, kGrassCoverSectors, 0, 0), 1);
	Check("cover half ring", GrassCoverAt(HalfRing, Half, 0, 0), 0.625f);
	GrassClump Thin[2] = { MakeClump(190, 0, 0, 128, 10, 0.5f), MakeClump(190, 0, 0, 128, 10, 0.5f) };
	Check("cover thin clump", GrassCoverAt(Thin, 1, 0, 0), 0.0625f);
	Check("cover compounding", GrassCoverAt(Thin, 2, 0, 0), 0.09375f);

	GrassClump Ray = MakeClump(0, 0, 0, 120, 50, 0.5f);
	Check("ray level", GrassClumpRayDepth(Ray, -200, 0, 60, 200, 0, 60), 50, 0.05f);
	Check("ray miss", GrassClumpRayDepth(Ray, -200, 100, 60, 200, 100, 60), 0);
	Check("ray above", GrassClumpRayDepth(Ray, -200, 0, 150, 200, 0, 150), 0);
	Check("ray descending", GrassClumpRayDepth(Ray, -200, 0, 200, 0, 0, 50), 31.25f, 0.05f);
	Check("ray steep", GrassClumpRayDepth(Ray, -100, 0, 400, 0, 0, 50), 36.40f, 0.05f);
	Check("ray vertical", GrassClumpRayDepth(Ray, 10, 0, 200, 10, 0, 50), 35, 0.05f);
	GrassClump Pair[2] = { Ray, MakeClump(100, 0, 0, 120, 50, 0.5f) };
	Check("ray sum", GrassCoverRayDepth(Pair, 2, -200, 0, 60, 200, 0, 60), 100, 0.05f);
	GrassClump Flat = MakeClump(0, 0, 0, 120, 0, 0.5f);
	Check("ray zero width", GrassClumpRayDepth(Flat, -200, 0, 60, 200, 0, 60), 0);

	Check("light none", FoliageLightScale(0, 0.75f, 0, 0.6f), 1);
	Check("light tree only", FoliageLightScale(1, 0.75f, 0, 0.6f), 0.25f);
	Check("light both", FoliageLightScale(1, 0.75f, 1, 0.6f), 0.1f);
	Check("light over-reduction", FoliageLightScale(1, 1.5f, 1, 1.5f), 0);
	Check("light partial", FoliageLightScale(0.5f, 0.75f, 0.5f, 0.6f), 0.4375f);
	Check("light negative reduction", FoliageLightScale(1, -0.5f, 0, 0), 1);

	CheckBool("sight tree at threshold", FoliageBlocksSight(64, 64, 0, 150), true);
	CheckBool("sight tree below threshold", FoliageBlocksSight(63.9f, 64, 0, 150), false);
	CheckBool("sight shared", FoliageBlocksSight(32, 64, 75, 150), true);
	CheckBool("sight grass off", FoliageBlocksSight(32, 64, 0, 0), false);
	CheckBool("sight both off", FoliageBlocksSight(100, 0, 100, 0), false);
	CheckBool("sight grass below", FoliageBlocksSight(0, 64, 149, 150), false);

	if (Failures) printf("%d FAILED\n", Failures);
	else printf("ALL PASS\n");
	return Failures ? 1 : 0;
}
```

- [ ] **Step 2: Run it to verify it fails** (PowerShell tool):

```powershell
$vc = 'C:\Development\Microsoft\Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat'
$inc = 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded\Core'
$dir = "$env:TEMP\grasscover-check"
cmd /c "call `"$vc`" >nul && cd /d `"$dir`" && cl /nologo /EHsc /W4 /I `"$inc`" GrassCoverCheck.cpp && GrassCoverCheck.exe"
```

Expected: compile error `cannot open include file: 'GrassCoverMath.h'`.

- [ ] **Step 3: Create `TESReloaded/Core/GrassCoverMath.h`:**

```cpp
#pragma once

#include "TreeCoverMath.h"

/// One tall grass clump near the player, after the player's collision push, in world units (z up).
struct GrassClump {
	float X, Y;			///< Centre, moved outward by the player's collision push
	float BaseZ;		///< Root height (the instance z)
	float TopZ;			///< Top height, lowered by the player's collision flattening
	float HalfWidth;	///< Horizontal half-width of the clump's cylinder
	float Weight;		///< How much the clump blocks, 0-1 (opacity × tallness)
};

/// The settings the grass cover math needs, in world units.
struct GrassCoverParams {
	float MinHeight;		///< Clump top above the player's feet where cover begins
	float FullHeight;		///< Clump top above the player's feet where a clump counts fully
	float Opacity;			///< Weight of one full-height clump
	float CollisionRadius;	///< The player's live grass collision radius; 0 when grass collision is off
	float PushStrength;		///< Maximum outward push at the collision ring (CollisionStrength)
	float FlattenStrength;	///< Maximum downward push at the collision centre (CollisionFlattenStrength)
};

/// Angular sectors around the player that GrassCoverAt averages.
static const int	kGrassCoverSectors	= 16;
/// Pi, for the sector angles.
static const float	kGrassCoverPi		= 3.14159265f;

/// HLSL smoothstep(E0, E1, X); E0 > E1 gives the falling edge, and equal edges a step at E0.
inline float GrassCoverSmoothstep(float E0, float E1, float X) {
	if (E1 == E0) return X >= E0 ? 1.0f : 0.0f;
	return TreeCoverSmoothstep((X - E0) / (E1 - E0));
}

/// Builds the clump for one grass instance (IX, IY, IZ) seen from the player's feet (PX, PY, PZ). The
/// collision push and flattening mirror GrassCollisionDisplacement in GrassCollision.hlsl for the player
/// source at full tip weight. Returns false when the clump gives no cover.
inline bool GrassCoverClumpOf(float PX, float PY, float PZ, float IX, float IY, float IZ, float ModelHeight, float HeightFactor, float HalfWidth, const GrassCoverParams& Params, GrassClump& Out) {
	float DX = IX - PX;
	float DY = IY - PY;
	float D = sqrtf(DX * DX + DY * DY);
	float X = IX;
	float Y = IY;
	float Top = IZ + ModelHeight * HeightFactor;
	if (Params.CollisionRadius > 0.0f) {
		float T = TreeCoverSaturate(D / Params.CollisionRadius);
		float Influence = GrassCoverSmoothstep(1.0f, 0.0f, T);
		float Push = Influence * GrassCoverSmoothstep(0.0f, 0.3f, T) * Params.PushStrength;
		if (D > 0.001f) {
			X += DX / D * Push;
			Y += DY / D * Push;
		}
		Top -= Influence * Params.FlattenStrength;
	}
	float Weight = TreeCoverSaturate(Params.Opacity * GrassCoverSmoothstep(Params.MinHeight, Params.FullHeight, Top - PZ));
	if (!(Weight > 0.0f)) return false;
	Out.X = X;
	Out.Y = Y;
	Out.BaseZ = IZ;
	Out.TopZ = Top;
	Out.HalfWidth = HalfWidth;
	Out.Weight = Weight;
	return true;
}

/// Adds a clump at squared distance DistSq to a list of at most Max (≥ 1) clumps, keeping the nearest.
/// DistSqs runs parallel to Clumps; Count is the list's current length.
inline void GrassCoverKeepNearest(GrassClump* Clumps, float* DistSqs, int& Count, int Max, const GrassClump& Clump, float DistSq) {
	int Slot = Count;
	if (Count < Max) {
		Count++;
	}
	else {
		Slot = 0;
		for (int i = 1; i < Count; i++) {
			if (DistSqs[i] > DistSqs[Slot]) Slot = i;
		}
		if (DistSq >= DistSqs[Slot]) return;
	}
	Clumps[Slot] = Clump;
	DistSqs[Slot] = DistSq;
}

/// Returns how fully the clumps surround the point (PX, PY), 0-1: the mean over kGrassCoverSectors
/// sectors of 1 − Π(1 − Weight) over the clumps each sector sees. A clump covers the sectors within its
/// angular half-width atan2(HalfWidth, distance) of its bearing; a clump containing the point covers all.
inline float GrassCoverAt(const GrassClump* Clumps, int Count, float PX, float PY) {
	float Exposure[kGrassCoverSectors];
	for (int s = 0; s < kGrassCoverSectors; s++) Exposure[s] = 1.0f;
	float SectorHalf = kGrassCoverPi / kGrassCoverSectors;
	for (int i = 0; i < Count; i++) {
		const GrassClump& Clump = Clumps[i];
		float Keep = 1.0f - Clump.Weight;
		float DX = Clump.X - PX;
		float DY = Clump.Y - PY;
		float D = sqrtf(DX * DX + DY * DY);
		if (D <= Clump.HalfWidth) {
			for (int s = 0; s < kGrassCoverSectors; s++) Exposure[s] *= Keep;
			continue;
		}
		float Bearing = atan2f(DY, DX);
		float Reach = atan2f(Clump.HalfWidth, D) + SectorHalf;
		for (int s = 0; s < kGrassCoverSectors; s++) {
			float Centre = (s + 0.5f) * 2.0f * SectorHalf - kGrassCoverPi;
			if (fabsf(remainderf(Centre - Bearing, 2.0f * kGrassCoverPi)) < Reach) Exposure[s] *= Keep;
		}
	}
	float Sum = 0.0f;
	for (int s = 0; s < kGrassCoverSectors; s++) Sum += 1.0f - Exposure[s];
	return Sum / kGrassCoverSectors;
}

/// Returns the weighted length, world units, of the segment A → B inside one clump's vertical cylinder:
/// its half-width circle in XY, from BaseZ to TopZ.
inline float GrassClumpRayDepth(const GrassClump& Clump, float AX, float AY, float AZ, float BX, float BY, float BZ) {
	float DX = BX - AX;
	float DY = BY - AY;
	float DZ = BZ - AZ;
	float FX = AX - Clump.X;
	float FY = AY - Clump.Y;
	float RadiusSq = Clump.HalfWidth * Clump.HalfWidth;
	float A = DX * DX + DY * DY;
	float T0 = 0.0f;
	float T1 = 1.0f;
	if (A < 1e-6f) {
		if (FX * FX + FY * FY > RadiusSq) return 0.0f;
	}
	else {
		float B = 2.0f * (FX * DX + FY * DY);
		float Disc = B * B - 4.0f * A * (FX * FX + FY * FY - RadiusSq);
		if (!(Disc > 0.0f)) return 0.0f;
		float Root = sqrtf(Disc);
		float Enter = (-B - Root) / (2.0f * A);
		float Exit = (-B + Root) / (2.0f * A);
		if (Enter > T0) T0 = Enter;
		if (Exit < T1) T1 = Exit;
		if (!(T0 < T1)) return 0.0f;
	}
	if (!TreeCoverClipSlab(AZ, DZ, Clump.BaseZ, Clump.TopZ, T0, T1)) return 0.0f;
	return (T1 - T0) * sqrtf(DX * DX + DY * DY + DZ * DZ) * Clump.Weight;
}

/// Returns the summed GrassClumpRayDepth of the clumps on the segment A → B.
inline float GrassCoverRayDepth(const GrassClump* Clumps, int Count, float AX, float AY, float AZ, float BX, float BY, float BZ) {
	float Depth = 0.0f;
	for (int i = 0; i < Count; i++) Depth += GrassClumpRayDepth(Clumps[i], AX, AY, AZ, BX, BY, BZ);
	return Depth;
}

/// Returns the detection light multiplier for tree and grass cover, 0-1. Each cover's reduction is
/// clamped on its own, so an out-of-range reduction can't flip the product's sign.
inline float FoliageLightScale(float TreeCover, float TreeReduction, float GrassCover, float GrassReduction) {
	return TreeCoverSaturate(1.0f - TreeCover * TreeReduction) * TreeCoverSaturate(1.0f - GrassCover * GrassReduction);
}

/// Returns true when tree and grass foliage together block a sight line: each depth over its threshold,
/// summed, reaches 1. A threshold ≤ 0 leaves its depth out.
inline bool FoliageBlocksSight(float TreeDepth, float TreeThreshold, float GrassDepth, float GrassThreshold) {
	float Sum = 0.0f;
	if (TreeThreshold > 0.0f) Sum += TreeDepth / TreeThreshold;
	if (GrassThreshold > 0.0f) Sum += GrassDepth / GrassThreshold;
	return Sum >= 1.0f;
}
```

- [ ] **Step 4: Run the harness** (Step 2 command). Expected: 44 `ok` lines, `ALL PASS`, no compiler warnings, `$LASTEXITCODE` 0.

- [ ] **Step 5: Project files.** In `OblivionReloaded.vcxproj`, after `<ClInclude Include="..\TESReloaded\Core\Seqlock.h" />` add `<ClInclude Include="..\TESReloaded\Core\GrassCoverMath.h" />`; in the `.filters`, after the `Seqlock.h` `</ClInclude>`:

```xml
    <ClInclude Include="..\TESReloaded\Core\GrassCoverMath.h">
      <Filter>Core</Filter>
    </ClInclude>
```

Release build → DLL line, no errors.

- [ ] **Step 6: Commit** (the harness stays in `%TEMP%`):

```bash
git add TESReloaded/Core/GrassCoverMath.h OblivionReloaded/OblivionReloaded.vcxproj OblivionReloaded/OblivionReloaded.vcxproj.filters
git commit -m "feat(Grass): Add grass cover math"
```

---

### Task 4: Grass model metrics

**Goal:** Expose the cached grass model size (height plus a new horizontal half-width) from `RenderHook.cpp`, keeping groundcover classification unchanged.

**Files:**
- Modify: `TESReloaded/Core/RenderHook.h`
- Modify: `TESReloaded/Core/RenderHook.cpp:885-930` (`GrassModelHeight` cache and `GetGrassModelHeight`)

**Acceptance Criteria:**
- [ ] `RenderHook.h` declares `struct GrassModelMetrics { float Height; float HalfWidth; }` and `GrassModelMetrics GetGrassModelMetrics(NiGeometry*)`, both with `///` comments
- [ ] `GetGrassModelHeight` returns `GetGrassModelMetrics(Geometry).Height`; the height computation is unchanged (max vertex z, else bound top; `FLT_MAX` without data)
- [ ] HalfWidth = max over vertices of √(x² + y²), else the bound radius; 0 without data
- [ ] The `[Groundcover]` log line also prints `halfWidth=%.1f`
- [ ] Release build succeeds

**Verify:** MSBuild → DLL line, no errors; `grep -n "GetGrassModelMetrics" TESReloaded/Core/RenderHook.h TESReloaded/Core/RenderHook.cpp` → declaration in the header, definition plus the one `GetGrassModelHeight` call in the .cpp.

**Steps:**

- [ ] **Step 1: Header** — append to `TESReloaded/Core/RenderHook.h`:

```cpp

/// Model-space size of a grass batch's clump model.
struct GrassModelMetrics {
	float Height;		///< Highest vertex z, or the bound top without a CPU copy; FLT_MAX without geometry data
	float HalfWidth;	///< Largest horizontal vertex distance from the model origin, or the bound radius
};

/// Returns the cached size of a grass batch geometry's model. Main thread only.
GrassModelMetrics GetGrassModelMetrics(NiGeometry* Geometry);
```

- [ ] **Step 2: Implementation** — in `RenderHook.cpp`, replace the block from the `/// Cached model height of one grass batch's geometry data.` comment through the end of `GetGrassModelHeight` with:

```cpp
/// Cached model size of one grass batch's geometry data. Vertices and BoundRadius detect a freed
/// pointer reused by a different model.
struct GrassModelCacheEntry {
	UInt16				Vertices;
	float				BoundRadius;
	GrassModelMetrics	Metrics;
};
static std::map<NiGeometryData*, GrassModelCacheEntry> GrassModelCache;
static std::map<int, bool> GrassModelHeightsLogged;

GrassModelMetrics GetGrassModelMetrics(NiGeometry* Geometry) {

	NiGeometryData* Data = Geometry->geomData;
	if (!Data) return { FLT_MAX, 0.0f };

	auto Cached = GrassModelCache.find(Data);
	if (Cached != GrassModelCache.end() && Cached->second.Vertices == Data->Vertices && Cached->second.BoundRadius == Data->Bound.Radius)
		return Cached->second.Metrics;

	GrassModelMetrics Metrics = { Data->Bound.Center.z + Data->Bound.Radius, Data->Bound.Radius };
	bool FromVertices = Data->Vertex && Data->Vertices;
	if (FromVertices) {
		float HalfWidthSq = 0.0f;
		Metrics.Height = -FLT_MAX;
		for (UInt32 i = 0; i < Data->Vertices; i++) {
			const NiPoint3& Vertex = Data->Vertex[i];
			Metrics.Height = max(Metrics.Height, Vertex.z);
			HalfWidthSq = max(HalfWidthSq, Vertex.x * Vertex.x + Vertex.y * Vertex.y);
		}
		Metrics.HalfWidth = sqrtf(HalfWidthSq);
	}

	if (GrassModelCache.size() >= 4096) GrassModelCache.clear();
	GrassModelCache[Data] = { Data->Vertices, Data->Bound.Radius, Metrics };

	int HeightKey = (int)(Metrics.Height * 10.0f);
	if (GrassModelHeightsLogged.size() < 64 && GrassModelHeightsLogged.find(HeightKey) == GrassModelHeightsLogged.end()) {
		GrassModelHeightsLogged[HeightKey] = true;
		Logger::Log("[Groundcover] %s verts=%u bound=(z %.1f r %.1f) height=%.1f halfWidth=%.1f from %s -> %s",
			Geometry->m_pcName ? Geometry->m_pcName : "-", Data->Vertices, Data->Bound.Center.z, Data->Bound.Radius,
			Metrics.Height, Metrics.HalfWidth, FromVertices ? "vertices" : "bound", Metrics.Height <= TheSettingManager->SettingsGrass.GroundcoverMaxHeight ? "groundcover" : "grass");
	}
	return Metrics;

}

// Model-space height of a grass batch's model: the highest vertex when the CPU copy is kept,
// otherwise the top of the bounding sphere (an overestimate for wide, flat models).
static float GetGrassModelHeight(NiGeometry* Geometry) {

	return GetGrassModelMetrics(Geometry).Height;

}
```

- [ ] **Step 3: Verify** — Release build → DLL line, no errors; the grep from **Verify**.

- [ ] **Step 4: Commit**

```bash
git add TESReloaded/Core/RenderHook.h TESReloaded/Core/RenderHook.cpp
git commit -m "feat(Grass): Expose cached grass model height and half-width"
```

---

### Task 5: Grass cover scan module

**Goal:** Add `GrassCover.h/.cpp`: a per-frame main-thread scan of the engine grass map that publishes the player's grass cover and the nearby tall clumps, plus a sight-depth query, called from `ShaderManager` each frame.

**Files:**
- Create: `TESReloaded/Core/GrassCover.h`, `TESReloaded/Core/GrassCover.cpp`
- Modify: `TESReloaded/Core/ShaderManager.cpp:10` (include), `:2734` (call after `UpdateTreeCover();`)
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj`, `OblivionReloaded/OblivionReloaded.vcxproj.filters`
- Regenerate (gitignored): `compile_commands.json`

**Acceptance Criteria:**
- [ ] `GrassCover.h` declares `UpdateGrassCover()`, `GetPlayerGrassCover()`, `GetGrassSightDepth(float, float, float)` with `///` comments
- [ ] The scan returns cover 0 and publishes an empty snapshot unless (`GrassCover` or `GrassCoverBlockLOS`), `Player`, `Player->process`, `Player->parentCell` and `Player->GetWorldSpace()` are all set and the player is sneaking and not swimming
- [ ] The scan rejects a map with a null table or > 65536 buckets, and a batch with a null owner/geomData/instances, 0 used, or used > capacity
- [ ] The collision push/flatten applies only when `SettingsMain.Shaders.Grass` is on, the source count is > 0, and source 0 is `GrassSource_Player`
- [ ] No batch or clump pointer survives the scan; engine offsets are named constants
- [ ] `ShaderManager.cpp` calls `UpdateGrassCover();` immediately after `UpdateTreeCover();`
- [ ] Release build succeeds; `compile_commands.json` lists `GrassCover.cpp`

**Verify:** MSBuild → DLL line, no errors; `grep -c "GrassCover.cpp" compile_commands.json` → ≥ 1.

**Steps:**

- [ ] **Step 1: Create `TESReloaded/Core/GrassCover.h`:**

```cpp
#pragma once

/// Grass cover for sneak detection: tall grass around the sneaking player lowers the light level fed to
/// Calc_DetectionLevel and can block observers' line of sight. TreeCover.cpp's detection hooks consume
/// it. Oblivion only.

/// Rescans the tall grass around the sneaking player and publishes their cover and the nearby clumps.
/// Main thread, once per frame.
void UpdateGrassCover();

/// Returns the sneaking player's grass cover this frame, 0-1; 0 when the scan is off. Any thread.
float GetPlayerGrassCover();

/// Returns the weighted depth of tall grass, world units, on the sight line from the eye point to the
/// player's torso this frame; 0 without a snapshot. Any thread.
float GetGrassSightDepth(float EyeX, float EyeY, float EyeZ);
```

- [ ] **Step 2: Create `TESReloaded/Core/GrassCover.cpp`:**

```cpp
#include "GrassCover.h"
#include "GrassCoverMath.h"
#include "RenderHook.h"
#include "Seqlock.h"

static const UInt32	kGrassMap				= 0x00B2CBC4; // NiTMap<GRAS formID, grass type>, main thread only
static const UInt32	kGrassMapBucketCount	= 0x04;
static const UInt32	kGrassMapBuckets		= 0x08;
static const UInt32	kGrassMapMaxBuckets		= 65536;
static const UInt32	kGrassTypeBatchList		= 0x24;
static const UInt32	kGrassBatchOwner		= 0x00;
static const UInt32	kGrassBatchCapacity		= 0x0C;
static const UInt32	kGrassBatchUsed			= 0x0E;
static const UInt32	kGrassBatchInstances	= 0x10;
static const float	kGrassBatchReach		= 6000.0f; // a batch spans at most one cell, so no instance is farther than this from its first
static const float	kGrassCoverTorsoHeight	= 50.0f;
static const UInt32	kMovementSneak			= 0x400;
static const UInt32	kMovementSwim			= 0x800;
static const int	kGrassCoverMaxClumps	= 96;

struct GrassMapItem {
	GrassMapItem*	Next;
	UInt32			FormID;
	UInt8*			Type;
};

struct GrassBatchNode {
	GrassBatchNode*	Next;
	GrassBatchNode*	Prev;
	UInt8*			Batch;
};

/// The tall clumps around the sneaking player this frame, for the line-of-sight hook.
struct GrassCoverSnapshot {
	int			Count;
	float		TorsoX, TorsoY, TorsoZ;
	GrassClump	Clumps[kGrassCoverMaxClumps];
};

/// Inputs shared by every batch of one scan, and the nearest-clump list it fills.
struct GrassCoverScan {
	float				PX, PY, PZ;
	float				Radius;
	float				GroundcoverMaxHeight;
	float				ScaleX, ScaleZ, ScaleBias;
	GrassCoverParams	Params;
	GrassCoverSnapshot*	Out;
	float				DistSqs[kGrassCoverMaxClumps];
};

static Seqlock<GrassCoverSnapshot>	Snapshot;
static volatile float				PlayerGrassCover = 0.0f;

// The player's live grass collision radius, or 0 when grass collision isn't bending grass this frame.
static float PlayerCollisionRadius() {

	if (!TheSettingManager->SettingsMain.Shaders.Grass || TheShaderManager->GrassCollisionSourceCount <= 0) return 0.0f;
	const ShaderManager::GrassSource& Source = TheShaderManager->GrassCollisionSources[0];
	if (Source.kind != ShaderManager::GrassSource_Player) return 0.0f;
	return TheSettingManager->SettingsGrass.CollisionRadius * Source.radiusScale;

}

static void ScanGrassBatch(GrassCoverScan& Scan, UInt8* Batch) {

	NiGeometry* Owner = *(NiGeometry**)(Batch + kGrassBatchOwner);
	UInt16 Capacity = *(UInt16*)(Batch + kGrassBatchCapacity);
	UInt16 Used = *(UInt16*)(Batch + kGrassBatchUsed);
	const float* Instances = *(const float**)(Batch + kGrassBatchInstances);
	if (!Owner || !Owner->geomData || !Instances || !Used || Used > Capacity) return;

	float Reach = kGrassBatchReach + Scan.Radius;
	float FirstX = Instances[0] - Scan.PX;
	float FirstY = Instances[1] - Scan.PY;
	if (FirstX * FirstX + FirstY * FirstY > Reach * Reach) return;

	GrassModelMetrics Model = GetGrassModelMetrics(Owner);
	if (Model.Height <= Scan.GroundcoverMaxHeight) return;
	float HalfWidth = Model.HalfWidth * Scan.ScaleX;
	float Near = Scan.Radius + HalfWidth;

	GrassCoverSnapshot& Out = *Scan.Out;
	for (UInt16 i = 0; i < Used; i++) {
		const float* Instance = Instances + i * 4;
		float DX = Instance[0] - Scan.PX;
		float DY = Instance[1] - Scan.PY;
		float DistSq = DX * DX + DY * DY;
		if (DistSq > Near * Near) continue;
		float HeightFactor = 0.01f * Instance[3] * Scan.ScaleZ + Scan.ScaleBias;
		GrassClump Clump;
		if (!GrassCoverClumpOf(Scan.PX, Scan.PY, Scan.PZ, Instance[0], Instance[1], Instance[2], Model.Height, HeightFactor, HalfWidth, Scan.Params, Clump)) continue;
		GrassCoverKeepNearest(Out.Clumps, Scan.DistSqs, Out.Count, kGrassCoverMaxClumps, Clump, DistSq);
	}

}

static float ScanGrassCover(GrassCoverSnapshot& Out) {

	const SettingsGrassStruct& Settings = TheSettingManager->SettingsGrass;
	if ((!Settings.GrassCover && !Settings.GrassCoverBlockLOS) || !Player || !Player->process || !Player->parentCell || !Player->GetWorldSpace()) {
		return 0.0f;
	}
	UInt32 Movement = Player->process->GetMovementFlags();
	if (!(Movement & kMovementSneak) || (Movement & kMovementSwim)) return 0.0f;

	UInt32 BucketCount = *(UInt32*)(kGrassMap + kGrassMapBucketCount);
	GrassMapItem** Buckets = *(GrassMapItem***)(kGrassMap + kGrassMapBuckets);
	if (!Buckets || BucketCount > kGrassMapMaxBuckets) return 0.0f;

	GrassCoverScan Scan = {};
	Scan.PX = Player->pos.x;
	Scan.PY = Player->pos.y;
	Scan.PZ = Player->pos.z;
	Scan.Radius = Settings.GrassCoverRadius;
	Scan.GroundcoverMaxHeight = Settings.GroundcoverMaxHeight;
	Scan.ScaleX = Settings.ScaleX;
	Scan.ScaleZ = Settings.ScaleZ;
	Scan.ScaleBias = Settings.MinHeight;
	Scan.Params.MinHeight = Settings.GrassCoverMinHeight;
	Scan.Params.FullHeight = Settings.GrassCoverFullHeight;
	Scan.Params.Opacity = Settings.GrassCoverOpacity;
	Scan.Params.CollisionRadius = PlayerCollisionRadius();
	Scan.Params.PushStrength = Settings.CollisionStrength;
	Scan.Params.FlattenStrength = Settings.CollisionFlattenStrength;
	Scan.Out = &Out;

	Out.TorsoX = Scan.PX;
	Out.TorsoY = Scan.PY;
	Out.TorsoZ = Scan.PZ + kGrassCoverTorsoHeight;

	for (UInt32 b = 0; b < BucketCount; b++) {
		for (GrassMapItem* Item = Buckets[b]; Item; Item = Item->Next) {
			if (!Item->Type) continue;
			for (GrassBatchNode* Node = *(GrassBatchNode**)(Item->Type + kGrassTypeBatchList); Node; Node = Node->Next) {
				if (Node->Batch) ScanGrassBatch(Scan, Node->Batch);
			}
		}
	}
	return GrassCoverAt(Out.Clumps, Out.Count, Scan.PX, Scan.PY);

}

void UpdateGrassCover() {

	static GrassCoverSnapshot Next;
	Next = {};
	PlayerGrassCover = ScanGrassCover(Next);
	Snapshot.Publish(Next);

}

float GetPlayerGrassCover() {

	return PlayerGrassCover;

}

float GetGrassSightDepth(float EyeX, float EyeY, float EyeZ) {

	GrassCoverSnapshot Clumps;
	if (!Snapshot.Read(Clumps) || !Clumps.Count) return 0.0f;
	return GrassCoverRayDepth(Clumps.Clumps, Clumps.Count, EyeX, EyeY, EyeZ, Clumps.TorsoX, Clumps.TorsoY, Clumps.TorsoZ);

}
```

- [ ] **Step 3: Call it each frame** — in `TESReloaded/Core/ShaderManager.cpp`, add `#include "GrassCover.h"` after `#include "TreeCover.h"` (line 10), and after `UpdateTreeCover();` (~2734) add:

```cpp
	UpdateGrassCover();
```

- [ ] **Step 4: Project files** — in `OblivionReloaded.vcxproj`, after `<ClInclude Include="..\TESReloaded\Core\GrassCoverMath.h" />` add `<ClInclude Include="..\TESReloaded\Core\GrassCover.h" />`, and after `<ClCompile Include="..\TESReloaded\Core\TreeCover.cpp" />` add `<ClCompile Include="..\TESReloaded\Core\GrassCover.cpp" />`. In the `.filters`, after the `GrassCoverMath.h` `</ClInclude>`:

```xml
    <ClInclude Include="..\TESReloaded\Core\GrassCover.h">
      <Filter>Core</Filter>
    </ClInclude>
```

and after the `TreeCover.cpp` `</ClCompile>`:

```xml
    <ClCompile Include="..\TESReloaded\Core\GrassCover.cpp">
      <Filter>Core</Filter>
    </ClCompile>
```

- [ ] **Step 5: Verify** — Release build (PowerShell tool) → DLL line, no errors. Then `powershell -File Tools\GenerateCompileCommands.ps1` and `grep -c "GrassCover.cpp" compile_commands.json` → ≥ 1.

- [ ] **Step 6: Commit** (`compile_commands.json` is gitignored):

```bash
git add TESReloaded/Core/GrassCover.h TESReloaded/Core/GrassCover.cpp TESReloaded/Core/ShaderManager.cpp OblivionReloaded/OblivionReloaded.vcxproj OblivionReloaded/OblivionReloaded.vcxproj.filters
git commit -m "feat(Grass): Scan tall grass around the sneaking player"
```

---

### Task 6: Combine grass cover in the detection hooks

**Goal:** Make the light hook and the LOS hook in `TreeCover.cpp` apply tree and grass cover together, through `FoliageLightScale` and `FoliageBlocksSight`, with tree-only behaviour unchanged when the grass switches are off.

**Files:**
- Modify: `TESReloaded/Core/TreeCover.cpp` (includes, `AdjustDetectionLight`, `DetectionLineOfSightHook`)
- Modify: `TESReloaded/Core/TreeCover.h` (doc comments)

**Acceptance Criteria:**
- [ ] `AdjustDetectionLight` returns early unless the target is the player and sneaking, and one of tree cover (with `TreeCover` on) or grass cover (with `GrassCover` on) is > 0; otherwise it scales light by `FoliageLightScale(tree, TreeCoverLightReduction, grass, GrassCoverLightReduction)`, with each disabled feature's cover passed as 0
- [ ] `DetectionLineOfSightHook` returns the engine's answer unless it is visible, the target is the player, and at least one of (`TreeCoverBlockLOS` with `TreeCoverLOSDepth > 0`) or (`GrassCoverBlockLOS` with `GrassCoverLOSDepth > 0`) holds; it then returns `!FoliageBlocksSight(...)` with each disabled feature's threshold passed as 0
- [ ] Neither hook touches cells, the scene graph or the grass map
- [ ] `TreeCover.h` comments mention grass cover and the four switches
- [ ] Release build succeeds

**Verify:** MSBuild → DLL line, no errors; `grep -c "FoliageLightScale\|FoliageBlocksSight\|GetPlayerGrassCover\|GetGrassSightDepth" TESReloaded/Core/TreeCover.cpp` → `4`.

**Steps:**

- [ ] **Step 1: Includes** — in `TreeCover.cpp`, after `#include "Seqlock.h"` add:

```cpp
#include "GrassCover.h"
#include "GrassCoverMath.h"
```

- [ ] **Step 2: Light hook** — replace `AdjustDetectionLight` with:

```cpp
static void __stdcall AdjustDetectionLight(Actor* Target, SInt32* Args) {

	const SettingsGrassStruct& Settings = TheSettingManager->SettingsGrass;
	if (Target != Player || !(UInt8)Args[kDetectionArgSneaking]) return;
	float Tree = Settings.TreeCover ? PlayerTreeCover : 0.0f;
	float Grass = Settings.GrassCover ? GetPlayerGrassCover() : 0.0f;
	if (!(Tree > 0.0f) && !(Grass > 0.0f)) return;
	float Scale = FoliageLightScale(Tree > 0.0f ? Tree : 0.0f, Settings.TreeCoverLightReduction, Grass > 0.0f ? Grass : 0.0f, Settings.GrassCoverLightReduction);
	Args[kDetectionArgLight] = (SInt32)(Args[kDetectionArgLight] * Scale);

}
```

- [ ] **Step 3: LOS hook** — replace `DetectionLineOfSightHook` (keep its comment line above it) with:

```cpp
static bool __fastcall DetectionLineOfSightHook(Actor* Observer, void* Edx, UInt32 Arg1, TESObjectREFR* Target, UInt32 Arg3, UInt32* Reason, UInt32 Arg5) {

	bool Visible = (UInt8)ThisCall(kActorHasLineOfSight, Observer, Arg1, Target, Arg3, Reason, Arg5);
	if (!Visible || Target != (TESObjectREFR*)Player) return Visible;

	const SettingsGrassStruct& Settings = TheSettingManager->SettingsGrass;
	float TreeThreshold = Settings.TreeCoverBlockLOS && Settings.TreeCoverLOSDepth > 0.0f ? Settings.TreeCoverLOSDepth : 0.0f;
	float GrassThreshold = Settings.GrassCoverBlockLOS && Settings.GrassCoverLOSDepth > 0.0f ? Settings.GrassCoverLOSDepth : 0.0f;
	if (TreeThreshold <= 0.0f && GrassThreshold <= 0.0f) return Visible;

	float EyeX = Observer->pos.x;
	float EyeY = Observer->pos.y;
	float EyeZ = Observer->pos.z + kTreeCoverEyeHeight * Observer->scale;

	float TreeDepth = 0.0f;
	TreeCoverSnapshot Shrubs;
	if (TreeThreshold > 0.0f && Snapshot.Read(Shrubs)) {
		for (int i = 0; i < Shrubs.Count; i++)
			TreeDepth += TreeCoverRayDepth(Shrubs.Shrubs[i], EyeX, EyeY, EyeZ, Shrubs.TorsoX, Shrubs.TorsoY, Shrubs.TorsoZ);
	}
	float GrassDepth = GrassThreshold > 0.0f ? GetGrassSightDepth(EyeX, EyeY, EyeZ) : 0.0f;
	return !FoliageBlocksSight(TreeDepth, TreeThreshold, GrassDepth, GrassThreshold);

}
```

- [ ] **Step 4: Header comments** — in `TESReloaded/Core/TreeCover.h`, replace the file comment and the `CreateTreeCoverHook` comment with:

```cpp
/// Foliage cover for sneak detection: while the player sneaks inside small SpeedTree trees and shrubs, or
/// in tall grass (GrassCover.h), the light level fed to Calc_DetectionLevel is scaled down, and observers
/// whose sight line crosses enough of that foliage lose line of sight. Oblivion only.

/// Patches the only call to Calc_DetectionLevel and the detection call to the actor LOS test. Always
/// installed; gated at runtime on SettingsGrass.TreeCover, GrassCover, TreeCoverBlockLOS and
/// GrassCoverBlockLOS.
```

- [ ] **Step 5: Verify** — Release build → DLL line, no errors; the grep from **Verify** → `4`.

- [ ] **Step 6: Commit**

```bash
git add TESReloaded/Core/TreeCover.cpp TESReloaded/Core/TreeCover.h
git commit -m "feat(Grass): Apply grass cover to sneak light and line of sight"
```

---

### Task 7: In-game play-test and tuning

**Goal:** Prove the spec's success criteria in-game with temporary instrumentation, tune the shipped Grass.ini values, remove the instrumentation, and record the results. Run by the coordinator session: it needs the user in the game.

**Files:**
- Modify (temporary, reverted before commit): `TESReloaded/Core/GrassCover.cpp`, `TESReloaded/Core/TreeCover.cpp`
- Modify (tuning, only if values change): `OblivionReloaded/Shaders/Grass/Grass.ini`
- Modify: `memory/grass-manager-generation.md` (or a new `memory/grass-cover.md`), `memory/MEMORY.md`, `docs/superpowers/specs/2026-11-02-grass-cover-sneak-design.md` (status line)

**Acceptance Criteria:**
- [ ] Logged in-game (`[GrassCover]` lines in `C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log`): field centre of the 128-unit grass ≥ 0.8; its edge roughly half of the centre value; 73–83-unit grass ≤ 0.3; short grass and bare ground 0
- [ ] Scan time ≤ 0.05 ms in the densest spot
- [ ] A hostile NPC on level ground across a tall field loses the sneaking player (LOS blocked in the log); one on a rise keeps them; one adjacent keeps them
- [ ] With `GrassCover = 0` and `GrassCoverBlockLOS = 0`, the logged light and LOS match tree-only behaviour; with the `Grass` shader off, cover is still non-zero in tall grass and nothing crashes; not sneaking, swimming or indoors gives cover 0
- [ ] Instrumentation removed: `grep -c "\[GrassCover\]\|\[FoliageLOS\]\|\[FoliageLight\]" TESReloaded/Core/*.cpp` → `0`; Release build succeeds
- [ ] Play-test results recorded in memory, and the spec status set to implemented, in a `docs:` commit

**Verify:** the log lines above, captured and quoted in the task close; the grep → `0`; MSBuild → DLL line, no errors.

**Steps:**

- [ ] **Step 1: Add temporary instrumentation** (do NOT commit). In `GrassCover.cpp`, replace `UpdateGrassCover` with:

```cpp
void UpdateGrassCover() {

	static GrassCoverSnapshot Next;
	static DWORD LastLog = 0;
	LARGE_INTEGER T0, T1, Freq;
	QueryPerformanceCounter(&T0);
	Next = {};
	PlayerGrassCover = ScanGrassCover(Next);
	QueryPerformanceCounter(&T1);
	QueryPerformanceFrequency(&Freq);
	Snapshot.Publish(Next);
	if (Player && Player->process && (Player->process->GetMovementFlags() & kMovementSneak) && GetTickCount() - LastLog > 2000) {
		LastLog = GetTickCount();
		Logger::Log("[GrassCover] cover=%.2f clumps=%d scan=%.3fms collision=%.0f pos=(%.0f,%.0f,%.0f)", PlayerGrassCover, Next.Count,
			(T1.QuadPart - T0.QuadPart) * 1000.0 / Freq.QuadPart, PlayerCollisionRadius(), Player->pos.x, Player->pos.y, Player->pos.z);
	}

}
```

In `TreeCover.cpp`, at the end of `AdjustDetectionLight` (after the light is scaled), and before the final `return` of `DetectionLineOfSightHook`, add throttled logs:

```cpp
	static DWORD LastLightLog = 0;
	if (GetTickCount() - LastLightLog > 2000) {
		LastLightLog = GetTickCount();
		Logger::Log("[FoliageLight] tree=%.2f grass=%.2f scale=%.2f light=%d", Tree, Grass, Scale, Args[kDetectionArgLight]);
	}
```

```cpp
	static DWORD LastSightLog = 0;
	if (GetTickCount() - LastSightLog > 1000) {
		LastSightLog = GetTickCount();
		Logger::Log("[FoliageLOS] observer=%08X tree=%.1f/%.0f grass=%.1f/%.0f blocked=%d", Observer->refID, TreeDepth, TreeThreshold, GrassDepth, GrassThreshold,
			(int)FoliageBlocksSight(TreeDepth, TreeThreshold, GrassDepth, GrassThreshold));
	}
```

Build (PowerShell tool) → DLL line, no errors.

- [ ] **Step 2: Play-test with the user.** Ask them to sneak 10–20 s at each spot, in order, and say when done: (1) centre of the 128-unit tall grass; (2) its edge; (3) the 73–83-unit grass; (4) short grass; (5) bare ground; (6) with a hostile NPC on level ground across the tall field, then from a rise, then adjacent; (7) standing (not sneaking) and swimming. Then read `[GrassCover]`, `[FoliageLight]` and `[FoliageLOS]` from `C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log` and check each acceptance criterion.

- [ ] **Step 3: Switch checks.** Ask the user to set `GrassCover = 0` and `GrassCoverBlockLOS = 0` in the in-game menu, sneak in a shrub inside tall grass and confirm `[FoliageLight]` shows `grass=0.00` with `scale` equal to `1 − tree·0.75`, and `[FoliageLOS]` shows `grass=0.0/0`. Restore both, turn the `Grass` shader off, and confirm `[GrassCover]` shows `collision=0` with non-zero cover in tall grass and no crash. Restore the shader.

- [ ] **Step 4: Tune.** If a criterion misses, adjust `GrassCoverMinHeight`/`FullHeight`/`Opacity`/`LightReduction`/`LOSDepth` in the in-game menu with the user, re-run the failing spot, and write the final values to `Grass.ini`. Report every value changed and why.

- [ ] **Step 5: Remove the instrumentation** — `git checkout -- TESReloaded/Core/GrassCover.cpp TESReloaded/Core/TreeCover.cpp`; the grep from **Acceptance Criteria** → `0`; Release build → DLL line, no errors.

- [ ] **Step 6: Commit tuning (only if Grass.ini values changed)**, through the hunk filter so `CollisionTrailSlots` stays out:

```bash
F=OblivionReloaded/Shaders/Grass/Grass.ini
git diff -U0 -- "$F" | python -c "import sys,re; t=sys.stdin.buffer.read(); p=re.split(rb'(?m)^(?=@@)',t); sys.stdout.buffer.write(p[0]+b''.join(h for h in p[1:] if b'CollisionTrailSlots' not in h))" | git apply --cached --unidiff-zero
git diff --cached -- "$F"
git commit -m "chore(Grass): Tune grass cover from the play-test"
```

- [ ] **Step 7: Record the results** — add the play-test numbers (cover per spot, scan time, LOS outcomes, final values) to memory (a `grass-cover` feature memory, linked from `grass-manager-generation`, plus its `MEMORY.md` line), and set the spec's `Status:` line to `implemented 2026-11-02, play-tested`. Commit:

```bash
git add memory docs/superpowers/specs/2026-11-02-grass-cover-sneak-design.md
git commit -m "docs: Record the grass cover play-test"
```
