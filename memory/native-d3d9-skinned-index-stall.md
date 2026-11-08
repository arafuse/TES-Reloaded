---
name: native-d3d9-skinned-index-stall
description: "DISPROVEN AS THE CAUSE - on native D3D9 the ~5.6 ms/frame NVIDIA driver wait sat under PackSkinnedGeometryBuffer's per-frame MANAGED index re-upload, but removing the upload only moved the wait (FPS unchanged); it is a throttle, and the native-vs-DXVK gap is elsewhere"
metadata:
  node_type: memory
  type: project
  originSessionId: ae91bd5d-ef17-461e-bc53-0af58fe36e78
  modified: 2026-11-08T15:30:50.690Z
---

Measured 2026-11-08, native D3D9 (game-folder `d3d9.dll` renamed to `.off`), NVIDIA RTX 5070 Ti,
driver 616.56: 76.7 FPS vs ~97 on DXVK. The sampler's game-frames breakdown ([[sampling-profiler]])
put 96.7% of the 5.6 ms/frame driver wait (`nvd3dum+0x13BF94` → `WaitForSingleObject`) on
`sub_7781F0+0x107` ← `NiDX9Renderer::PackSkinnedGeometryBuffer+0x253` (call at 0x76337E).

**It was where the wait happened, not why.** The ProtoPlugin repo's `SkinIndexUpload` module skipped
the re-upload (9708 uploads skipped, 1 kept). Native still ran 76-78 FPS against 92-93 on DXVK, and
the same leaf still cost 5.63 ms/frame, now under a different call. A wait that keeps its size and
moves when its call site goes away is a throttle: the CPU blocks at whichever call first needs the
GPU to catch up. Full write-up: ProtoPlugin `memory/oblivion-skinned-index-reupload-stall.md`.

Still-open suspects for the gap:
- the GPU is slower per frame on native D3D9 (GPU 13.06 ms vs ~11 on DXVK; main pass 7.05 vs ~5.9);
- a D3D9-only render-ahead limit, e.g. NVIDIA Low Latency Mode, which applies to DX9-11, not Vulkan;
- driver-profile settings that apply only to D3D (transparency AA, forced AO or AF).
Special K is in the process but its Oblivion profile has `[API.Hook] d3d9=false`, so it does not hook
native D3D9; its limiter is 222 FPS.

Engine facts from the disassembly, still true:
- `sub_7781F0` (on renderer `+0x8B4`) is the INDEX buffer helper: it reuses the existing IB if its desc
  matches (INDEX16, usage 0 or SOFTWAREPROCESSING, pool 1 = MANAGED, size fits), else
  `CreateIndexBuffer`, then `Lock(0, size, &p, 0)` + memcpy of the partition's triangle list.
- `PackSkinnedGeometryBuffer` (0x763130) sets the dirty mask to 0xFFF for volatile data or arg6 (force)
  at 0x763241, so it re-uploads identical indices every frame. Its main-render caller `sub_77A310`
  passes force = 0.
- The vertex side (`sub_7774C0` on `+0x8B0`) does not stall.

**How to apply:** do not chase per-call driver waits on native D3D9 until GPU busy time is known;
fix the wait site and the wait just moves. Next step is a same-session `ProfileFrame = 2` A/B (native
vs DXVK) and a look at the NVIDIA profile. See [[game-runs-on-dxvk]], [[gpu-frame-breakdown]].
