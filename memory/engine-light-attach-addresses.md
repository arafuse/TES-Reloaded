---
name: engine-light-attach-addresses
description: "Reverse-engineered Oblivion.exe addresses for the ShadowSceneNode per-frame light processing (the \"unoptimized lights\" hotspot)"
metadata:
  type: reference
---

From disassembling `Oblivion.exe` (1.2.0.416, ImageBase 0x400000).

- Scene-graph pointer table: `0x00B42F54` is `table[index*4]`, not a single pointer. Getter `0x007B4280`, setter `0x007B4270`. The plugin's `kShadowSceneNode` reads index 0. (So there are NO direct-global refs to hook — the attach code takes the node by pointer.)
- **ShadowSceneNode vtable: `0x00A904B4`** (39 virtuals; RTTI TypeDescriptor `0x00B2D16C`). Impl cluster: `0x0070A000–0x0070C200` (inherited/overridden NiNode) + `0x007C7000–0x007C8300` (SSN-specific).
- **Per-frame processing entry: `0x007C78D0`** (vtable slot 31, ~1255 B, SEH frame). Takes a NiCamera arg. Walks the ENTIRE `lights` NiTList (`ShadowSceneNode+0xE4`, entry: [0]=next,[8]=data=ShadowSceneLight), dispatches per-light, then sets up cull bounds/matrices (globals `0xB46638+`). Called ONLY through the vtable (no direct callers).
- Per-light dispatch inside 0x7C78D0:
  - `[SSL+0x104]!=0` → `0x007C6020` (211 B)
  - `[SSL+0xF4]!=0` (shadow-casting flag) → `0x007D6390` — the heavy path: 1379 B, 4 nested loops, 25 geometry-list traversals (per-shadow-light × geometry association).
  - else (ordinary light) → `0x007C77C0` (cheap, 184 B, no geometry loop; calls 0x7D5F80)
  - finalize → `0x007C71B0` (390 B, 2 loops)

Takeaway: Oblivion's light cost is LIGHT-centric, not O(objects×lights) per-object attach. MEASURED with in-game counters: the light flags at +0xF4/+0x104 are zero everywhere in this fork (engine shadows are off), so 0x7D6390 never runs and only the cheap path 0x7C77C0 -> 0x7D5F80 executes over ~19 lights/frame. Engine light processing is NOT a bottleneck here; do not re-derive.
