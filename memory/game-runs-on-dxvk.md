---
name: game-runs-on-dxvk
description: "The game folder's d3d9.dll is DXVK 3.1.1 (D3D9 on Vulkan): the main thread's idle wait is DXVK syncing in Present, d3d9.dll samples are DXVK recording cost, every D3D9 call pays DXVK's device lock, the RX 6700 XT is a spoof, and dxvk.conf sets floatEmulation = Strict"
metadata:
  type: project
---

`C:\Games\Steam\steamapps\common\Oblivion\d3d9.dll` is DXVK 3.1.1 (gcc 16.2 build; 2.7.1 and 2.5.3
kept as `d3d9.dll.dxvk-2.7.1` / `.dxvk-2.5.3`). Active `dxvk.conf` lines: `dxgi.hideNvidiaGpu = False`,
`d3d9.floatEmulation = Strict`, and `d3d9.hideNvidiaGpu = True`, which OR's depth copy needs
([[dxvk-resz-vendor-gate]]). No frame cap is set there (Special K is also loaded and could cap; unchecked).

Two different AMD names appear, so don't confuse them. The "AMD Radeon RX 6700 XT" in
`Oblivion_d3d9.log` is the `hideNvidiaGpu` spoof. "AMD Radeon(TM) Graphics" is real: the Ryzen 7
9850X3D's on-die GPU, listed in Win32_VideoController and enumerated by Vulkan, which loads
`amdvlk32.dll` into the process, so its frames can show up in sampler stacks. The D3D9 device is
created on the RTX 5070 Ti (the log's "Creating device" entry).

What it changes:
- DXVK records D3D9 calls on the main thread and replays them on its own worker thread. The main
  thread blocks in `SleepConditionVariableSRW` whenever it has to sync. The ~4-5 ms/frame
  `NtWaitForAlertByThreadId` sits under `sub_7D7210`, the engine's end of frame: renderer
  `0xB3F928`, vtable `+0x134` EndFrame, then `+0x138` DisplayFrame/Present. It is the Present
  latency wait, so the frame is GPU-bound (or DXVK-thread-bound), not game-logic-bound.
- `d3d9.dll` rows in sampler output are DXVK's main-thread recording cost. Names like
  `Direct3DCreate9On12Ex+0x330FB` are just the nearest export; no symbols exist for them. Offsets
  change with every DXVK build. To read one in 3.1.1: rva = export rva `0x1870` + offset (image base
  0x62440000), then `dumpbin /disasm` the dll. Wait sites: `+0x3D3DB` is the Present wait under
  `sub_7D7210`; `+0x1595A3` is a LockRect wait ([[d3d9-gpu-readback-stall]]).
- **Every D3D9 call takes DXVK's device lock.** The engine creates the device multithreaded, and DXVK
  only locks then. In 3.1.1 the lock is a recursive spinlock inlined into ~130 API entry points
  (`call [GetCurrentThreadId]`, `lock cmpxchg [device+0x580]`, spinning with `pause`; released with
  `xchg [owner], 0`); samples land on the instruction after either one. Uncontended but expensive:
  ~0.33 ms/frame in the usual test scene (1.22 ms/frame was measured in combat on 2.7.1, when the
  shadow passes still issued ~33k draws per frame).
  Cost scales with API call count, ours included (`ShaderRecord::SetCT`, `DrawGeoArrays`,
  per-bone constants). Fewer calls is the only lever: dropping `D3DCREATE_MULTITHREADED` is not an
  option, because the loader thread creates textures ([[dxvk-combat-cpu-headroom]]).
- `floatEmulation = Strict` is DXVK's most expensive float mode. Untested GPU lever: A/B it against
  `True`, and watch for black or NaN pixels, which may be why it was set.

**Why:** without this, the idle wait reads as engine update time, and d3d9 rows read as the Windows
runtime or the driver.
**How to apply:** before calling a frame CPU-bound, subtract the Present wait. Treat D3D9 CPU cost
as DXVK recording cost. Remember the driver's work is on threads the sampler never sees. Native
D3D9 is slower and GPU-bound ([[native-d3d9-gpu-bound]]).
