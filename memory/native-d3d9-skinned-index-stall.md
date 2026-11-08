---
name: native-d3d9-skinned-index-stall
description: "RESOLVED - native D3D9 is GPU-bound (PresentMon GPU busy 12.63 of 12.67 ms); the ~5.6 ms/frame NVIDIA driver wait is the CPU blocked in Present, and both of its sampler game frames (PackSkinnedGeometryBuffer, then RenderTriGeometries) were stale; the native-vs-DXVK gap is GPU execution time"
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
the same leaf still cost 5.63 ms/frame, now under `sub_7672F0+0x21E` ← `RenderTriGeometries+0xFC`.
That frame is stale too: 0x76750E returns from `NiDX9RenderState` vtable `+0xFC` = 0x77B330, which only
stores one byte. The module was dropped before merge.

**PresentMon settled it (same session, gameplay medians):** 12.67 ms between presents, GPU busy 12.63,
GPU wait 0.15, CPU busy 6.83, MsInPresentAPI 5.82. The sampler's inclusive list has
`NiDX9Renderer::PresentScene` at 5.66 ms/frame. The wait was always the CPU blocked in Present on a
GPU-bound frame. The gap to DXVK (~10.8 ms/frame) is GPU execution time for the same frame.
Present mode was "Composed: Copy with GPU GDI" (blt model through DWM). The user ruled out Special K
and NVIDIA latency settings (Low Latency Mode). Full write-up: ProtoPlugin
`memory/oblivion-skinned-index-reupload-stall.md`.

Engine facts from the disassembly, still true:
- `sub_7781F0` (on renderer `+0x8B4`) is the INDEX buffer helper: it reuses the existing IB if its desc
  matches (INDEX16, usage 0 or SOFTWAREPROCESSING, pool 1 = MANAGED, size fits), else
  `CreateIndexBuffer`, then `Lock(0, size, &p, 0)` + memcpy of the partition's triangle list.
- `PackSkinnedGeometryBuffer` (0x763130) sets the dirty mask to 0xFFF for volatile data or arg6 (force)
  at 0x763241, so it re-uploads identical indices every frame. Its main-render caller `sub_77A310`
  passes force = 0. Skipping it is safe if the index count, triangle count, strip count and triangle
  list pointer all match the stored ones (`+0x28/+0x3C/+0x44/+0x4C`); it saves nothing measurable.
- The vertex side (`sub_7774C0` on `+0x8B0`) does not stall.

**How to apply:** for native-D3D9 FPS, find which pass is slower on the GPU: a same-session
`ProfileFrame = 2` A/B (native vs DXVK) or OR effect toggles. Nothing CPU-side in the engine will
close it. Still-open GPU suspects: driver-profile settings that apply only to D3D (transparency AA,
forced AO or AF), and the blt present model. See [[game-runs-on-dxvk]], [[gpu-frame-breakdown]],
[[profiling-method-rules]].
