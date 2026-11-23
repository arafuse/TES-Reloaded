---
name: native-d3d9-gpu-bound
description: "Native D3D9 (no DXVK) is GPU-bound: the ~5.6 ms/frame NVIDIA driver wait is the CPU blocked in Present, not the skinned index re-upload the sampler points at; the native-vs-DXVK gap is GPU execution time; engine index-buffer helper facts"
metadata:
  type: project
---

On native D3D9 (game-folder `d3d9.dll` renamed away), RTX 5070 Ti, the game runs ~77 FPS against
~93-97 on DXVK. PresentMon: 12.67 ms between presents, GPU busy 12.63, CPU busy 6.83,
MsInPresentAPI 5.82. **The frame is GPU-bound.** The sampler puts a ~5.6 ms/frame driver wait
(`nvd3dum` → `WaitForSingleObject`) under `NiDX9Renderer::PackSkinnedGeometryBuffer`'s index upload,
but that is where the CPU happens to block, not the cause: skipping the re-upload (tried in
ProtoPlugin, 9708 of 9709 uploads skipped) left the same wait under a different, stale game frame.
`NiDX9Renderer::PresentScene` shows ~5.66 ms/frame inclusive. Present mode was "Composed: Copy with
GPU GDI"; Special K and NVIDIA Low Latency Mode were ruled out. Full write-up: ProtoPlugin
`memory/oblivion-skinned-index-reupload-stall.md`.

Engine facts from the disassembly:
- `sub_7781F0` (on renderer `+0x8B4`) is the INDEX buffer helper: it reuses the existing IB if its desc
  matches (INDEX16, usage 0 or SOFTWAREPROCESSING, pool 1 = MANAGED, size fits), else
  `CreateIndexBuffer`, then `Lock(0, size, &p, 0)` + memcpy of the partition's triangle list.
- `PackSkinnedGeometryBuffer` (0x763130) sets the dirty mask to 0xFFF for volatile data or arg6 (force)
  at 0x763241, so it re-uploads identical indices every frame. Its main-render caller `sub_77A310`
  passes force = 0. Skipping it is safe if the index count, triangle count, strip count and triangle
  list pointer all match the stored ones (`+0x28/+0x3C/+0x44/+0x4C`); it saves nothing measurable.
- The vertex side (`sub_7774C0` on `+0x8B0`) does not stall ([[volatile-geometry-pack-reuse]]).

**How to apply:** for native-D3D9 FPS, find which pass is slower on the GPU: a same-session
`ProfileFrame = 2` A/B (native vs DXVK) or OR effect toggles. Nothing CPU-side in the engine will
close it. Open GPU suspects: driver-profile settings that apply only to D3D (transparency AA,
forced AO or AF), and the blt present model. See [[game-runs-on-dxvk]], [[gpu-frame-breakdown]],
[[profiling-method-rules]].
