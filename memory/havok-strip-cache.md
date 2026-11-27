---
name: havok-strip-cache
description: "hkNiTriStripsShape's O(strips) child-shape walk (0x915D00, loop 0x915D60) is answered from a per-mesh table by ProtoPlugin's HavokStripCache (moved out of OR); it assumes strips never change after first lookup, so OR code must never rewrite strips in place; HavokStripProfile stays in OR"
metadata:
  type: project
---

**Engine fact:** hkNiTriStripsShape's child-shape lookup (vtable slot 10 = 0x915D00) walks the
strip-length array from strip 0 on every call (loop 0x915D60-0x915D9F). In the dense town
([[dense-town-cpu-budget]]) one 143-strip collision mesh the NPCs stand on took 20-28k lookups per
frame (~1 ms/frame in the loop); caching it took the median frame 23.2 -> 21.0 ms, likely via the
Havok fixed-timestep feedback in [[many-actors-havok-cost]].

**The cache lives in ProtoPlugin** ([[protoplugin-sibling]]): `ProtoPlugin/HavokStripCache.cpp`,
`[Havok] bCacheStripLookups` (default 1) in `ProtoPlugin.ini`, with a jump at 0x915D3F and the
`~NiTriStripsData` call at 0x71A113 rewritten. Its full write-up is ProtoPlugin's
`memory/module-havokstripcache.md`. OR's `[Main] HavokStripCache` key is gone; a leftover line in an
old INI is ignored. Never hook 0x915D3F-0x915DA1 or 0x71A113 from OR.

**OR keeps `HavokStripProfile.cpp`** (`[Develop] ProfileFrame`): a slot-10 vtable hook that counts
lookups per mesh and call site. It reaches the engine function, so it works over ProtoPlugin's patch,
and notes "strip walk patched" when 0x915D3F holds a jump.

**How to apply:** the cache assumes a NiTriStripsData's strips never change after their first lookup.
Any OR code that rewrites +0x44/+0x48 in place after load would serve stale answers to Havok. Never
join collision strips: the NIF's MOPP tree addresses triangles by index.
