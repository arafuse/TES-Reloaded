---
name: distant-lod-scene-graph
description: "Where Oblivion's distant LOD terrain, statics and tree billboards actually live in the scene graph, and why Tes->LODRoot is misnamed"
metadata:
  type: reference
---

Measured in game; do not re-derive.

`WorldSceneGraph` has exactly 2 children: `"WorldRoot CameraNode"` and `"shadow scene node"`
(VFT `0x00A904B4`, the ShadowSceneNode from [[engine-light-attach-addresses]]). Everything else hangs off
the shadow scene node, whose 6 children are Sky / Weather / LODRoot / ObjectLODRoot / MagicProjectileRoot
/ Grass.

**`Tes->LODRoot` (`Game.h` OBLIVION `TES`, offset 0x10) is misnamed.** It points at the node called
`"LandLOD"` — 12 `NiTriShape` terrain quadrants of world-bound radius ~90000 on a ±65536 / ±196608 grid —
*not* at the scene node called `"LODRoot"`. The NEWVEGAS/SKYRIM `TES` blocks name the same field
`landLOD`, which confirms it. The real container is `Tes->LODRoot->m_parent`, whose children are
`LandLOD`, `DistantRefLOD`, `LODWaterRoot`.

**`"DistantRefLOD"` holds the distant statics** (~2075 children):
- `[0]` is `"LOD Trees"`: ~426 children of VFT `0x00A9595C`, one merged mesh per tree species (hence
  bound radii up to ~150000). That VFT also appears under `Grass` and is in no VFT table this fork has.
- `[1..N]` are per-distant-cell `NiNode` wrappers, each holding one `BSFadeNode` of
  `MergedLOD\Tamriel_X_Y_far.NIF` or an individual `..._far.NIF`.

**`Tes->ObjectLODRoot` is not an LOD root** despite the name — it is the loaded 5×5 cell grid: full-res
`Block (X, Y)` terrain quadrants, `BSFadeNode` clutter, and skinned actors including the player. See
[[cellinfo-ninode-is-water-node]].

`Tes->gridDistantArray` is a `size²` array (`size = uGridsToLoad + 2*uGridDistantCount`, 65 in practice)
of 16-byte records where `unk04` is an `NiNode*` and `unk08`/`unk0C` are signed cell X/Y. It indexes the
same per-cell nodes reachable from `DistantRefLOD`, so it is redundant for traversal.

Two gotchas when walking any of this: `NiGeometryData::BuffData` is created lazily on first draw, so
objects that have never rendered legitimately have `BuffData == NULL` (4 of 12 LandLOD quadrants at any
moment); and a hardcoded VFT whitelist silently drops the unidentified classes out here — classify by
walking the `NiRTTI{name,parent}` chain from `NiObject::GetType()` (GameNi.h) for `"NiGeometry"` /
`"NiNode"` instead. Blind-casting an unverified VFT to `NiNode*` crashes (e.g. on `NiPointLight`).
