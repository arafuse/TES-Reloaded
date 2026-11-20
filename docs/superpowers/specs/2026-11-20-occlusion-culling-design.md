# Occlusion Culling — Design

Date: 2026-11-20
Branch: `feat/misc-10`
Source: ProtoPlugin handoff (dense-town profiling, `ProtoPlugin/memory/oblivion-dense-town-frame.md`)

## Intent

A dense mod town (Anequina / Elsweyr content) runs 35 ms frames on DXVK. The frame is CPU-bound
(GPU busy 9.8 ms). 22 ms of it is the main scene call `0x0070C0B0`, which draws 10,716 geometries
at about 1.4 µs of CPU setup each. ProtoPlugin's occlusion census found that **85.6 % of those
draws (86 % of draw time) are completely hidden** behind nearer geometry in the same frame's
far-pass depth. Most of them are 4096-8192 units away, behind the walls of the street.

Skip drawing references that were hidden last frame. That removes their draw cost and the engine's
walk of their subtrees. The ceiling is roughly 13 ms of draw plus part of the 6.4 ms walk.

## Goals

- Cut the dense-town frame by skipping placed references that were fully hidden in the previous
  frame's far pass.
- Visual impact stays minor. The only accepted artefact is a **one-frame lag** when something is
  revealed (stepping around a corner, a door opening, the camera jumping). The user accepted this,
  provided the lag is reduced where it would show.
- Behind one ini switch, **off by default**. Off, the feature creates nothing, issues nothing and
  touches no flag.
- Low cost in scenes with little occlusion: the ordinary exterior control (5.8 ms) must not regress
  measurably.

## Non-goals

- The water reflection pass (4.3 ms in the town), at first. The same results may apply later.
- Actors, trees, lights, land, grass, water, sky and distant LOD. See "What is a candidate".
- Unstitched triangle-strip meshes (a separate ~1.5-2.5 ms lever) and mod script hitches.
- Lowering fade distances or any other quality setting.

## Anti-goals (failure even if it "works")

- Visible holes: an object that should be on screen missing for more than one frame, or flicker.
- Disturbing the near-shell order (`memory/near-shell-frame-order.md`): the shell, reflections,
  shadow maps, first-person and post-processing must see exactly what they see today.
- Any GPU-to-CPU surface lock (`memory/d3d9-gpu-readback-stall.md`). On DXVK a lock syncs the
  command-stream thread, which cost about 2.5 ms in the worst frames.
- A real disable (a `Disable`d reference, or a hidden torch flame) coming back on because we cleared
  a flag the engine had set.

## Approaches considered

1. **Chosen: test each reference, hide it with `NiAVObject::kFlag_AppCulled` around the far pass.**
   OR builds the candidate list from the loaded cells, as the shadow collector does. It issues one
   occlusion query per candidate against the far-pass depth. Next frame it sets `AppCulled` on the
   root of each reference that came back hidden, for the far pass only. The engine then skips the
   whole subtree, which saves both draw and walk. No new reverse engineering of the cull walk is
   needed.
   Risk: anything that reads `AppCulled` *during* the far pass sees our flag.
2. Same test, but skip inside the engine's cull walk with a detour on the per-node cull call (the
   unused `TrackCullingBSFadeNode` declaration in `RenderHook.cpp`). It changes no flag, but needs RE
   of the walk first. **This is the fallback if option 1 hits a mid-pass reader of `AppCulled`.**
3. Test each geometry and skip at the renderer's geometry slots (`0x00A88EA4 +0x158..+0x178`). It
   would hide the most, but saves draw only (not the walk), needs ~10k queries per frame and is the
   most invasive hook.

All three use hardware occlusion queries, read back one frame later. A CPU readback of a Hi-Z depth
pyramid (the handoff's first option) was rejected because of the DXVK lock stall. An occluder
prepass was rejected because the user accepted the one-frame lag, and the prepass costs an extra
pass and per-content tuning.

## Design

### Frame flow (inside `TrackRenderObject`, `RenderHook.cpp`, main scene pass only)

```
far pass entry
  1. Read last frame's query slot (GetData, flags 0, never waits).
  2. For each reference judged hidden: if its root node is NOT already AppCulled, set
     AppCulled and remember the node.
  RenderObject (far pass)                     <- engine skips hidden subtrees
  3. Clear AppCulled on exactly the nodes remembered in step 2.
  ... existing pre-water fallback, ResolveDepthBuffer (unchanged) ...
  4. Collect this frame's candidates and issue their queries against the bound
     depth-stencil, which still holds the full far pass.
shell clear, shell render, flatten (unchanged)
```

- Step 3 comes immediately after the far pass's `RenderObject` returns, before anything else in
  `TrackRenderObject` runs, so the shell, the reflection, first-person and the next frame's shadow
  maps never see our flag.
- Step 4 must come before the shell's `Clear` to `ShellClearDepth`. After that clear the depth
  buffer holds only the shell, and a test reads 0 % occluded. That cost ProtoPlugin three runs to
  find. With the shell off (`NearShellEnabled = 0` or `ShellActive` false), step 4 runs at the same
  point, against the full-range depth.
- The queries write no depth and no colour, so they don't disturb the resolve, the shell or the
  flatten. They run with OR's own device state, saved and restored around them, as the probes do.

### What is a candidate

A reference in the loaded cells, walked as `ShadowManager::BuildExteriorGeoItems` does: the
`Tes->gridCellArray` grid in a worldspace, otherwise `Player->parentCell`. It is a candidate when:

- `Ref->GetNode()` is non-null, and its root is not already `AppCulled`;
- its base form type is one of static, furniture, container, door, activator, misc item and other
  clutter types, or flora. **Excluded (my call):**
  - actors and the player: they move between the query and its use, and their roots carry
    equipment and the torch flame that EquipmentManager toggles;
  - trees: SpeedTree leaves are batched outside the normal draw path (`memory/speedtree-shader-variants.md`),
    so it isn't known whether a culled root hides its leaves cleanly;
  - lights (`LIGH`): a light's root may hold its `NiPointLight`, and hiding it could switch the
    light off.
  Each exclusion can be lifted after an in-game check, as a follow-up.
- its world bound (`NiAVObject` `+0x20`, radius `+0x2C`) passes the main camera's view frustum.

A candidate is **always drawn** (never culled), but still queried, when:

- the distance from the camera to its bound's surface is below `OcclusionAlwaysDrawDistance`
  (default 1024). The census found under 2 % of the hidden draw time within 1024 units, and nearby
  objects are where pop-in would show.
- its bound crosses the far pass's near plane, or any corner projects behind the camera. These are
  not queried, so they are drawn.
- water is present (`ShaderConst.HasWater`) and its bound reaches below the water height
  (`ShaderConst.Water.waterSettings.x`). Underwater objects show through refraction, but water
  depth may mark them hidden. This rule can be dropped if the builder confirms water writes no
  depth in the far pass.

### The test

Same as the census (conservative: a visible reference is never reported hidden):

- Project the bound's AABB (center ± r, with r inflated by the camera's translation over the last
  frame) with the **far pass's** view-projection. Take one screen rect at the nearest corner depth.
- The depth row must be the one the far pass rasterised with: `(ShellBoundary, RealFar)` when the
  shell is active (near 15, far ~399,472 in the town), `(RealNear, RealFar)` otherwise. The NiCamera's
  own world-to-clip matrix (`+0xAC`) has the wrong depth (near 1, far 400,000). It is correct for
  x/y. Build the depth from `RenderManager::DepthRow`'s formula.
- Draw the rect inside one `D3DQUERYTYPE_OCCLUSION` query, as pre-transformed vertices (`XYZRHW`):
  `ZENABLE TRUE`, `ZFUNC LESSEQUAL`, `ZWRITEENABLE FALSE`, colour writes 0, no blending, no
  stencil, no clip planes, full viewport. Zero samples means hidden.
- Write all rects into one dynamic vertex buffer per frame. Then per candidate: `Begin`,
  `DrawPrimitive` at its offset, `End`. That is three device calls per reference. Use no
  `DrawPrimitiveUP`.

### Results

- Ring of 3 query slots, each holding up to `OcclusionMaxQueries` (default 8192) queries plus the
  `(TESObjectREFR*, NiNode*)` pair each one tested. Queries are created lazily. Past the cap,
  references go untested and are drawn; log the first overflow.
- At the next far pass, read only the **most recent** slot. A query that returns `S_FALSE` (not
  ready) or fails means visible. There is **no fallback to an older slot**, because that would be a
  two-frame lag, which the user didn't accept. Count how many queries were ready.
- Apply a result only if the reference still has the same root node (`Ref->GetNode() == Node`) and
  is still in a loaded cell. Node pointers are recycled when cells unload.
- **Draw everything this frame** (results are dropped, queries are still issued) when:
  - the camera moved more than `OcclusionCameraCutDistance` (default 128 units) since the queried
    frame. This covers teleports, fast travel, the first/third-person switch and load doors. A pure
    rotation doesn't change what is occluded, and references newly in frustum have no result, so
    they are drawn.
  - the cell or worldspace changed, a game was loaded, or the queried frame wasn't the previous
    main scene (menus, the load screen).
- Drop all slots on load and new game (the probes' reset hooks: co-save load and new-game
  callbacks).

### Settings

`OblivionReloaded.ini`, `[Main]` (names are my call; follow the existing `Main` parsing in
`SettingManager.cpp`, including the save path and the in-game menu list):

| Key | Default | Meaning |
|---|---|---|
| `OcclusionCulling` | 0 | Master switch. 0 = no queries, no flags, no allocations. |
| `OcclusionAlwaysDrawDistance` | 1024 | References whose bound is closer than this are never culled. |
| `OcclusionCameraCutDistance` | 128 | Camera move per frame above which results are dropped. |
| `OcclusionMaxQueries` | 8192 | Query cap per frame. |

`[Develop] OcclusionCullDebug` (my call):
- 1 logs a stats line about once a second: candidates, queried, ready, hidden, culled, always-drawn,
  dropped frames.
- 2 freezes the culled set: stop applying new results and keep culling the last set. You can then
  walk around and see exactly what is being skipped, which is the standard visual check for holes.

Switching `OcclusionCulling` off at runtime must clear any flag we hold (there is none outside the
far pass by construction) and release the queries and buffer.

### Code placement

- New module `TESReloaded/Core/OcclusionCulling.cpp/.h` (Oblivion only), in the style of
  `RainProbe` and `SunShadowStealth`: free functions with documentation comments. Calls from
  `TrackRenderObject`: `BeginOcclusionCull()` (steps 1-2), `EndOcclusionCull()` (step 3) and
  `IssueOcclusionQueries()` (step 4). Each is a no-op when the setting is off.
- Re-run `Tools/GenerateCompileCommands.ps1` after adding the files.
- FrameProfiler: one bucket for each of the three calls, plus counters for the stats above, so
  `ProfileFrame` shows our cost next to the main pass.

## Risks the builder must check

1. **Mid-pass readers of `AppCulled`.** Find what reads a reference root's `AppCulled` between the far
   pass's start and its end. Check OR (`RenderShadowsMidScene` only applies; shadow maps render
   outside the pass) and the engine (BSFadeNode cull, ShadowSceneNode light processing `0x007C78D0`
   if it runs inside the walk). If anything reads it, switch to approach 2.
2. **BSFadeNode fade state.** If BSFadeNode keeps a per-frame fade alpha that restarts after a culled
   frame, an un-culled object would fade in visibly. Check in game with debug mode 2 off and on. If
   it happens, approach 2 or preserving the fade field is the fix.
3. **Query cost.** About 3 device calls per candidate. The town may have 3-5k candidates in frustum,
   so measure the issue bucket. If it eats a large share of the saving, re-query visible references
   only every few frames (CHC++ style), and keep hidden ones every frame. That is a follow-up, not
   v1.
4. **Readiness.** If the most recent slot is often not ready by the next far pass (more likely in
   GPU-bound scenes), the culler saves little there. That is safe but must be measured, and brought
   back to the user before considering a two-frame fallback.

## Verification

- **Off is inert:** with `OcclusionCulling = 0`, no query objects or buffers are created and no flag
  is written. Confirm by code inspection, and with a ProtoPlugin RenderCount run matching the
  baseline geometry count.
- **Counts (ProtoPlugin RenderCount, same street spot):** `sceneGeoms` drops from ~10.7k toward
  ~1.5-3k (per reference, a little higher than per geometry). The census hidden share drops
  sharply.
- **Timing (PresentMon, all probes off):** town frame against the 35.2 ms baseline, and the ordinary
  exterior control against 5.8 ms (no measurable regression). Report the FrameProfiler buckets for
  our three calls.
- **Visual:** in the same spot with the culler on and off, compare screenshots, pan fast and step
  around corners. Then walk through a door, use fast travel, toggle first/third person and stand
  by water. Use debug mode 2 to look for holes. State any pop-in seen.
- **Coexistence:** ProtoPlugin patches device vtable slots (Clear 43, the Draw slots 81-84) and the
  queries go through the device, so run with ProtoPlugin loaded and confirm both still work.

## Left to the builder

- The exact form-type list within the included categories, and the vertex format and buffer size
  details.
- Where the cell-change / load signals come from (reuse what the probes and ShadowManager use).
- Stats wording and the FrameProfiler bucket names.
