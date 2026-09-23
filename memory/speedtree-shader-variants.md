---
name: speedtree-shader-variants
description: "Stock SpeedTree VS variant map (STB20xx sun vs point-light passes, STFROND, STLEAF), which we override, free constants, BSTreeNode lookup, bend weight; groundwork for small-tree collision"
metadata:
  node_type: memory
  type: project
  originSessionId: 2d5bf1ce-a619-4141-b064-9b3913855781
  modified: 2026-09-22T23:55:48.155Z
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

**Tree draws are BATCHED — `TrackSetupShaderPrograms` does NOT see each tree** (capture-measured:
one call per ST variant per frame, carrying the first tree only). Drivers like `sub_7F6FC0` (siblings
at 0x7F7680/7EE0/86C0/8DB0/9410) call vtable slots 12/13/14 for a batch's first geometry, then loop
(0x7F73B0) calling only slot 13 `SetupTransformations` (+0x34) and slot 15 (+0x3C) per geometry.
Slot 13 is 8-arg thiscall, `ret 0x20`, same args as SetupShaderPrograms (Geometry, …, NiTransform*
world, NiBound*): branch = 0x7C9230 (BSShader base, shared — do not Detour it), leaf = 0x7F15E0,
frond = 0x80DDA0. Branch slot 14 (0x80FC20) just forwards to base 0x77A1F0.
- **Branches:** patch slot 13 in the branch vtable 0xA9459C. CAPTURE-VERIFIED: fires once per tree
  (18 trees in one STB2005 batch), its NiTransform matches the BSTreeNode exactly, and it fires in
  the EQUAL follow-up batches too.
- **Leaves: slot 13 does NOT fire per tree** (once per frame, measured). The leaf loop in
  `sub_7F86C0` skips slot 13 and calls 0x7F0BC0 directly per tree (0x7F8B51/0x7F8C66); the first
  tree reaches it via leaf slot 11 (0x7F13B0 = UpdatePipeline wrapper). 0x7F0BC0 = leaf
  UpdatePipeline body, thiscall 7 args `ret 0x1c` (Geometry, Skin, BuffData, PropertyState,
  EffectState, NiTransform*, NiBound*); leaf-only callers → Detour it for per-tree leaves (static
  evidence, not yet capture-verified). 0x7F0100 (leaf-only) takes the tree's INVERSE world matrix
  and writes model-space light vectors to globals 0xB46738+.
- Order trap: for a batch's first geometry, UpdatePipeline/SetupTransformations run BEFORE
  SetupShaderPrograms, whose SetCT re-uploads every declared TESR_ constant — so per-tree collision
  registers must NOT be TESR_-named or SetCT overwrites the first tree's values.
- Fronds: none drawn in vanilla Great Forest captures (no STFROND pass, no frond xform).

**Per-tree lookup:** walk `m_parent` to vtable `0x00A65854` (BSTreeNode; depth 2 for both leaves
and branches) and use its `m_kWorldBound.Radius` so a tree's draws agree on "small". Measured
bounds: shrubs 190-617 (AzaleaPink 190, Boxwood 206, Inkberry 210-221, MugoPine 253, Azalea 317,
Euonymus 471, JapHoneysuckle 617); young trees 1331-1475 (SugarMapleYoung, WillowOakYoung,
WhitePineYoung); forest trees 1597-4369. Shrub-only threshold ≈ 700; young trees would need ≈ 1500,
uncomfortably close to EnglishOakForest 1597.

**Multipass is real (capture-measured):** first passes STB2005/2007/2009 are zfunc LESSEQUAL, zwrite
on. Follow-ups are **ZFUNC EQUAL, zwrite off**: STB2015 (sun specular, additive ONE/ONE), STB2016
(point specular, additive, STOCK today), STB1009 (fog, SRCALPHA/INVSRCALPHA, STOCK vs_1_1). So
every pass must displace bit-identically — any variant left stock will z-fail on bent geometry.
Leaves are single-pass (STLEAF001, alpha test ref 84). Shadows: `ShadowMap.vso` already has leaf
(`ShadowData.x == 2`) and branch-wind (`== 3`) paths to extend.

**How to apply:** the `[TreeCapDbg]` instrumentation in RenderHook.cpp (temporary; rides the
`[GrassOrderDbg]` one-frame capture on the `Develop.LogShaders` key; `xform` lines come from the slot-13
vtable wrap) is the ground truth for which variants draw. Related:
[[speedtree-property-not-pp-lighting]], [[shader-pipeline-facts]], [[oblivion-pdb-symbols]].
