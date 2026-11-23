---
name: speedtree-shader-variants
description: "SpeedTree VS variant map (STB20xx sun vs point-light passes, STFROND, STLEAF) and which we override; tree draws are BATCHED: per-tree hook = branch vtable slot 13, leaf UpdatePipeline 0x7F0BC0 (not SetupShaderPrograms); follow-up passes are ZFUNC EQUAL; BSTreeNode lookup and bounds; c240-c245 + VS s0 = shrub trail collision"
metadata:
  type: project
---

Vanilla ships 31 `shrub*.spt` plus sapling/young trees, and SI adds 10 bushes — undergrowth IS
SpeedTree. Shrubs bend away from actors (`TreeCollision.cpp`, the shrub layer of [[grass-trail-map]]).

**Stock variants** (from `shaderpackage019.sdp`, extraction per [[par-shader-interpolator-layout]]):
- Branches `STB2000-2017.vso`, all vs_2_0. Sun-lit first passes carry fog: 2004-2011, 2014, 2015
  (odd = ShadowProj). Point-light passes carry `LightPosition` c16: 2000-2003, 2012, 2013, 2016,
  2017 (2017 also `ObjToCubeSpace`). `STB1000-1009` are the SM1 path.
- Fronds `STFROND000-003.vso`, vs_1_1, **not overridden**; none drawn in vanilla Great Forest captures.
  Leaves `STLEAF000-003.vso`, vs_1_1.
- Overridden in `Shaders/ExtraShaders`: STB1009 (VS+PS), STB2003/2005/2007/2009/2015/2016 VS, all four
  STLEAF VS, STLEAF2000/2001 PS. `isTree` (ShaderIOHook `TreeVertexShaders`) covers only 2005/2007/2009/2015.

**Constant space is roomy** (branch overrides reach c73, leaves c81 + c100-103 + c150-153, fronds
c33), unlike [[grass-vs-constant-budget]]. Every stock tree VS bends by `blendindices.x` (SpeedTree
wind weight, 0 at trunk base), which is reused as the collision bend weight. Vertices are MODEL space,
so actor XY must be taken into the tree's frame (inverse rot/scale of the WorldTransform). Leaves:
displace `IN.position` (cluster centre) before the LeafBase billboard offset. Wind palette is one
global (`kWindMatrixes` 0xB467B8), not per tree.

**Tree draws are BATCHED — `TrackSetupShaderPrograms` does NOT see each tree** (one call per ST
variant per frame, carrying the first tree only). Drivers like `sub_7F6FC0` (siblings at
0x7F7680/7EE0/86C0/8DB0/9410) call vtable slots 12/13/14 for a batch's first geometry, then loop
(0x7F73B0) calling only slot 13 `SetupTransformations` (+0x34) and slot 15 (+0x3C) per geometry.
Slot 13 is 8-arg thiscall, `ret 0x20`, same args as SetupShaderPrograms (Geometry, …, NiTransform*
world, NiBound*): branch = 0x7C9230 (BSShader base, shared — do not Detour it), leaf = 0x7F15E0,
frond = 0x80DDA0. Branch slot 14 (0x80FC20) just forwards to base 0x77A1F0.
- **Branches:** `TreeCollision.cpp` patches slot 13 in the branch vtable 0xA9459C. It fires once per
  tree, its NiTransform matches the BSTreeNode exactly, and it fires in the EQUAL follow-up batches too.
- **Leaves:** slot 13 does NOT fire per tree. The leaf loop in `sub_7F86C0` skips slot 13 and calls
  0x7F0BC0 directly per tree (0x7F8B51/0x7F8C66); the first tree reaches it via leaf slot 11
  (0x7F13B0 = UpdatePipeline wrapper). 0x7F0BC0 = leaf UpdatePipeline body, thiscall 7 args
  `ret 0x1c` (Geometry, Skin, BuffData, PropertyState, EffectState, NiTransform*, NiBound*);
  `TreeCollision.cpp` Detours it. 0x7F0100 (leaf-only) takes the tree's INVERSE world matrix and writes
  model-space light vectors to globals 0xB46738+.
- Order trap: for a batch's first geometry, UpdatePipeline/SetupTransformations run BEFORE
  SetupShaderPrograms, whose SetCT re-uploads every declared TESR_ constant — so per-tree collision
  registers must NOT be TESR_-named or SetCT overwrites the first tree's values.

**Per-tree lookup:** walk `m_parent` to vtable `0x00A65854` (BSTreeNode; depth 2 for both leaves
and branches) and use its `m_kWorldBound.Radius` so a tree's draws agree on "small". Measured
bounds: shrubs 190-617 (AzaleaPink 190, Boxwood 206, Inkberry 210-221, MugoPine 253, Azalea 317,
Euonymus 471, JapHoneysuckle 617); young trees 1331-1475 (SugarMapleYoung, WillowOakYoung,
WhitePineYoung); forest trees 1597-4369. Shrub-only threshold ≈ 700; young trees would need ≈ 1500,
uncomfortably close to EnglishOakForest 1597.

**Multipass:** first passes STB2005/2007/2009 are zfunc LESSEQUAL, zwrite on. Follow-ups are **ZFUNC
EQUAL, zwrite off**: STB2015 (sun specular, additive ONE/ONE), STB2016 (point specular, additive),
STB1009 (fog, SRCALPHA/INVSRCALPHA). So every pass must displace bit-identically — any variant left
stock z-fails on bent geometry. Leaves are single-pass (STLEAF001, alpha test ref 84).

**Sun shadow tree math is shared:** the leaf billboard and branch wind code lives in
`Shaders/Shadows/ShadowMapTree.hlsl`, used by `ShadowMap.vso` (per tree, with tree collision first;
`ShadowData.x == 2` leaves, `== 3` branch wind) and `ShadowMapTreeInstanced.vso` (batched dynamic-tree
composites, no collision: any tree that `TreeCollisionReaches` draws per tree). Change tree motion
there, never in one shader, or batched and single tree shadows split apart.

**Shrub collision registers:** c240-c242 = rows of `scale * rot` with world x/y in c240.w/c241.w,
c243 = (live flag, push / scale², flatten / scale², scale / bound); c244-c245 =
`TESR_GrassTrailWindow` / `TESR_GrassTrailTime`; vertex sampler s0 = `TESR_GrassTrailShrub`. c240-c243
are declared in `ExtraShaders/Includes/TreeCollision.hlsl`; c244-c245 and the sampler are declared in
EACH top-level includer, because TextureManager parses `TESR_` sampler declarations from the top-level
source only (one declared in an include stays silently unbound; `Tools/Tests/Check-ShaderSamplerDecls.ps1`
catches it). The include nests `#include "../Grass/Includes/GrassTrail.hlsl"`, which D3DX resolves
from the TOP-LEVEL shader's folder ([[fxc-verify-shader-edits]]).

**How to apply:** the `[GrassOrderDbg]` one-frame capture (`Develop.LogShaders` key) is the ground
truth for which variants draw. Related: [[speedtree-property-not-pp-lighting]],
[[shader-pipeline-facts]], [[oblivion-pdb-symbols]].
