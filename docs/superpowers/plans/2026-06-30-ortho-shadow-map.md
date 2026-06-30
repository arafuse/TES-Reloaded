# Ortho Shadow Map — Gating + Single-Pass Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the precipitation-occlusion ortho shadow map cost nothing when no precip effect is contributing, and rebuild it via a single frustum-culled pass when it is.

**Architecture:** Add a per-frame gate (`OrthoNeeded()`) that early-returns from `RenderExteriorShadows` unless an ortho-sampling precip effect is active. Then collapse the old 4-pass collection machinery (`BuildExteriorGeoItems` un-culled + `ShadowRefGroups` + per-pass cull in `RenderShadowMap`) into one fused walk that frustum-culls and filters during collection and renders straight from a flat list, reusing the existing instancing path.

**Tech Stack:** C++ (v145 / VS2019), DirectX 9 (D3DX), OBSE plugin. No automated test harness — verification is build success + in-game behavior + FrameProfiler buckets.

## Global Constraints

- Build only via the solution (force-include paths use `$(SolutionDir)`):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
- Platform x86 / toolset v145. `OBLIVION` preprocessor path is the live one.
- Edits are confined to `TESReloaded/Core/ShadowManager.cpp` and `TESReloaded/Core/ShadowManager.h`.
- Behavior while precipitating must stay **visually identical** to pre-change.
- Do not modify the `#if 0 // SHADOWS DISABLED` reference blocks except where a task explicitly says so.
- Spec: `docs/superpowers/specs/2026-06-30-ortho-shadow-map-design.md`.

---

### Task 1: Gate the ortho render on active precipitation

**Files:**
- Modify: `TESReloaded/Core/ShadowManager.h` (add method declaration near `RenderExteriorShadows`, ~line 71)
- Modify: `TESReloaded/Core/ShadowManager.cpp` (`RenderExteriorShadows`, ~lines 1041-1061)

**Interfaces:**
- Consumes: `TheSettingManager->SettingsMain.Effects.Precipitations`, `.SnowAccumulation` (bool); `TheShaderManager->ShaderConst.Precipitations.RainData.x`, `.SnowData.x`, `ShaderConst.SnowAccumulation.Params.w` (float).
- Produces: `bool ShadowManager::OrthoNeeded();` — true when an ortho-sampling precip effect is contributing.

- [ ] **Step 1: Declare the helper**

In `ShadowManager.h`, add below the `RenderExteriorShadows` declaration (line 71):

```cpp
		void					RenderExteriorShadows();
		bool					OrthoNeeded();
```

- [ ] **Step 2: Implement the helper**

In `ShadowManager.cpp`, immediately above `RenderExteriorShadows` (just before the comment block at line 1035), add:

```cpp
// The ortho depth map's only consumers are the precipitation effects (Rain/Snow/SnowAccumulation),
// which sample it for occlusion. Rebuild it only while one of them is still contributing: rain/snow
// intensity ramps (RainData.x / SnowData.x) or the snow-accumulation amount (Params.w), which keeps
// decreasing for a while after snow stops. If both effects are disabled in the INI nothing can ever
// sample the map, so skip unconditionally. Values are maintained in ShaderManager::UpdateConstants;
// reading them here may be one frame stale relative to that update, which is invisible for a depth
// occlusion map across weather ramps.
bool ShadowManager::OrthoNeeded() {
	SettingsMainStruct::EffectsStruct* Effects = &TheSettingManager->SettingsMain.Effects;
	if (!Effects->Precipitations && !Effects->SnowAccumulation) return false;
	if (Effects->Precipitations &&
		(TheShaderManager->ShaderConst.Precipitations.RainData.x > 0.0f ||
		 TheShaderManager->ShaderConst.Precipitations.SnowData.x > 0.0f)) return true;
	if (Effects->SnowAccumulation &&
		TheShaderManager->ShaderConst.SnowAccumulation.Params.w > 0.0f) return true;
	return false;
}
```

- [ ] **Step 3: Call the gate at the top of `RenderExteriorShadows`**

In `RenderExteriorShadows` (the live ortho-only version, ~line 1041), add the gate right after the worldspace guard:

```cpp
void ShadowManager::RenderExteriorShadows() {
	if (!Player->GetWorldSpace()) return;
	if (!OrthoNeeded()) return;
	ScopeTimer profile(Phase_ExtTotal);
```

- [ ] **Step 4: Build**

Run:
```
& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal
```
Expected: `Build succeeded`, 0 errors. (Confirm `SettingsMainStruct::EffectsStruct` is the correct type name for `SettingsMain.Effects`; it is declared in `SettingManager.h:180`. If the compiler reports a name mismatch, use the type as declared there.)

- [ ] **Step 5: In-game behavior check (manual)**

- Clear weather, exterior: `Buck_ShadowMaps` / `Phase_ExtTotal` in the FrameProfiler should read ~0 (ortho skipped).
- Start rain: rain must still be occluded under roofs/overhangs exactly as before.
- This is a manual check; if you cannot run the game, note it for the user to verify and proceed.

- [ ] **Step 6: Commit**

```
git add "TESReloaded/Core/ShadowManager.h" "TESReloaded/Core/ShadowManager.cpp"
git commit -m "feat(Shadows): gate ortho map on active precipitation"
```

---

### Task 2: Fuse frustum cull + filter into collection; render from a flat single pass

This rewrites the ortho geometry path so the cull and per-pass filter happen **once during collection** (against the ortho frustum, which is now computed before collection) instead of collecting everything un-culled and culling per-pass in `RenderShadowMap`. The per-ref grouping (`ShadowRefGroups`) is removed.

**Files:**
- Modify: `TESReloaded/Core/ShadowManager.cpp`:
  - `RenderExteriorShadows` (~line 1041) — compute matrices before collection
  - `SetupShadowMapMatrices` call site removal in `RenderShadowMap` (~line 979)
  - `BuildExteriorGeoItems` (~lines 726-763)
  - `CollectExteriorGeo` (~lines 770-824)
  - `RenderShadowMap` group-loop body (~lines 999-1031)
- Modify: `TESReloaded/Core/ShadowManager.h` — remove `ShadowRefGroup` struct + `ShadowRefGroups` member (~lines 234-244)

**Interfaces:**
- Consumes: `RootInShadowFrustum(MapOrtho, center, radius)`, `LeafInShadowFrustum(MapOrtho, center, radius)`, `IsShadowCastableType(TypeID)`, `FormsAllows(Forms, TypeID)`, `MinRadii[MapOrtho]` (== 100.0f), `ShadowMapFrustum[MapOrtho]` (filled by `SetupShadowMapMatrices`→`GetShadowFrustum`).
- Produces: flat `ShadowGeoPool[0 .. ShadowGeoCount)` of already-culled, already-filtered `ShadowGeoItem`s; `RenderShadowMap` consumes it without re-culling. `RenderShadowMap` now assumes the caller has already called `SetupShadowMapMatrices` for the map being rendered.

- [ ] **Step 1: Move matrix/frustum setup ahead of collection in `RenderExteriorShadows`**

Replace the body from the `BuildExteriorGeoItems` line through `RenderShadowMap` (~lines 1053-1058) with:

```cpp
	D3DXVECTOR3 At, SkinAt;
	ComputeExteriorLookAt(At, SkinAt, ShadowsExteriors);

	// Matrices/frustum first: collection culls geometry against ShadowMapFrustum[MapOrtho], so the
	// frustum must exist before the walk. SetupShadowMapMatrices also publishes
	// ShadowCameraToLight[MapOrtho] (-> TESR_ShadowCameraToLightTransformOrtho) and Billboard vectors.
	SetupShadowMapMatrices(MapOrtho, ShadowsExteriors, &At, &OrthoDir);

	{ ScopeTimer profileBuild(Phase_BuildGeoItems); BuildExteriorGeoItems(ShadowsExteriors); }

	RenderShadowMap(MapOrtho, ShadowsExteriors, &At, &OrthoDir, ShadowData);
```

(Leave the `CurrentVertex/CurrentPixel` assignments and the `OrthoData->z = ...` tail unchanged.)

- [ ] **Step 2: Remove the now-redundant matrix setup from `RenderShadowMap`**

In `RenderShadowMap` (~line 979), delete the `SetupShadowMapMatrices(...)` call and document the contract. The block at lines 978-983 becomes:

```cpp
	AlphaEnabled = ShadowsExteriors->AlphaEnabled[ShadowMapType];
	// Matrices/frustum are set up by the caller (RenderExteriorShadows) before geometry collection,
	// so the pool is already culled to this map's frustum; do not recompute here.
	if (!ShadowMapSurface[ShadowMapType]) return; // only MapOrtho is allocated in the dummied-out build
	Device->SetRenderTarget(0, ShadowMapSurface[ShadowMapType]);
	Device->SetDepthStencilSurface(ShadowMapDepthSurface[ShadowMapType]);
	Device->SetViewport(&ShadowMapViewPort[ShadowMapType]);
	Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
	if (!ShadowsExteriors->Enabled[ShadowMapType]) return;
```

- [ ] **Step 3: Rewrite `BuildExteriorGeoItems` to filter + root-cull during the walk (flat list)**

Replace the whole function body (~lines 726-763) with:

```cpp
void ShadowManager::BuildExteriorGeoItems(SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors) {
	ShadowGeoCount = 0;
	SettingsShadowStruct::ExcludedFormsList* ExcludedForms = &ShadowsExteriors->ExcludedForms;
	bool HasExcluded = ExcludedForms->size() > 0;
	bool HasWater = TheShaderManager->ShaderConst.HasWater;
	// Single live pass = ortho. Apply its Forms filter and frustum cull here instead of per-pass.
	SettingsShadowStruct::FormsStruct* Forms = &ShadowsExteriors->Forms[MapOrtho];
	for (UInt32 x = 0; x < *SettingGridsToLoad; x++) {
		for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
			TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y);
			if (!Cell) continue;
			TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First;
			for (; Entry; Entry = Entry->next) {
				TESObjectREFR* Ref = Entry->item;
				NiNode* Node;
				if (!Ref || !(Node = Ref->GetNode()) || (Ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows)) continue;
				TESForm* Form = Ref->baseForm;
				UInt8 TypeID = Form->formType;
				if (!IsShadowCastableType(TypeID)) continue;
				if (!FormsAllows(Forms, TypeID)) continue;
				if (HasExcluded && std::binary_search(ExcludedForms->begin(), ExcludedForms->end(), Form->refID)) continue;
				NiBound* RootBound = Node->GetWorldBound();
				if (!RootBound) continue;
				D3DXVECTOR3 RootCenter = { RootBound->Center.x - TheRenderManager->CameraPosition.x, RootBound->Center.y - TheRenderManager->CameraPosition.y, RootBound->Center.z - TheRenderManager->CameraPosition.z };
				if (!RootInShadowFrustum(MapOrtho, RootCenter, RootBound->Radius)) continue; // whole-subtree cull
				CollectExteriorGeo(Node, HasWater);
			}
		}
	}
}
```

Also update the function's doc comment (~lines 720-725) to describe the single-pass behavior:

```cpp
// Flatten every ortho-frustum-visible, Forms-allowed shadow-casting ref in the loaded grid into
// ShadowGeoPool once per frame. Node/flag/excluded/type/Forms eligibility, the ref-root frustum cull,
// world transforms, bounds, the water test, the per-geo leaf cull, the MinRadius cut, and instancing
// eligibility are all resolved here; RenderShadowMap then draws the flat list with no further culling.
```

- [ ] **Step 4a: Insert the radius cut + leaf cull after the water test in `CollectExteriorGeo`**

This is an **insertion**, not a wholesale replacement — the `ModelBuff`/`DrawViaSkin`/`BaseInstanceable`/`HasAlphaMask` resolution that follows must be preserved. Find the water-test line (~line 788):

```cpp
	// Water test (frame-constant): drop submerged opaque geo when water is present.
	if (!(Geo->skinInstance || !HasWater || Bound->Center.z > 0.0f)) return;
```

and insert immediately after it (before the `// Resolve the buffer Render() will use:` comment):

```cpp
	// Per-pass cuts, now applied at collection (single live pass = ortho): drop sub-MinRadius geo and
	// anything outside the ortho frustum. Center is reused for the stored item below.
	if (Bound->Radius < MinRadii[MapOrtho]) return;
	D3DXVECTOR3 Center = { Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y, Bound->Center.z - TheRenderManager->CameraPosition.z };
	if (!LeafInShadowFrustum(MapOrtho, Center, Bound->Radius)) return;
```

- [ ] **Step 4b: Reuse the computed `Center` in the item-store block**

At the end of the function (~line 819), the item store currently recomputes the center:

```cpp
	Item.Center = { Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y, Bound->Center.z - TheRenderManager->CameraPosition.z };
```

Replace just that one line with the reused local:

```cpp
	Item.Center = Center;
```

- [ ] **Step 5: Render straight from the flat pool in `RenderShadowMap`**

Replace the group-loop section (~lines 999-1031, from `SettingsShadowStruct::FormsStruct* Forms = ...` through the closing brace of the `for (const ShadowRefGroup& Group ...)` loop, but keep the `if (UseInstancing) FlushInstanceGroups(ShadowData);` and `Device->EndScene();` that follow) with:

```cpp
	bool UseInstancing = ShadowsExteriors->UseInstancing && ShadowMapInstancedVertexShader;
	if (UseInstancing) { InstanceGroupIndex.clear(); InstanceGroupCount = 0; }

	// Pool is already culled to this map's frustum and Forms-filtered (BuildExteriorGeoItems), so just
	// draw: batch instanceable opaque statics, draw everything else immediately.
	for (int i = 0; i < ShadowGeoCount; i++) {
		ShadowGeoItem& Item = ShadowGeoPool[i];
		if (UseInstancing && Item.BaseInstanceable && !(AlphaEnabled && Item.HasAlphaMask)) {
			AddInstance(Item.GeoData, i);
			ProfileCount(Cnt_DirItemsInstanced);
		} else {
			Render(Item.Geo, ShadowData, Item.GeoData ? &Item.World : NULL);
			if (!Item.BaseInstanceable) ProfileCount(Cnt_DirItemsImmNonInst);
			else ProfileCount(Cnt_DirItemsImmAlpha);
		}
	}
```

- [ ] **Step 6: Remove the `ShadowRefGroup` struct and `ShadowRefGroups` member**

In `ShadowManager.h`, delete the `ShadowRefGroup` struct (~lines 234-240) and the `std::vector<ShadowRefGroup> ShadowRefGroups;` member (~line 244). Keep `ShadowGeoItem`, `ShadowGeoPool`, and `ShadowGeoCount`. Confirm no remaining references:

```
git grep -n "ShadowRefGroup" "TESReloaded/Core/"
```
Expected: no matches.

- [ ] **Step 7: Build**

Run:
```
& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal
```
Expected: `Build succeeded`, 0 errors.

- [ ] **Step 8: In-game behavior + profiler check (manual)**

- In rain/snow, exterior: occlusion under roofs/overhangs is visually identical to pre-change.
- `Phase_BuildGeoItems` should be **lower** than before (frustum-culled walk emits fewer items than the old collect-all).
- Open plain in rain with nothing nearby: rain still falls correctly (empty pool → cleared ortho target = fully sky-exposed).
- Manual check; if you cannot run the game, note it for the user and proceed.

- [ ] **Step 9: Commit**

```
git add "TESReloaded/Core/ShadowManager.h" "TESReloaded/Core/ShadowManager.cpp"
git commit -m "feat(Shadows): single-pass frustum-culled ortho collection"
```

---

## Notes for the implementer

- **No automated tests exist** in this repo; the build succeeding is the hard gate, and behavior is verified in-game. Do not invent a test framework.
- **Line numbers are approximate** and shift as you edit — anchor on the function names and the quoted surrounding code, not the numbers.
- **`MinRadii`** is the file-static `static const float MinRadii[4] = { 9.0f, 100.0f, 100.0f, 0.0f };` defined just above `BuildExteriorGeoItems`; `MinRadii[MapOrtho]` is `100.0f`.
- The empty-pool case is already handled: `RenderShadowMap` clears the ortho target before the draw loop, so zero collected geometry yields a fully-cleared (sky-exposed) map.
- Do **not** touch the `#if 0 // SHADOWS DISABLED` reference blocks; they are not compiled and are kept verbatim for the future rewrite.
```
