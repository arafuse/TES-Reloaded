---
name: dxvk-combat-cpu-headroom
description: "MEASURED 2026-11-08 (PresentMon) - DXVK market combat is CPU-bound with ~3.6 ms GPU idle, but no engine-side lever beats ~0.4 ms; the MULTITHREADED device flag must stay; the rigid-pack cost is volatile morph geometry (goblin heads), not FaceGen; OR's shadow passes are the biggest CPU item"
metadata:
  type: project
---

Spike done in the ProtoPlugin repo on the levers in its `docs/engine-cpu-report.md`. Full write-up:
ProtoPlugin `memory/oblivion-cpu-levers-combat.md`.

**Headroom (PresentMon 2.6, DXVK, market-district combat with `placeatme` goblins, medians):**
14.71 ms between presents (~66 FPS), CPU busy 14.63, MsInPresentAPI 0.08, GPU busy 10.99 (75%),
GPU wait 3.59. CPU-bound, unlike native D3D9 ([[native-d3d9-skinned-index-stall]]), so up to ~3.6 ms
of main-thread savings would show as FPS. Only 427 steady frames were captured.

**DXVK device lock (1.22 ms/frame, [[game-runs-on-dxvk]]): the flag that enables it cannot go.**
`CreateDevice` is called at 0x76A545 (IDirect3D9 at `[0xB42154]`, vtable `+0x40`). `D3DCREATE_MULTITHREADED`
is OR'd in at 0x76A30A when renderer-create flags bit `0x20` is set. The loader thread creates
textures through D3DX while the main thread renders ([[load-screen-texture-creation]]), so the flag
must stay. The only lever left is fewer D3D9 calls, and most of those are ours (shadow passes).

**Rigid vertex packing, ~0.8 ms/frame, is not FaceGen.** Measured around `PackGeometryBuffers`
(0x767B40; call sites 0x77A373 in `sub_77A310` force 0, 0x7692D2 force 1), ~3,200 combat frames:
- total 0.79 ms/frame; FaceGen head packs 0.08; clean early-outs 0.03;
- volatile data that changed since the last draw: 18.5 calls, 0.27 ms;
- volatile data that had **not** changed: 105.5 calls, **0.41 ms**, the only avoidable part;
- by source: goblin `Head:0` (goblinhead.nif) 69% of volatile time, about half unchanged; NPC
  FaceGen hair, teeth, tongue, eyes and mouth ~20%, never changed.

Mechanism:
- `NiTimeController::SetTarget` permanently marks a controller's geometry data VOLATILE (0x715D52:
  `or cx, 8000h` into `+0x2E`) when the controller's vtable `+0x60` returns true.
  `NiGeomMorpherController` is one such controller.
- The morpher marks its own changes: vtable `+0x68` = 0x6D0CF0 sets dirty bit 1 at 0x6D0D12. Render
  clears dirty bits (0x76964E, 0x7698A3), so a dirty bit at pack time means "changed since the last draw".
- FaceGen morph apply: `BSFaceGenNiNode` update 0x55D4C0 → 0x55D1B0; 0x5508F0 writes vertices and
  sets dirty bit 1 (0x550969). Head morph CPU is ~0.07 ms/frame.
- Not checked: whether the buffer manager picks the D3D pool or usage from the consistency, so
  clearing VOLATILE could move these buffers off DYNAMIC and cause lock stalls. Skipping an unchanged
  repack also runs into [[volatile-geometry-pack-reuse]].

**Other items:** refcount atomics (~6,200 IAT call sites at 0xA28078 / 0xA2807C; redirecting the IAT
saves nothing, removing get/release pairs in the hottest callers ~0.1 ms); extra-data critical
section ~0.2 ms.

**How to apply:** don't build engine-side micro-patches for these. The largest is ~0.4 ms in a
`placeatme` goblin scene and ~0.1-0.15 ms in natural scenes. The bigger CPU items are OR's shadow
passes (most of the API calls that pay the DXVK lock; see [[many-actors-havok-cost]]) and the flat
~5.3 ms update phase, which needs a per-function exclusive profile in a natural crowd scene.
