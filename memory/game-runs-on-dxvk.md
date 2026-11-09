---
name: game-runs-on-dxvk
description: "The game folder's d3d9.dll is DXVK (D3D9 on Vulkan): the main thread's idle wait is DXVK syncing in Present, d3d9.dll samples are DXVK internals, and dxvk.conf sets floatEmulation = Strict"
metadata:
  node_type: memory
  type: project
  originSessionId: 021854aa-fcf7-4d1b-8c67-7ee9a679ad40
  modified: 2026-11-09T02:23:16.877Z
---

`C:\Games\Steam\steamapps\common\Oblivion\d3d9.dll` (DXVK 3.1.1 since 2026-11-08, gcc 16.2 build;
2.7.1 and 2.5.3 kept as `d3d9.dll.dxvk-2.7.1` / `.dxvk-2.5.3`) replaces the Windows runtime. Active
`dxvk.conf` lines: `dxgi.hideNvidiaGpu = False`, `d3d9.floatEmulation = Strict`, and
`d3d9.hideNvidiaGpu = True`, which OR's depth copy needs ([[dxvk-resz-vendor-gate]]).
No frame cap is set there (Special K is also loaded and could cap; unchecked).
Two different AMD names appear, so don't confuse them. The "AMD Radeon RX 6700 XT" in
`Oblivion_d3d9.log` is the `hideNvidiaGpu` spoof. "AMD Radeon(TM) Graphics" is real: the Ryzen 7
9850X3D's on-die GPU, listed in Win32_VideoController and enumerated by Vulkan, which loads
`amdvlk32.dll` into the process. That is why its frames can show up in sampler stacks. The D3D9
device is still created on the RTX 5070 Ti (the log's "Creating device" entry).

What it changes (checked 2026-11-07 with the sampler's CALLERS section, see [[sampling-profiler]]):
- DXVK records D3D9 calls on the main thread and replays them on its own worker thread. The main
  thread blocks in `SleepConditionVariableSRW` whenever it has to sync. The ~5 ms/frame
  `NtWaitForAlertByThreadId` sits under `sub_7D7210`, the engine's end of frame: renderer
  `0xB3F928`, vtable `+0x134` EndFrame, then `+0x138` DisplayFrame/Present. It is the Present
  latency wait, so the frame is GPU-bound (or DXVK-thread-bound), not game-logic-bound.
- `d3d9.dll` rows in sampler output are DXVK's main-thread recording cost. Names like
  `Direct3DCreate9On12Ex+0x330FB` are just the nearest export; no symbols exist for them. Offsets
  change with every DXVK build. To read one in 3.1.1: rva = export rva `0x1870` + offset (image base
  0x62440000), then `dumpbin /disasm` the dll.
- **Every D3D9 call takes DXVK's device lock.** The engine creates the device multithreaded, and DXVK
  only locks then. The lock is a recursive spinlock: `GetCurrentThreadId`, then
  `lock cmpxchg [owner]`, spinning with `pause` when taken. It is uncontended but expensive.
  - 2.7.1: one out-of-line function at rva `0x146D80`. 0.29 ms/frame in the forest, 1.22 ms/frame in
    market combat (~33k shadow draws).
  - 3.1.1 (checked 2026-11-08): inlined into ~130 API entry points (`call [GetCurrentThreadId]`,
    `lock cmpxchg [device+0x580]`) and released with `xchg [owner], 0`. Samples land on the
    instruction after either one. In the usual test scene the four hottest d3d9 render-phase rows are
    this lock: release `+0x330FB` 0.14 and `+0x35688` 0.07, acquire `+0x3578A` 0.08 and `+0x3689A`
    0.04, ~0.33 ms/frame in all, so no real change from 2.7.1.
  - Cost scales with API call count, ours included (`ShaderRecord::SetCT`, `DrawGeoArrays`,
    per-bone constants). Fewer calls is the only lever: dropping `D3DCREATE_MULTITHREADED` is not an
    option, because the loader thread creates textures ([[dxvk-combat-cpu-headroom]]).
- 3.1.1 wait sites, both `call [SleepConditionVariableSRW]`: `+0x3D3DB` is the Present wait under
  `sub_7D7210`, and `+0x1595A3` is the sun probe's `LockRect` ([[d3d9-gpu-readback-stall]]).
- `floatEmulation = Strict` is DXVK's most expensive float mode. Untested GPU lever: A/B it against
  `True`, and watch for black or NaN pixels, which may be why it was set.

**Why:** without this, the idle wait reads as engine update time, and d3d9 rows read as the Windows
runtime or the driver.
**How to apply:** before calling a frame CPU-bound, subtract the Present wait. Treat D3D9 CPU cost
as DXVK recording cost. Remember the driver's work is on threads the sampler never sees.
