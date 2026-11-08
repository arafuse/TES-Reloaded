---
name: game-runs-on-dxvk
description: "The game folder's d3d9.dll is DXVK (D3D9 on Vulkan): the main thread's idle wait is DXVK syncing in Present, d3d9.dll samples are DXVK internals, and dxvk.conf sets floatEmulation = Strict"
metadata:
  type: project
---

`C:\Games\Steam\steamapps\common\Oblivion\d3d9.dll` (4.28 MB, product "DXVK") replaces the Windows
runtime. Active `dxvk.conf` lines: `dxgi.hideNvidiaGpu = False` and `d3d9.floatEmulation = Strict`.
No frame cap is set there (Special K is also loaded and could cap; unchecked).

What it changes (checked 2026-11-07 with the sampler's CALLERS section, see [[sampling-profiler]]):
- DXVK records D3D9 calls on the main thread and replays them on its own worker thread. The main
  thread blocks in `SleepConditionVariableSRW` whenever it has to sync. The ~5 ms/frame
  `NtWaitForAlertByThreadId` sits under `sub_7D7210`, the engine's end of frame: renderer
  `0xB3F928`, vtable `+0x134` EndFrame, then `+0x138` DisplayFrame/Present. It is the Present
  latency wait, so the frame is GPU-bound (or DXVK-thread-bound), not game-logic-bound.
- `d3d9.dll` rows in sampler output are DXVK's main-thread recording cost. Names like
  `Direct3DCreate9On12Ex+0x145535` are just the nearest export; no symbols exist for them. The
  hottest one (0.29 ms/frame) is reached from `SetVertexShaderConstantF` / `SetPixelShaderConstantF`
  in `sub_9A8E30`.
- The readback stall in [[d3d9-gpu-readback-stall]] is a DXVK sync. `Mid:ProbeRead`'s occasional
  ~3 ms worst frame is probably the same sync, even on the sysmem path.
- `floatEmulation = Strict` is DXVK's most expensive float mode. Untested GPU lever: A/B it against
  `True`, and watch for black or NaN pixels, which may be why it was set.

**Why:** without this, the idle wait reads as engine update time, and d3d9 rows read as the Windows
runtime or the driver.
**How to apply:** before calling a frame CPU-bound, subtract the Present wait. Treat D3D9 CPU cost
as DXVK recording cost. Remember the driver's work is on threads the sampler never sees.
