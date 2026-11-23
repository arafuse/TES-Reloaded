---
name: grass-trail-map
description: CPU-written grass/shrub trail map (3 RGBA16 textures: tall, ground, shrub); layout, neutral encoding, 256 s time wrap and sweep, hold, ring rule, trail clock, upload, VS sampler binding via the device path, core test command
metadata:
  type: project
---

Grass and shrub collision is a CPU-written trail map of three RGBA16 textures, not per-batch source
slots. Source: `TESReloaded/Core/GrassTrailMap.{h,cpp}` (pure C++, no D3D), `TESReloaded/Core/GrassTrail.{h,cpp}`
(engine side), `OblivionReloaded/Shaders/Grass/Includes/GrassTrail.hlsl`.

**Window and cells.** 512 x 512 cells, 8 world units each, 4096 units across, toroidal addressing
(cell `floor(x/8) mod 512`). The window recentres on the player only when the player is more than
512 units from its centre (`kRecentreDistance`), so at least 1536 units around the player stay covered.
Layers: `Layer_Tall` (VS sampler `TESR_GrassTrailTall`), `Layer_Ground` (PS sampler `TESR_GrassTrailGround`, s4)
and `Layer_Shrub` (VS sampler `TESR_GrassTrailShrub` in the SpeedTree and ShadowMap VS). The shrub layer uses the
tall encoding, profile and springiness curve at `TreeCollisionRadius x actor scale` (0, so unstamped, when
`TreeCollision` is off), and exists only when the tall layer does (vertex texture fetch). Shrubs share the grass
recovery and springiness. Per-tree constants and the bend are in [[speedtree-shader-variants]].

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

**Hold.** A press holds its full peak for `kStampHold` (0.1 s, mirrored as `GrassTrailHold` in
`GrassTrail.hlsl`) before the spring starts; the sweep expires a cell at `recovery + hold`. Without it a
standing actor's 33 ms restamp snaps a slightly recovered bend back to full every other frame, which
reads as wind and collision fighting. A `static_assert` in `GrassTrail.cpp` ties the hold to the
restamp interval; keep it if either changes.

**Sweep.** Per row the map keeps a live flag and the live cell span. Each BeginFrame visits 8 rows per
layer (`kSweepRowsPerFrame`). In a live row it checks every live cell's own age and neutralises each cell
whose age is at least the recovery time (R,G = 32768, B = 0, A kept). It then re-derives the row's live
flag and span from the surviving cells. A full pass takes about 64 frames. The check is per cell because
a row can stay busy with a standing actor; a row-level rule would never sweep its old cells, and they
would resurrect after the 256 s wrap.

**Ring rule.** Stamping writes press time with zero peak into a margin of cells just beyond the
radius (`kStampMargin` = 1.5 cells, cells with zero peak and no live peak). Bilinear filtering between
a live cell and a never-pressed neighbour would otherwise blend a live press time with an unrelated
one and eat the edge of the wake. Neighbours in the ring share the press time with zero peak, so the
blend stays correct.

**Stamping.** A cell takes the new stamp only if its new peak is at least its current weight
(`peak x spring(age)`). Stampers are the player plus up to 64 living actors within +-2048 units,
not view-culled, nearest first (collected in `ShaderManager`'s grass constant update). A stationary
stamper is skipped when it moved less than 2 units and its last stamp is under 33 ms old (per-RefId
cache, capped at 256 entries). A teleport (over 1024 units) or a worldspace change resets both layers.
The separate view-culled `GrassCollisionSources` list (player + nearest NPCs) is read only by GrassCover
(player slot) and TreeCover (non-empty check).

**Trail clock.** The clock advances per rendered frame by the delta from `FrameRateManager::GetPerformance()`
clamped to 0.1 s, so hitches and load screens do not age the trail; menus that keep rendering the world do.
`GrassTrail::Reset()` (resets the map, the stamp cache and the clock bookkeeping) is called from the
load-game hook, so a loaded save never keeps the previous run's wake. Interiors have no grass: with a
NULL worldspace `Update` clears the trail once and returns, so a return to an exterior starts clean.

**Upload.** Dirty cells are tracked per row as 8-cell column chunks. Consecutive dirty rows merge into one
band only while their chunks overlap (within one chunk), and a band splits into one rectangle per column run,
so distant actors upload separately instead of as a near-full-width band. Rectangles are copied into one of
3 round-robin SYSTEMMEM staging textures per layer (a non-discard lock on a surface the GPU still reads
stalls on DXVK), then to the default texture with `UpdateSurface`. Standing actors restamp at most every
33 ms, so a "stationary" scene still uploads small rectangles. `LiveBounds` (padded by half a cell for the
bilinear footprint, clipped to the window) is computed once per frame at the end of `Update` and cached;
`TallReaches` and `GroundReaches` test a batch's instance bounds against the cache, and the batch's
c253.w / c32.y flags are gated on that (the batch bounds are only read when a flag could be set).

**Ground curve.** Groundcover ignores `CollisionSpringiness`: its weight is always `(1-t) cos(0.5 pi t)`
(0 at `t >= 1`, never negative), on both the CPU (`Weight`) and the shader (`GrassTrailGroundSpring`).
Tall grass keeps the springiness curve.

**Wrap seam.** The 256 s time wrap is not handled in the encoding: a bilinear fetch between a cell stamped
just before the wrap and one just after blends the time to about 0.5 and reads as expired, leaving a one-cell
gap for the recovery time once per 256 s. Accepted and documented in `GrassTrail.hlsl`.

**Vertex sampler binding.** `ShaderRecord::SetCT` binds a vertex-shader `TESR_` sampler at
`D3DVERTEXTEXTURESAMPLER0 + RegisterIndex`. Its sampler states must go through `device->SetSamplerState`
directly. `TheRenderManager->SetSamplerState` goes through the engine's `NiDX9RenderState` cache, which
is indexed by pixel sampler, so vertex sampler index 257 or more would write past it.

**Creation.** `GrassTrail::Create` runs in the ShaderManager constructor (under `Shaders.Grass`), before
any shader record loads its samplers, because TextureRecord captures the texture pointer at load time.
The tall layer is enabled only if `D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER` is supported
for the format; the ground layer is pixel-side only.

**Profiling.** FrameProfiler detail bucket `GrassTrail` is nested inside `UpdateConstants`.

**Constants:** see [[grass-vs-constant-budget]] for the grass VS and groundcover PS registers.

**Tests.** `powershell -File Tools\Tests\Run-GrassTrailMapTest.ps1` builds `GrassTrailMap.cpp` standalone
with `cl` (vcvars32) and runs `Tools/Tests/GrassTrailMapTest.cpp`. It prints `GrassTrailMap: all tests passed`.

**Why this design:** storing when a cell was pressed, instead of how bent it is now, makes the wake
continuous, lets recovery follow the setting, and means a scene only uploads what actors keep
restamping. Per-batch source slots (a few footprint points per actor) left most of the wake empty at
walking speed. A GPU-stamped map was rejected because the frame is GPU-bound ([[gpu-frame-breakdown]]).

**How to apply:** do not reintroduce per-batch source packing or cell-0 "cleared" values. Keep the
neutral encoding, the time-keeping sweep, and the ring when changing the stamp or sweep. Use the device
sampler path for any new vertex-texture sampler. Run the core test after any change to `GrassTrailMap.cpp`.

Related: [[grass-manager-generation]], [[grass-batched-per-geometry-hook]], [[d3d9-gpu-readback-stall]],
[[game-runs-on-dxvk]].
