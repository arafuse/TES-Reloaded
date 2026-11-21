# Reflection Occlusion Culling — Design

Date: 2026-11-20
Branch: `feat/misc-10`
Builds on: `docs/superpowers/specs/2026-11-20-occlusion-culling-design.md` (the far-pass culler,
`TESReloaded/Core/OcclusionCulling.cpp`). Read that design first; this one only states what differs.

## Intent

The far-pass culler took the dense mod town from 33.8 to 25.3 ms, and that spot is still CPU-bound.
The ProfileFrame split (commit 9b96f14) shows where the draws are now, per frame:

| Pass | Draws |
|---|---|
| Main far pass | 5,098 |
| Near shell | 26 |
| Other (the water reflection pass) | 3,293 |

ProtoPlugin timed the reflection pass at 4.3 ms of CPU before any culling
(`ProtoPlugin/memory/oblivion-dense-town-frame.md`), and nothing culls it by occlusion today.
`WaterReflectionCullMinSize = 0.5` is already on and still leaves ~3.3k draws.

Cull the reflection pass the same way the far pass is culled: query each reference's bound against
the reflection map's own depth, and skip the references that came back hidden in the next frame's
reflection pass.

The user's test spot is the worst case: a thin strip of water is visible from the street, so the
reflection pass must run and cannot simply be skipped. (Skipping the pass when no water is visible
was option 1 and is out of scope here.)

## Goals

- Cut the reflection pass's draws in the dense town by skipping references hidden in the last
  frame's reflection map.
- Visual impact as for the far pass: at most a one-frame lag when something is revealed in the
  reflection.
- Behind one INI switch, off by default. Off, it creates nothing, issues nothing and skips nothing.
- Own CPU cost well below the saving. The far pass's culler costs 0.43 ms for 1255 queries; aim for
  the same order here.
- No regression in an ordinary exterior with water.

## Non-goals

- Skipping the whole reflection pass when no water is visible (option 1).
- Changing the far-pass culler's behaviour. It may be refactored to share code, but its results,
  timing and rules stay as they are.
- New candidate types. Same set as the far pass (statics, containers, doors, furniture, activators,
  flora and items; never actors, trees, lights or the player).
- Water refraction, the near shell, shadow maps, first person.

## Anti-goals (failure even if it "works")

- An object missing from the reflection for more than one frame, or flicker in the reflection.
- Any change in what the far pass, the shell or the shadow maps draw.
- Any GPU-to-CPU surface lock (`memory/d3d9-gpu-readback-stall.md`).
- Skipping nodes in any culling process other than the reflection pass's.

## How the reflection pass works (RE, Oblivion 1.2.416, verified by disassembly)

- `NiRenderer_Render` (0x40C830) calls `CombinedWaterRelfectionPAsses` (0x49E880) at 0x40C8EC.
  That calls `NiRenderer_ReflectionPass` (0x49BEF0) from exactly one site, 0x49E9BF, at most once a
  frame. It runs after the main scene (see `memory/water-reflection-pass-detection.md`).
- Inside 0x49BEF0:
  - 0x49C229 `BSRenderedTexture::UseTextureToRender`, 0x49C231 `NiRenderer_BeginScene`: the
    reflection texture (`[Main] WaterReflectionMapSize`, 1024² in the user's INI) becomes the
    render target.
  - The reflection camera is the `NiCamera*` at `[[esp+0x24]]` (pushed to `SetCameraViewProj`
    0x701970 at 0x49C4EF). A user clip plane at the water height is set at 0x49C70E.
  - A `TESWaterCullingProcess` is built on the stack (0x49C426), and two `NiAVObject_Render` calls
    (0x49C512, 0x49C76A) walk the scene with it. The shader accumulator draws at 0x49C778.
  - The byte at **0xB42CE8** is set to 1 at 0x49C4CC and back to 0 at 0x49C809. It is the
    "rendering the reflection" flag for this path. The commonly used 0xB42E86 is NOT set here,
    which is why the profiler's reflection row read 0.
  - 0x49C81B `call NiRenderer_EndScene` (0x7D72D0) ends the reflection scene. It takes no stack
    arguments (cdecl, nothing pushed).
- Every node the walk visits goes through `TESWaterCullingProcess_ReflectionCull` (0x49CBF0,
  `__thiscall`, one `NiAVObject*`). OR already detours it (`RenderHook::TrackWaterReflectionCull`)
  for `WaterReflectionCullMinSize`. Returning without calling the original skips the node's whole
  subtree. The function is also referenced from 0x4E67C7 and the vtable 0xA3F384, so it may run for
  another culling process too.
- ProtoPlugin patches nothing inside 0x49BEF0, at 0x49CBF0 or at 0x7D72D0. Its FrameSplitProbe only
  patches the call site 0x40C8EC.

## Approaches considered

1. **Chosen: skip in the existing per-node reflection cull hook; query before the reflection's
   EndScene.** No flags are set on scene-graph nodes, so nothing else can see our state; the skip
   only happens inside the reflection pass's own culling process.
2. Set `AppCulled` on hidden roots around the reflection pass, as the far pass does. Works, but
   needs bracketing hooks at the pass's start and end and carries the far pass's risk of a flag
   being read by something else. Option 1 needs neither.
3. Skip the whole pass when no water is visible (option 1 of the earlier discussion). Rejected for
   this work: the test spot has visible water, so it saves nothing there.

## Design

### Frame flow

```
far pass entry: BeginOcclusionCull (existing)
  - existing: read last far-pass results, walk cells, AppCull the hidden far-pass candidates
  - NEW: during the same cell walk, build this frame's reflection candidate list
         (key, root node, bound)
far pass, resolve, IssueOcclusionQueries, shell ... (all unchanged)
reflection pass (0x49BEF0)
  - NEW, on the first TrackWaterReflectionCull call with 0xB42CE8 set: read last frame's
    reflection results and turn them into the set of this frame's candidate roots to skip
  - per node: TrackWaterReflectionCull skips nodes in that set while 0xB42CE8 is set
  - NEW, at 0x49C81B, before EndScene: issue one query per reflection candidate against the
    reflection map's depth, into a reflection query ring
```

### Candidates

- Built in `BeginOcclusionCull`'s existing cell walk, from the same references and the same type
  filter, so there is one walk per frame. The reflection list is NOT filtered by the main view's
  frustum: a reflection can show objects the main camera does not see directly.
- Excluded from the reflection list:
  - roots already `AppCulled` (as now);
  - bounds entirely below the water plane (`Center.z + Radius < waterSettings.x`); they cannot
    reflect. This matches the existing hook's own rule.
  - bounds whose surface is within `OcclusionAlwaysDrawDistance` of the main camera: always drawn
    in the reflection too, so a reveal near the player never lags. (The far pass's
    "reaches below the water" always-draw rule is for refraction and does not apply here.)
- Off-view references are rejected at issue time by the projection (off-screen or behind the
  reflection camera), so only on-screen bounds get a query. If issue time per frame is too high
  in the town, a cheap frustum test against the reflection view-projection may go first (builder's
  call).

### The test

- Same conservative test as the far pass: the bound's world AABB (center ± (radius + camera move))
  projected to one screen rect at its nearest depth; `XYZRHW` quads, `ZFUNC LESSEQUAL`, no depth or
  colour writes, no blending, stencil, clip planes or scissor; one `D3DQUERYTYPE_OCCLUSION` per
  candidate; zero samples means hidden.
- **Projection comes from the reflection pass's own matrices**, not from a `CullCamera` built from
  the NiCamera's basis: the reflection camera is mirrored about the water plane, and a basis-derived
  projection risks a handedness error. Read `D3DTS_VIEW` and `D3DTS_PROJECTION` from the device at
  the issue point (the far pass's depth-row log proved the device's projection matches what the
  engine rasterised with), and the viewport from `GetViewport` (expected 1024²).
  - Add a matrix-based variant of `ProjectBoundRect` to `OcclusionCullMath.h`: project the 8 AABB
    corners with the combined view-projection; any corner with `w <= near` (or clip z < 0) means
    `Rect_CrossesNear` and the reference is not queried (drawn). Depth is the nearest corner's
    `z/w`. Extend `Tools/Tests/OcclusionCullMathTest.cpp` with cases for it, including a mirrored
    (negative-determinant) view matrix.
- Device state is captured and restored around the queries with `TheShaderManager->CaptureDeviceState()` /
  `CachedStateBlock->Apply()`, as the far-pass issue does.

### Hook placement

- **Issue:** `WriteRelCall(0x49C81B, ...)` to a function that issues the reflection queries and
  then calls `NiRenderer_EndScene` (0x7D72D0). The reflection texture and its depth surface are
  still bound there and every reflection draw has been submitted. The wrapper must preserve the
  registers the caller relies on (use a naked thunk that saves and restores them around the C++
  call, or verify from the disassembly after 0x49C820 that only `eax`/`ecx`/`edx` are clobbered).
  Install it only when the setting is on at load, or install it always and early-out (builder's
  call; the far pass's hooks always install).
- **Skip:** in `RenderHook::TrackWaterReflectionCull`, before the existing size test: if
  `*(UInt8*)0xB42CE8` is set, the reflection culling is enabled, and `Object` is in the reflection
  skip set, return without calling the original. Gating on 0xB42CE8 keeps any other caller of
  0x49CBF0 unaffected.
- The skip set holds `NiAVObject*` root pointers from this frame's candidate list, so the hook
  only compares pointers and never dereferences a stale one. It is built lazily on the first
  reflection-cull call of the pass (0xB42CE8 set, set not yet built this frame) and cleared at the
  next `BeginOcclusionCull`.
- **Why the read waits for the reflection pass (my call, from the builder check):** the queries are
  issued at the very end of the reflection scene, after the far pass's. Reading them at the next
  far-pass entry would leave only the post effects, Present and the next frame's shadow maps for the
  GPU to finish them; in a GPU-bound scene most would come back not ready and nothing would be
  culled. Reading at the next reflection pass gives them a full frame, like the far pass's.

### Results

- A separate ring of 3 slots for reflection queries (same struct as the far pass's, its own pool
  and its own `OcclusionMaxQueries` cap). Results are read with `GetData(..., 0)` only; `S_FALSE`
  or failure means visible.
- Read only the most recent reflection slot, at the start of the reflection pass (see Hook
  placement), and only when it was issued in the previous frame (same `Serial` as the far pass's
  ring, which `BeginOcclusionCull` increments). Report the not-ready count in the stats line. A frame with no
  reflection pass leaves no slot, so the next frame draws the reflection in full.
- Drop the results (draw everything in the reflection, but still issue queries) when, in addition
  to the far pass's rules (camera move above `OcclusionCameraCutDistance`, world change, load),
  **the water height changed** since the queried frame. Store the water height in the slot's meta.
- `ResetOcclusionCull` drops the reflection ring and sets too.

### Settings

| Key | Default | Meaning |
|---|---|---|
| `[Main] OcclusionCullReflection` | 0 | Cull the water reflection pass. Has no effect unless `OcclusionCulling` is 1. |

Wire it like `OcclusionCulling`: INI read in `SettingManager.cpp`, the save path, the in-game menu
list, and the tracked INI template (shipped off). Switching it off at runtime releases the
reflection queries and clears the skip set.

`[Develop] OcclusionCullDebug` applies to both culls:
- 1: a second stats line, `[Occlusion] reflection per pass over N: ...`, with the same fields as
  the far pass's line, plus the water height. The first time with debug ≥ 1, log the bound depth
  surface's description and the device view/projection next to the reflection NiCamera's position
  (see Risks 1 and 3).
- 2: freezes the reflection's hidden set as well as the far pass's.
- 3: measure only; the reflection pass skips nothing.

### Profiler

- Fix the reflection draw row: count a draw as reflection when `*(UInt8*)0xB42CE8` is set (checked
  before the far-pass test), restoring the `reflection` row that commit 9b96f14 removed. This is
  the measure of success, so it lands first.
- One CPU bucket `Occ:ReflIssue` for the reflection query issue; the reflection's part of the cell
  walk stays inside `Occ:Begin`. Counters for reflection candidates, queried, hidden and skipped.

## Risks the builder must check

1. **The reflection texture must have a depth buffer with depth testing during the pass.** If it
   has none, or depth is off, every query passes and nothing is culled (safe, but useless). Check
   with the debug log (bound depth surface size matches the reflection map) and with debug mode 3's
   hidden count before relying on it. If there is no usable depth, stop and report back.
2. **Other users of 0x49CBF0.** The skip is gated on 0xB42CE8, so the real check is that no other
   culling process calls 0x49CBF0 while the flag is set. With debug ≥ 1, count 0x49CBF0 calls with
   the flag set and clear, and confirm the flag-set calls all come from inside 0x49BEF0 (e.g. the
   count drops to zero with `bUseWaterReflections=0`).
3. **The device matrices at 0x49C81B.** OR's per-draw hook rebuilds renderer matrices mid-draw
   (`memory/mid-draw-renderer-matrices.md`), and the accumulator's last draw may leave a different
   view or projection than the camera's. Log once and compare against the reflection NiCamera's
   world position mirrored about the water plane. If they disagree, build the view-projection from
   the reflection NiCamera instead (`NiCamera` world-to-clip at `+0xAC` is correct for x/y; derive
   the depth row from its frustum).
4. **Mirrored winding.** Query quads are drawn with culling off, so winding does not matter;
   keep `D3DRS_CULLMODE = NONE`.
5. **The user clip plane.** The queries disable clip planes. A rect under the water plane is
   therefore tested too; that only makes the test more conservative.

## Kill gate (user's decision)

How much of the reflection's ~3,300 draws belong to candidate references is unmeasured; land, LOD,
trees, actors and sky are not candidates, so the saving may be well under the ~4.3 ms ceiling.
Build in two phases:

1. **Gate build:** the reflection draw-row fix, then the minimum that culls: the candidate list,
   the reflection query ring, the 0x49C81B issue thunk, the skip in `TrackWaterReflectionCull`, the
   `OcclusionCullReflection` INI read and the debug-1 stats line. No in-game menu entry, INI
   template change or save path yet. Unit tests for the matrix projection are still written,
   since a wrong projection would make the measurement meaningless.
2. Measure in the town spot (steps 1-2 below). **Keep going only if the town frame improves by
   at least 1 ms net** (PresentMon frame time, off vs on; cross-check with the reflection draw-row
   drop × ~1.1 µs per draw, less `Occ:ReflIssue` and the extra `Occ:Begin`). Under 1 ms: stop,
   remove the culling code (keep the profiler row fix), and record the measured numbers in
   `memory/` as a shelved lever, like `memory/static-draw-instancing-not-worth-it.md`.
3. **Finish:** settings plumbing (menu, save path, INI template), debug modes 2 and 3 for the
   reflection, the control and visual checks, and the results write-up.

## Measuring

Same spot as the far-pass measurements, all ProtoPlugin probes off:
1. `ProfileFrame = 1`, `OcclusionCullDebug = 1`: the reflection draw row, its hidden share and
   `Occ:ReflIssue`, with `OcclusionCullReflection` 0 and then 1.
2. PresentMon, 60 s standing still, off and on: frame time, CPU busy.
3. The ordinary exterior control with water: no regression.
4. Visual: debug mode 2 in the town and at a lake shore; walk around the frozen spot and look for
   holes in the reflection. Normal play: turn and walk along water; no reflection pop-in beyond a
   frame.

Record the results in `memory/occlusion-culling.md`, in a `docs:` commit of its own.

## Left to the builder

- Whether the far pass and the reflection share one generic "query ring" type or keep two copies.
- Whether a frustum pre-test is needed before projection (decide from `Occ:ReflIssue`).
- The thunk's exact register handling at 0x49C81B.
- Names of new functions, counters and stats fields.
