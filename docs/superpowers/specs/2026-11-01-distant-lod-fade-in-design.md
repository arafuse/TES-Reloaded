# Distant LOD Fade-In — Design

Date: 2026-11-01
Status: Approved in chat, awaiting written-spec review

## Problem

When a distant cell enters LOD range at the far edge, its LOD statics snap into existence.

## Goal

Distant LOD objects that first appear at the **far edge** dither in over `FadeTime` (default 1.0 s)
instead of popping. Success: outdoor travel shows no visible snap as distant LOD streams in, and
frame time is unchanged while nothing is fading.

## Scope

In scope: fade-in of new children of `DistantRefLOD` (per-cell merged `_far.NIF` nodes and individual
`_far.NIF` objects) whose cell is at least two rings outside the loaded grid.

Out of scope:

- Fade-out of departing LOD. It needs pinning and re-attaching nodes in engine-owned scene graph,
  the crash risk of the earlier attempt.
- The LOD-to-full-model handoff on cell load, and `LandLOD` terrain.
- LOD appearing in the ring adjacent to the loaded grid. That LOD replaces a cell that just unloaded,
  whose full models vanish instantly; fading it in opens a hole (measured on the earlier branch). It
  keeps popping, as today.
- The instanced trees under `DistantRefLOD[0]` (`"LOD Trees"`). They are one long-lived merged mesh
  per species spanning every distant cell, so a per-draw fade cannot fade one cell's trees. A
  world-space region test in `DISTLOD2002/2003.vso` is the follow-up if they pop noticeably.
- Shadow maps. Distant LOD casts full shadow while fading; at that range it is not visible.

## Prior work

`origin/feat/lod-dither-fade` (2026-08-20..22, unmerged) attempted every LOD pop. Its spec,
`docs/superpowers/specs/2026-08-20-lod-dither-fade-design.md` on that branch, and its "Corrections
from in-game measurement" section are the evidence base. This design keeps only the parts that were
proven in game:

- the `DistantRefLOD` set-membership poller (detected arrivals correctly; `resolved>0 minAlpha<1.0`
  proved the pipeline end to end),
- the per-draw `TESR_GEOM_FadeParams` constant at `c110` and the `LODFadeClip` dither include,
- the empty-placeholder arrival queue.

It drops pinning, holders, refcounting, the cell and `LandLOD` tiers, and the inverted threshold.

## Architecture

One new manager, `LODFadeManager` (`TESReloaded/Core/LODFadeManager.h/.cpp`), singleton
`TheLODFadeManager` in `Managers.h`. Created from `Main.cpp` only when `[LODFade] Enabled = 1` and
`FadeTime > 0`. `Update()` runs once per frame from the existing per-frame entry point.

### Detection

- **Container.** `Tes->LODRoot` is misnamed and is the `LandLOD` node; its `m_parent` holds
  `LandLOD`, `DistantRefLOD`, `LODWaterRoot`. Resolve the child whose `m_pcName` is
  `"DistantRefLOD"`. Resolve every frame (three children, so it is cheap); a changed result counts as
  a discontinuity. Log once under `LogLODFade`:
  `[LODFade] DistantRefLOD resolved name=%s children=%d` (expect ~2075).
  **`Tes->gridDistantArray` must not be used**: its entry layout is unverified, and on the earlier
  branch reading it as node pointers produced thousands of arbitrary memory writes per frame.
- **Diff.** Each frame build the set of non-NULL children, scanning to `m_children.end` (removal NULLs
  a slot without compacting). Arrivals = in the set now, absent last frame. Departures only drop
  their record.
- **Discontinuity.** If more than a quarter of the set changed, or there is no previous set (first
  frame, re-resolve), adopt the new set without fading.
- **Far-edge rule.** Convert the arrival's world-bound center to cell coordinates (4096 units per
  cell) and take `ring = max(|dx|, |dy|)` from the player's cell. Fade only when
  `ring >= *SettingGridsToLoad / 2 + 2`; otherwise skip.
- **Empty placeholders.** The engine sometimes attaches an empty node and streams the mesh in after.
  An arrival with `m_children.numObjs == 0` (and not a geometry leaf) is queued and promoted when it
  gains children, or dropped after 8 s or when it leaves the set.
- **Menu guard.** When `Player` or `Tes` is NULL, clear all state and return.

### Fade table

Fixed capacity `MaxFades` of `FadeRecord { NiAVObject* Root; float StartMs; }`. Alpha is
`saturate((Now - StartMs) / (FadeTime * 1000))`, with `Now` from `TheFrameRateManager->GetPerformance()`
(`FrameRateManager::ElapsedTime` is never assigned). A record retires when alpha reaches 1, and is
dropped immediately when its root leaves the set. A full table means the arrival pops.

Roots are raw pointers. Nothing is reference-counted and nothing in the scene graph is written. A
root is only dereferenced while it was in the current frame's child set, and the geometry cache is
cleared whenever a record is added or removed, so no freed pointer is ever followed.

### Draw-time plumbing

- `ShaderConst.LODFade.Params` (float4): `.x` alpha, `.y` per-frame dither seed, `.zw` unused.
- `ShaderProgram::SetPerGeomConstantTableValue` maps `TESR_GEOM_FadeParams` to it. `CreateCT`, which
  already enumerates `TESR_GEOM_` constants, sets `ShaderProgram::HasFadeParams` when it finds it.
- In `RenderHook::TrackSetupShaderPrograms`, after the existing Toggles/Wetness per-geometry blocks
  (so `SetPerGeomCT()` re-uploads their already-set values), and only while the table is live or a
  reset pass is pending, for pixel shaders with `HasFadeParams`:
  1. Look the geometry up in `unordered_map<NiAVObject*, FadeRecord*>`. On a miss, walk `m_parent` up
     to depth 10 looking for a live root; cache the result, including `nullptr`.
  2. Publish the record's alpha, or `1.0` when there is none, and call `SetPerGeomCT()`.
- After the last record retires, one more frame publishes `1.0` to every covered draw, then the block
  goes quiet. With an empty table the hook costs one bool test.

### Shader side

New include `OblivionReloaded/Shaders/ExtraShaders/Includes/LODFade.hlsl`, beside `WetActor.hlsl`:

```hlsl
float4 TESR_GEOM_FadeParams : register(c110);	// x = alpha, y = dither seed

void LODFadeClip(float2 vpos) {
	float a = TESR_GEOM_FadeParams.x;
	[branch]
	if (a < 1.0) {
		float n = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + TESR_GEOM_FadeParams.y);
		clip(a - n);
	}
}
```

- `c100`..`c103` are taken (Toggles, SpecularData, Wetness, WetActorData); `c110` is free in every
  shader on `main` and leaves headroom.
- Interleaved gradient noise, not a `sin` hash: ps_3_0 expands `sin` into a long sequence that pushed
  the largest shaders over the 512-slot limit on the earlier branch, and `fxc` does not fail on that.
- `[branch]` is uniform per draw (a constant), so settled draws skip the hash.
- Every pass of an object sees the same alpha and seed, so `ZFUNC EQUAL` follow-up passes dither
  identically.

Each covered pixel shader gains the include, `float2 vpos : VPOS` on its input struct, and
`LODFadeClip(IN.vpos);` as the first statement of `main`. Under TAA the per-frame seed resolves to a
smooth dissolve; without TAA it reads as noise for the fade duration.

### Coverage is measured, not assumed

The covered shader set is decided by an in-game census, implemented before any shader is edited.
Under `LogLODFade`, every draw whose geometry has `DistantRefLOD` in its parent chain (fading or not)
is counted by pixel and vertex shader name; the tally is logged once after 30 s of exterior play.
Only the listed pixel shaders get the clip. A listed shader shared with full-resolution models is
still safe: non-fading draws get `1.0` and skip the branch. The finding is recorded as a memory note.

## Configuration

`OblivionReloaded.ini`, read in `SettingManager`:

| Key | Default | Meaning |
|---|---|---|
| `[LODFade] Enabled` | 1 | Install the manager |
| `[LODFade] FadeTime` | 1.0 | Fade duration in seconds; `<= 0` disables |
| `[LODFade] MaxFades` | 256 | Fade table capacity |
| `[Develop] LogLODFade` | 0 | Census, resolve line, one line per arrival (`fade`/`skip`, ring, discontinuity) |

The grid size comes from `*SettingGridsToLoad` (`kSettingGridsToLoad`), so grid-size mods are handled.

## Failure handling

Every failure degrades to today's pop:

- `DistantRefLOD` not found: manager idles and logs once.
- Table full: arrival pops.
- Root leaves the set mid-fade: record and cache entries dropped before the next draw.
- `Player`/`Tes` NULL: all state cleared.

## Performance

Idle: one hash-set build of ~2075 pointers per frame and one bool test per draw. During a fade:
one cached map probe and one per-geometry constant upload per covered draw.

## Verification

No test framework exists; verification is build plus in-game.

1. Build succeeds; every edited shader compiles under `fxc` (with `/I` for the include) within ps_3_0
   limits.
2. Census session (user-run) lists the shaders to cover.
3. With `LogLODFade = 1`, travel outdoors across several cells: far-edge arrivals log `fade` and
   visibly dither in over ~1 s; inner-ring arrivals log `skip`; fast travel logs a discontinuity and
   fades nothing.
4. No flicker under TAA.
5. Idle frame time matches `main` (`Develop.ProfileFrame`, shader-hook bucket).

## Risks

- **Census shows distant statics use no per-draw-hooked shader** (e.g. drawn instanced like the
  trees). Then this approach cannot fade them, and the region-test follow-up becomes the design.
- **TAA history rejection** may treat changing dither coverage as disocclusion and flicker.
- **Far-edge rule misclassifies** a large `_far.NIF` whose bound center lies in a different cell than
  its content. The worst case is a skipped fade (a pop) or a fade on an inner-ring object (a brief
  thin-out), not a hole across a whole cell.
