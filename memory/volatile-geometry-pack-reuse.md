---
name: volatile-geometry-pack-reuse
description: "NEVER reuse a PackGeometryBuffer result across draws: volatile geometry repacks into a recycled shared VB pool every call; skipping per-draw packs drew stray triangles (dd07726, reverted)"
metadata:
  node_type: memory
  type: project
  originSessionId: ae91bd5d-ef17-461e-bc53-0af58fe36e78
  modified: 2026-11-08T14:41:32.977Z
---

`NiDX9Renderer::PackGeometryBuffers` (0x767B40) reads the consistency field of the
`NiGeometryData` (`+0x2E & 0xF000`). Volatile data (`0x8000`) is marked fully dirty and repacked on
EVERY call, into a fresh chunk allocated from the renderer's shared vertex-buffer pool
(`sub_7774C0` on the manager at renderer `+0x8B0`). Later packs recycle that pool, so a chunk is
only safe to draw right after the pack that made it. (`sub_7781F0` on `+0x8B4` is the INDEX buffer
helper, not the vertex pool; see [[native-d3d9-skinned-index-stall]].) Static geometry returns early (cheap check, ~0.6 µs).

dd07726 packed each cube-bake item only in the first face that drew it and reused the pack in the
other faces. Faces 2-6 then drew from recycled chunks: stray triangles at the light, seen as whole
cube faces flickering dark (worst near NPCs; animated faces are the likely volatile source). It only
saved ~0.04 ms and was reverted in b8cf27d; the user confirmed the fix in-game (2026-11-08).

**Why:** the pack result is not a property of the mesh but of the moment it was packed.
**How to apply:** every draw of engine geometry keeps its own Pack*GeometryBuffer call
immediately before it. Batching or instancing schemes may share a pack only for data known to be
static (consistency not `0x8000`), and only within one uninterrupted run of draws.
See [[many-actors-havok-cost]] for the shadow-pass context.
