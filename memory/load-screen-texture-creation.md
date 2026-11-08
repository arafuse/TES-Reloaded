---
name: load-screen-texture-creation
description: "MEASURED in ProtoPlugin - OR's MemoryManagement rewrite of the texture create (0x7610D3) uses D3DX_DEFAULT mips, so mipless DDS files get a chain built (DXT decode/re-encode) inside the global NiStream lock; fixmips.py + BillboardBatch took a cold exterior load from 9.1s to 2.6s"
metadata:
  type: project
---

Measured 2026-10-31 in the ProtoPlugin repo with its LoadProbe (full write-up there:
`memory/oblivion-load-screen-shape.md`).

**OR's hook is on this path.** The engine's one 2D texture create is the call at 0x7610D3 in
`NiDX9SourceTextureData_LoadTextureFile` (0x760DA0), normally `D3DXCreateTextureFromFileInMemory`
from d3dx9_27. `[Main] MemoryManagement = 1` rewrites it (`TESReloaded/Core/MemoryManagement.cpp`) to
`D3DXCreateTextureFromFileInMemoryEx` from d3dx9_43 with `D3DX_DEFAULT` sizes, mips and filters and
`D3DPOOL_DEFAULT`.

**Mipless textures are the cost.** With `MipLevels = D3DX_DEFAULT`, D3DX builds a missing chain by
DXT decode, filter and re-encode at ~35 ns per top-level pixel. Full-mip textures copy at ~1.9 GB/s.
On a cold exterior load, 3.31 of 4.43 s of texture creation came from 34 mipless files:
`textures\lowres\javwd_cyrodiil_atlas.dds` (8192² DXT1, 1 mip) alone 2.26 s, plus 28
`textures\landscapelod\generated\*` quads (~37 ms each). 11,281 of 57,409 loose DDS files ≥64 px lack
a full chain. ProtoPlugin's `tools/fixmips.py` adds the chains offline (mips ≤ 1 only; partial chains
are cheap); the GPU-side result is unchanged because D3DX built the same chain anyway.

**It happens inside the global NiStream lock.** `0xB3FC00` is a recursive critical section entered at
0x712930 (owner tid `[0xB3FC78]`); NIF loading is serialised process-wide on it. The loader thread
creates textures while holding it, and the main thread waited 2.7 s for it on the cold load. This
loader-thread texture creation is also why the device must stay `D3DCREATE_MULTITHREADED`
([[dxvk-combat-cpu-headroom]]).

**The other half was tree billboards.** `QueuedTreeBillboard` completion 0x437600 updated and re-inited
the property state of the whole distant-LOD root `[0xB34424]` per billboard (~13,700 per cold load,
2.4 s). ProtoPlugin's BillboardBatch defers it to one pass per IO drain ([[protoplugin-sibling]]).

| cold exterior load | wall | texture creation | main waiting on NiStream lock |
|---|---|---|---|
| before | 9.06 s | 4.43 s | 2.80 s |
| fixmips.py | 5.60 s | 1.22 s | 0.60 s |
| + BillboardBatch | 2.60 s | | |

**How to apply:** if `MemoryManagement` or the texture-create hook is ever changed, keep the mip
behaviour in mind. A D3DX flag or pool change here runs on the loader thread, inside that lock. Load
stalls from new assets are usually mipless DDS files; check with fixmips.py before blaming code.
