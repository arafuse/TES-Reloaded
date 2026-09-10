---
name: cellinfo-ninode-is-water-node
description: "GridCellArray::CellInfo::niNode is the cell's WATER node (culled), not its object container; loaded cell objects hang off Tes->ObjectLODRoot"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 11258809-5bd7-4c79-ac78-077c42d72a19
  modified: 2026-08-22T23:05:15.297Z
---

`GridCellArray::CellInfo` in `Game.h` is declared `WaterPlaneData* waterData; NiNode* niNode; // ...`
— a partial guess, not a verified layout. `niNode` resolves in game to `Water Node`, a child of
`WaterRoot`, and it is `AppCulled` while a cell is arriving. It is NOT the cell's object container
and the renderer never descends into it.

The loaded cell's actual objects hang under **`Tes->ObjectLODRoot`** (offset 0x0C on `TES`), one
direct child per loaded cell, measured by printing drawn geometry's `m_parent` chain:

```
LandscapeLogBirchMoss01: < BASE Landscape\Landscape < ? < ? < ?@270B9800 < ObjectLODRoot
CDoor00:1                < BASE Dungeons\Caves\CDoo < ? < ? < ?@270C6140 < ObjectLODRoot
WolfBody_def             < BASE creatures\rat\racco < Raccoon ref < ? < ?@270C6140 < ObjectLODRoot
```

So `ObjectLODRoot -> cellNode -> subNode -> (refNode) -> BASE node -> geometry`. Diffing
`ObjectLODRoot`'s children by set membership is the reliable way to detect cells loading and
unloading, and a departing cell IS a removed child (so it can be pinned/held). Note the name is
misleading in the same way `Tes->LODRoot` is — see [[distant-lod-scene-graph]].

Cost me five sessions on the LOD dither fade: the cell tier diffed `CellInfo::niNode`, detected
transitions correctly, and faded a node nothing ever drew. See [[lod-dither-fade]].
