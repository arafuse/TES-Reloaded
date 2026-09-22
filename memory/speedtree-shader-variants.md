---
name: speedtree-shader-variants
description: "Stock SpeedTree VS variant map (STB20xx sun vs point-light passes, STFROND, STLEAF), which we override, free constants, BSTreeNode lookup, bend weight; groundwork for small-tree collision"
metadata:
  node_type: memory
  type: project
  originSessionId: 2d5bf1ce-a619-4141-b064-9b3913855781
  modified: 2026-09-22T23:07:04.599Z
---

Spike findings (2026-09-22) on bending small SpeedTree trees/shrubs away from actors, like grass
collision. Verdict: feasible; not built. Vanilla ships 31 `shrub*.spt` plus sapling/young trees; SI
adds 10 bushes — undergrowth IS SpeedTree.

**Stock variants** (from `shaderpackage019.sdp`, extraction per [[par-shader-interpolator-layout]]):
- Branches `STB2000-2017.vso`, all vs_2_0. Sun-lit first passes carry fog: 2004-2011, 2014, 2015
  (odd = ShadowProj). Point-light passes carry `LightPosition` c16: 2000-2003, 2012, 2013, 2016,
  2017 (2017 also `ObjToCubeSpace`). `STB1000-1009` are the SM1 path.
- Fronds `STFROND000-003.vso`, vs_1_1, **not overridden**. Leaves `STLEAF000-003.vso`, vs_1_1.
- Overridden in `Shaders/ExtraShaders`: STB2003/2005/2007/2009/2015, all four STLEAF VS,
  STLEAF2000/2001 PS. `isTree` (ShaderIOHook) covers only 2005/2007/2009/2015.

**Why it's easier than grass:** constant space is roomy (branch overrides reach c73, leaves c81 +
c100-103 + c150-153, fronds c33), so a c240+ block is free everywhere, unlike [[grass-vs-constant-budget]].
Every stock tree VS bends by `blendindices.x` (SpeedTree wind weight, 0 at trunk base) — reuse it as
the collision bend weight. Vertices are MODEL space, so actor XY must be taken into the tree's frame
(inverse rot/scale of the WorldTransform). Leaves: displace `IN.position` (cluster centre) before the
LeafBase billboard offset. Wind palette is one global (`kWindMatrixes` 0xB467B8), not per tree.

**Per-tree lookup:** `TrackSetupShaderPrograms` sees every tree draw; walk `m_parent` to the node
with vtable `0x00A65854` (BSTreeNode; `Geo->m_parent->m_parent` for leaves) and use its
`m_kWorldBound.Radius` so branch/frond/leaf draws of one tree agree on "small".

**Main risk:** branches are multipass. Any variant left stock won't bend, and if follow-up passes use
ZFUNC EQUAL (as PAR's do) their displacement must be bit-identical to the first pass. Plan: same
include call in EVERY variant that draws (13 new STB + 4 STFROND overrides). Shadows:
`ShadowMap.vso` already has leaf (`ShadowData.x == 2`) and branch-wind (`== 3`) paths to extend.

**How to apply:** before building, run the `[TreeCapDbg]` capture (RenderHook.cpp, rides the
`[GrassOrderDbg]` one-frame capture on the `Develop.LogShaders` key) by day and at night by a torch:
it logs every ST* pass with override flags (`*`), zfunc/zwrite/blend and the BSTreeNode bound. Record
the results here. Related: [[speedtree-property-not-pp-lighting]], [[shader-pipeline-facts]].
