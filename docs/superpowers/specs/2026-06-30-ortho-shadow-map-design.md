# Ortho Shadow Map — Gating + Single-Pass Refactor

**Date:** 2026-06-30
**Branch:** feat/new-shadows-experiment
**Status:** Approved design, pending implementation plan

## Context

On `feat/new-shadows-experiment` the entire shadow implementation is dummied out
(`#if 0` reference blocks) except the **ortho depth map**, which is kept alive solely
because the precipitation effects sample it for occlusion. See the project memory
`shadows-dummied-out` and `shader-pipeline-facts`.

The ortho map's **only** runtime consumers are three image-space effects that sample
`TESR_OrthoMapBuffer` (`ShadowMapTexture[MapOrtho]`) and
`TESR_ShadowCameraToLightTransformOrtho`:

- `OblivionReloaded/Shaders/Precipitations/Rain.fx.hlsl`
- `OblivionReloaded/Shaders/Precipitations/Snow.fx.hlsl`
- `OblivionReloaded/Shaders/Precipitations/SnowAccumulation.fx.hlsl`

(WetWorld / puddles do **not** sample ortho.)

### Decisions that scope this work

1. **Ortho's end-state role: precipitation-occlusion only.** The new shadow system
   will get its own maps, *modeled on* this refactored code as a template — it will not
   share this ortho map at runtime. This makes hard gating valid.
2. **In-scope levers: gated execution + single-pass refactor.** Multithreading and SIMD
   are deferred as YAGNI: gating removes the map's cost for the majority of playtime, so
   there is no measured bottleneck to justify them. Revisit only if the active
   (precipitating) state profiles as a spike.

### Current per-frame cost (the problem)

Inside `ShadowManager::RenderShadowMaps` → `RenderExteriorShadows` (ShadowManager.cpp):

1. `ComputeExteriorLookAt` — pick the shadow-map center.
2. `BuildExteriorGeoItems` — walks the **entire** loaded cell grid (`GridsToLoad²`
   cells), every ref, recursively flattening **un-culled** shadow-casting geometry into
   `ShadowGeoPool`, grouped per ref in `ShadowRefGroups` (world matrices + bounds +
   instancing eligibility). This is the heavy CPU work.
3. `RenderShadowMap(MapOrtho)` — one orthographic depth pass: terrain + collected geo,
   frustum-culled per-pass, with hardware instancing.

`BuildExteriorGeoItems` collects un-culled because it was architected to amortize one
grid-walk across **4** passes (Near/Far/Ortho/Skin), each with a different frustum. Only
Ortho remains live, so that amortization is gone — we pay a full-grid walk + per-ref
grouping to feed a single pass, then throw most of it away in the per-pass cull.

## Goals

- Pay **zero** ortho cost when no ortho-sampling effect is contributing (the common case).
- When ortho *is* needed, do one walk / one cull / one filter / one draw, touching only
  geometry inside the ortho frustum.
- Leave the live path as a clean, copyable "directional shadow pass" template for the
  future shadow system — without pre-building a generalized abstraction.
- Behavior while precipitating must be **visually identical** to today.

## Non-goals (YAGNI)

- Multithreading the collection walk.
- SIMD-vectorizing cull / matrix math.
- Temporal throttling of the ortho rebuild while gated ON (every-frame rebuild in v1).
- A generalized cascade / multi-map abstraction. The new system copies this single-pass
  unit when it is written.

## Design

### 1. Gating

Two-level gate, evaluated once per frame at the top of `RenderExteriorShadows` via a
small private helper `OrthoNeeded()`:

- **INI-level:** if `!Effects.Precipitations && !Effects.SnowAccumulation`, the ortho map
  can never be sampled → skip unconditionally.
- **Per-frame "still contributing":**
  ```
  needOrtho =
      (Effects.Precipitations  && (ShaderConst.Precipitations.RainData.x > 0 ||
                                   ShaderConst.Precipitations.SnowData.x > 0))
   || (Effects.SnowAccumulation &&  ShaderConst.SnowAccumulation.Params.w > 0)
  ```
  All three values already live in `ShaderConst` and are maintained in
  `ShaderManager::UpdateConstants` (`UpdatePrecipitation` / `UpdateSnowAccumulation`,
  themselves gated by the same `Effects.*` flags). The `Params.w > 0` term keeps the map
  alive through the snow-melt / ramp-down tail, when `SnowData.x == 0` but accumulation
  still occludes.

When gated OFF, `RenderExteriorShadows` returns early — skipping `ComputeExteriorLookAt`,
geometry collection, and the ortho render pass. `RenderShadowMaps` continues to perform
its canopy-map fixup and `Global->RenderShadowMaps()` work.

**Correctness notes (accepted, documented):**

- *1-frame staleness:* `RenderShadowMaps` runs from a different hook than
  `UpdateConstants`. If shadows render before the precip update in a frame, the gate reads
  last frame's values. Across weather ramps this is invisible for a depth-occlusion map,
  so it is accepted. Actual hook order to be confirmed during implementation; no reorder
  planned.
- *No stale-content frame on re-entry:* when the gate flips ON, that same frame renders a
  fresh ortho map before the precip effects sample it. The map is not cleared while gated
  off — it simply is not read.

### 2. Single-pass refactor

Replace the 4-pass-amortization collection with a fused, frustum-culled single pass:

1. **Compute matrices/frustum first.** Move `SetupShadowMapMatrices(MapOrtho, …)` ahead of
   collection so the ortho frustum exists before geometry is walked. (Today it runs inside
   `RenderShadowMap`, after collection.)
2. **Collect with the cull fused in.** Replace `BuildExteriorGeoItems` + `ShadowRefGroups`
   + the per-pass `RootInShadowFrustum`/`LeafInShadowFrustum` split with one walk:
   - Per ref: root-bound cull against the ortho frustum → reject the whole subtree early
     (keep this two-level cull; it is the valuable part).
   - Per geo: leaf-bound cull + `MinRadius` (100, `MinRadii[MapOrtho]`) + `Forms[MapOrtho]`
     filter, applied once during collection.
   - Survivors append to a single flat draw list (world matrix + `GeoData` + instancing
     flags), already filtered — no per-pass re-filter, no `FormsAllowed[256]` rebuild, no
     `ShadowRefGroup` indirection.
3. **Render straight from the flat list**, reusing the existing instancing path
   (`InstancePool` / `DrawInstancedGroup` / `FlushInstanceGroups`) and the skinned /
   SpeedTree-leaf fallbacks unchanged.

**Resulting shape:**

```
RenderShadowMaps()
  └─ canopy-map fixup + Global->RenderShadowMaps()      (unchanged)
  └─ RenderExteriorShadows()
       ├─ if (!OrthoNeeded()) return;                    ← gate (§1)
       ├─ ComputeExteriorLookAt(At)
       ├─ SetupShadowMapMatrices(MapOrtho, At, OrthoDir) ← moved earlier
       ├─ CollectOrthoGeo(frustum)  → flat draw list     ← fused walk+cull+filter
       └─ RenderOrthoMap(drawList)  → depth target + ShadowCameraToLight[MapOrtho]
```

The render unit stays parameterized by `ShadowMapTypeEnum` so it reads as a reusable
directional-shadow pass (the template for the new system), but only `MapOrtho` is wired
live.

**Dead-field cleanup (in scope):** remove `ShadowRefGroups` / `ShadowRefGroup` and the
per-ref grouping in `ShadowGeoPool` (collapse into the flat list). `MinRadii` and the
per-`ShadowMapType` arrays stay typed as-is but only `MapOrtho` is indexed live.

### 3. Edge cases / error handling

- **Worldspace guard:** keep `if (!Player->GetWorldSpace()) return;` — interiors skip
  ortho.
- **Empty draw list** (precip active, nothing in frustum, e.g. open plain): still clear
  the ortho depth target so consumers sample "fully sky-exposed," never a stale map.
- **Surfaces:** per the dummied-out work, only `ShadowMapTexture[MapOrtho]` is allocated;
  Near/Far/Skin are NULL. The single-pass path only touches `MapOrtho`. Guard/assert the
  ortho surface is non-NULL before rendering.

## Verification

No automated test harness exists (render mod). Verify by:

1. Build via the solution: `MSBuild TESReloaded.sln /p:Configuration=Release
   /p:Platform=x86 /t:OblivionReloaded`.
2. In rain/snow: occlusion under cover is visually identical to pre-change (no rain
   through roofs/overhangs).
3. In clear weather: ortho cost drops to ~0 — FrameProfiler `Buck_ShadowMaps` /
   `Phase_ExtTotal` near zero while not precipitating.
4. Melt tail: stop snow, confirm accumulation still respects cover until
   `SnowAccumulation.Params.w` reaches 0, then ortho gates off.

## Future work (out of scope)

- Temporal throttle while gated ON (rebuild on look-at recenter + N-frame cap).
- MT / SIMD on the active-state collection, if it ever profiles as a spike.
- New shadow system copies this single-pass unit as its directional-pass template.
