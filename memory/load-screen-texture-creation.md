---
name: load-screen-texture-creation
description: "OR's MemoryManagement rewrite of the texture create (0x7610D3) uses D3DX_DEFAULT mips, so mipless DDS files get a chain built (DXT decode/re-encode) on the loader thread inside the global NiStream lock; mipless DDS are the usual cause of load stalls"
metadata:
  type: project
---

Measured in the ProtoPlugin repo with its LoadProbe (full write-up there:
`memory/oblivion-load-screen-shape.md`).

**OR's hook is on this path.** The engine's one 2D texture create is the call at 0x7610D3 in
`NiDX9SourceTextureData_LoadTextureFile` (0x760DA0), normally `D3DXCreateTextureFromFileInMemory`
from d3dx9_27. `[Main] MemoryManagement = 1` rewrites it (`TESReloaded/Core/MemoryManagement.cpp`) to
`D3DXCreateTextureFromFileInMemoryEx` from d3dx9_43 with `D3DX_DEFAULT` sizes, mips and filters and
`D3DPOOL_DEFAULT`.

**Mipless textures are the cost.** With `MipLevels = D3DX_DEFAULT`, D3DX builds a missing chain by
DXT decode, filter and re-encode at ~35 ns per top-level pixel. Full-mip textures copy at ~1.9 GB/s.
One 8192² DXT1 atlas without mips cost 2.26 s on its own; ~11k of the install's ~57k loose DDS files
≥64 px lacked a full chain. ProtoPlugin's `tools/fixmips.py` adds the chains offline (mips ≤ 1 only;
partial chains are cheap); the GPU-side result is unchanged because D3DX built the same chain anyway.
With fixmips.py and ProtoPlugin's BillboardBatch ([[protoplugin-sibling]]; distant tree billboard
completion re-inited the whole LOD root per billboard), a cold exterior load went 9.1 s → 2.6 s.

**It happens inside the global NiStream lock.** `0xB3FC00` is a recursive critical section entered at
0x712930 (owner tid `[0xB3FC78]`); NIF loading is serialised process-wide on it. The loader thread
creates textures while holding it, so the main thread waits on it during loads. This loader-thread
texture creation is also why the device must stay `D3DCREATE_MULTITHREADED`
([[dxvk-combat-cpu-headroom]]).

**How to apply:** if `MemoryManagement` or the texture-create hook is ever changed, keep the mip
behaviour in mind. A D3DX flag or pool change here runs on the loader thread, inside that lock. Load
stalls from new assets are usually mipless DDS files; check with fixmips.py before blaming code.
