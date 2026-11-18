---
name: grass-trail-map
description: CPU-written grass trail map (replaces the 2-footprint + 3-source-per-batch collision); layout, encodings, time wrap and sweep, ring rule, trail clock, VS sampler binding, core test command
metadata:
  type: project
---

Grass collision is a CPU-written trail map of two RGBA16 textures, not per-batch source slots.
Source: `TESReloaded/Core/GrassTrailMap.{h,cpp}` (pure C++, no D3D), `TESReloaded/Core/GrassTrail.{h,cpp}`
(engine side), `OblivionReloaded/Shaders/Grass/Includes/GrassTrail.hlsl`.

**Window and cells.** 512 x 512 cells, 8 world units each, 4096 units across, toroidal addressing
(cell `floor(x/8) mod 512`). The window recentres on the player only when the player is more than
512 units from its centre (`kRecentreDistance`), so at least 1536 units around the player stay covered.
Layers: `Layer_Tall` (VS sampler `TESR_GrassTrailTall`) and `Layer_Ground` (PS sampler `TESR_GrassTrailGround`, s4).

**Encodings** (16-bit UNORM per channel, `D3DFMT_A16B16G16R16`):
- Tall: R,G = signed push vector (`v*0.5+0.5`), B = peak influence, A = press time.
- Ground: R,G = signed pull vector divided by `kPullScale` (32), B = press peak, A = press time.
- Cleared cells: R,G = **32768 (neutral)**, never 0. Shaders decode `v*2-1`, so a raw 0 is a push
  of (-1,-1). A cleared cell's time is kept (see sweep).

**Time.** The press time is `frac(now / 256 s)` quantised to 16 bits, about 4 ms precision. The shader
computes `age = frac(nowN - timeN) * 256` and the spring weight from it. `NowN()` is quantised to the
stored 16-bit value, so equal-tick comparisons are exact. Recovery is clamped to `[0.05, 128]` s.
Because the time wraps every 256 s, a cell with time 0 would look like it was pressed just now and
fall inside the spring window for `recovery` seconds after each wrap. Cleared cells keep their time
and neutral vectors to avoid this.

**Sweep.** Per row the map keeps the newest press time and a has-live-peaks flag. Each BeginFrame
visits 8 rows (`kSweepRowsPerFrame`). A live row whose newest press is older than the recovery time
has its peaks and vectors zeroed (neutral), and its time kept. A full pass takes about 64 frames.

**Ring rule.** Stamping writes press time with zero peak into a margin of cells just beyond the
radius (`kStampMargin` = 1.5 cells, cells with zero peak and no live peak). Bilinear filtering between
a live cell and a never-pressed neighbour would otherwise blend a live press time with an unrelated
one and eat the edge of the wake. Neighbours in the ring share the press time with zero peak, so the
blend stays correct.

**Stamping.** A cell takes the new stamp only if its new peak is at least its current weight
(`peak x spring(age)`). Stampers are the player plus up to 64 living actors within +-2048 units,
not view-culled, nearest first. A stationary stamper is skipped when it moved less than 2 units and
its last stamp is under 33 ms old (per-RefId cache, capped at 256 entries). A teleport (over 1024
units) or a worldspace change resets both layers.

**Trail clock.** The clock is the frame delta from `FrameRateManager::GetPerformance()` clamped to
0.1 s per frame. Menus and pauses therefore freeze recovery instead of letting it run out.

**Upload.** Only dirty row bands are written into a staging texture, then copied to the default texture
with `UpdateSurface`. Nothing locks the whole texture with LOCK_DISCARD. `LiveBounds` pads the live bounds by half a cell (the
bilinear footprint) and clips them to the window; `TallReaches` and `GroundReaches` test a batch's
instance bounds against it, and the batch's c253.w / c32.y flags are gated on that.

**Vertex sampler binding.** `ShaderRecord::SetCT` binds a vertex-shader `TESR_` sampler at
`D3DVERTEXTEXTURESAMPLER0 + RegisterIndex`. Its sampler states must go through `device->SetSamplerState`
directly. `TheRenderManager->SetSamplerState` goes through the engine's `NiDX9RenderState` cache, which
is indexed by pixel sampler, so vertex sampler index 257 or more would write past it.

**Creation.** `GrassTrail::Create` runs in the ShaderManager constructor (under `Shaders.Grass`), before
any shader record loads its samplers, because TextureRecord captures the texture pointer at load time.
The tall layer is enabled only if `D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER` is supported
for the format; the ground layer is pixel-side only.

**Profiling.** FrameProfiler detail bucket `GrassTrail` is nested inside `UpdateConstants`.

**Constants.** Grass VS: c248 `TESR_GrassScale`, c249 `TESR_GrassTrailWindow` (origin XY, 4096, 1/4096),
c250 `TESR_GrassTrailTime` (nowN, 256, 1/recovery, omega), c253 `TESR_GrassCollisionParams` (x unused,
y push, z flatten, w trail-reaches flag), c254-c255 free. Groundcover PS: c32 `TESR_GroundcoverPressParams`
(x unused, y strength, z darkening, w model height), c33 `TESR_GrassTrailWindow`, c34 `TESR_GrassTrailTime`.
See [[grass-vs-constant-budget]] for the full register budget.

**Tests.** `powershell -File Tools\Tests\Run-GrassTrailMapTest.ps1` builds `GrassTrailMap.cpp` standalone
with `cl` (vcvars32) and runs `Tools/Tests/GrassTrailMapTest.cpp`. It prints `GrassTrailMap: all tests passed`.

**Why:** the old wake was two footprint points, each planted every recovery/2 seconds (`kGrassStampCount = 2`).
At walking speed they were about 210 units apart, so trail coverage 80-800 units behind the player was
14-38%, and 0% at an 8 s recovery. The settings appeared to do nothing. Each batch also had only 3
collision slots, and actors outranked footprints for them. Storing when a cell was pressed, instead
of how bent it is now, makes the wake continuous, lets recovery follow the setting, and means a
stationary scene uploads nothing. The GPU-stamped alternative was rejected because the frame is
GPU-bound ([[gpu-frame-breakdown]]).

**How to apply:** do not reintroduce per-batch source packing or cell-0 "cleared" values. Keep the
neutral encoding, the time-keeping sweep, and the ring when changing the stamp or sweep. Use the device
sampler path for any new vertex-texture sampler. Footprint stamps (`kGrassStampCount`) now feed only
TreeCollision. Check the core test after any change to `GrassTrailMap.cpp`.

Related: [[grass-vs-constant-budget]], [[grass-batch-footprints]], [[grass-batched-per-geometry-hook]],
[[d3d9-gpu-readback-stall]], [[game-runs-on-dxvk]].
