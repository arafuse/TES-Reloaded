# Reflection Occlusion Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Skip, in the water reflection pass, references that were hidden in the previous frame's reflection map, so the dense town's ~3,300 reflection draws shrink. Keep it only if the town frame improves by at least 1 ms.

**Architecture:**
- The existing far-pass culler (`OcclusionCulling.cpp`) gains a second, reflection-only path. Its per-frame cell walk also builds a reflection candidate list.
- On the first reflection-cull call of the reflection pass, last frame's reflection queries are read into a set of root nodes to skip. `TrackWaterReflectionCull` skips those nodes.
- A redirected call at 0x49C81B queries every candidate against the reflection map's depth just before the reflection scene's `EndScene`.
- Projection uses the device's view and projection matrices, through a new matrix-based function in `OcclusionCullMath.h`, proven by the existing standalone test harness.

**Tech Stack:** C++ (MSVC v145, x86), D3D9 occlusion queries on DXVK, OBSE plugin, Microsoft Detours / `WriteRelCall`.

**Spec:** `docs/superpowers/specs/2026-11-20-reflection-occlusion-culling-design.md`. It builds on `docs/superpowers/specs/2026-11-20-occlusion-culling-design.md`; read both.

## Global Constraints

- **Scope:** Oblivion only. All new code goes inside the existing `#if defined(OBLIVION)` regions.
- **Coding style (AGENTS.md):** public symbols get `///` doc comments; inline comments only when necessary, 1-3 lines at most.
- **Build:** through the PowerShell tool, never Bash (memory `build-via-powershell-not-bash`):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  Expected: the `OblivionReloaded.vcxproj -> ...\OblivionReloaded.dll` line and no ` error ` lines. The game must not be running.
- **Math tests:** `powershell -File Tools\Tests\Run-OcclusionCullMathTest.ps1`. Expected last line: `0 failures`, exit code 0.
- **Off is inert:** with `[Main] OcclusionCullReflection = 0` or `OcclusionCulling = 0`, no reflection query or set is created, the walk builds no reflection list, and `TrackWaterReflectionCull` behaves exactly as today.
- **Never lock a GPU surface.** Read only `GetData(&Pixels, sizeof(DWORD), 0)`. `S_FALSE` or failure means visible.
- **One-frame lag only:** use only the slot issued in the immediately previous frame (`Slot.Serial + 1 == Serial`).
- **Skip only inside the reflection pass:** every skip is gated on `*(UInt8*)0xB42CE8 != 0`.
- **The far pass is unchanged:** its results, hidden counts, flags and placement stay exactly as they are.
- **Kill gate (user's decision):** after Task 5, measure. Keep going only if the town frame improves by **at least 1 ms net**; otherwise do Task 6B.
- **Commits:** code as `feat(Occlusion): ...` or `feat(Profiler): ...`. Memory and plan changes only in separate `docs:` commits. Do not push.
- **In-game steps need the user.** The agent builds; the user runs the game and returns the log or PresentMon numbers.

**Plan calls (not in the spec, flagged for review):**
- `OcclusionCullDebug = 3` (measure only) is implemented for the reflection in Task 4, not in phase 3. Risk 1's check needs it at the gate.
- The reflection issue reuses the far pass's rect vertex buffer (`EnsureRectBuffer`). Locking it with `D3DLOCK_DISCARD` a second time in a frame is safe, because the far pass's draws are already submitted.
- The redirected `EndScene` call is a plain `__cdecl` C++ function. 0x7D72D0 takes no arguments, and the caller rewrites `eax`, `ecx` and `edx` before reading them (0x49C820-0x49C88D), so no naked thunk is needed.

## Review Focus

1. **No water in the loaded cells, or reflections off in Oblivion.ini.** The reflection pass never runs. Expected: no reflection queries, no skip set built, the next frame's results dropped, nothing culled. Pinned by `ReflectionResultsUnusableWithoutPreviousPass` (Task 2) and the stats check in Task 5.
2. **Crossing from one water body to another at a different height** (e.g. a lake to the sea). Expected: one frame drawn in full, then culling resumes. Pinned by `ReflectionResultsUnusableAfterWaterHeightChange` (Task 2).
3. **Mirrored camera** (negative-determinant view). Expected: rects identical to projecting the mirrored point with the unmirrored camera. Pinned by `MatrixProjectionHandlesMirroredView` (Task 2).
4. **A bound straddling the reflection camera's near plane, or behind it.** Expected: not queried, so drawn. Pinned by `MatrixProjectionCrossesNear` (Task 2).
5. **A cell unloading between a reflection pass and the next frame.** Expected: no stale pointer is dereferenced. The skip set holds only roots from this frame's walk, and the hook only compares pointers. Reviewed in Task 4: no `NiNode*` from a previous frame is stored outside a `UInt64` key.

---

### Task 1: Reflection row in the draw counter

**Files:**
- Modify: `TESReloaded/Core/FrameProfiler.h` (the `Cnt_DrawsFar, Cnt_DrawsShell, Cnt_DrawsOther` line and its comment)
- Modify: `TESReloaded/Core/RenderHook.cpp` (`CounterNames`; the per-pass count in `TrackSetupShaderPrograms`)

**Interfaces:**
- Produces: `static UInt8* const kIsRenderingReflectionPass = (UInt8*)0x00B42CE8;` in `RenderHook.cpp`, next to `kIsRenderingWaterReflections`. Task 5 uses it.

- [ ] **Step 1:** Change the counters to `Cnt_DrawsReflection, Cnt_DrawsFar, Cnt_DrawsShell, Cnt_DrawsOther`. Use the names `"  reflection", "  far pass", "  shell", "  other"`. Classify a draw as reflection when `*kIsRenderingReflectionPass` is set, checked before `InMainScenePass`. Doc comment: 0xB42CE8 is set by `NiRenderer_ReflectionPass` 0x49BEF0, from 0x49C4CC to 0x49C809.
- [ ] **Step 2:** Build. Expected: success.
- [ ] **Step 3:** Commit `feat(Profiler): Count reflection pass draws by the 0xB42CE8 flag`.

### Task 2: Matrix projection and the reflection result rule

**Files:**
- Modify: `TESReloaded/Core/OcclusionCullMath.h`
- Test: `Tools/Tests/OcclusionCullMathTest.cpp`

**Interfaces:**
- Produces:
  - `float WaterHeight;` added to `SlotMeta`. The far pass leaves it 0 and never reads it.
  - `inline RectResult ProjectBoundRectMatrix(const float ViewProj[16], float Near, const float Center[3], float Radius, float ViewportX, float ViewportY, float ViewportW, float ViewportH, QueryRect* Out)`. `ViewProj` is D3D row-vector order (`clip = [x y z 1] * M`, element `[row*4 + col]`). `Near` is the projection's near distance (`-_43 / _33`). Any corner with `clip.w < Near` returns `Rect_CrossesNear`. The screen mapping, pixel widening and clamping match `ProjectBoundRect`. `Out->Z` is the minimum corner `clip.z / clip.w`.
  - `inline bool ReflectionResultsUsable(const SlotMeta& Slot, unsigned CurrentSerial, const float CameraPos[3], const void* WorldKey, float CutDistance, float WaterHeight)`. Returns `ResultsUsable(...)` and `Slot.WaterHeight == WaterHeight`.

- [ ] **Step 1: Write the failing tests.** Test-only helpers:
  - `ViewFromCam(const CullCamera&, float Out[16])`: rows `[Rx Ux Fx 0] [Ry Uy Fy 0] [Rz Uz Fz 0] [-R·P -U·P -F·P 1]`.
  - `ProjFromCam(const CullCamera&, float Out[16])`: `_11 = 2/(RightEdge-Left)`, `_22 = 2/(Top-Bottom)`, `_31 = -(RightEdge+Left)/(RightEdge-Left)`, `_32 = -(Top+Bottom)/(Top-Bottom)`, `_33 = Far/(Far-Near)`, `_34 = 1`, `_43 = -Near*Far/(Far-Near)`.
  - `Mul4(A, B, Out)`.

  Tests:
  - `MatrixProjectionMatchesBasisProjection`: with `Cam()` and the 1920x1080 viewport, bound (0,1000,0) r10 gives `X0 949.5, X1 970.5, Y0 529.5, Y1 550.5`, and `Z` within 1e-5 of `FarPassDepth(990, 15, 400000)`. Also bound (300,2000,-100) r50 matches `ProjectBoundRect`'s rect within 1e-3.
  - `MatrixProjectionCrossesNear`: (0,20,0) r10 and (0,-1000,0) r10 both return `Rect_CrossesNear`.
  - `MatrixProjectionOffScreen`: (100000,1000,0) r10 returns `Rect_OffScreen`.
  - `MatrixProjectionHandlesMirroredView`: camera at (0,0,100), water at z = 0, mirror `Diag(1,1,-1,1)`. ViewProj `Mirror * View * Proj` projecting (0,1000,50) r10 equals the plain ViewProj projecting (0,1000,-50) r10: all four edges and `Z` within 1e-4.
  - `ReflectionResultsUnusableAfterWaterHeightChange`: a slot that is otherwise usable, with `WaterHeight` 0 against current -100, returns false; with equal heights it returns true.
  - `ReflectionResultsUnusableWithoutPreviousPass`: `Slot.Serial` two behind returns false.
- [ ] **Step 2:** Run the math tests. Expected: compile error (undefined functions).
- [ ] **Step 3:** Implement the three items in `OcclusionCullMath.h`, with doc comments.
- [ ] **Step 4:** Run the math tests. Expected: `0 failures`.
- [ ] **Step 5:** Commit `feat(Occlusion): Project bounds through a view-projection matrix`.

### Task 3: The OcclusionCullReflection switch (INI read only)

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h` (MainStruct, after `OcclusionMaxQueries`)
- Modify: `TESReloaded/Core/SettingManager.cpp` (INI read, after the `OcclusionMaxQueries` line)

**Interfaces:**
- Produces: `bool OcclusionCullReflection; ///< also cull the water reflection pass (needs OcclusionCulling)`, read with `GetPrivateProfileIntA("Main", "OcclusionCullReflection", 0, Filename)`.

- [ ] **Step 1:** Add the field and the INI read. No save path, menu or template yet (Task 7).
- [ ] **Step 2:** Build. Expected: success.
- [ ] **Step 3:** Commit together with Task 4. The switch has no effect on its own.

### Task 4: Reflection culling in the culler module

**Files:**
- Modify: `TESReloaded/Core/OcclusionCulling.h`, `TESReloaded/Core/OcclusionCulling.cpp`
- Modify: `TESReloaded/Core/FrameProfiler.h`, plus `BucketNames` and `CounterNames` in `RenderHook.cpp`

**Interfaces:**
- Consumes: `ProjectBoundRectMatrix`, `ReflectionResultsUsable` and `SlotMeta::WaterHeight` (Task 2); `SettingsMain.Main.OcclusionCullReflection` (Task 3).
- Produces (in `OcclusionCulling.h`, documented):
  - `bool SkipInReflection(NiAVObject* Object)`. Called only while 0xB42CE8 is set. On its first call after a `BeginOcclusionCull`, it reads last frame's reflection slot and builds the skip set (timed as `Buck_OccReflBegin`). It returns true when `Object` is in the set and culling is allowed (debug ≠ 3). It counts every call and every skip.
  - `void IssueReflectionOcclusionQueries()`. Called just before the reflection scene's `EndScene`. Timed as `Buck_OccReflIssue`.
- Profiler additions:
  - top-level disjoint buckets `Buck_OccReflBegin`, `Buck_OccReflIssue`, named `"Occ:ReflBegin"` and `"Occ:ReflIssue"`;
  - counters `Cnt_OccReflCandidates`, `Cnt_OccReflQueried`, `Cnt_OccReflHidden`, `Cnt_OccReflSkipped`.

- [ ] **Step 1: Collect the candidates.** In `CollectCell`, a reference that passes the existing node, AppCulled, type and bound checks is added to the reflection list when all of these hold:
  - the reflection is enabled;
  - `HasWater`;
  - `Center.z + Radius >= WaterHeight`;
  - the distance to its surface is at least `OcclusionAlwaysDrawDistance`.
  This happens before and independently of the main-frustum test. The far pass's flow is untouched. Store `{ UInt64 Key; NiNode* Node; float Center[3]; float Radius; }`, copied this frame, never kept across frames.
- [ ] **Step 2: Add the reflection ring.**
  - A second `Slot ReflRing[kRingSize]` with its own query pool.
  - The slot meta stores `Serial`, the main camera position, `CurrentWorldKey()` and `WaterHeight` (`ShaderConst.Water.waterSettings.x` at issue).
  - `ReleaseResources`, `HoldsResources` and `ResetOcclusionCull` cover it.
  - `BeginOcclusionCull` releases the reflection resources when the switch is off, and clears the skip set and the "read this frame" latch every frame.
- [ ] **Step 3: Implement `SkipInReflection`.**
  - The lazy read uses `ReflectionResultsUsable` with this frame's `Serial`, the main camera position from the walk and the current water height.
  - Hidden keys found in this frame's reflection list put their `Node` into an `std::unordered_set<NiAVObject*>`.
  - A dropped read counts as a dropped frame.
- [ ] **Step 4: Implement `IssueReflectionOcclusionQueries`.**
  - Return if disabled or the list is empty.
  - Get `D3DTS_VIEW` and `D3DTS_PROJECTION` with `Device->GetTransform`, combine them into ViewProj, take `Near = -P._43 / P._33`, and read the viewport.
  - Inflate each radius by the main camera's move since the last reflection issue, capped at `OcclusionCameraCutDistance`, with its own `IssueCameraPos`.
  - Draw the quads with the same render states, state capture and restore as `IssueOcclusionQueries`. Factor the state block into a shared static function rather than copying it.
  - Respect the `OcclusionMaxQueries` cap and log the first overflow.
- [ ] **Step 5: Debug output.**
  - With `OcclusionCullDebug ≥ 1`, log once, on the first reflection issue:
    - the bound depth-stencil surface's `Width`, `Height` and `Format`, and the render target's (`GetDepthStencilSurface` / `GetRenderTarget`, `GetDesc`, `Release`; a missing depth surface logs `none`);
    - the eye position recovered from the inverse of the device view matrix;
    - the main camera's position mirrored about the water height, `(x, y, 2h - z)`.
  - Every second, log `[Occlusion] reflection per pass over N: candidates, queried, ready, not ready, hidden, skipped; cull calls in pass X, outside pass Y; dropped frames, overflow frames; water height H`.
  - "Outside pass" counts `TrackWaterReflectionCull` calls while 0xB42CE8 is clear (Task 5 reports them).
- [ ] **Step 6:** Build. Expected: success.
- [ ] **Step 7:** Commit Tasks 3 and 4 as `feat(Occlusion): Query and skip references hidden in the reflection map`.

### Task 5: Hook the reflection pass

**Files:**
- Modify: `TESReloaded/Core/RenderHook.cpp` (`TrackWaterReflectionCull`; the Oblivion `WriteRelCall`s in the hook installer)
- Modify: `TESReloaded/Core/OcclusionCulling.h`, `TESReloaded/Core/OcclusionCulling.cpp` (the outside-pass call count)

**Interfaces:**
- Consumes: `SkipInReflection`, `IssueReflectionOcclusionQueries` (Task 4); `kIsRenderingReflectionPass` (Task 1).
- Produces: `void CountReflectionCullOutsidePass()` in `OcclusionCulling.h`, which Task 5's hook calls.

- [ ] **Step 1:** In `TrackWaterReflectionCull`, before the size test:
  - if `*kIsRenderingReflectionPass` is set and `SkipInReflection(Object)` returns true, return without calling the original;
  - if the flag is clear, call `CountReflectionCullOutsidePass()` and continue as today.
- [ ] **Step 2:** Add `static void __cdecl ReflectionPassEndScene()`. It calls `IssueReflectionOcclusionQueries()`, then `((void (__cdecl*)())0x007D72D0)()`. Install it with `WriteRelCall(0x0049C81B, (UInt32)ReflectionPassEndScene)` in the Oblivion block, always (it early-outs when off). Doc comment: the call site is in `NiRenderer_ReflectionPass` and runs once per reflection pass, while the reflection texture and its depth are still bound.
- [ ] **Step 3:** Build. Expected: success.
- [ ] **Step 4:** Commit `feat(Occlusion): Issue reflection queries before the reflection pass ends`.

### Task 6: Gate measurement (user in game)

- [ ] **Step 1: Risk checks.** The user sets `ProfileFrame = 1`, `OcclusionCullDebug = 3`, `OcclusionCullReflection = 1` and stands at the town spot. From the log:
  - **Risk 1:** the depth surface exists and matches `WaterReflectionMapSize` (1024x1024).
  - **Risk 3:** the device eye position is within ~1 unit of the mirrored main camera.
  - **Risk 2:** "cull calls outside pass" is 0, or comes only from a source identified by disassembly of 0x4E67C7.
  - **Query health:** "not ready" is under 5 % of queried.
  - **Hidden share:** recorded.
  If Risk 1 or Risk 3 fails, stop and report to the user. The fallback for Risk 3 (building the matrix from the reflection NiCamera) is a new decision.
- [ ] **Step 2: Profiler A/B.** `OcclusionCullDebug = 1`, then `ProfileFrame = 1` with `OcclusionCullReflection` 0 and 1. Record the reflection draw row, `Occ:ReflBegin`, `Occ:ReflIssue` and `Occ:Begin`.
- [ ] **Step 3: PresentMon A/B.** 60 s standing still, ProtoPlugin probes off, switch 0 and 1. Record the median frame time and CPU busy.
- [ ] **Step 4: Decide.** A frame time improvement of at least 1 ms continues to Task 7. Anything less goes to Task 6B.
- [ ] **Step 5:** Record the RE facts in `memory/water-reflection-pass-detection.md`: 0x49BEF0's structure, the 0xB42CE8 flag, the 0x49C81B EndScene site and the depth-surface finding. Commit as `docs: Record the reflection pass structure`.

### Task 6B (only if the gate fails): Shelve it

- [ ] **Step 1:** `git revert` the Task 4 and Task 5 commits, and Task 2's if nothing else uses `ProjectBoundRectMatrix`. Keep Task 1. Remove the Task 3 field along with its commit.
- [ ] **Step 2:** Build and run the math tests. Expected: success and `0 failures`.
- [ ] **Step 3:** Write `memory/reflection-occlusion-not-worth-it.md` with the measured numbers (draw row, hidden share, own cost, PresentMon), add its `MEMORY.md` line, and commit as `docs: Shelve reflection occlusion culling`.

### Task 7 (only if the gate passes): Finish the settings and debug modes

**Files:**
- Modify: `TESReloaded/Core/SettingManager.cpp` (the save path next to line 1481, the menu list next to line 2171, the setter next to line 2784)
- Modify: `OblivionReloaded/OblivionReloaded.ini` (after `OcclusionCulling` at line 118, shipped `0`)
- Modify: `TESReloaded/Core/OcclusionCulling.cpp`

- [ ] **Step 1:** Wire `OcclusionCullReflection` into the save path, the menu list and the setter, following `OcclusionCulling`. Add `OcclusionCullReflection     = 0` to the template, aligned like its neighbours.
- [ ] **Step 2:** Debug mode 2 freezes the reflection's hidden key set as well. Use `ShouldReadResults` with a separate `ReflFrozenSetCaptured`.
- [ ] **Step 3:** Turning the switch off at runtime from the menu releases the reflection ring on the next `BeginOcclusionCull` (already covered by Task 4 Step 2; verify in game).
- [ ] **Step 4:** Build. Commit `feat(Occlusion): Expose reflection culling in the menu and INI template`.

### Task 8 (only if the gate passes): Control, visual check, write-up (user in game)

- [ ] **Step 1:** Ordinary exterior with water (the usual lake-shore scene), PresentMon, switch 0 and 1. Expected: no regression beyond noise.
- [ ] **Step 2:** Debug mode 2 in the town and at the lake shore: walk away from the frozen spot and confirm the holes appear only in the reflection. In normal play, turn and walk along the water: no reflection pop-in beyond a frame.
- [ ] **Step 3:** Add the results to `memory/occlusion-culling.md` (a reflection table like the far pass's, plus the placement rules: the lazy read, the 0xB42CE8 gate, the 0x49C81B site). Commit as `docs: Record the reflection occlusion culling results`.
