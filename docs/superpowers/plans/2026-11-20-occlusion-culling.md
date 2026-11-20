# Occlusion Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Skip drawing placed references that were completely hidden in the previous frame's far pass, so the dense-town main scene draws ~1.5-3k geometries instead of ~10.7k.

**Architecture:** A new Oblivion-only module `OcclusionCulling.cpp` hooks into three points of `TrackRenderObject`.
- **Before the far pass:** walk the loaded cells for candidates. Set `AppCulled` on those whose last query came back with zero pixels.
- **Right after the far pass:** clear exactly those flags.
- **After the far-pass depth resolve, before the shell's clear:** issue one occlusion query per candidate's projected bound rect against the far-pass depth.

All projection and policy math lives in an engine-free header, `OcclusionCullMath.h`, proven by a standalone harness in `Tools/Tests`.

**Tech Stack:** C++ (MSVC v145, x86), D3D9 occlusion queries (`D3DQUERYTYPE_OCCLUSION`) on DXVK, OBSE plugin, Gamebryo scene-graph flags.

**Spec:** `docs/superpowers/specs/2026-11-20-occlusion-culling-design.md`

## Global Constraints

- **Scope:** Oblivion only. Wrap the new module in `#if defined(OBLIVION)`, as `RainProbe.cpp` is.
- **Coding style (AGENTS.md):** public symbols get `///` doc comments. Code is self-documenting; inline comments only when necessary, 1-3 lines at most.
- **Build:** only through the PowerShell tool, never Bash (fake MSB3073):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  Expected: the `OblivionReloaded.vcxproj -> ...\OblivionReloaded.dll` line and no ` error ` lines. The game must not be running (the post-build copies the DLL into Plugins).
- **Off is inert:** with `[Main] OcclusionCulling = 0`, no query, vertex buffer or set is created and no node flag is written. Every entry point returns before touching the device.
- **Never lock a GPU surface for readback** (memory `d3d9-gpu-readback-stall`). Read only `IDirect3DQuery9::GetData(&Pixels, sizeof(DWORD), 0)`, never with `D3DGETDATA_FLUSH`. `S_FALSE` or failure means visible.
- **One-frame lag only:** use results solely from the immediately previous far pass. Never fall back to an older slot.
- **Near-shell order** (memory `near-shell-frame-order`):
  - `EndOcclusionCull()` runs immediately after the far pass's `RenderObject` returns, before `GpuEnd`, `InMainScenePass = false`, or anything else.
  - `IssueOcclusionQueries()` runs after `ResolveDepthBuffer()` and before `ApplyPass(PassNear)` and the shell's `Clear`.
  - Nothing else in that sequence moves.
- **Projection:** issue-time depth uses the main camera's `Frustum.Near/Far` *as they stand during the far pass*: `(ShellBoundary, RealFar)` when the shell is on, `(RealNear, RealFar)` when off. Never use the NiCamera world-to-clip matrix at `+0xAC` for depth. NiCamera rotation columns: `data[*][0]` forward, `[*][1]` up, `[*][2]` right (as `BuildViewCullPlanes` in `ShadowManager.cpp`).
- **Candidate form types:** Activator, Apparatus, Armor, Book, Clothing, Container, Door, Ingredient, Misc, Stat, Flora, Furniture, Weapon, Ammo, SoulGem, Key, AlchemyItem, SigilStone. **Never:** Light, Tree, Grass, NPC, Creature, LeveledCreature, or the player.
- **Defaults** (spec): `OcclusionAlwaysDrawDistance = 1024`, `OcclusionCameraCutDistance = 128`, `OcclusionMaxQueries = 8192`. Query ring of **3** slots.
- **Commits:** code as `feat(Occlusion): ...`. Memory, AGENTS.md and plan changes go only in separate `docs:` commits. Do not push.

**Plan calls (not in the spec, flagged for review):**
- Candidates that are always drawn (near, or reaching below water) are **not queried**: their result could never be used.
- `Develop.OcclusionCullDebug = 3` adds a measure-only mode (query and log, never cull), so the projection can be validated against the census before any culling.
- Hidden results are keyed by the `(TESObjectREFR*, NiNode*)` pair and applied only to references found again in this frame's walk of the loaded cells, so no stored pointer is ever dereferenced.

## Review Focus

- **A distant object smaller than a pixel** must never be reported hidden just because its rect covers no pixel centre. Pinned by `SubPixelBoundStillCoversAPixel` (Task 1).
- **A bound crossing the near plane or behind the camera** must not be queried, so it is drawn. Pinned by `CrossesNearPlane` and `BehindCamera` (Task 1).
- **Teleports, load doors, fast travel and the first/third-person switch** must draw everything for that frame. Pinned by `ResultsUnusableAfterCameraCut`, `ResultsUnusableAcrossWorlds` (Task 1) and the load-door step in Task 4.
- **A `Disable`d reference or a reference the engine already culled** must stay culled after our restore. Pinned by the `WasAlreadyCulled` rule in Task 4 and its in-game step.
- **Turning the switch off at runtime** (in-game menu) releases everything and holds no flag. Pinned by the menu toggle step in Task 4.

---

## File Structure

| File | Responsibility |
|---|---|
| `TESReloaded/Core/OcclusionCullMath.h` (create) | Engine-free: camera struct, far-pass depth, sphere-in-view, bound → pixel rect, candidate classification, result usability |
| `Tools/Tests/OcclusionCullMathTest.cpp` + `Run-OcclusionCullMathTest.ps1` (create) | Standalone harness for the header |
| `TESReloaded/Core/OcclusionCulling.h/.cpp` (create) | Candidate walk, query ring, readback, flag set/restore, stats, reset |
| `TESReloaded/Core/SettingManager.h/.cpp` (modify) | Four `[Main]` keys and `Develop.OcclusionCullDebug` |
| `TESReloaded/Core/FrameProfiler.h`, `RenderHook.cpp` (modify) | Three buckets and four counters, plus their name tables |
| `TESReloaded/Core/RenderHook.cpp` (modify) | Three calls in `TrackRenderObject` |
| `TESReloaded/Core/CoSave.cpp` (modify) | `ResetOcclusionCull()` on load and new game |
| `TESReloaded/Core/ShadowManager.cpp` (modify) | Guard: log once if flags are held when shadow maps render |
| `OblivionReloaded/OblivionReloaded.vcxproj(.filters)` (modify) | New files |

---

### Task 1: Occlusion math header and harness

**Files:**
- Create: `TESReloaded/Core/OcclusionCullMath.h`
- Create: `Tools/Tests/OcclusionCullMathTest.cpp`, `Tools/Tests/Run-OcclusionCullMathTest.ps1`

**Interfaces:**
- Produces (namespace `OcclusionCullMath`, header-only, `<cmath>` only, no Windows or engine headers):
  - `struct CullCamera { float Pos[3]; float Forward[3]; float Up[3]; float Right[3]; float Left, RightEdge, Top, Bottom; float Near, Far; };`
    The frustum edges are the NiFrustum's (view-space slopes at unit distance).
  - `float FarPassDepth(float ViewZ, float Near, float Far);`
    Returns `Far/(Far-Near) * (1 - Near/ViewZ)`, the depth that `RenderManager::DepthRow` encodes.
  - `bool SphereInView(const CullCamera& C, const float Center[3], float Radius);`
    Tests the four side planes and the eye plane, as `BuildViewCullPlanes` does. There is no far plane.
  - `struct QueryRect { float X0, Y0, X1, Y1, Z; };`
  - `enum RectResult { Rect_Ok, Rect_CrossesNear, Rect_OffScreen };`
  - `RectResult ProjectBoundRect(const CullCamera& C, const float Center[3], float Radius, float ViewportX, float ViewportY, float ViewportW, float ViewportH, QueryRect* Out);`
    - Projects the 8 AABB corners (center ± Radius on world axes). If any corner has view z < `Near`, the result is `Rect_CrossesNear`.
    - Projects as `ndcX = (2·x/z − (RightEdge+Left)) / (RightEdge−Left)` and `ndcY = (2·y/z − (Top+Bottom)) / (Top−Bottom)`, then `sx = VX + (ndcX+1)/2·W` and `sy = VY + (1−ndcY)/2·H`.
    - Expands to whole pixels as `X0 = floor(min sx) − 0.5`, `X1 = ceil(max sx) + 0.5` (Y likewise), then clamps to `[VX − 0.5, VX + W − 0.5]` and `[VY − 0.5, VY + H − 0.5]`.
    - The rect is empty when `X1 <= X0` or `Y1 <= Y0` after clamping. An empty rect is `Rect_OffScreen`.
    - `Z = FarPassDepth(min corner z)`.
  - `enum CandidateMode { Mode_AlwaysDraw, Mode_Cullable };`
    `Mode_AlwaysDraw` means never culled and not queried.
  - `CandidateMode ClassifyCandidate(float DistToSurface, float AlwaysDrawDistance, bool HasWater, float BoundBottomZ, float WaterHeight);`
    The result is `Mode_AlwaysDraw` when `DistToSurface < AlwaysDrawDistance`, or when `HasWater && BoundBottomZ < WaterHeight`. Otherwise it is `Mode_Cullable`.
  - `struct SlotMeta { unsigned Serial; float CameraPos[3]; const void* WorldKey; bool Valid; };`
  - `bool ResultsUsable(const SlotMeta& Slot, unsigned CurrentSerial, const float CameraPos[3], const void* WorldKey, float CutDistance);`
    True only when `Slot.Valid`, `Slot.Serial + 1 == CurrentSerial`, `Slot.WorldKey == WorldKey`, and the camera moved `<= CutDistance`.

- [ ] **Step 1: Write the failing harness** `Tools/Tests/OcclusionCullMathTest.cpp`. Use `GrassTrailMapTest.cpp`'s `CHECK` / `CHECK_NEAR` macros, print `N failures` and `return Failures ? 1 : 0`.

  Shared fixture `Cam()`:
  - `Pos {0,0,0}`, `Forward {0,1,0}`, `Up {0,0,1}`, `Right {1,0,0}`
  - `Left −1`, `RightEdge 1`, `Bottom −0.5625`, `Top 0.5625`
  - `Near 15`, `Far 400000`
  - viewport `0, 0, 1920, 1080`

  Tests:
  - `DepthEndpoints`: `FarPassDepth(15,15,400000) == 0` within 1e-7; `FarPassDepth(400000,15,400000)` within 1e-6 of 1.
  - `DepthMatchesDeviceRow`: `FarPassDepth(990,15,400000)` within 1e-5 of `0.9848854`.
  - `CenteredBound`: center `{0,1000,0}`, r 10 gives `Rect_Ok` with `X0 949.5`, `X1 970.5`, `Y0 529.5`, `Y1 550.5` (each within 1e-3) and `Z` within 1e-5 of `FarPassDepth(990,15,400000)`.
  - `CrossesNearPlane`: center `{0,20,0}`, r 10 gives `Rect_CrossesNear`.
  - `BehindCamera`: center `{0,−1000,0}`, r 10 gives `Rect_CrossesNear`.
  - `OffScreen`: center `{5000,1000,0}`, r 10 gives `Rect_OffScreen`.
  - `ClampsAtScreenEdge`: center `{1000,1000,0}`, r 50 gives `Rect_Ok`, `X1 == 1919.5` and `X0 < 1919.5`.
  - `SubPixelBoundStillCoversAPixel`: center `{0,100000,0}`, r 1 gives `Rect_Ok`, `X1 − X0 >= 1` and `Y1 − Y0 >= 1`.
  - `ViewportOffset`: viewport `100, 50, 1920, 1080`, center `{0,1000,0}`, r 10 gives `X0 1049.5` and `Y0 579.5`.
  - `SphereInViewCases`: `{0,1000,0}` r 10 is true; `{0,−1000,0}` r 10 is false; `{0,−5,0}` r 10 is true; `{5000,1000,0}` r 10 is false; `{1100,1000,0}` r 200 is true.
  - `ClassifyCases`:
    - `(2000,1024,false,0,0)` gives `Mode_Cullable`;
    - `(500,1024,false,0,0)` gives `Mode_AlwaysDraw`;
    - `(2000,1024,true,10,20)` gives `Mode_AlwaysDraw`;
    - `(2000,1024,true,30,20)` gives `Mode_Cullable`;
    - `(2000,1024,false,10,20)` gives `Mode_Cullable`.
  - `ResultsUsableWhenConsecutive`: slot `{7, {0,0,0}, &k, true}`, serial 8, cam `{0,0,0}`, key `&k`, cut 128 gives true.
  - `ResultsUnusableWhenStale`: the same slot with serial 9 gives false. With `Valid = false` it also gives false.
  - `ResultsUnusableAfterCameraCut`: cam `{0,129,0}` gives false; cam `{0,127,0}` gives true.
  - `ResultsUnusableAcrossWorlds`: key `&other` gives false.

  `Run-OcclusionCullMathTest.ps1`: copy `Run-GrassTrailMapTest.ps1`, compiling only the test `.cpp` (the header is self-contained), with exe `OcclusionCullMathTest.exe`.

- [ ] **Step 2: Run it to confirm it fails.** Run through the PowerShell tool: `powershell -File Tools\Tests\Run-OcclusionCullMathTest.ps1`. Expected: a compile error, because `OcclusionCullMath.h` doesn't exist.

- [ ] **Step 3: Implement `TESReloaded/Core/OcclusionCullMath.h`** with the interfaces above. Mark every function `inline`, and give each public symbol a `///` comment.

- [ ] **Step 4: Run the harness.** Expected: `0 failures`, exit code 0.

- [ ] **Step 5: Commit** `OcclusionCullMath.h` and the two test files as `feat(Occlusion): Add the occlusion cull math and harness`.

---

### Task 2: Settings, profiler buckets and module skeleton

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h`
  - `Main` struct: after `float WaterReflectionCullMinSize;` (line 108)
  - `Develop` struct: next to `UInt8 NearShellDebug;` (line 301)
- Modify: `TESReloaded/Core/SettingManager.cpp`
  - load: after `WaterReflectionCullMinSize` near line 86, and Develop near line 452
  - save: near line 1472
  - menu map: near line 2158
  - menu set: near line 2767
- Modify: `TESReloaded/Core/FrameProfiler.h`, `TESReloaded/Core/RenderHook.cpp` (name tables, lines 21-45)
- Create: `TESReloaded/Core/OcclusionCulling.h`, `TESReloaded/Core/OcclusionCulling.cpp`
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj` and `.filters`, next to the `RainProbe` entries
- Modify: `TESReloaded/Core/CoSave.cpp` (both callbacks, next to `ResetRainProbe()`)

**Interfaces:**
- Produces settings:
  - `SettingsMain.Main.OcclusionCulling` (`bool`, default 0)
  - `SettingsMain.Main.OcclusionAlwaysDrawDistance` (`float`, default 1024, clamped `>= 0`)
  - `SettingsMain.Main.OcclusionCameraCutDistance` (`float`, default 128; values `<= 0` become 128)
  - `SettingsMain.Main.OcclusionMaxQueries` (`UInt32`, default 8192, clamped to 256..65536)
  - `SettingsMain.Develop.OcclusionCullDebug` (`UInt8`, default 0): 1 = stats log, 2 = freeze, 3 = measure only

  The four `Main` keys go through load, save, menu map and menu set. `OcclusionCullDebug` is load only, like `NearShellDebug`.
- Produces profiler entries:
  - `FrameProfiler::Buck_OccBegin`, `Buck_OccEnd`, `Buck_OccIssue`: top-level, inserted after `Buck_ShaderHook` and before `Buck_HookEngine`. Names `"Occ:Begin"`, `"Occ:End"`, `"Occ:Issue"`.
  - `Cnt_OccCandidates`, `Cnt_OccQueried`, `Cnt_OccHidden`, `Cnt_OccCulled`, appended before `Cnt_COUNT`. Names `"OccCandidates"`, `"OccQueried"`, `"OccHidden"`, `"OccCulled"`.
- Produces `OcclusionCulling.h` (every function is main thread only and returns immediately while `OcclusionCulling` is 0):
  - `void BeginOcclusionCull();`: before the far pass.
  - `void EndOcclusionCull();`: immediately after the far pass.
  - `void IssueOcclusionQueries();`: after the far-pass depth resolve, before the shell clear.
  - `void ResetOcclusionCull();`: drops all slots and the hidden set (load and new game).
  - `bool OcclusionCullFlagsHeld();`: true between a Begin that set flags and its End.

- [ ] **Step 1: Add the settings fields, with `///` comments, and their four plumbing sites.** Mirror `WaterReflectionCullMinSize` exactly at each site.
- [ ] **Step 2: Add the buckets, counters and name strings.** The `BucketNames` / `CounterNames` arrays are sized by the enum, so a missing name is a compile error. Check that the counts match.
- [ ] **Step 3: Create the module with empty bodies**, including the `#if defined(OBLIVION)` wrapper. Add it to the vcxproj and filters. Call `ResetOcclusionCull()` in both CoSave callbacks.
- [ ] **Step 4: Regenerate clangd data and build.** Run `powershell -File Tools\GenerateCompileCommands.ps1`, then the Global Constraints build. Expected: the DLL line and no errors.
- [ ] **Step 5: Check the setting plumbing.** In `git diff TESReloaded/Core/SettingManager.cpp`, confirm that each of the four `Main` keys appears at all four sites (load, save, map, set), and that `OcclusionCullDebug` appears at the Develop load site only.
- [ ] **Step 6: Commit** as `feat(Occlusion): Add the occlusion culling settings and module skeleton`.

---

### Task 3: Candidate walk, query issue and readback (measure only)

The task gives the module everything except the flag write. Until Task 4, `BeginOcclusionCull` reads results and counts what *would* be culled, but never sets a flag. This proves the projection in game before any object can disappear.

**Files:**
- Modify: `TESReloaded/Core/OcclusionCulling.cpp`
- Modify: `TESReloaded/Core/RenderHook.cpp`, in `TrackRenderObject` (lines 1566-1613)

**Interfaces:**
- Consumes: Task 1's `OcclusionCullMath` and Task 2's settings, buckets, counters and declarations.
- Module-private state (names fixed so Task 4 can extend it):
  - `struct QueryEntry { UInt64 Key; UInt32 Query; };`
  - `struct Slot { SlotMeta Meta; std::vector<QueryEntry> Entries; std::vector<IDirect3DQuery9*> Queries; };`
  - `Slot Ring[3]`, `UInt32 Serial` (incremented per Begin), `std::unordered_set<UInt64> HiddenSet`
  - `std::vector<Candidate> Candidates`, with `struct Candidate { TESObjectREFR* Ref; NiNode* Node; UInt64 Key; NiBound* Bound; };`. Holds only `Mode_Cullable`, in-view references.
  - `IDirect3DVertexBuffer9* RectVB`: dynamic, write-only, `D3DFVF_XYZRHW`, `OcclusionMaxQueries × 4` vertices, `D3DPOOL_DEFAULT`
  - the key is `((UInt64)(UInt32)Ref << 32) | (UInt32)Node`

- [ ] **Step 1: Implement `BeginOcclusionCull()` without the flag write.**
  1. If `OcclusionCulling` is off, release everything (queries, VB, sets) when it is held, and return.
  2. Gate: return when `Player`, `Tes`, `WorldSceneGraph` or its camera is NULL (memory `beginscene-pre-init-window`).
  3. Build a `CullCamera` from the main camera. `Frustum.Near/Far` already hold the far pass's values, because `ApplyPass(PassFar)` has run.
  4. Usability: `++Serial` (only after the gates pass). The previous slot is `Ring[(Serial − 1) % 3]`. `WorldKey` is `Player->GetWorldSpace()`, or `Player->parentCell` when that is NULL.
     - When `ResultsUsable` holds and debug isn't 2 (freeze), rebuild `HiddenSet`. For each entry, `GetData(&Pixels, sizeof(DWORD), 0) == S_OK && Pixels == 0` puts its key in the set. Count each `S_FALSE` as not ready.
     - When results aren't usable and debug isn't 2, clear `HiddenSet`.
  5. Walk the cells as `ShadowManager::BuildExteriorGeoItems` does (grid when there is a worldspace, else `Player->parentCell`). For each reference:
     - Skip it when its node is NULL, its root is already `AppCulled`, it is the player, or its form type is not a candidate type.
     - `Bound = Node->GetWorldBound()`. Skip it when `SphereInView` fails.
     - Classify with `DistToSurface = |center − cam| − r`, `HasWater = ShaderConst.HasWater`, `WaterHeight = ShaderConst.Water.waterSettings.x`. `Mode_AlwaysDraw` only counts. `Mode_Cullable` is pushed to `Candidates`.
     - If its key is in `HiddenSet`, count it as hidden. (Task 4 adds the flag write here.)
  6. Count `Cnt_OccCandidates`, wrapped in `FrameProfiler::Scope(Buck_OccBegin)`.
- [ ] **Step 2: Implement `IssueOcclusionQueries()`.**
  1. Return when `OcclusionCulling` is off or `Candidates` is empty. Begin clears `Candidates` first thing, including when it gates out, so Issue never queries a stale list. Clear the target slot's `Entries` and mark its meta invalid before filling it.
  2. `CaptureDeviceState()` (TheShaderManager). Bail out if it returns false.
  3. Rebuild the `CullCamera` (same frustum). Read `GetViewport`. Take the camera move since the last Issue, and give each candidate an inflated radius of `r + |move|`.
  4. Lock `RectVB` with `D3DLOCK_DISCARD`. For each candidate, while under the cap:
     - `ProjectBoundRect`. Only `Rect_Ok` writes 4 vertices `(x, y, Z, 1)` in strip order `(X0,Y0) (X1,Y0) (X0,Y1) (X1,Y1)`, plus a `QueryEntry`.
     - Create queries lazily into the slot.
     - Past the cap, log once, then stop.
  5. State: `SetVertexShader(NULL)`, `SetPixelShader(NULL)`, `SetFVF(D3DFVF_XYZRHW)`, `SetStreamSource(0, RectVB, 0, 16)`, and the render states:
     - `ZENABLE TRUE`, `ZFUNC LESSEQUAL`, `ZWRITEENABLE FALSE`
     - `COLORWRITEENABLE 0`
     - `ALPHABLENDENABLE`, `ALPHATESTENABLE`, `STENCILENABLE`, `SCISSORTESTENABLE` all FALSE
     - `CULLMODE NONE`, `CLIPPLANEENABLE 0`, `FOGENABLE FALSE`
     - `DEPTHBIAS 0`, `SLOPESCALEDEPTHBIAS 0`

     Per entry: `Issue(D3DISSUE_BEGIN)`, `DrawPrimitive(D3DPT_TRIANGLESTRIP, 4·i, 2)`, `Issue(D3DISSUE_END)`.
  6. Restore with `CachedStateBlock->Apply()`. The render targets are untouched, so `RestoreTargets` isn't needed. Expose a `ShaderManager::RestoreDeviceState()` that applies `CachedStateBlock`, if no public path exists.
  7. Store the slot meta `{Serial, camera pos, WorldKey, Valid = true}` in `Ring[Serial % 3]`. Count `Cnt_OccQueried`, wrapped in `Scope(Buck_OccIssue)`.
  8. On the first issue with debug >= 1, log the computed `_33/_43` (`Far/(Far−Near)`, `−Near·Far/(Far−Near)`) next to `GetTransform(D3DTS_PROJECTION)`'s, to confirm the depth row.
- [ ] **Step 3: Implement the stats log.** With debug >= 1, about once a second (`GetTickCount` delta >= 1000), log `[Occlusion]` with:
  - candidates, always-drawn, queried, ready, not-ready, hidden, culled
  - dropped (results unusable), overflow

  These are per-second averages per far pass.
- [ ] **Step 4: Wire it into `TrackRenderObject`.**
  - `BeginOcclusionCull()` inside `if (MainScenePass)`, after `ApplyPass(PassFar)` and `GpuBegin`, immediately before `RenderObject`.
  - `if (MainScenePass) EndOcclusionCull();` on the line straight after `RenderObject`.
  - `IssueOcclusionQueries();` right after `TheRenderManager->ResolveDepthBuffer();` (line 1593), before `if (RenderManager::ShellActive)`.
- [ ] **Step 5: Build** (Global Constraints). Expected: no errors.
- [ ] **Step 6: Check in game (the user runs the game).**
  - Set `OcclusionCulling = 1` and `Develop.OcclusionCullDebug = 3` (measure only), and stand in the town street spot.
  - Expected:
    - the depth-row log line matches the device `_33 ≈ 1.00004`, `_43 ≈ −15.0006`;
    - `ready` is near `queried`;
    - `hidden / queried` is about 0.8 or more, in line with the census's 85.6 % per geometry.
  - In the ordinary exterior control, expect a much lower hidden share.
  - Nothing on screen may change.
- [ ] **Step 7: Commit** as `feat(Occlusion): Query each reference's bound against the far-pass depth`.

---

### Task 4: Cull hidden references around the far pass

**Files:**
- Modify: `TESReloaded/Core/OcclusionCulling.cpp`
- Modify: `TESReloaded/Core/ShadowManager.cpp` (top of `ShadowManager::RenderShadowMaps`, line 2633)

**Interfaces:**
- Consumes: Task 3's state.
- Module-private: `std::vector<NiNode*> CulledNodes`.

- [ ] **Step 1: Write the flags in Begin.** In item 5 of Task 3 Step 1 (the cell walk in `BeginOcclusionCull`), apply this when debug isn't 3: if a candidate's key is in `HiddenSet`, its root is not already `AppCulled` (the `WasAlreadyCulled` rule; such roots never become candidates), and it wasn't already pushed this frame, then set `m_flags |= kFlag_AppCulled` and push it to `CulledNodes`. Count `Cnt_OccCulled`.
- [ ] **Step 2: Implement `EndOcclusionCull()`.** Clear `kFlag_AppCulled` on each node in `CulledNodes`, then empty the list, wrapped in `Scope(Buck_OccEnd)`. `OcclusionCullFlagsHeld()` returns `!CulledNodes.empty()`. When the setting goes off, `BeginOcclusionCull` must call the same clear before releasing anything.
- [ ] **Step 3: Add the guard** at the top of `ShadowManager::RenderShadowMaps`: if `OcclusionCullFlagsHeld()`, log `"ERROR: Occlusion cull flags held while shadow maps render"` once.
- [ ] **Step 4: Build** (Global Constraints).
- [ ] **Step 5: Check in game (the user runs the game).** `OcclusionCulling = 1`, `OcclusionCullDebug = 1`. In the town spot:
  - [ ] Stats show `culled` close to `hidden`, and the guard line never appears.
  - [ ] ProtoPlugin RenderCount (`bCountRender = 1`) `sceneGeoms` falls from ~10.7k toward 1.5-3k.
  - [ ] Visual:
    - pan fast, step around corners and walk through a door;
    - check a load door, fast travel and the first/third-person toggle, which must draw everything that frame;
    - stand by water and confirm underwater objects show through it.

    Report any pop-in seen.
  - [ ] Debug 2 (freeze): walk away from the spot and confirm that the skipped objects are exactly the ones hidden from the original spot (holes appear only where the view was blocked).
  - [ ] Fade risk (spec risk 2): watch objects that reappear around a corner. A fade-in from transparent is a failure. If it happens, stop and report it (approach 2 fallback).
  - [ ] A disabled reference (`disable` on a clutter item in the console), viewed around a corner, stays hidden afterwards.
  - [ ] Turn `OcclusionCulling` off in the in-game menu: the stats stop, the scene draws everything, and no object stays missing.
- [ ] **Step 6: Commit** as `feat(Occlusion): Skip references hidden in the last far pass`.

---

### Task 5: Measure and record

- [ ] **Step 1: Timing (the user runs PresentMon with all ProtoPlugin probes off).** Measure the town street spot and the ordinary exterior control, culling on and off. Expect the town well under 35.2 ms and the control within noise of 5.8 ms. Also report the `Occ:Begin`, `Occ:End` and `Occ:Issue` buckets from a `Develop.ProfileFrame = 1` run.
- [ ] **Step 2: If the `Occ:Issue` cost is a large share of the saving,** stop and bring the numbers to the user before any CHC++-style re-query interval (spec risk 3).
- [ ] **Step 3: Write memory `occlusion-culling.md`** (type `project`): the measured town and control results, the hidden share, the bucket costs, any pop-in seen, and the rules that held (the Begin/End/Issue placement, never querying after the shell clear, keys rather than pointers). Add its `MEMORY.md` line. Add `OcclusionCulling` to AGENTS.md's "Other Core modules" list.
- [ ] **Step 4: Commit** the memory, MEMORY.md and AGENTS.md as `docs: Record the occlusion culling results`.
