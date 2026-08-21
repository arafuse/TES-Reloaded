# LOD Dither Fade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cross-dissolve LOD and full-model load transitions with a screen-space dither over a configurable duration defaulting to 1.0 seconds, replacing the instantaneous pops that happen today when distant cells stream in, cells load, and `LandLOD` quadrants are rebuilt.

**Architecture:** A new `LODFadeManager` singleton polls the engine's `gridDistantArray`, `gridCellArray` and `LandLOD` child list once per frame and diffs them against shadow copies to detect load transitions. Detected transitions become `FadeRecord`s in a fixed-capacity table. At draw time, `RenderHook::TrackSetupShaderPrograms` resolves the drawn geometry to a record via a lazily-cached parent-chain walk and publishes a `TESR_GEOM_FadeParams` constant, which a shared `LODFade.hlsl` include turns into a per-pixel `clip()` in the static, tree and terrain pixel shaders.

**Tech Stack:** C++ (MSVC v145, x86), Direct3D 9 / D3DX9, HLSL ps_3_0, OBSE plugin, Microsoft Detours.

**Spec:** `docs/superpowers/specs/2026-08-20-lod-dither-fade-design.md`

## Global Constraints

- **Target define:** `OBLIVION`. All engine struct work must be inside the Oblivion block. In `GameNi.h` that is **lines 1822-3610** (`#elif defined(OBLIVION)`); the blocks before and after it are NewVegas and Skyrim and define the same class names with different layouts. In `Game.h` the Oblivion block is the **middle** of three.
- **Build command** (must be the `.sln`, not the `.vcxproj`, because `$(SolutionDir)` is used in force-include paths):
  ```
  powershell -Command "& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal"
  ```
  Run builds through the **PowerShell tool, not Bash** — under Bash the `TEMP` variable is the literal string `%TEMP%`, which produces a bogus MSB3073 that looks exactly like a locked output DLL.
- **Shader validation command** (offline, no game launch needed):
  ```
  "C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x64\fxc.exe" /T ps_3_0 /E main /I <the shader's own directory> <file.pso.hlsl>
  ```
  The `/I` is required: the game's D3DX resolves nested relative includes from the top-level `.pso` directory, but `fxc` resolves them from the including file.
- **Shader recompile gate:** edited `.hlsl` does **not** take effect in game until `[Develop] CompileShaders=1` is set in `OblivionReloaded.ini`. Compiled output is cached as an extension-less file beside each `.hlsl` and is **not** timestamp-checked.
- **Shader deployment:** the game folder's `Shaders` directory is a symbolic link to `OblivionReloaded/Shaders` in this repo. Editing the repo file is deploying it. The post-build step copies only the DLL and PDB.
- **No test framework exists in this repository.** There is no pytest/gtest/npm equivalent and none is to be added. Every task's verification is: `fxc` clean, MSBuild clean, and where stated, an in-game observation using the `Develop.LogLODFade` log channel.
- **Per-geometry constant registers:** `c100` = `TESR_GEOM_Toggles` (pixel), `c128` = `TESR_GEOM_EyePosition` (vertex). This feature uses **`c110` (pixel)**. `c101` was the initial choice but is already `TESR_SpecularData` in five Task 7 shaders (`SLS2003`, `SLS2012`, `SLS2018`, `SLS2033`, `SLS2039`) and `c102` is `TESR_TerrainData` in `SLS2033`; `c103`+ is clear across `ExtraShaders`, `Terrain`, `POM` and `POMExterior`, so `c110` was picked with headroom. Do not use table-allocated registers; follow the explicit convention.
- **Fade time default:** `1.0` seconds.
- **Coding style:** public symbols get documentation comments; inline comments are 1-3 lines maximum.
- **Commits:** work happens on branch `feat/lod-dither-fade`, which already exists and already contains the spec commit.

---

### Task 1: INI settings

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h:269-295` (add `LogLODFade` to `DevelopStruct`, add `LODFadeStruct` and its member)
- Modify: `TESReloaded/Core/SettingManager.cpp:424-431` (read the new keys)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `TheSettingManager->SettingsMain.LODFade.Enabled` — `bool`
  - `TheSettingManager->SettingsMain.LODFade.FadeTime` — `float`
  - `TheSettingManager->SettingsMain.LODFade.PinDeparting` — `bool`
  - `TheSettingManager->SettingsMain.LODFade.MaxFades` — `UInt32`
  - `TheSettingManager->SettingsMain.Develop.LogLODFade` — `UInt8`

There is no `OblivionReloaded.ini` checked into this repository; it lives only in the game folder. The defaults passed to `GetPrivateProfileIntA` / `GetPrivateProfileStringA` are therefore the shipped behaviour for anyone whose INI lacks the section, and no repo file needs a new section.

- [ ] **Step 1: Add the settings struct**

In `TESReloaded/Core/SettingManager.h`, immediately before `struct DevelopStruct {` (line 269), add:

```cpp
	/// Dither cross-fade for LOD and full-model load transitions.
	struct LODFadeStruct {
		bool	Enabled;
		float	FadeTime;
		bool	PinDeparting;
		UInt32	MaxFades;
	};
```

- [ ] **Step 2: Add the log toggle and the struct member**

In the same file, add `LogLODFade` as the last member of `DevelopStruct`:

```cpp
		UInt8	NearShellDebug;
		UInt8	LogLODFade;
	};
```

and add the member alongside the other struct instances, immediately before `DevelopStruct Develop;` (line 294):

```cpp
	LODFadeStruct				LODFade;
	DevelopStruct				Develop;
```

- [ ] **Step 3: Read the keys from the INI**

In `TESReloaded/Core/SettingManager.cpp`, immediately before the `SettingsMain.Develop.CompileShaders` line (line 424), add:

```cpp
	SettingsMain.LODFade.Enabled = GetPrivateProfileIntA("LODFade", "Enabled", 1, Filename);
	GetPrivateProfileStringA("LODFade", "FadeTime", "1.0", value, SettingStringBuffer, Filename);
	SettingsMain.LODFade.FadeTime = atof(value);
	SettingsMain.LODFade.PinDeparting = GetPrivateProfileIntA("LODFade", "PinDeparting", 1, Filename);
	SettingsMain.LODFade.MaxFades = GetPrivateProfileIntA("LODFade", "MaxFades", 256, Filename);
```

and add the log toggle after the `NearShellDebug` line (line 430):

```cpp
	SettingsMain.Develop.LogLODFade = GetPrivateProfileIntA("Develop", "LogLODFade", 0, Filename);
```

- [ ] **Step 4: Build**

Run the build command from Global Constraints via the **PowerShell tool**.
Expected: `Build succeeded`, 0 errors. No behaviour change yet — nothing reads these settings.

- [ ] **Step 5: Commit**

```bash
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp
git commit -m "feat(LODFade): Add [LODFade] INI settings"
```

---

### Task 2: Shader constant plumbing

**Files:**
- Modify: `TESReloaded/Core/ShaderManager.h:178-180` (add `LODFadeStruct`), `:319` (add the member), `:343-361` (add `HasFadeParams` to `ShaderProgram`)
- Modify: `TESReloaded/Core/ShaderManager.cpp:514-524` (`SetPerGeomConstantTableValue`), `:664-700` (`CreateCT`)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `TheShaderManager->ShaderConst.LODFade.Params` — `D3DXVECTOR4`, where `.x` is fade alpha, `.y` the per-frame dither seed, `.z` the invert flag, `.w` unused.
  - `ShaderProgram::HasFadeParams` — `bool`, true when that shader declares `TESR_GEOM_FadeParams`.
  - Shader-visible constant name `TESR_GEOM_FadeParams`.

- [ ] **Step 1: Add the constant struct**

In `TESReloaded/Core/ShaderManager.h`, immediately after `struct GeometryStruct { ... };` (ends line 180), add:

```cpp
	/// x = fade alpha, y = per-frame dither seed, z = invert flag, w = unused.
	struct LODFadeStruct {
		D3DXVECTOR4		Params;
	};
```

- [ ] **Step 2: Add the member to ShaderConstants**

In the same file, immediately after `GeometryStruct Geometry;` (line 319), add:

```cpp
	LODFadeStruct			LODFade;
```

- [ ] **Step 3: Add the HasFadeParams flag**

In `class ShaderProgram` (line 343), add a member after `TextureShaderValuesCount`:

```cpp
	/// True when this shader declares TESR_GEOM_FadeParams and therefore participates in LOD fading.
	bool					HasFadeParams;
```

Then fix `ShaderProgram::ShaderProgram()` at `ShaderManager.cpp:259-266`. It currently initialises only four of its six value members:

```cpp
ShaderProgram::ShaderProgram() {

	FloatShaderValues = NULL;
	TextureShaderValues = NULL;
	FloatShaderValuesCount = 0;
	TextureShaderValuesCount = 0;

}
```

`PerGeomFloatShaderValues` and `PerGeomFloatShaderValuesCount` are left uninitialised, yet `CreateCT` does `PerGeomFloatShaderValuesCount += 1` on that garbage at line 679 and then `malloc`s from it at line 686. This is a pre-existing latent bug that happens to survive on zeroed allocations, and this feature depends directly on that counter. Fix it as part of this task:

```cpp
ShaderProgram::ShaderProgram() {

	FloatShaderValues = NULL;
	TextureShaderValues = NULL;
	PerGeomFloatShaderValues = NULL;
	FloatShaderValuesCount = 0;
	TextureShaderValuesCount = 0;
	PerGeomFloatShaderValuesCount = 0;
	HasFadeParams = false;

}
```

- [ ] **Step 4: Map the constant name**

In `TESReloaded/Core/ShaderManager.cpp`, in `ShaderProgram::SetPerGeomConstantTableValue` (line 514), add a branch before the `else { return false; }`:

```cpp
	else if (!strcmp(Name, "TESR_GEOM_FadeParams")) {
		PerGeomFloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.LODFade.Params;
		HasFadeParams = true;
	}
```

- [ ] **Step 5: Initialise the constant to fully opaque**

`Params.x` must default to `1.0` so that a shader carrying the clip renders normally before any fade ever runs. In `ShaderManager::ShaderManager()` (`ShaderManager.cpp:1062`), add:

```cpp
	ShaderConst.LODFade.Params = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f);
```

- [ ] **Step 6: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors. `HasFadeParams` stays false everywhere because no shader declares the constant yet.

- [ ] **Step 7: Commit**

```bash
git add TESReloaded/Core/ShaderManager.h TESReloaded/Core/ShaderManager.cpp
git commit -m "feat(LODFade): Add TESR_GEOM_FadeParams shader constant"
```

---

### Task 3: LODFadeManager skeleton and fade table

**Files:**
- Create: `TESReloaded/Core/LODFadeManager.h`
- Create: `TESReloaded/Core/LODFadeManager.cpp`
- Modify: `TESReloaded/Core/Managers.h` (include, extern)
- Modify: `TESReloaded/Core/Managers.cpp` (definition, construction)
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj` (add both files to the project)
- Modify: `TESReloaded/Core/ShaderManager.cpp:2315` (call `Update()` once per frame)

**Interfaces:**
- Consumes: `TheSettingManager->SettingsMain.LODFade.*` from Task 1.
- Produces:
  - `class LODFadeManager` with `void Update();`, `float GetAlpha(FadeRecord* Record);`, `bool AnyFadesLive();`
  - `struct LODFadeManager::FadeRecord { NiAVObject* Root; UInt8 Direction; float StartTime; bool Pinned; bool HasPartner; }`
  - `enum { FadeDir_In = 0, FadeDir_Out = 1 }`
  - `FadeRecord* LODFadeManager::AddFade(NiAVObject* Root, UInt8 Direction);` — returns `NULL` when the table is full.
  - Singleton `TheLODFadeManager`.

This task builds the table and its lifecycle only. Nothing detects transitions and nothing draws differently; `AnyFadesLive()` always returns false. That is deliberate — it gives the later tasks a verified foundation.

- [ ] **Step 1: Write the header**

Create `TESReloaded/Core/LODFadeManager.h`:

```cpp
#pragma once

/// Tracks LOD and full-model load transitions and publishes a per-geometry dither fade alpha.
/// Detection is by polling the engine grid arrays; see
/// docs/superpowers/specs/2026-08-20-lod-dither-fade-design.md.
class LODFadeManager {
public:
	LODFadeManager();

	enum {
		FadeDir_In	= 0,
		FadeDir_Out	= 1,
	};

	/// One in-flight transition. Root is the scene-graph node whose subtree fades.
	struct FadeRecord {
		NiAVObject*	Root;
		UInt8		Direction;
		float		StartTime;
		bool		Pinned;
		bool		HasPartner;
	};

	/// Advances all live fades and retires completed ones. Called once per frame.
	void			Update();

	/// Starts a fade. Returns NULL when the table is full, in which case the caller pops as before.
	FadeRecord*		AddFade(NiAVObject* Root, UInt8 Direction);

	/// Fade fraction for a record: 0 to 1 for a fade-in, 1 to 0 for a fade-out.
	float			GetAlpha(FadeRecord* Record);

	/// True when at least one fade is in flight. Gates all per-draw work.
	bool			AnyFadesLive() { return LiveCount > 0; }

	/// Seed published in TESR_GEOM_FadeParams.y to animate the dither pattern per frame.
	float			DitherSeed;

private:
	std::vector<FadeRecord>	Fades;
	UInt32					LiveCount;
	float					CurrentTime;

	void			Retire(UInt32 Index);
};
```

- [ ] **Step 2: Write the implementation**

Create `TESReloaded/Core/LODFadeManager.cpp`:

```cpp
#include "LODFadeManager.h"

LODFadeManager::LODFadeManager() {

	TheLODFadeManager = this;
	Fades.clear();
	LiveCount = 0;
	CurrentTime = 0.0f;
	DitherSeed = 0.0f;

}

LODFadeManager::FadeRecord* LODFadeManager::AddFade(NiAVObject* Root, UInt8 Direction) {

	if (!Root) return NULL;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) return NULL;

	FadeRecord Record;
	Record.Root = Root;
	Record.Direction = Direction;
	Record.StartTime = CurrentTime;
	Record.Pinned = false;
	Record.HasPartner = false;
	Fades.push_back(Record);
	LiveCount = Fades.size();

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] start root=%08X dir=%d live=%d", (UInt32)Root, Direction, LiveCount);

	return &Fades.back();

}

float LODFadeManager::GetAlpha(FadeRecord* Record) {

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	if (FadeTime <= 0.0f) return 1.0f;

	float t = (CurrentTime - Record->StartTime) / FadeTime;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return Record->Direction == FadeDir_In ? t : 1.0f - t;

}

void LODFadeManager::Retire(UInt32 Index) {

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] retire root=%08X", (UInt32)Fades[Index].Root);

	Fades.erase(Fades.begin() + Index);

}

void LODFadeManager::Update() {

	CurrentTime = (float)GetTickCount() * 0.001f;
	DitherSeed = (float)(GetTickCount() & 0xFFFF);

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	for (SInt32 i = (SInt32)Fades.size() - 1; i >= 0; i--) {
		if (CurrentTime - Fades[i].StartTime >= FadeTime) Retire(i);
	}
	LiveCount = Fades.size();

}
```

- [ ] **Step 3: Register the singleton**

In `TESReloaded/Core/Managers.h`, add `#include "LODFadeManager.h"` after the `ShadowManager.h` include, and add after the `TheScriptManager` extern:

```cpp
extern LODFadeManager*		TheLODFadeManager;
```

In `TESReloaded/Core/Managers.cpp`, add after the `TheScriptManager` definition:

```cpp
LODFadeManager*		TheLODFadeManager = NULL;
```

and add to `InitializeManagers()`, after `new ScriptManager();`:

```cpp
	new LODFadeManager();
```

- [ ] **Step 4: Add both files to the project**

Open `OblivionReloaded/OblivionReloaded.vcxproj` and add `LODFadeManager.cpp` to the `<ClCompile>` item group and `LODFadeManager.h` to the `<ClInclude>` item group, copying the exact relative-path style used by the neighbouring `ShadowManager` entries. Find them with:

```bash
grep -n "ShadowManager" OblivionReloaded/OblivionReloaded.vcxproj
```

- [ ] **Step 5: Call Update once per frame**

In `TESReloaded/Core/ShaderManager.cpp`, in `ShaderManager::UpdateConstants()` (line 2315), add immediately after the `TheRenderManager->GetSceneCameraData();` line:

```cpp
	if (TheSettingManager->SettingsMain.LODFade.Enabled) TheLODFadeManager->Update();
```

`UpdateConstants` is called from three sites in `RenderHook.cpp` (lines 124, 251, 899) but only one runs per frame for a given render path, and `Update` is idempotent within a frame because it recomputes from the wall clock rather than accumulating.

- [ ] **Step 6: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 7: Verify in game**

Launch the game, load an exterior save, walk for 30 seconds, quit.
Expected: no visual change, no crash, and no `[LODFade]` lines in the log (nothing calls `AddFade` yet). A crash here means the singleton or the per-frame call site is wrong, not the feature.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/LODFadeManager.h TESReloaded/Core/LODFadeManager.cpp TESReloaded/Core/Managers.h TESReloaded/Core/Managers.cpp OblivionReloaded/OblivionReloaded.vcxproj TESReloaded/Core/ShaderManager.cpp
git commit -m "feat(LODFade): Add LODFadeManager skeleton and fade table"
```

---

### Task 4: Grid pollers for the unpaired fade-ins

**Files:**
- Modify: `TESReloaded/Core/LODFadeManager.h` (poller state and methods)
- Modify: `TESReloaded/Core/LODFadeManager.cpp` (poller implementation)

**Interfaces:**
- Consumes: `AddFade`, `FadeDir_In` from Task 3.
- Produces:
  - `void LODFadeManager::PollDistantGrid();`
  - `void LODFadeManager::PollLandLOD();`
  - `bool LODFadeManager::IsRegisteredRoot(NiAVObject* Node);` — used by Task 5's parent walk.
  - `std::unordered_map<NiAVObject*, LODFadeManager::FadeRecord*> RootIndex;` — root node to record.

**Scope note, deliberate:** this task handles only the two transitions that have **no partner** — a distant slot gaining a node, and a `LandLOD` slot gaining a new quadrant. Cell loading is *not* handled here even though it is a fade-in, because without the paired pin from Task 8 the LOD would vanish while the full model was still transparent, leaving a hole that is worse than today's pop. Every checkpoint in this plan must be an improvement on the previous one.

Engine data, all inside the Oblivion block:
- `Tes->gridDistantArray` (`Game.h:8109`): `GridDistantArray` with `UInt32 size` and `DistantGridEntry* grid`, an array of `size * size` 16-byte records. `unk04` is the `NiNode*`; `unk08` and `unk0C` are signed cell X and Y.
- `Tes->LODRoot` (`Game.h:8170`): despite the name this is the `LandLOD` node. Its terrain quadrants are `LODRoot->m_children`, an `NiTArray<NiAVObject*>` where `data` is `NiAVObject**` and entries may be `NULL` up to `numObjs`.

- [ ] **Step 1: Add poller state to the header**

In `LODFadeManager.h`, add to the private section:

```cpp
	std::vector<NiAVObject*>							PrevDistant;
	std::vector<NiAVObject*>							PrevLandLOD;
	std::unordered_map<NiAVObject*, FadeRecord*>		RootIndex;
	bool												PrevValid;

	void			PollDistantGrid();
	void			PollLandLOD();
	void			ResyncShadowCopies();
```

and to the public section:

```cpp
	/// True when Node is the root of a live fade. Used by the draw-time parent-chain walk.
	bool			IsRegisteredRoot(NiAVObject* Node) { return RootIndex.count(Node) != 0; }
```

Initialise `PrevValid = false;` in the constructor.

- [ ] **Step 2: Implement the distant-grid poller**

Add to `LODFadeManager.cpp`:

```cpp
void LODFadeManager::PollDistantGrid() {

	GridDistantArray* Grid = Tes->gridDistantArray;
	if (!Grid || !Grid->grid || !Grid->size) return;

	UInt32 Slots = Grid->size * Grid->size;
	if (PrevDistant.size() != Slots) {
		PrevDistant.assign(Slots, NULL);
		PrevValid = false;
	}

	// Count first, so a teleport is suppressed before any fade is started rather than after.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = (NiAVObject*)Grid->grid[i].unk04;
		if (Node != PrevDistant[i]) Changed++;
	}

	bool Discontinuity = !PrevValid || Changed > (Slots / 4);
	if (Discontinuity) {
		if (PrevValid && TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] distant discontinuity: %d of %d slots changed, suppressed", Changed, Slots);
		for (UInt32 i = 0; i < Slots; i++) PrevDistant[i] = (NiAVObject*)Grid->grid[i].unk04;
		return;
	}

	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = (NiAVObject*)Grid->grid[i].unk04;
		if (Node != PrevDistant[i]) {
			if (Node && !RootIndex.count(Node)) {
				FadeRecord* Record = AddFade(Node, FadeDir_In);
				if (Record) RootIndex[Node] = Record;
			}
			PrevDistant[i] = Node;
		}
	}

}
```

- [ ] **Step 3: Implement the LandLOD poller**

The size-mismatch branch resyncs `PrevLandLOD` to the actual current child pointers rather than `NULL`, because stamping `NULL` would make every already-loaded quadrant look newly-populated on the very next poll and fire a spurious mass fade-in; the discontinuity guard below mirrors `PollDistantGrid`'s count-first shape but with its own threshold, since LandLOD has only about a dozen slots.

```cpp
void LODFadeManager::PollLandLOD() {

	NiNode* LandLOD = Tes->LODRoot;
	if (!LandLOD) return;

	UInt32 Count = LandLOD->m_children.numObjs;
	if (PrevLandLOD.size() != Count) {
		PrevLandLOD.assign(Count, NULL);
		for (UInt32 i = 0; i < Count; i++) PrevLandLOD[i] = LandLOD->m_children.data[i];
		return;
	}

	// Count first, so a teleport is suppressed before any fade is started rather than after.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Count; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (Node != PrevLandLOD[i]) Changed++;
	}

	if (Changed > (Count / 2)) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] landlod discontinuity: %d of %d slots changed, suppressed", Changed, Count);
		for (UInt32 i = 0; i < Count; i++) PrevLandLOD[i] = LandLOD->m_children.data[i];
		return;
	}

	for (UInt32 i = 0; i < Count; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (Node != PrevLandLOD[i]) {
			if (Node && !RootIndex.count(Node)) {
				FadeRecord* Record = AddFade(Node, FadeDir_In);
				if (Record) RootIndex[Node] = Record;
			}
			PrevLandLOD[i] = Node;
		}
	}

}
```

- [ ] **Step 4: Call the pollers and keep RootIndex in sync**

`RootIndex` holds `FadeRecord*` into a `std::vector`, which is invalidated by `push_back` and `erase`. Rebuild it at the end of every `Update` rather than patching it. Replace the body of `Update` with:

```cpp
void LODFadeManager::Update() {

	CurrentTime = (float)GetTickCount() * 0.001f;
	DitherSeed = (float)(GetTickCount() & 0xFFFF);

	// Player and Tes are NULL at the main menu; every per-frame hook that touches them must guard.
	// Fades.clear() below invalidates every FadeRecord* GeomCache holds, so the cache must be
	// dropped here too - this early return would otherwise skip the FadeSetDirty-gated clear
	// that normally runs at the end of this function (see Task 5).
	if (!Player || !Tes) {
		Fades.clear();
		RootIndex.clear();
		GeomCache.clear();
		FadeSetDirty = false;
		LiveCount = 0;
		FadeResetPending = false;
		TheShaderManager->ShaderConst.LODFade.Params.x = 1.0f;
		TheShaderManager->ShaderConst.LODFade.Params.z = 0.0f;
		PrevValid = false;
		return;
	}

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	for (SInt32 i = (SInt32)Fades.size() - 1; i >= 0; i--) {
		if (CurrentTime - Fades[i].StartTime >= FadeTime) Retire(i);
	}

	PollDistantGrid();
	PollLandLOD();
	PrevValid = true;

	RootIndex.clear();
	for (UInt32 i = 0; i < Fades.size(); i++) RootIndex[Fades[i].Root] = &Fades[i];
	LiveCount = Fades.size();

}
```

Note: `GeomCache`, `FadeSetDirty`, and `FadeResetPending` are only introduced in Task 5; this guard branch is finished off there once those members exist (Task 5 Step 3/5 supersede the bare version above).

Then remove the now-redundant `RootIndex[Node] = Record;` assignments from both pollers, keeping the `!RootIndex.count(Node)` guards, which read the previous frame's index and are what stop a node being re-faded every frame.

- [ ] **Step 5: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 6: Verify in game with logging**

Set `[Develop] LogLODFade=1` in the game folder's `OblivionReloaded.ini`. Launch, load an exterior save, then:

1. Stand still for 10 seconds. Expected: no `[LODFade] start` lines — a still camera streams nothing.
2. Walk in a straight line across at least two cell boundaries. Expected: `[LODFade] start` lines appear in small bursts, each followed about 1 second later by a matching `[LODFade] retire`.
3. Fast-travel to a distant city. Expected: exactly one `[LODFade] distant discontinuity: N of M slots changed, suppressed` line, and **no** burst of hundreds of `start` lines.

Still no visual change: nothing consumes the alpha yet. If step 3 produces a flood of `start` lines, the discontinuity threshold is wrong — do not proceed to Task 5.

- [ ] **Step 7: Commit**

```bash
git add TESReloaded/Core/LODFadeManager.h TESReloaded/Core/LODFadeManager.cpp
git commit -m "feat(LODFade): Poll distant and LandLOD grids for stream-in transitions"
```

---

### Task 5: Draw-time resolution and constant publishing

**Files:**
- Modify: `TESReloaded/Core/LODFadeManager.h` (resolution cache and lookup)
- Modify: `TESReloaded/Core/LODFadeManager.cpp` (parent-chain walk)
- Modify: `TESReloaded/Core/RenderHook.cpp:381` (publish the constant per draw)

**Interfaces:**
- Consumes: `AnyFadesLive()`, `IsRegisteredRoot()`, `GetAlpha()`, `DitherSeed`, `RootIndex` from Tasks 3 and 4; `ShaderConst.LODFade.Params` and `HasFadeParams` from Task 2.
- Produces: `LODFadeManager::FadeRecord* ResolveGeometry(NiAVObject* Geometry);` — returns `NULL` when the geometry belongs to no fade.

- [ ] **Step 1: Add the resolution cache**

In `LODFadeManager.h`, add to the private section:

```cpp
	std::unordered_map<NiAVObject*, FadeRecord*>	GeomCache;
```

and to the public section:

```cpp
	/// Maps a drawn geometry to the fade it belongs to by walking m_parent to a registered root.
	/// The answer is cached for the duration of the fade episode, misses included.
	FadeRecord*		ResolveGeometry(NiAVObject* Geometry);
```

- [ ] **Step 2: Implement the walk**

```cpp
LODFadeManager::FadeRecord* LODFadeManager::ResolveGeometry(NiAVObject* Geometry) {

	std::unordered_map<NiAVObject*, FadeRecord*>::iterator Cached = GeomCache.find(Geometry);
	if (Cached != GeomCache.end()) return Cached->second;

	FadeRecord* Found = NULL;
	NiAVObject* Node = Geometry;
	for (UInt32 Depth = 0; Node && Depth < 16; Depth++) {
		std::unordered_map<NiAVObject*, FadeRecord*>::iterator Root = RootIndex.find(Node);
		if (Root != RootIndex.end()) {
			Found = Root->second;
			break;
		}
		Node = (NiAVObject*)Node->m_parent;
	}

	// Misses are cached too - a miss is the common case and the walk must not repeat per frame.
	GeomCache[Geometry] = Found;
	return Found;

}
```

- [ ] **Step 3: Clear the cache when the fade set changes**

The cache is only valid while `RootIndex` is unchanged. Do NOT invalidate it by comparing `LiveCount` before/after: a fade retiring and another starting in the same frame leaves `LiveCount` unchanged while `RootIndex`'s contents are entirely different, so a `LiveCount` comparison would serve `FadeRecord*` values dangling into an erased-and-repushed `std::vector` (Ruling F6).

Instead, add `bool FadeSetDirty;` to the private section, initialised `false` in the constructor. Set `FadeSetDirty = true;` in `AddFade` (success path, after `push_back`) and in `Retire` (before `erase`). In `Update`, after `RootIndex` is rebuilt:

```cpp
	UInt32 PrevLive = LiveCount;
	RootIndex.clear();
	for (UInt32 i = 0; i < Fades.size(); i++) RootIndex[Fades[i].Root] = &Fades[i];
	LiveCount = Fades.size();

	if (FadeSetDirty) {
		GeomCache.clear();
		FadeSetDirty = false;
	}
```

`PrevLive` is kept only to compute `FadeResetPending` in Step 5, not to decide cache invalidity.

The `!Player || !Tes` guard branch added in Task 4 Step 4 must uphold the same invariant: it calls `Fades.clear()`, which invalidates every `FadeRecord*` `GeomCache` holds, but its early `return` skips the `FadeSetDirty`-gated clear above. That branch must therefore also clear `GeomCache` directly, reset `FadeSetDirty = false;`, and set `FadeResetPending = false;` (Ruling F9a) — it cannot rely on the end-of-`Update` logic it never reaches.

- [ ] **Step 4: Publish the constant per draw**

In `TESReloaded/Core/RenderHook.cpp`, inside `TrackSetupShaderPrograms`, in the `if (VertexShader && PixelShader) {` block, immediately after the existing `if (PixelShader->ShaderProg && PixelShader->isSkin)` block (around line 531), add:

```cpp
		// LOD dither fade. Only pixel shaders that declare TESR_GEOM_FadeParams carry the clip, and
		// the whole block is inert unless a fade is actually in flight. Covered draws that are NOT
		// fading are explicitly given 1.0, so a fading draw's alpha cannot leak into the next draw.
		if (PixelShader->ShaderProg && PixelShader->ShaderProg->HasFadeParams &&
			TheSettingManager->SettingsMain.LODFade.Enabled &&
			(TheLODFadeManager->AnyFadesLive() || TheLODFadeManager->FadeResetPending)) {
			LODFadeManager::FadeRecord* Record = TheLODFadeManager->ResolveGeometry(Geometry);
			float Alpha = Record ? TheLODFadeManager->GetAlpha(Record) : 1.0f;
			TheShaderManager->ShaderConst.LODFade.Params.x = Alpha;
			TheShaderManager->ShaderConst.LODFade.Params.y = TheLODFadeManager->DitherSeed;
			TheShaderManager->ShaderConst.LODFade.Params.z = (Record && Record->Invert) ? 1.0f : 0.0f;
			PixelShader->ShaderProg->SetPerGeomCT();
		}
```

- [ ] **Step 5: Add the two members that block references**

`FadeResetPending` guarantees one final frame that pushes `1.0` into every covered shader after the last fade retires, so nothing is left clipped. `Invert` is declared now and only used in Task 8, so the draw hook does not have to be reopened later.

In `LODFadeManager.h`, add `bool Invert;` to `FadeRecord` and, to the public section:

```cpp
	/// Set for one frame after the last fade retires, so every covered shader is reset to opaque.
	bool			FadeResetPending;
```

Initialise `FadeResetPending = false;` in the constructor, set `Record.Invert = false;` in `AddFade`, and at the end of `Update` add:

```cpp
	FadeResetPending = (LiveCount == 0 && PrevLive > 0);
```

**Ruling F9b:** `ShaderRecord::SetPerGeomCT()` publishes every per-geom constant the bound shader declares, not just the one its caller cared about. `RenderHook.cpp`'s `isSkin` block (Task 5 Step 4's neighbour, unconditional, outside the LODFade gate) calls `SetPerGeomCT()` on any shader that also happens to declare `TESR_GEOM_FadeParams`, republishing whatever `ShaderConst.LODFade.Params` currently holds — even with the LODFade gate shut. If the `!Player || !Tes` guard bails mid-fade, that stale sub-1.0 alpha would otherwise sit there forever with no live fade left to reset it. So "no fade in flight" must mean the published constant is always fully opaque, not merely that the gate is shut. Add, immediately after the `FadeResetPending` line above:

```cpp
	if (LiveCount == 0) {
		TheShaderManager->ShaderConst.LODFade.Params.x = 1.0f;
		TheShaderManager->ShaderConst.LODFade.Params.z = 0.0f;
	}
```

and the same two assignments in the `!Player || !Tes` guard branch (Task 4 Step 4 / Step 3 above), since that path returns before reaching this line. `FadeResetPending` then becomes a belt-and-braces optimisation (skip the gate quickly once opaque) rather than the sole guarantee of opacity.

- [ ] **Step 6: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 7: Verify in game**

Launch, load an exterior save, walk across two cell boundaries.
Expected: still **no** visual change, because no shader declares the constant yet, so `HasFadeParams` is false everywhere and the block never runs. No crash and no frame-rate change. This confirms the hook is correctly gated before any shader depends on it.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/LODFadeManager.h TESReloaded/Core/LODFadeManager.cpp TESReloaded/Core/RenderHook.cpp
git commit -m "feat(LODFade): Resolve geometry to fade records and publish the constant per draw"
```

---

### Task 6: The shader include, proven on one shader

**Files:**
- Create: `OblivionReloaded/Shaders/Includes/LODFade.hlsl`
- Modify: `OblivionReloaded/Shaders/ExtraShaders/DISTLOD2001.pso.hlsl`

**Interfaces:**
- Consumes: `TESR_GEOM_FadeParams` at `c110` from Task 2, published per draw by Task 5.
- Produces: `void LODFadeClip(float2 vpos)` — clips the pixel when it falls outside the current fade coverage.

This task deliberately covers **one** shader. It proves the include path, the `VPOS` semantic, the register choice and the end-to-end fade before 40-odd files are touched. `DISTLOD2001.pso` is the right one: it draws the distant statics that Task 4's distant-grid poller already fades.

- [ ] **Step 1: Write the include**

Create `OblivionReloaded/Shaders/Includes/LODFade.hlsl`:

```hlsl
// Screen-space dither fade for LOD and full-model load transitions.
// x = fade alpha, y = per-frame seed, z = invert flag. c100 is TESR_GEOM_Toggles.
float4 TESR_GEOM_FadeParams : register(c110);

// Clips the pixel unless it falls inside the current fade coverage. The invert flag selects the
// complementary threshold, so a paired in-fade and out-fade together always cover exactly 100%.
// Branches around the hash so fully-settled draws (the common case) pay no per-pixel ALU cost.
void LODFadeClip(float2 vpos) {
    float a = TESR_GEOM_FadeParams.x;
    float z = TESR_GEOM_FadeParams.z;
    [branch]
    if (a < 1.0 || z > 0.5) {
        float n = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + TESR_GEOM_FadeParams.y);
        clip(z > 0.5 ? (n - a) : (a - n));
    }
}
```

- [ ] **Step 2: Apply it to DISTLOD2001**

In `OblivionReloaded/Shaders/ExtraShaders/DISTLOD2001.pso.hlsl`, add the include immediately after the existing `sampler2D DiffuseMap : register(s0);` declaration:

```hlsl
#include "../Includes/LODFade.hlsl"
```

add `vpos` to the input struct:

```hlsl
struct VS_OUTPUT {
    float2 DiffuseUV : TEXCOORD0;			// partial precision
    float3 texcoord_4 : TEXCOORD4_centroid;			// partial precision
    float4 texcoord_5 : TEXCOORD5;			// partial precision
    float4 color_0 : COLOR0;
    float2 vpos : VPOS;
};
```

and call the clip as the first statement of `main`:

```hlsl
PS_OUTPUT main(VS_OUTPUT IN) {
    PS_OUTPUT OUT;

    LODFadeClip(IN.vpos);

    float3 q0;
    float4 r0;
```

- [ ] **Step 3: Validate with fxc**

```bash
"C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x64\fxc.exe" /T ps_3_0 /E main /I "OblivionReloaded/Shaders/ExtraShaders" "OblivionReloaded/Shaders/ExtraShaders/DISTLOD2001.pso.hlsl" /Fc "%TEMP%/claude/distlod.asm"
```

Expected: exit 0, `compilation succeeded`.

- [ ] **Step 4: Confirm the register landed at c110**

```bash
grep -n "c110" "%TEMP%/claude/distlod.asm"
```

Expected: at least one match, and the `// Registers:` comment block in the generated assembly lists `TESR_GEOM_FadeParams` at `c110`. If it landed elsewhere the explicit register was ignored and the draw hook will write to the wrong slot — stop and fix before proceeding.

- [ ] **Step 5: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors. The DLL is unchanged in behaviour but the shader source has changed.

- [ ] **Step 6: Verify in game**

Set `[Develop] CompileShaders=1` **and** `[Develop] LogLODFade=1` in the game folder's `OblivionReloaded.ini`. The recompile gate is mandatory: without it the cached extension-less binary beside the `.hlsl` is loaded and the edit is silently ignored.

Launch, load an exterior save, and check the load log for `TESR_GEOM_FadeParams` (the `CreateCT` logging prints every `TESR_` constant it binds). Then walk in a straight line across two cell boundaries and watch the horizon.

Expected: distant statics now **dissolve in** over about one second instead of appearing instantly. With TAA on the dissolve should look smooth; with TAA off it will look like crawling noise, which is expected and accepted.

Failure triage:
- No `TESR_GEOM_FadeParams` in the load log means the shader did not recompile — `CompileShaders` was not set.
- Distant statics permanently invisible means `Params.x` is not reaching `1.0` in the steady state — check Task 2 Step 5's constructor default.
- Distant statics flicker permanently means `FadeResetPending` never fires or the fades never retire.

- [ ] **Step 7: Commit**

```bash
git add OblivionReloaded/Shaders/Includes/LODFade.hlsl OblivionReloaded/Shaders/ExtraShaders/DISTLOD2001.pso.hlsl
git commit -m "feat(LODFade): Add LODFade.hlsl dither include, applied to DISTLOD2001"
```

---

### Task 7: Roll the clip out to the remaining covered shaders

**Files:**
- Modify: `OblivionReloaded/Shaders/ExtraShaders/*.pso.hlsl` — `SLS1003`, `SLS1004`, `SLS1006`, `SLS2000`, `SLS2002`, `SLS2003`, `SLS2005`, `SLS2008`, `SLS2009`, `SLS2010`, `SLS2011`, `SLS2012`, `SLS2013`, `SLS2014`, `SLS2015`, `SLS2016`, `SLS2018`, `SLS2019`, `SLS2020`, `SLS2021`, `SLS2022`, `SLS2033`, `SLS2034`, `SLS2039`, `SM3000`, `SM3001`, `SM3003`, `SM3LL000`, `SM3LL001`, `SM3LL003`, `SM3LL010`, `SM3LL011`, `SM3LL013`, `STLEAF2000`, `STLEAF2001`
- Modify: `OblivionReloaded/Shaders/Terrain/` — `SLS2001.pso.hlsl`, `SLS2048.pso.hlsl`, `SLS2049.pso.hlsl`, `SLS2068.pso.hlsl`
- Modify: `OblivionReloaded/Shaders/POM/PAR*.pso.hlsl` and `OblivionReloaded/Shaders/POMExterior/*.pso.hlsl`

**Interfaces:**
- Consumes: `LODFadeClip` from Task 6.
- Produces: nothing new.

Every file gets exactly the same three edits as Task 6 Step 2: the `#include`, a `float2 vpos : VPOS;` member on the pixel shader's input struct, and `LODFadeClip(IN.vpos);` as the first statement of `main`. Two things vary and must be checked per file:

1. **The include path is relative to the file's own directory.** `ExtraShaders`, `Terrain`, `POM` and `POMExterior` are all one level below `Shaders`, so `#include "../Includes/LODFade.hlsl"` is correct for all of them. Files inside a `Includes` subdirectory would need a different path — none in this list are.
2. **The input struct name varies** (`VS_OUTPUT` in most, but confirm per file) and a few shaders already consume a `VPOS`-like value. Read each file before editing; do not pattern-replace blind.

- [ ] **Step 1: Enumerate the exact file list**

```bash
ls OblivionReloaded/Shaders/ExtraShaders/*.pso.hlsl OblivionReloaded/Shaders/Terrain/*.pso.hlsl OblivionReloaded/Shaders/POM/*.pso.hlsl OblivionReloaded/Shaders/POMExterior/*.pso.hlsl
```

Exclude from the result: `DISTLOD2001.pso.hlsl` (done in Task 6), `SKYTEX.pso.hlsl` (sky, not a fading object), and anything under `Blood`, `Water`, `Grass`, `Skin`, `SkinVanilla`, `NightEye`, `Shadows`, `Depth` — all out of scope per the spec.

- [ ] **Step 2: Apply the three edits to each file**

For each file, add after the last `sampler2D`/`float4` declaration at the top:

```hlsl
#include "../Includes/LODFade.hlsl"
```

add to the pixel shader's input struct:

```hlsl
    float2 vpos : VPOS;
```

and add as the first statement of `main`, before any local declarations:

```hlsl
    LODFadeClip(IN.vpos);
```

- [ ] **Step 3: Validate every touched shader with fxc**

```bash
for f in OblivionReloaded/Shaders/ExtraShaders/*.pso.hlsl OblivionReloaded/Shaders/Terrain/*.pso.hlsl OblivionReloaded/Shaders/POM/*.pso.hlsl OblivionReloaded/Shaders/POMExterior/*.pso.hlsl; do
  d=$(dirname "$f")
  "C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x64\fxc.exe" /T ps_3_0 /E main /I "$d" "$f" /Fo NUL >/dev/null 2>&1 || echo "FAILED: $f"
done
```

Expected: no `FAILED:` lines. Some files emit pre-existing `X3570` warnings; those are not failures and predate this work.

- [ ] **Step 4: Check the instruction budget did not overflow**

The clip adds roughly 8 instructions. `SLS2018` and `SLS2039` are the largest shaders in the set. Recompile those two with `/Fc` and confirm the reported instruction count is still under the ps_3_0 limit:

```bash
"C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x64\fxc.exe" /T ps_3_0 /E main /I "OblivionReloaded/Shaders/ExtraShaders" "OblivionReloaded/Shaders/ExtraShaders/SLS2018.pso.hlsl" /Fc "%TEMP%/claude/sls2018.asm" && tail -3 "%TEMP%/claude/sls2018.asm"
```

Expected: a trailing `// approximately N instruction slots used` comment, compilation succeeded.

- [ ] **Step 5: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 6: Verify in game**

With `CompileShaders=1` still set, launch and load an exterior save.

Expected: everything renders normally in the steady state — this is the critical check, because a mistake in any one of 44 files shows up as that object type being invisible or permanently stippled. Walk around for two minutes looking specifically at: rocks and buildings (`SLS2xxx`), trees and foliage (`STLEAF*`), parallax-mapped surfaces (`PAR*`), and terrain.

Then cross a `LandLOD` quadrant boundary and confirm the terrain quadrant dissolves rather than pops.

- [ ] **Step 7: Commit**

```bash
git add OblivionReloaded/Shaders/
git commit -m "feat(LODFade): Apply the dither clip to the static, tree and terrain shaders"
```

---

### Task 8: Pinning and paired handoff

**Files:**
- Modify: `TESReloaded/Core/LODFadeManager.h` (pin state, cell poller, pairing)
- Modify: `TESReloaded/Core/LODFadeManager.cpp` (pin, unpin, cell poller, pairing)

**Interfaces:**
- Consumes: everything from Tasks 3-5; `FadeRecord::Invert` and `FadeResetPending` declared in Task 5.
- Produces:
  - `void LODFadeManager::PollCellGrid();`
  - `bool LODFadeManager::Pin(FadeRecord* Record);`
  - `void LODFadeManager::Unpin(FadeRecord* Record);`
  - `NiNode* LODFadeManager::HolderNode;` — plugin-owned parent for re-attached nodes.

This is the risky half of the feature, and it is last for that reason: everything before it is useful on its own. It is gated on `SettingsMain.LODFade.PinDeparting`, so it can be switched off in the field without losing Tasks 4-7.

**This task implements only the un-cull pin** — the case where the departing node still has an `m_parent`. The re-attach path for genuinely detached nodes is Task 9, and whether it is needed at all is decided by this task's measurement. That split is deliberate: un-culling touches no lifetimes beyond a refcount, while re-attachment manipulates the live scene graph, and there is no reason to risk the second before knowing whether the engine ever takes that path.

Engine data: `Tes->gridCellArray` (`Game.h:8125`) is a `GridCellArray` whose `GridEntry` holds a `TESObjectCELL* cell` and a `CellInfo* info` with `NiNode* niNode`.

**Member names matter here.** The Oblivion `GridCellArray` exposes `UInt32 size` and `GridEntry* grid`. The `gridSize` / `gridCells` names belong to the *NewVegas* block earlier in the same header (`Game.h:4133`) and will not compile against the Oblivion target. Slot count is `size * size`.

Refcounting follows the pattern already used in `TESReloaded/Core/Animation.cpp:54,84`:

```cpp
	InterlockedIncrement(&Node->m_uiRefCount);
	if (!InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);
```

- [ ] **Step 1: Add pin state**

In `LODFadeManager.h`, add to `FadeRecord`:

```cpp
		NiNode*		OriginalParent;
```

and to the public section:

```cpp
	/// Keeps a departing node alive and drawn for the fade duration. Returns false if it could not
	/// be held, in which case the caller must not start a fade for it.
	bool			Pin(FadeRecord* Record);

	/// Releases a pin, restoring the cull flag and dropping the reference taken by Pin.
	void			Unpin(FadeRecord* Record);
```

and to the private section:

```cpp
	std::vector<NiAVObject*>	PrevCell;

	void			PollCellGrid();
```

- [ ] **Step 2: Implement Pin and Unpin**

```cpp
bool LODFadeManager::Pin(FadeRecord* Record) {

	NiAVObject* Node = Record->Root;
	if (!Node) return false;

	Record->OriginalParent = Node->m_parent;

	if (!Node->m_parent) {
		// Already detached from the graph. Re-attaching it is Task 9 and is not attempted here;
		// log it so the measurement in Step 9 can tell us whether that path is needed at all.
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] pin declined, node already detached root=%08X", (UInt32)Node);
		return false;
	}

	// Still in the graph: un-culling is all that is needed, and it touches no lifetime but the
	// refcount. The reference stops the engine freeing the node while we are still drawing it.
	InterlockedIncrement(&Node->m_uiRefCount);
	Node->m_flags &= (UInt16)~NiAVObject::kFlag_AppCulled;
	Record->Pinned = true;

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] pin root=%08X", (UInt32)Node);

	return true;

}

void LODFadeManager::Unpin(FadeRecord* Record) {

	NiAVObject* Node = Record->Root;
	if (!Node || !Record->Pinned) return;

	Record->Pinned = false;
	if (!InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] unpin root=%08X", (UInt32)Node);

}
```

- [ ] **Step 3: Unpin on retire, including the timeout path**

Replace `Retire` with:

```cpp
void LODFadeManager::Retire(UInt32 Index) {

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] retire root=%08X pinned=%d", (UInt32)Fades[Index].Root, Fades[Index].Pinned ? 1 : 0);

	if (Fades[Index].Pinned) Unpin(&Fades[Index]);
	Fades.erase(Fades.begin() + Index);

}
```

The hard timeout is already structural: `Update`'s retire loop fires on elapsed time alone and does not consult the partner, so a pin whose partner never completes is released at `FadeTime` regardless. Add the spec's explicit 2x safety by changing the retire condition to account for pinned records held open by a partner:

```cpp
	for (SInt32 i = (SInt32)Fades.size() - 1; i >= 0; i--) {
		float Elapsed = CurrentTime - Fades[i].StartTime;
		bool HardTimeout = Elapsed >= FadeTime * 2.0f;
		bool Complete = Elapsed >= FadeTime;
		if (HardTimeout || (Complete && !Fades[i].HasPartner)) Retire(i);
	}
```

- [ ] **Step 4: Implement the cell-grid poller with pairing**

```cpp
void LODFadeManager::PollCellGrid() {

	GridCellArray* Grid = Tes->gridCellArray;
	if (!Grid) return;

	UInt32 Dim = Grid->size;
	UInt32 Slots = Dim * Dim;
	if (!Slots || !Grid->grid) return;

	if (PrevCell.size() != Slots) {
		PrevCell.assign(Slots, NULL);
		return;
	}

	// Crossing one boundary shifts Dim slots and a corner shifts 2*Dim-1, so 2*Dim sits one above
	// the worst legitimate case and far below a full reload of Dim squared.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Slots; i++) {
		GridCellArray::GridEntry* Entry = &Grid->grid[i];
		NiAVObject* Node = Entry->info ? (NiAVObject*)Entry->info->niNode : NULL;
		if (Node != PrevCell[i]) Changed++;
	}

	if (Changed > Dim * 2) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] cell discontinuity: %d of %d slots changed, suppressed", Changed, Slots);
		for (UInt32 i = 0; i < Slots; i++) {
			GridCellArray::GridEntry* Entry = &Grid->grid[i];
			PrevCell[i] = Entry->info ? (NiAVObject*)Entry->info->niNode : NULL;
		}
		return;
	}

	for (UInt32 i = 0; i < Slots; i++) {
		GridCellArray::GridEntry* Entry = &Grid->grid[i];
		NiAVObject* Node = Entry->info ? (NiAVObject*)Entry->info->niNode : NULL;
		if (Node == PrevCell[i]) continue;

		if (Node && !RootIndex.count(Node)) {
			// Cell gained: full models fade in. The paired LOD node is pinned by the distant
			// poller's own slot change in the same frame, so no cross-poller lookup is needed.
			AddFade(Node, FadeDir_In);
		}
		else if (!Node && PrevCell[i] && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
			// Cell lost: hold the departing full models while the LOD fades back in.
			FadeRecord* Record = AddFade(PrevCell[i], FadeDir_Out);
			if (Record && !Pin(Record)) Retire((UInt32)(Fades.size() - 1));
		}
		PrevCell[i] = Node;
	}

}
```

Call it from `Update` immediately after `PollDistantGrid();`.

- [ ] **Step 5: Pin departing distant nodes**

In `PollDistantGrid`, extend the change branch so a slot losing its node fades out instead of vanishing:

```cpp
		if (Node != PrevDistant[i]) {
			if (Node && !RootIndex.count(Node)) {
				AddFade(Node, FadeDir_In);
			}
			else if (!Node && PrevDistant[i] && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
				FadeRecord* Record = AddFade(PrevDistant[i], FadeDir_Out);
				if (Record && !Pin(Record)) Retire((UInt32)(Fades.size() - 1));
			}
			PrevDistant[i] = Node;
		}
```

- [ ] **Step 6: Cross-dither the replaced LandLOD quadrant**

In `PollLandLOD`, when a slot's pointer changes from one non-NULL node to another, the old quadrant is the in-fade's partner and must run on the partner's rising alpha with `Invert` set, so the two together cover exactly 100%:

```cpp
		if (Node != PrevLandLOD[i]) {
			bool Paired = false;
			if (Node && !RootIndex.count(Node)) {
				AddFade(Node, FadeDir_In);
				Paired = true;
			}
			if (PrevLandLOD[i] && Paired && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
				FadeRecord* Out = AddFade(PrevLandLOD[i], FadeDir_In);
				if (Out) {
					// Runs on the partner's rising alpha with the complementary threshold, so it is
					// FadeDir_In with Invert set, not FadeDir_Out. Same StartTime as the partner,
					// so no cross-record lookup is needed to stay in step with it.
					Out->Invert = true;
					Out->HasPartner = true;
					if (!Pin(Out)) Retire((UInt32)(Fades.size() - 1));
					else Fades[Fades.size() - 2].HasPartner = true;
				}
			}
			PrevLandLOD[i] = Node;
		}
```

- [ ] **Step 7: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 8: Verify in game, safe configuration first**

Set `[LODFade] PinDeparting=0` and confirm Tasks 4-7 still behave exactly as they did after Task 7 — distant statics and terrain quadrants dissolve in, nothing is invisible, no crash. This proves the kill switch works before the risky path is exercised.

- [ ] **Step 9: Verify in game, pinning enabled**

Set `[LODFade] PinDeparting=1`. Save first — this step can crash.

1. Walk across a cell boundary. Expected: `[LODFade] pin` lines, each followed by `[LODFade] unpin` within about a second, and the LOD-to-full handoff dissolves rather than popping.
2. Ride away from a loaded area on horseback for two minutes. Expected: full-to-LOD handoffs dissolve.
3. Fast-travel repeatedly between distant cities, five times. Expected: the discontinuity guard fires each time and no pins are taken.
4. Play normally for ten minutes in the Great Forest.

**This step is also the measurement that decides Task 9.** Count the two log lines:

```bash
grep -c "LODFade\] pin root" <logfile>
grep -c "pin declined, node already detached" <logfile>
```

If `declined` is zero or negligible, the engine expresses removal as a cull flag, Task 9 is unnecessary, and this feature is complete. If `declined` dominates, the engine detaches, the fade-out half is mostly not running, and Task 9 is required to deliver it.

Any crash here: capture the log tail, set `PinDeparting=0` to confirm it is the pin path, and report before changing anything. Per the spec's risk section, if refcounting proves insufficient the correct outcome is to ship with `PinDeparting` defaulting to `0`, not to force it.

- [ ] **Step 10: Commit**

```bash
git add TESReloaded/Core/LODFadeManager.h TESReloaded/Core/LODFadeManager.cpp
git commit -m "feat(LODFade): Add pinning, cell-grid polling and paired handoff"
```

---

### Task 9: Re-attach path for detached nodes — CONDITIONAL

**Run this task only if Task 8 Step 9's measurement showed `pin declined, node already detached` dominating.** If the engine culls rather than detaches, this task is dead code and must not be written.

**Files:**
- Modify: `TESReloaded/Core/LODFadeManager.h` (holder node)
- Modify: `TESReloaded/Core/LODFadeManager.cpp` (`Pin` / `Unpin` re-attach branch)

**Interfaces:**
- Consumes: `Pin`, `Unpin`, `FadeRecord::OriginalParent` from Task 8.
- Produces: `NiNode* LODFadeManager::HolderNode;`

Verified engine facts for this task: `WorldSceneGraph` is a `SceneGraph*` (`Game.h:13013`), and `SceneGraph` derives from `NiNode` in the Oblivion block (`GameNi.h:1994`), so `AddObject` / `RemoveObject` are available on it. `NiNode::New(UInt16)` is `ThisCall(0x0070B780, ...)` and `MemoryAlloc` for the Oblivion target is at `Game.h:8866`.

- [ ] **Step 1: Add the holder node**

In `LODFadeManager.h`, add to the private section:

```cpp
	NiNode*			HolderNode;
```

and initialise `HolderNode = NULL;` in the constructor.

- [ ] **Step 2: Replace Pin's decline branch with re-attachment**

Replace the `if (!Node->m_parent) { ... return false; }` block written in Task 8 with:

```cpp
	if (!Node->m_parent) {
		// Detached already. Re-attach under a plugin-owned holder so it culls and draws normally.
		if (!HolderNode) {
			HolderNode = (NiNode*)MemoryAlloc(sizeof(NiNode));
			if (!HolderNode) return false;
			HolderNode->New(8);
			InterlockedIncrement(&HolderNode->m_uiRefCount);
			WorldSceneGraph->AddObject(HolderNode, 1);
		}
		InterlockedIncrement(&Node->m_uiRefCount);
		HolderNode->AddObject(Node, 1);
		Node->m_flags &= (UInt16)~NiAVObject::kFlag_AppCulled;
		Record->Pinned = true;
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] pin reattached root=%08X", (UInt32)Node);
		return true;
	}
```

- [ ] **Step 3: Detach on unpin**

In `Unpin`, before the `InterlockedDecrement`, add:

```cpp
	if (!Record->OriginalParent && HolderNode) {
		NiAVObject* Removed = NULL;
		HolderNode->RemoveObject(&Removed, Node);
	}
```

- [ ] **Step 4: Build**

Run the build command via the PowerShell tool.
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 5: Verify in game**

Save first — this is the highest-risk step in the plan. With `PinDeparting=1`, ride away from a loaded area for two minutes and confirm `[LODFade] pin reattached` lines appear, each followed by an `unpin`, and that full-to-LOD handoffs now dissolve. Then play for ten minutes.

If this crashes, set `PinDeparting=0`, confirm stability returns, and ship with that default. Per the spec's risk section, a working fade-in half with `PinDeparting` defaulting to `0` is an acceptable outcome; forcing the re-attach path is not.

- [ ] **Step 6: Commit**

```bash
git add TESReloaded/Core/LODFadeManager.h TESReloaded/Core/LODFadeManager.cpp
git commit -m "feat(LODFade): Re-attach detached nodes for the duration of a fade-out"
```

---

## Notes carried from the spec that no single task owns

- **LOD trees are unverified.** `DistantRefLOD[0]` ("LOD Trees") may bind a shader outside the Task 7 list. During Task 7 Step 6, look specifically at distant tree billboards; if they pop while distant rocks dissolve, capture the bound shader name with `[Develop] TraceShaders=1` and add that shader to the covered set.
- **TAA history rejection is unverified.** Dithered coverage changes every frame by construction. If Task 6 Step 6 shows the fade flickering rather than resolving smoothly under TAA, the dither pattern is the suspect, not the manager.
- **`uGridsToLoad` is assumed to be the default 5** in the cell discontinuity threshold, but the code reads `Grid->size` rather than hardcoding it, so a modified INI is handled.
- **Struct-name hazard.** `Game.h` and `GameNi.h` each define the same class names two or three times, once per game target. Oblivion is `Game.h:5084-8872` and `GameNi.h:1822-3610`. Reading a member name out of the wrong block compiles only if that name happens to exist in both, so confirm the line number of any struct you consult falls inside those ranges. The `gridSize` / `gridCells` versus `size` / `grid` mismatch on `GridCellArray` is exactly this trap.
