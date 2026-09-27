---
name: actor-light-level-formula
description: "RE'd actor light level (detection arg 5): HighProcess_GetLightLevel 0x655FE0 = clamp((Σ point-light falloff + T)×100); exterior T = unoccluded sun max(diffuse) added by ONE call at 0x6561FD (actor in edi); sun SSL (+0x118) is not in the SSN light list"
metadata:
  type: project
---

Statically RE'd 2026-09-27 for sun shadow stealth (spec
docs/superpowers/specs/2026-09-27-sun-shadow-stealth-design.md). Complements [[sneak-detection-formula]].

**`HighProcess_GetLightLevel` 0x655FE0**: process vfunc **0x3AC** on High/MiddleHigh (Low/MiddleLow/Base:
0x60D080), thiscall `(Actor*, 0)`, `ret 8`, float 0–100. Game.h's Oblivion block names slot 0xEA
`GetLightAmount`, which is off by one (0xEA×4 = 0x3A8).
- Point lights: the player path uses `sub_7C6570` (ShadowSceneNode `lights` list at +0xE4, first entry
  +0xE8); NPCs walk the process's own list at +0x184 (`sub_7ED160`/`sub_7ED180`). Each light is
  `sub_7D31B0(SSL, x, y, z, exclude)`, thiscall `ret 0x10`: `(1 − clamp(dist/radius)²) × max(diffuse rgb)`
  when SSL `+0xFC` is set (point light), else just `max(diffuse rgb)`. There is **no occlusion** anywhere.
- Then T: an interior (`TESObjectCELL_IsInterior`) adds max(ambient rgb) of the sun NiLight. An exterior adds
  `sub_7D31B0(SSN->directionalLight, pos, 0)` at **0x6561FD**, with `ecx` = sun SSL and **`edi` = the actor**.
- ×100 (0xA309F0), clamped to [0, 100]. The detection caller then applies night-eye and `ftol`.
- `ShadowSceneNode::directionalLight` +0x118 is set by `sub_7C5850` (new 0x220-byte SSL, `0x7D3400` binds
  the NiLight) and is NOT inserted into `lights`, so the sun is counted only at 0x6561FD.

Consumers of vfunc 0x3AC: 0x4F64A9 (`AI_GetDetected`), 0x5E8C3B, 0x5F65E2 (detection), 0x5FCD96 and
0x5FCE85 (`Actor_ProcessAction` attack/enchant), 0x60A8BE.

**How to apply:** to change how lit the player counts as, wrap 0x6561FD (sun term) or the detection arg
(see [[sneak-detection-formula]]). Wrapping at the source affects all six consumers.
