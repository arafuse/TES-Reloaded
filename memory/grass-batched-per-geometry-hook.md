---
name: grass-batched-per-geometry-hook
description: Grass draws are BATCHED like SpeedTree - SetupShaderPrograms sees only a batch's first grass geometry; per-geometry grass constants must hook TallGrassShader instance staging 0x7E6A90
metadata:
  type: project
---

**Engine fact (static disasm of the grass batch driver `sub_7F7680`, 2026-11-01):** grass is
drawn by the same batched-driver family as SpeedTree ([[speedtree-shader-variants]]). For a batch's
FIRST geometry it calls `TallGrassShader` vtable (`0x00A9202C`) slots 10, 11, 12, 13, 15, then 14 =
SetupShaderPrograms (ret addr `0x7F799F`, the grass call site in [[grass-drawn-after-water]]). For
every LATER geometry with the same pass (`je 0x7F7D1B`) it skips all shader slots and calls only
`sub_7E6A90`.

`sub_7E6A90` = per-geometry instance staging: thiscall (`this` = TallGrassShader, arg1 = the
NiGeometry being drawn, arg2 = TallGrassShaderProperty*), `ret 8`. It memcpys the instance array
(`[prop+9Ch]+10h`, count `+0Ch`) into the shader. Its only other caller is TallGrassShader slot 11
(`0x7E8E10`), so EVERY grass geometry passes through it. Slot 13 (`0x7E6710`, grass wind/transform
setup) is NOT reached on the fast path.

**Symptom this caused:** per-draw grass constants uploaded from `TrackSetupShaderPrograms`
(groundcover classification, batch-relative collision sources) leaked from the batch's first geometry
to all the rest, so the result flipped with camera pitch (which changes what draws first).

**How to apply:** per-geometry grass data goes in the `GrassInstanceHook` Detour on `0x7E6A90`
(RenderHook.cpp), using `Geometry->m_worldTransform` (Oblivion NiAVObject `+0x64`; the driver passes a
copy of the same). Mirror the values into `ShaderConst` so the first geometry's later SetCT re-uploads
the same thing. `isGrass`/`TrackSetupShaderPrograms` is only good for per-BATCH data.
Related: [[grass-vs-constant-budget]].
