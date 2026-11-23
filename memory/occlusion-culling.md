---
name: occlusion-culling
description: "One-frame-lag occlusion culling of the far pass (OcclusionCulling.cpp, [Main] OcclusionCulling, on by default) took the dense-town spot 33.8 -> 25.3 ms for 0.44 ms of its own CPU; the placement and key rules it depends on"
metadata:
  type: project
---

`OcclusionCulling.cpp` queries each cullable reference's bound as one screen rect, drawn at the
bound's nearest depth, against the finished far-pass depth. On the next frame it sets `kFlag_AppCulled`
on the roots that came back with zero pixels, for the far pass only. It is on by default (code default
and the tracked template INI both 1). Pure math lives in `OcclusionCullMath.h`
(`Tools\Tests\Run-OcclusionCullMathTest.ps1`).

**Measured** (PresentMon, 60 s standing still, `NearShellEnabled = 1`):

| Spot | Off | On | Change |
|---|---|---|---|
| Dense mod town street | 33.84 ms (p99 39.3) | 25.26 ms (p99 28.5) | -8.58 ms (-25 %); GPU busy 9.62 -> 7.15 |
| Ordinary exterior control | 4.66 ms (p99 6.0) | 4.49 ms (p99 5.3) | -0.17 ms; no regression |

- **Hidden share:** in the town, 1255 candidates are queried and 1094 are hidden (87 %); elsewhere ~80 %.
- **Own cost (`ProfileFrame = 1`, town):** `Occ:Begin` 0.26 ms (results read plus the cell walk),
  `Occ:Issue` 0.18 ms (1255 queries), `Occ:End` 0.001 ms. About 5 % of the saving, so a CHC++-style
  re-query interval isn't worth it.
- The town is still CPU-bound after culling ([[dense-town-cpu-budget]]).
- **Visual behaviour:** nothing pops in while walking or turning normally. Objects within
  `OcclusionAlwaysDrawDistance` (1024) are never culled. A camera jump of more than
  `OcclusionCameraCutDistance` (128), a world change, or a load draws everything for one frame.

**Rules that hold it together:**
- **Pass placement:**
  - `BeginOcclusionCull` and `EndOcclusionCull` bracket only the main far pass's `RenderObject`. The AppCulled flags must never be held during the shadow maps, the reflection pass, the shell or first person; `RenderShadowMaps` logs an ERROR if they are.
  - `IssueOcclusionQueries` runs after the far pass's `ResolveDepthBuffer` and before the shell's depth clear. After the clear, every query reads `ShellClearDepth` and passes. See [[near-shell-frame-order]].
- **Depth row:** the query depth is computed from the far pass's [M=15, F=400000] frustum with `FarPassDepth`, not from the NiCamera matrix (it matches the device's: `_33 1.000038`, `_43 -15.0006`; debug 1 logs both once).
- **Keys, not pointers:** results are keyed by the (ref, root node) pair and applied only to references found again in this frame's cell walk. A stale pointer is never dereferenced, so a cell that unloads between frames can't crash it.
- **Query results:** read them with `GetData(..., 0)` only, and treat S_FALSE as visible. Never read them back through LockRect; see [[d3d9-gpu-readback-stall]].
- **Candidate types:** statics, containers, doors, furniture, activators and items. Lights, trees, grass, actors and the player are never candidates. Bounds that reach below the water plane are always drawn, because they show through refraction.
- **Debug modes (`[Develop] OcclusionCullDebug`, read at load):** 1 logs stats each second; 2 freezes the first non-empty hidden set after a load (save at the spot to inspect; holes show in the wrong places only when you walk away from the frozen spot); 3 measures only and never culls.
- **The water reflection pass is not culled** ([[reflection-occlusion-not-worth-it]]).
