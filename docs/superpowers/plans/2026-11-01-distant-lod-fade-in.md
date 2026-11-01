# Distant LOD Fade-In Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Distant LOD objects that first appear at the far edge of the distant grid dither in over `FadeTime` instead of popping.

**Architecture:** A new `LODFadeManager` diffs `DistantRefLOD`'s children once per frame and starts a timed fade for each far-edge arrival. The per-draw hook uploads each covered draw's remaining fade into one pixel-shader register (`TESR_GEOM_FadeParams`, c110), and a shared HLSL include turns it into a screen-space dither `clip()`. Which pixel shaders get the clip is decided by an in-game census before any shader is edited.

**Tech Stack:** C++ (MSVC v145, `stdcpplatest`), Direct3D 9, HLSL ps_3_0, OBSE plugin, Microsoft Detours.

**Spec:** `docs/superpowers/specs/2026-11-01-distant-lod-fade-in-design.md`

## Global Constraints

- Build only through the solution, from the **PowerShell** tool (never Bash; its `TEMP` produces a fake MSB3073):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
- Oblivion-only code goes inside `#if defined(OBLIVION)` in shared files. `Game.h` / `GameNi.h` define structs three times; OBLIVION is the **middle** block.
- `TESR_GEOM_FadeParams` is `register(c110)`. `.x` is the **remaining** fade (`1 - alpha`), where **0 = opaque**. `.y` is the per-frame dither seed. Never invert this encoding: D3D9 starts constants at 0, and 0 must never hide geometry.
- Every draw whose pixel shader has `HasFadeParams` uploads that one register with `SetPixelShaderConstantF`, every time. Never use `SetPerGeomCT()` for it, which would re-upload the shader's other per-geometry constants with values that may be stale.
- Nothing in `LODFadeManager` reference-counts, frees, attaches, detaches or writes any engine object. Fade roots are raw pointers, trusted only while they are in the current frame's `DistantRefLOD` child set.
- Never read `Tes->gridDistantArray` (unverified layout; it caused arbitrary memory writes on the old branch).
- Dither hash is interleaved gradient noise inside `[branch]`, never a `sin` hash (blows the ps_3_0 512-slot limit, and `fxc` does not fail on that).
- Fade time comes from `TheFrameRateManager->GetPerformance()` (ms). `FrameRateManager::ElapsedTime` is always 0.
- Coding style: public symbols get `///` doc comments; inline comments only when necessary, 1–3 lines.
- Shader `.hlsl` edits only take effect after a recompile (`[Develop] CompileShaders = 1`); compiled `.pso` files are gitignored.
- Commits: conventional style `feat(LODFade): ...`. Changes to memory and design docs go in their own `docs:` commits.

**User decisions (already made):**
- Scope: distant LOD fade-**in** only. No fade-out, no LOD→full-model handoff, no LandLOD terrain.
- Approach A: per-draw, node-keyed dither, porting only the old branch's proven distant tier. Instanced LOD trees may still pop in v1.
- Only far-edge arrivals fade; LOD in the ring next to the loaded grid stays instant (fading it opens a hole when a cell unloads).
- Covered shaders are decided by an in-game census the user runs, not guessed.
- Defaults: `[LODFade] Enabled=1, FadeTime=1.0, MaxFades=256`; `[Develop] LogLODFade=0`.

## Review Focus

1. **Main menu ↔ save load, and load from one save to another.** `Player`/`Tes` go NULL and `DistantRefLOD` may be a new node. Expected: no crash, no stale fade carried into the new world, and the first frame after load fades nothing (discontinuity).
2. **Fast travel / `coc` teleport.** Expected: the whole child set churns, a discontinuity is logged, and nothing dithers.
3. **Crossing a cell boundary next to the loaded grid.** Expected: LOD replacing a just-unloaded cell appears instantly (`skip` in the log), never fading in over a hole.
4. **A covered shader shared with full-resolution models.** Expected: full-res geometry never dithers (it is never under a fade root) and its other per-geometry constants (`TESR_GEOM_Toggles`, `TESR_GEOM_Wetness`) are unchanged.
5. **Feature disabled (`Enabled=0` or `FadeTime<=0`) with the clip compiled in.** Expected: the hook still uploads 0, so covered geometry is fully opaque.

Each line is exercised by an in-game check in Task 4 (no automated test framework exists in this repo).

---

## File Structure

| File | Responsibility |
|---|---|
| `TESReloaded/Core/LODFadeManager.h/.cpp` (new) | Detection, fade table, per-geometry fade lookup, shader census |
| `TESReloaded/Core/SettingManager.h/.cpp` | `[LODFade]` settings and `Develop.LogLODFade` |
| `TESReloaded/Core/Managers.h/.cpp` | Singleton declaration, conditional creation |
| `TESReloaded/Core/RenderHook.cpp` | Per-frame `Update()` call; per-draw census call and fade upload |
| `TESReloaded/Core/ShaderManager.h/.cpp` | `ShaderConst.LODFade`, `HasFadeParams`/`FadeParamsRegister`, constructor fix |
| `OblivionReloaded/Shaders/ExtraShaders/Includes/LODFade.hlsl` (new) | `LODFadeClip()` dither |
| census-listed `*.pso.hlsl` | include + `VPOS` + clip call |
| `OblivionReloaded/OblivionReloaded.vcxproj(.filters)` | Register the new source files |

---

### Task 1: LODFadeManager detection, fade table and settings

**Goal:** A per-frame manager that detects far-edge `DistantRefLOD` arrivals, runs a fade table, and logs every decision, configured from `[LODFade]`.

**Files:**
- Create: `TESReloaded/Core/LODFadeManager.h`, `TESReloaded/Core/LODFadeManager.cpp`
- Modify: `TESReloaded/Core/SettingManager.h` (after `FlyCamStruct` ~line 280; `DevelopStruct` ~line 294; members ~line 311)
- Modify: `TESReloaded/Core/SettingManager.cpp` (after the FlyCam reads ~line 431; after `LogWetActors` ~line 445)
- Modify: `TESReloaded/Core/Managers.h`, `TESReloaded/Core/Managers.cpp`
- Modify: `TESReloaded/Core/RenderHook.cpp:474` (Oblivion `TrackRender`)
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj`, `OblivionReloaded/OblivionReloaded.vcxproj.filters`

**Acceptance Criteria:**
- [ ] `DistantRefLOD` resolved each frame by name among `Tes->LODRoot->m_parent`'s children; `gridDistantArray` never read
- [ ] Set diff over non-NULL children scanned to `min(end, capacity)`
- [ ] Arrivals fade only at ring `>= *SettingGridsToLoad / 2 + 2`; others log `skip`
- [ ] `Arrived + Departed > Count / 4`, or no previous set, adopts silently (logs `discontinuity`)
- [ ] Unready arrivals (radius ≤ 0, or node with no children) are queued, promoted when ready, dropped after 8 s or on leaving the set
- [ ] Records are dropped the frame their root leaves the set; retire at alpha 1; table capped at `MaxFades`
- [ ] `Player`/`Tes` NULL clears all state; changing `DistantRefLOD` pointer clears all state
- [ ] No refcount or scene-graph writes anywhere in the manager
- [ ] Manager only created when `Enabled && FadeTime > 0`
- [ ] Solution builds with 0 errors

**Verify:** the MSBuild command from Global Constraints → `Build succeeded`, 0 errors, and `OblivionReloaded\Release\OblivionReloaded.dll` has a fresh timestamp.

**Steps:**

- [ ] **Step 1: Add the settings**

In `TESReloaded/Core/SettingManager.h`, after `struct FlyCamStruct { ... };` inside `SettingsMainStruct`:

```cpp
	/// Dithers distant LOD in when it first appears at the far edge of the distant grid.
	struct LODFadeStruct {
		bool	Enabled;
		float	FadeTime;	// seconds; <= 0 disables the feature
		UInt32	MaxFades;
	};
```

In `DevelopStruct`, after `UInt8 LogWetActors; ...`:

```cpp
		UInt8	LogLODFade;				// logs distant LOD arrivals and a one-shot census of the shaders drawing them
```

In the member list, after `FlyCamStruct FlyCam;`:

```cpp
	LODFadeStruct				LODFade;
```

In `TESReloaded/Core/SettingManager.cpp`, after `SettingsMain.FlyCam.StepValue = atof(value);`:

```cpp

	SettingsMain.LODFade.Enabled = GetPrivateProfileIntA("LODFade", "Enabled", 1, Filename);
	GetPrivateProfileStringA("LODFade", "FadeTime", "1.0", value, SettingStringBuffer, Filename);
	SettingsMain.LODFade.FadeTime = atof(value);
	SettingsMain.LODFade.MaxFades = GetPrivateProfileIntA("LODFade", "MaxFades", 256, Filename);
```

After `SettingsMain.Develop.LogWetActors = ...;`:

```cpp
	SettingsMain.Develop.LogLODFade = GetPrivateProfileIntA("Develop", "LogLODFade", 0, Filename);
```

- [ ] **Step 2: Create `TESReloaded/Core/LODFadeManager.h`**

```cpp
#pragma once

#include <unordered_map>
#include <unordered_set>

/// Dithers newly streamed distant LOD in at the far edge of the distant grid.
///
/// Update() diffs DistantRefLOD's children once per frame and starts a timed fade for each far-edge
/// arrival. The draw hook asks GetFadeRemaining() for every covered draw and uploads the answer as
/// TESR_GEOM_FadeParams.x, which LODFade.hlsl turns into a screen-space dither clip.
/// Nothing here reference-counts or writes engine objects: roots are raw pointers, trusted only while
/// they are in the current frame's child set.
class LODFadeManager {
public:
	LODFadeManager();

	/// Once per frame, before any draw: resolves DistantRefLOD, diffs its children, starts and retires fades.
	void		Update();

	/// Remaining fade for a drawn geometry: 1 - alpha under a live fade root, otherwise 0 (opaque).
	float		GetFadeRemaining(NiAVObject* Geometry);

	/// Counts one draw by shader pair when Geometry sits under DistantRefLOD. Called only while CensusArmed.
	void		NoteCensusDraw(NiAVObject* Geometry, const char* PixelShaderName, const char* VertexShaderName, bool PixelHasProgram);

	/// Per-frame dither seed in [0, 1).
	float		DitherSeed;

	/// True while the one-shot shader census is collecting (Develop.LogLODFade only).
	bool		CensusArmed;

private:
	struct FadeRecord {
		NiAVObject*	Root;
		double		StartMs;
	};

	struct CensusEntry {
		const char*	PixelShader;
		const char*	VertexShader;
		bool		PixelHasProgram;
		bool		Tree;
		UInt32		Draws;
	};

	static const UInt32 kCensusMax = 64;

	static bool	IsNiNodeType(NiAVObject* Object);
	NiNode*		ResolveDistantRef();
	void		Reset();
	void		PollDistantRef();
	void		DropDeparted(const std::unordered_set<NiAVObject*>& Live);
	void		AddArrival(NiAVObject* Node);
	bool		IsReady(NiAVObject* Node);
	void		TryStartFade(NiAVObject* Node);
	void		DrainPending();
	void		RetireFinished();
	void		RebuildIndex();
	float		GetAlpha(const FadeRecord& Record) const;
	void		ReportCensus();

	std::vector<FadeRecord>						Fades;
	std::unordered_map<NiAVObject*, UInt32>		RootIndex;		// fade root -> index into Fades
	std::unordered_map<NiAVObject*, int>		GeometryCache;	// drawn geometry -> index into Fades, -1 = none
	std::unordered_set<NiAVObject*>				LiveChildren;	// DistantRefLOD's children as of the last poll
	std::unordered_set<NiAVObject*>				ScanChildren;	// scratch set for the current poll
	std::unordered_map<NiAVObject*, double>		Pending;		// unready arrival -> first-seen ms
	NiNode*										DistantRef;
	bool										PrevValid;
	bool										ResolveLogged;
	bool										MissingLogged;
	double										NowMs;

	CensusEntry									Census[kCensusMax];
	UInt32										CensusCount;
	UInt32										CensusOverflow;
	double										CensusStartMs;
};
```

- [ ] **Step 3: Create `TESReloaded/Core/LODFadeManager.cpp`**

```cpp
#include "LODFadeManager.h"
#include <cmath>
#include <cstdlib>

static const double	kPendingTimeoutMs	= 8000.0;
static const double	kCensusDelayMs		= 30000.0;
static const float	kCellSize			= 4096.0f;
static const UInt32	kMaxPending			= 256;
static const UInt32	kMaxParentDepth		= 10;
static const UInt32	kCensusMaxDepth		= 12;

LODFadeManager::LODFadeManager() {

	Logger::Log("Starting the LOD fade manager...");
	TheLODFadeManager = this;

	DitherSeed = 0.0f;
	CensusArmed = TheSettingManager->SettingsMain.Develop.LogLODFade != 0;
	CensusCount = 0;
	CensusOverflow = 0;
	CensusStartMs = 0.0;
	DistantRef = NULL;
	PrevValid = false;
	ResolveLogged = false;
	MissingLogged = false;
	NowMs = 0.0;

}

/// True when Object's runtime type derives from NiNode, decided by walking the NiRTTI chain.
/// Oblivion's NiObject has no GetAsNiNode slot, and a VFT whitelist misclassifies BSFadeNode and
/// friends; a broken chain answers "not a node", which only makes an arrival count as a leaf.
bool LODFadeManager::IsNiNodeType(NiAVObject* Object) {

	if (!Object) return false;

	NiRTTI* Type = Object->GetType();
	for (UInt32 Depth = 0; Type && Depth < 16; Depth++) {
		if (Type->name && !strcmp(Type->name, "NiNode")) return true;
		Type = Type->parent;
	}
	return false;

}

void LODFadeManager::Update() {

	if (!Player || !Tes) {
		Reset();
		return;
	}

	NowMs = TheFrameRateManager->GetPerformance();
	DitherSeed += 0.618034f;
	if (DitherSeed >= 1.0f) DitherSeed -= 1.0f;

	NiNode* Resolved = ResolveDistantRef();
	if (Resolved != DistantRef) {
		Reset();
		DistantRef = Resolved;
	}
	if (!DistantRef) return;

	PollDistantRef();
	DrainPending();
	RetireFinished();

	if (CensusArmed) {
		if (CensusStartMs == 0.0 && Player->IsExteriorLike()) CensusStartMs = NowMs;
		if (CensusStartMs != 0.0 && NowMs - CensusStartMs >= kCensusDelayMs) ReportCensus();
	}

}

/// Tes->LODRoot is misnamed: it is the LandLOD node, and its parent holds LandLOD, DistantRefLOD and
/// LODWaterRoot. Resolved by name every frame (three children), so a rebuilt container is picked up.
NiNode* LODFadeManager::ResolveDistantRef() {

	NiNode* LandLOD = Tes->LODRoot;
	NiNode* Container = LandLOD ? LandLOD->m_parent : NULL;
	NiNode* Found = NULL;

	if (Container) {
		NiTArray<NiAVObject*>& Children = Container->m_children;
		UInt32 Slots = Children.end < Children.capacity ? Children.end : Children.capacity;
		for (UInt32 i = 0; i < Slots && !Found; i++) {
			NiAVObject* Child = Children.data[i];
			if (Child && Child->m_pcName && !strcmp(Child->m_pcName, "DistantRefLOD")) Found = (NiNode*)Child;
		}
	}

	if (Found && !ResolveLogged && TheSettingManager->SettingsMain.Develop.LogLODFade) {
		ResolveLogged = true;
		Logger::Log("[LODFade] DistantRefLOD resolved name=%s children=%d", Found->m_pcName, Found->m_children.numObjs);
	}
	if (!Found && !MissingLogged) {
		MissingLogged = true;
		Logger::Log("[LODFade] DistantRefLOD not found; distant LOD will not fade until it is");
	}
	return Found;

}

void LODFadeManager::Reset() {

	Fades.clear();
	RootIndex.clear();
	GeometryCache.clear();
	Pending.clear();
	LiveChildren.clear();
	ScanChildren.clear();
	PrevValid = false;
	DistantRef = NULL;

}

/// Set-membership diff of DistantRefLOD's children. Scanned to `end`, not `numObjs`: removing a child
/// NULLs its slot without compacting. A large churn (teleport, fast travel) is adopted without fading.
void LODFadeManager::PollDistantRef() {

	ScanChildren.clear();
	NiTArray<NiAVObject*>& Children = DistantRef->m_children;
	UInt32 Slots = Children.end < Children.capacity ? Children.end : Children.capacity;
	if (ScanChildren.bucket_count() < Slots) ScanChildren.reserve(Slots);
	for (UInt32 i = 0; i < Slots; i++) {
		if (NiAVObject* Child = Children.data[i]) ScanChildren.insert(Child);
	}

	UInt32 Arrived = 0;
	UInt32 Departed = 0;
	for (NiAVObject* Child : ScanChildren) if (!LiveChildren.count(Child)) Arrived++;
	for (NiAVObject* Child : LiveChildren) if (!ScanChildren.count(Child)) Departed++;

	if (Departed) DropDeparted(ScanChildren);

	UInt32 Count = (UInt32)ScanChildren.size();
	if (!PrevValid || Arrived + Departed > Count / 4) {
		if (PrevValid && TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] discontinuity: %u arrived, %u departed of %u, not fading", Arrived, Departed, Count);
	}
	else if (Arrived) {
		for (NiAVObject* Child : ScanChildren) {
			if (!LiveChildren.count(Child)) AddArrival(Child);
		}
	}

	LiveChildren.swap(ScanChildren);
	PrevValid = true;

}

void LODFadeManager::DropDeparted(const std::unordered_set<NiAVObject*>& Live) {

	bool Dropped = false;
	for (size_t i = 0; i < Fades.size(); ) {
		if (Live.count(Fades[i].Root)) {
			i++;
			continue;
		}
		Fades[i] = Fades.back();
		Fades.pop_back();
		Dropped = true;
	}
	if (Dropped) RebuildIndex();

	for (auto it = Pending.begin(); it != Pending.end(); ) {
		if (Live.count(it->first)) ++it;
		else it = Pending.erase(it);
	}

}

void LODFadeManager::AddArrival(NiAVObject* Node) {

	if (IsReady(Node)) TryStartFade(Node);
	else if (Pending.size() < kMaxPending) Pending.emplace(Node, NowMs);

}

/// Drawable now: a valid world bound, and either a leaf or a node that already has children. The
/// engine sometimes attaches an empty container and streams the mesh in afterwards.
bool LODFadeManager::IsReady(NiAVObject* Node) {

	if (Node->m_kWorldBound.Radius <= 0.0f) return false;
	return !IsNiNodeType(Node) || ((NiNode*)Node)->m_children.numObjs != 0;

}

/// Starts a fade only for far-edge arrivals. The ring next to the loaded grid is LOD replacing a cell
/// that just unloaded; its full models are already gone, so fading it in would open a hole.
void LODFadeManager::TryStartFade(NiAVObject* Node) {

	NiPoint3& Center = Node->m_kWorldBound.Center;
	int dx = abs((int)floorf(Center.x / kCellSize) - (int)floorf(Player->pos.x / kCellSize));
	int dy = abs((int)floorf(Center.y / kCellSize) - (int)floorf(Player->pos.y / kCellSize));
	int Ring = dx > dy ? dx : dy;
	int MinRing = (int)(*SettingGridsToLoad / 2) + 2;
	bool Log = TheSettingManager->SettingsMain.Develop.LogLODFade != 0;
	const char* Name = (Node->m_pcName && Node->m_pcName[0]) ? Node->m_pcName : "?";

	if (Ring < MinRing) {
		if (Log) Logger::Log("[LODFade] skip %s@%08X ring=%d min=%d", Name, (UInt32)Node, Ring, MinRing);
		return;
	}
	if (RootIndex.count(Node)) return;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) {
		if (Log) Logger::Log("[LODFade] table full, %s@%08X pops", Name, (UInt32)Node);
		return;
	}

	Fades.push_back({ Node, NowMs });
	RebuildIndex();
	if (Log) Logger::Log("[LODFade] fade %s@%08X ring=%d live=%u", Name, (UInt32)Node, Ring, (UInt32)Fades.size());

}

void LODFadeManager::DrainPending() {

	for (auto it = Pending.begin(); it != Pending.end(); ) {
		NiAVObject* Node = it->first;
		if (!LiveChildren.count(Node) || NowMs - it->second > kPendingTimeoutMs) {
			it = Pending.erase(it);
			continue;
		}
		if (IsReady(Node)) {
			TryStartFade(Node);
			it = Pending.erase(it);
			continue;
		}
		++it;
	}

}

void LODFadeManager::RetireFinished() {

	bool Retired = false;
	for (size_t i = 0; i < Fades.size(); ) {
		if (GetAlpha(Fades[i]) < 1.0f) {
			i++;
			continue;
		}
		Fades[i] = Fades.back();
		Fades.pop_back();
		Retired = true;
	}
	if (Retired) RebuildIndex();

}

/// Any change to Fades invalidates every cached index, so the geometry cache is rebuilt lazily.
void LODFadeManager::RebuildIndex() {

	RootIndex.clear();
	for (UInt32 i = 0; i < Fades.size(); i++) RootIndex[Fades[i].Root] = i;
	GeometryCache.clear();

}

float LODFadeManager::GetAlpha(const FadeRecord& Record) const {

	double Duration = TheSettingManager->SettingsMain.LODFade.FadeTime * 1000.0;
	if (Duration <= 0.0) return 1.0f;

	double t = (NowMs - Record.StartMs) / Duration;
	if (t <= 0.0) return 0.0f;
	if (t >= 1.0) return 1.0f;
	return (float)t;

}

float LODFadeManager::GetFadeRemaining(NiAVObject* Geometry) {

	if (Fades.empty() || !Geometry) return 0.0f;

	int Index = -1;
	auto Cached = GeometryCache.find(Geometry);
	if (Cached != GeometryCache.end()) {
		Index = Cached->second;
	}
	else {
		NiAVObject* Node = Geometry;
		for (UInt32 Depth = 0; Node && Depth < kMaxParentDepth; Depth++) {
			auto Root = RootIndex.find(Node);
			if (Root != RootIndex.end()) {
				Index = (int)Root->second;
				break;
			}
			Node = Node->m_parent;
		}
		GeometryCache[Geometry] = Index;
	}

	if (Index < 0) return 0.0f;
	return 1.0f - GetAlpha(Fades[Index]);

}

void LODFadeManager::NoteCensusDraw(NiAVObject* Geometry, const char* PixelShaderName, const char* VertexShaderName, bool PixelHasProgram) {

	if (CensusStartMs == 0.0 || !DistantRef || !Geometry) return;

	NiAVObject* Child = NULL;
	NiAVObject* Node = Geometry;
	for (UInt32 Depth = 0; Node && Depth < kCensusMaxDepth; Depth++) {
		if (Node->m_parent == DistantRef) {
			Child = Node;
			break;
		}
		Node = Node->m_parent;
	}
	if (!Child) return;

	bool Tree = Child->m_pcName && !strcmp(Child->m_pcName, "LOD Trees");
	for (UInt32 i = 0; i < CensusCount; i++) {
		CensusEntry& Entry = Census[i];
		if (Entry.PixelShader == PixelShaderName && Entry.VertexShader == VertexShaderName && Entry.Tree == Tree) {
			Entry.Draws++;
			return;
		}
	}
	if (CensusCount >= kCensusMax) {
		CensusOverflow++;
		return;
	}
	Census[CensusCount++] = { PixelShaderName, VertexShaderName, PixelHasProgram, Tree, 1 };

}

void LODFadeManager::ReportCensus() {

	CensusArmed = false;
	Logger::Log("[LODFade] census: %u shader pairs drew under DistantRefLOD in %.0f s (overflow %u)",
		CensusCount, kCensusDelayMs / 1000.0, CensusOverflow);
	for (UInt32 i = 0; i < CensusCount; i++) {
		CensusEntry& Entry = Census[i];
		Logger::Log("[LODFade] census ps=%s vs=%s replaced=%d tree=%d draws=%u",
			Entry.PixelShader ? Entry.PixelShader : "(unnamed)",
			Entry.VertexShader ? Entry.VertexShader : "(unnamed)",
			Entry.PixelHasProgram ? 1 : 0, Entry.Tree ? 1 : 0, Entry.Draws);
	}

}
```

- [ ] **Step 4: Register the singleton**

`TESReloaded/Core/Managers.h`: add `#include "LODFadeManager.h"` after `#include "SampleProfiler.h"`, and `extern LODFadeManager*		TheLODFadeManager;` after `extern ScriptManager*		TheScriptManager;`.

`TESReloaded/Core/Managers.cpp`: add `LODFadeManager*		TheLODFadeManager = NULL;` after the `TheScriptManager` definition, and at the end of `InitializeManagers()`:

```cpp
	if (TheSettingManager->SettingsMain.LODFade.Enabled && TheSettingManager->SettingsMain.LODFade.FadeTime > 0.0f) new LODFadeManager();
```

- [ ] **Step 5: Call Update once per frame**

In `TESReloaded/Core/RenderHook.cpp`, Oblivion `RenderHook::TrackRender`, directly after `TheShaderManager->UpdateConstants();` (line 474):

```cpp
	if (TheLODFadeManager) TheLODFadeManager->Update();
```

- [ ] **Step 6: Register the files in the project**

In `OblivionReloaded/OblivionReloaded.vcxproj`, next to the `WetActors` entries:

```xml
    <ClInclude Include="..\TESReloaded\Core\LODFadeManager.h" />
```
```xml
    <ClCompile Include="..\TESReloaded\Core\LODFadeManager.cpp" />
```

In `OblivionReloaded/OblivionReloaded.vcxproj.filters`, next to the `WetActors` entries:

```xml
    <ClInclude Include="..\TESReloaded\Core\LODFadeManager.h">
      <Filter>Core</Filter>
    </ClInclude>
```
```xml
    <ClCompile Include="..\TESReloaded\Core\LODFadeManager.cpp">
      <Filter>Core</Filter>
    </ClCompile>
```

Then regenerate clangd's database (PowerShell): `powershell -File Tools\GenerateCompileCommands.ps1`

- [ ] **Step 7: Build**

Run the MSBuild command from Global Constraints (PowerShell tool).
Expected: `Build succeeded`, 0 errors. `Framework.h` (force-included) pulls in `Managers.h`, so `TheLODFadeManager`, `Player`, `Tes`, `SettingGridsToLoad` and `Logger` are visible in `LODFadeManager.cpp` without further includes.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/LODFadeManager.h TESReloaded/Core/LODFadeManager.cpp TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp TESReloaded/Core/Managers.h TESReloaded/Core/Managers.cpp TESReloaded/Core/RenderHook.cpp OblivionReloaded/OblivionReloaded.vcxproj OblivionReloaded/OblivionReloaded.vcxproj.filters
git commit -m "feat(LODFade): Detect far-edge distant LOD arrivals and run a fade table"
```

```json:metadata
{"files": ["TESReloaded/Core/LODFadeManager.h", "TESReloaded/Core/LODFadeManager.cpp", "TESReloaded/Core/SettingManager.h", "TESReloaded/Core/SettingManager.cpp", "TESReloaded/Core/Managers.h", "TESReloaded/Core/Managers.cpp", "TESReloaded/Core/RenderHook.cpp", "OblivionReloaded/OblivionReloaded.vcxproj", "OblivionReloaded/OblivionReloaded.vcxproj.filters"], "verifyCommand": "& 'C:\\Development\\Microsoft\\Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe' 'C:\\Users\\Adam\\Code\\Oblivion\\Oblivion Reloaded E3 Custom\\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal", "acceptanceCriteria": ["DistantRefLOD resolved by name each frame, gridDistantArray never read", "set diff to min(end, capacity)", "ring >= grid/2 + 2 fades, else skip logged", "discontinuity adopts silently", "unready arrivals queued, promoted, dropped after 8 s or on leaving", "records dropped on departure, retired at alpha 1, capped at MaxFades", "Player/Tes NULL and DistantRefLOD change clear state", "no refcount or scene-graph writes", "created only when Enabled && FadeTime > 0", "build 0 errors"], "modelTier": "standard"}
```

---

### Task 2: Shader census of distant LOD draws

**Goal:** Measure, in one game session, which pixel/vertex shader pairs draw geometry under `DistantRefLOD` and whether each pixel shader has a plugin replacement.

**Files:**
- Modify: `TESReloaded/Core/RenderHook.cpp` (`TrackSetupShaderPrograms`, after the `isSkin` / `isWetActor` per-geometry block ending ~line 1225)

**Acceptance Criteria:**
- [ ] The census call costs one pointer test plus one bool load per draw when `LogLODFade = 0`
- [ ] After 30 s of exterior play with `LogLODFade = 1`, `OblivionReloaded.log` contains one `[LODFade] census:` header and one `census ps=... vs=... replaced=... tree=... draws=...` line per pair
- [ ] The census result (the list of `tree=0` pixel shaders) is recorded in the Task 3 description before Task 3 starts

**Verify:** Build succeeds; then the user's game session log contains the `[LODFade] census` lines.

**Steps:**

- [ ] **Step 1: Add the census call to the draw hook**

In `TESReloaded/Core/RenderHook.cpp`, `RenderHook::TrackSetupShaderPrograms`, directly after the `#endif` that closes the `else if (PixelShader->ShaderProg && PixelShader->isWetActor) { ... }` block and before `if (PixelShader->isRefraction) {`:

```cpp
#if defined(OBLIVION)
		if (TheLODFadeManager && TheLODFadeManager->CensusArmed)
			TheLODFadeManager->NoteCensusDraw(Geometry, PixelShader->ShaderName, VertexShader->ShaderName, PixelShader->ShaderProg != NULL);
#endif
```

- [ ] **Step 2: Build**

Run the MSBuild command from Global Constraints. Expected: `Build succeeded`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add TESReloaded/Core/RenderHook.cpp
git commit -m "diag(LODFade): Census the shaders that draw under DistantRefLOD"
```

- [ ] **Step 4: STOP — ask the user to run the census session**

The game cannot be driven from here. Ask the user to:
1. Set `[Develop] LogLODFade = 1` in `Data\OBSE\Plugins\OblivionReloaded.ini` (add the line if absent).
2. Load an **exterior** save, stay outdoors for at least 40 s, and walk or ride across two or three cells.
3. Quit, then paste every `[LODFade]` line from `OblivionReloaded.log` (game folder).

- [ ] **Step 5: Read the census and record the covered set**

From the pasted log:
- Confirm the `DistantRefLOD resolved ... children=N` line (N around 2000; wildly different means the traversal is wrong — stop and report).
- Confirm `fade` lines have rings ≥ `uGridsToLoad/2 + 2` and `skip` lines have smaller rings.
- Covered set = every `census ps=X` with `tree=0`. For each, note `replaced`.
- If any `tree=0` shader has `replaced=0`, it has no plugin replacement and cannot receive the constant: stop and report to the user (adding a replacement means extracting the stock shader, which is out of scope for this plan).
- If there are no `tree=0` lines at all, distant statics are not drawn through this hook: stop and report (spec risk 1; the region-test approach becomes the design).

Write the covered list into Task 3's description with TaskUpdate (e.g. "Covered: DISTLOD2001.pso, SLS2002.pso").

```json:metadata
{"files": ["TESReloaded/Core/RenderHook.cpp"], "verifyCommand": "& 'C:\\Development\\Microsoft\\Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe' 'C:\\Users\\Adam\\Code\\Oblivion\\Oblivion Reloaded E3 Custom\\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal", "acceptanceCriteria": ["census call gated on CensusArmed", "census header and per-pair lines in the user's log after 30 s outdoors", "covered tree=0 list recorded in Task 3 before it starts"], "modelTier": "standard"}
```

---

### Task 3: Per-draw fade constant and the dither clip

**Goal:** Every census-listed pixel shader clips by its draw's remaining fade, so far-edge arrivals visibly dither in over `FadeTime`.

**Files:**
- Create: `OblivionReloaded/Shaders/ExtraShaders/Includes/LODFade.hlsl`
- Modify: `TESReloaded/Core/ShaderManager.h` (`ShaderConstants` ~line 202–329; `ShaderProgram` ~line 350)
- Modify: `TESReloaded/Core/ShaderManager.cpp` (`ShaderProgram` ctor/dtor ~line 242; `SetPerGeomConstantTableValue` ~line 500; `CreateCT` ~line 679; `ShaderManager` ctor ~line 1152)
- Modify: `TESReloaded/Core/RenderHook.cpp` (the `#if defined(OBLIVION)` census block added in Task 2)
- Modify: every census-listed `*.pso.hlsl` (from Task 2; every copy under `OblivionReloaded/Shaders/ExtraShaders*/`)

**Acceptance Criteria:**
- [ ] `TESR_GEOM_FadeParams` mapped to `ShaderConst.LODFade.Params`; `HasFadeParams` and `FadeParamsRegister` set in `CreateCT`
- [ ] `ShaderProgram` ctor initializes `PerGeomFloatShaderValues`, its count, `HasFadeParams`, `FadeParamsRegister`; dtor frees `PerGeomFloatShaderValues`
- [ ] Every draw with `HasFadeParams` uploads exactly one register via `SetPixelShaderConstantF`; uploads 0 when the manager is absent
- [ ] `LODFadeClip`: c110, `f = .x`, `[branch] if (f > 0)`, IGN hash, `clip(n - f)`
- [ ] Each covered shader: include, `float2 vpos : VPOS` input, `LODFadeClip(IN.vpos);` as the first statement of `main`
- [ ] Every edited shader compiles under `fxc /T ps_3_0` with no errors and ≤ 512 instruction slots, and its `.asm` shows `if_lt`/`endif` (or `if_gt`) around the hash
- [ ] Solution builds with 0 errors

**Verify:** `fxc` per edited shader (Step 6) → no errors, slot count ≤ 512; MSBuild → `Build succeeded`.

**Steps:**

- [ ] **Step 1: Shader constant and ShaderProgram fields**

`TESReloaded/Core/ShaderManager.h`, inside `struct ShaderConstants`, after `struct WetActorStruct { ... };`:

```cpp
	/// Per draw (TESR_GEOM_FadeParams): x = remaining LOD fade (0 = opaque), y = per-frame dither seed.
	struct LODFadeStruct {
		D3DXVECTOR4		Params;
	};
```

and after `WetActorStruct			WetActor;` in the member list:

```cpp
	LODFadeStruct			LODFade;
```

In `class ShaderProgram`, after `UInt32 TextureShaderValuesCount;`:

```cpp

	/// True when the shader declares TESR_GEOM_FadeParams, i.e. carries the LOD fade dither clip.
	bool					HasFadeParams;
	/// Register TESR_GEOM_FadeParams compiled to; the draw hook uploads only this one.
	UInt32					FadeParamsRegister;
```

- [ ] **Step 2: Constructor fix, mapping and register capture**

`TESReloaded/Core/ShaderManager.cpp`, replace the `ShaderProgram` ctor/dtor bodies:

```cpp
ShaderProgram::ShaderProgram() {

	FloatShaderValues = NULL;
	TextureShaderValues = NULL;
	PerGeomFloatShaderValues = NULL;
	FloatShaderValuesCount = 0;
	TextureShaderValuesCount = 0;
	PerGeomFloatShaderValuesCount = 0;
	HasFadeParams = false;
	FadeParamsRegister = 0;

}

ShaderProgram::~ShaderProgram() {

	if (FloatShaderValues) free(FloatShaderValues);
	if (TextureShaderValues) free(TextureShaderValues);
	if (PerGeomFloatShaderValues) free(PerGeomFloatShaderValues);

}
```

In `ShaderProgram::SetPerGeomConstantTableValue`, before the final `else {`:

```cpp
	else if (!strcmp(Name, "TESR_GEOM_FadeParams"))
		PerGeomFloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.LODFade.Params;
```

In `ShaderRecord::CreateCT`, inside `if (SetPerGeomConstantTableValue(ConstantDesc.Name, PerGeomFloatIndex)) {`, before `PerGeomFloatIndex++;`:

```cpp
							if (!strcmp(ConstantDesc.Name, "TESR_GEOM_FadeParams")) {
								HasFadeParams = true;
								FadeParamsRegister = ConstantDesc.RegisterIndex;
							}
```

In `ShaderManager::ShaderManager()`, right after `TheShaderManager = this;`:

```cpp
	ShaderConst.LODFade.Params = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
```

- [ ] **Step 3: Upload the fade on every covered draw**

In `TESReloaded/Core/RenderHook.cpp`, replace the Task 2 block with:

```cpp
#if defined(OBLIVION)
		// Every covered draw uploads its own register, so a fade value never carries over to the next draw.
		if (PixelShader->ShaderProg && PixelShader->ShaderProg->HasFadeParams) {
			D3DXVECTOR4& Fade = TheShaderManager->ShaderConst.LODFade.Params;
			Fade.x = TheLODFadeManager ? TheLODFadeManager->GetFadeRemaining(Geometry) : 0.0f;
			Fade.y = TheLODFadeManager ? TheLODFadeManager->DitherSeed : 0.0f;
			TheRenderManager->device->SetPixelShaderConstantF(PixelShader->ShaderProg->FadeParamsRegister, (const float*)&Fade, 1);
		}
		if (TheLODFadeManager && TheLODFadeManager->CensusArmed)
			TheLODFadeManager->NoteCensusDraw(Geometry, PixelShader->ShaderName, VertexShader->ShaderName, PixelShader->ShaderProg != NULL);
#endif
```

- [ ] **Step 4: Create `OblivionReloaded/Shaders/ExtraShaders/Includes/LODFade.hlsl`**

Use CRLF line endings, no BOM, matching `Includes/WetActor.hlsl`:

```hlsl
// Distant LOD fade-in dither. x = remaining fade (0 = opaque), y = per-frame seed.
float4 TESR_GEOM_FadeParams : register(c110);

// Clips the pixel while its dither threshold is below the remaining fade. Zero, the D3D9 initial
// register value, is fully opaque, and settled draws skip the hash through a uniform branch.
// Interleaved gradient noise, not a sin hash: ps_3_0 expands sin past the 512-slot limit.
void LODFadeClip(float2 vpos) {
    float f = TESR_GEOM_FadeParams.x;
    [branch]
    if (f > 0.0) {
        float n = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + TESR_GEOM_FadeParams.y);
        clip(n - f);
    }
}
```

- [ ] **Step 5: Add the clip to each covered shader**

For every covered pixel shader recorded in this task's description, edit **every** copy named `<name>.hlsl` under `OblivionReloaded/Shaders/ExtraShaders*/` (check `ExtraShadersDialog` and `ExtraShadersExterior` too). Three edits per file, shown on `DISTLOD2001.pso.hlsl`:

1. After the parameter declarations near the top (after `sampler2D DiffuseMap : register(s0);`):
   ```hlsl
   #include "Includes/LODFade.hlsl"
   ```
   For a file in `ExtraShadersDialog` or `ExtraShadersExterior`, use `#include "../ExtraShaders/Includes/LODFade.hlsl"`.
2. In the pixel shader input struct (`struct VS_OUTPUT` in `DISTLOD2001`), add as the last member:
   ```hlsl
       float2 vpos : VPOS;
   ```
   If the struct already has a `VPOS` member, reuse it and skip this edit.
3. As the first statement of `main`, right after `PS_OUTPUT OUT;`:
   ```hlsl
       LODFadeClip(IN.vpos);
   ```
   Use the shader's own input parameter name if it is not `IN`.

Before adding, grep the file for `register(c110)`; if anything else already uses c110, stop and report.

- [ ] **Step 6: Verify every edited shader with fxc (PowerShell)**

```powershell
$fxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe'
$out = 'C:\WINDOWS\TEMP\claude\C--Users-Adam-Code-Oblivion-Oblivion-Reloaded-E3-Custom\56bea7e4-b0f7-44d3-9dc2-bff8b2461c97\scratchpad'
Get-ChildItem -Recurse OblivionReloaded\Shaders -Filter *.pso.hlsl | Where-Object { Select-String -Path $_.FullName -Pattern 'LODFadeClip' -Quiet } | ForEach-Object {
    & $fxc /nologo /T ps_3_0 /E main /I $_.DirectoryName /Fo "$out\$($_.BaseName)" /Fc "$out\$($_.BaseName).asm" $_.FullName
    Select-String -Path "$out\$($_.BaseName).asm" -Pattern 'instruction slots used'
}
```

Expected per shader: compiles without errors, `approximately N instruction slots used` with N ≤ 512, and the `.asm` contains an `if_` / `endif` pair around the `frc` hash instructions (`Select-String -Path <asm> -Pattern 'if_|endif'`). Any pre-existing warnings are fine; compare against the HEAD version of the same file before attributing a warning to this edit.

- [ ] **Step 7: Build**

Run the MSBuild command from Global Constraints. Expected: `Build succeeded`, 0 errors.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/ShaderManager.h TESReloaded/Core/ShaderManager.cpp TESReloaded/Core/RenderHook.cpp OblivionReloaded/Shaders/ExtraShaders/Includes/LODFade.hlsl <each edited .pso.hlsl>
git commit -m "feat(LODFade): Dither far-edge distant LOD in through a per-draw fade constant"
```

```json:metadata
{"files": ["OblivionReloaded/Shaders/ExtraShaders/Includes/LODFade.hlsl", "TESReloaded/Core/ShaderManager.h", "TESReloaded/Core/ShaderManager.cpp", "TESReloaded/Core/RenderHook.cpp"], "verifyCommand": "& 'C:\\Development\\Microsoft\\Visual Studio\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe' 'C:\\Users\\Adam\\Code\\Oblivion\\Oblivion Reloaded E3 Custom\\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal", "acceptanceCriteria": ["FadeParams mapped, HasFadeParams + FadeParamsRegister set in CreateCT", "ShaderProgram ctor/dtor fix", "every covered draw uploads one register, 0 without manager", "LODFadeClip c110 f>0 branch IGN clip(n-f)", "include + VPOS + first-statement call in every covered copy", "fxc clean, <=512 slots, real branch", "build 0 errors"], "modelTier": "standard"}
```

---

### Task 4: In-game verification and memory note

**Goal:** Confirm the fade in game against the Review Focus items, and record the census finding as agent memory.

**Files:**
- Create: `memory/distant-lod-shaders.md`
- Modify: `memory/MEMORY.md`

**Acceptance Criteria:**
- [ ] User reports far-edge LOD visibly dithers in over ~1 s with `fade` lines in the log (rings ≥ grid/2 + 2)
- [ ] Inner-ring arrivals log `skip` and the user sees no hole when cells unload (Review Focus 3)
- [ ] Fast travel / `coc` logs `discontinuity` and nothing dithers (Review Focus 2)
- [ ] Main menu → load → load another save: no crash, no stale dither (Review Focus 1)
- [ ] Nearby full-resolution objects drawn with a covered shader never dither, and wet/skin actors look unchanged (Review Focus 4)
- [ ] `[LODFade] Enabled = 0`: covered LOD fully opaque, nothing dithers (Review Focus 5)
- [ ] No flicker under TAA
- [ ] `Develop.ProfileFrame` shader-hook bucket standing still is within noise of `main`
- [ ] Memory note committed in its own `docs:` commit

**Verify:** The user's in-game report and pasted `[LODFade]` log lines cover every criterion above.

**Steps:**

- [ ] **Step 1: STOP — ask the user to run the verification session**

Ask the user to set `[Develop] CompileShaders = 1` (once, to rebuild the edited shaders) and `LogLODFade = 1`, then:
1. Load an exterior save; ride across several cells and watch the far edge of the LOD.
2. Walk back and forth across one cell boundary and watch the cells next to the loaded area as they unload.
3. Fast travel once, and `coc` to another exterior once.
4. Return to the main menu, load a save, then load a different save.
5. Look at nearby statics and at an actor in the rain (the wet look shares shaders with world objects).
6. Repeat step 1 with `[LODFade] Enabled = 0`.
7. Stand still with `Develop.ProfileFrame` on, on this build and on a `main` build, and compare the shader-hook bucket.
8. Paste the `[LODFade]` log lines and report what was seen (smooth dissolve vs flicker, any holes, any dither on near objects).

- [ ] **Step 2: Act on the report**

Map each finding to an acceptance criterion. Anything failing is a bug: use superpowers-extended-cc:systematic-debugging before changing code.

- [ ] **Step 3: Write the memory note**

Create `memory/distant-lod-shaders.md` with this frontmatter, filling the body from the census and verification logs (shader pairs, tree vs static, `replaced` flags, measured children count, whether the ring rule held):

```markdown
---
name: distant-lod-shaders
description: "MEASURED: which pixel/vertex shaders draw geometry under DistantRefLOD (statics vs LOD Trees), and which carry the LODFade dither clip"
metadata:
  type: reference
---
```

Link `[[distant-lod-scene-graph]]` from it, and add to `memory/MEMORY.md`:

```markdown
- [Distant LOD shaders](distant-lod-shaders.md) — MEASURED census of the shaders drawing under DistantRefLOD; which carry the LODFade clip (c110, 0 = opaque)
```

- [ ] **Step 4: Commit**

```bash
git add memory/distant-lod-shaders.md memory/MEMORY.md
git commit -m "docs: Record the shaders that draw distant LOD and carry the fade clip"
```

```json:metadata
{"files": ["memory/distant-lod-shaders.md", "memory/MEMORY.md"], "verifyCommand": "", "acceptanceCriteria": ["far-edge fade seen and logged", "inner ring skip, no hole", "teleport discontinuity", "menu/load cycle safe", "near objects and wet actors unaffected", "Enabled=0 opaque", "no TAA flicker", "idle shader-hook bucket within noise of main", "memory note in its own docs commit"], "modelTier": "standard"}
```
