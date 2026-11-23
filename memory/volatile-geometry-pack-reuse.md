---
name: volatile-geometry-pack-reuse
description: "NEVER reuse a PackGeometryBuffer result across draws: volatile geometry repacks into a recycled shared VB pool every call, so a reused pack draws stray triangles"
metadata:
  type: project
---

`NiDX9Renderer::PackGeometryBuffers` (0x767B40) reads the consistency field of the
`NiGeometryData` (`+0x2E & 0xF000`). Volatile data (`0x8000`) is marked fully dirty and repacked on
EVERY call, into a fresh chunk allocated from the renderer's shared vertex-buffer pool
(`sub_7774C0` on the manager at renderer `+0x8B0`). Later packs recycle that pool, so a chunk is
only safe to draw right after the pack that made it. (`sub_7781F0` on `+0x8B4` is the INDEX buffer
helper, not the vertex pool; see [[native-d3d9-gpu-bound]].) Static geometry returns early (cheap
check, ~0.6 µs).

**Why:** the pack result is not a property of the mesh but of the moment it was packed. Packing a
point-light cube item once and reusing it in the other faces drew stray triangles at the light (whole
cube faces flickering dark near NPCs, whose animated faces are volatile), for a ~0.04 ms saving.

**How to apply:** every draw of engine geometry keeps its own Pack*GeometryBuffer call
immediately before it. Batching or instancing schemes may share a pack only for data known to be
static (consistency not `0x8000`), and only within one uninterrupted run of draws.
See [[many-actors-havok-cost]] for the shadow-pass context.
