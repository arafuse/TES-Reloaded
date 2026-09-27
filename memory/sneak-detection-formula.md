---
name: sneak-detection-formula
description: "RE'd sneak detection: Actor_GetDetectionLevel 0x5F6540 → single call to cdecl Calc_DetectionLevel 0x5463F0 at 0x5F68DB; full 16-arg map; LOS call at 0x5F6647 wrapped in TreeCover.cpp (live-tested 2026-09-25); TreeCover.cpp scales the LIGHT arg 5 (implemented, live-tested), chameleon arg 6 is an unused alternative lever; threaded AI caveat"
metadata:
  node_type: memory
  type: project
  originSessionId: 5774dcbb-bbee-4a7a-8aac-e9b8b163f7e7
  modified: 2026-09-24T01:02:55.558Z
---

Statically RE'd 2026-09-23 (capstone + [[oblivion-pdb-symbols]]) for the shrub-cover sneak feature.
Arg layout disassembly-verified and the hook live-tested 2026-09-25 (tree cover feature; design in
docs/superpowers/specs/2026-09-24-tree-cover-sneak-design.md).

**`Actor_GetDetectionLevel` 0x5F6540** — thiscall, `this` = observer (kept in `ebx`), target
Actor* = 2nd stack arg (kept in `ebp`, never reassigned), 3rd arg = out byte (detection state).
Returns the cached level from the detection list unless forced. Invisibility > 0 or chameleon ≥ 100
short-circuit to -100 before the formula. 12 callers (AI_GetDetected, Actor_MagicHit, the detection
update in sub_5F7900/5F7A80, …) — all funnel through it.

**`Calc_DetectionLevel` 0x5463F0** — pure cdecl over scalars, NO actor pointers, exactly ONE
caller: `call` at **0x5F68DB** (`add esp, 0x40` after). Args (arg0 pushed last):
0 observer sneak (luck-modified) · 1 target sneak · 2 hasLOS (sub_5F2820 Havok LOS) ·
3 distance (float) · 4 observer Blindness (AV 0x2D) · 5 target light level (process vfunc 0x3AC,
night-eye applied) · 6 target Chameleon (AV 0x2E) · 7 boot weight · 8 target moving ·
9 target sneaking · 10 target attacking (bypasses max range) · 11 in combat · 12 running ·
13 swimming · 14 observer asleep · 15 exterior (fSneakExteriorDistanceMult). Arg 7 (boot weight)
comes from `sub_5F3B50`; arg 9 (target sneaking) comes from `sub_5E0550`.

Light term = (light + fDetectionSneakLightMod) × LOS-mult × (100−blind)/100 × (100−chameleon)/100
× fSneakLightMult; the sound term (boots, running) is separate. Raising arg 6 hides the target
VISUALLY only, an alternative lever for foliage concealment (cham' = 100 − (100−cham)(1−cover)); the
tree cover feature scales light arg 5 instead, per the user's decision.

**Hook shape:** `WriteRelCall(0x5F68DB, stub)` — naked stub: `pushad`; `lea eax,[esp+0x24]` (arg 0);
`push eax`; `push ebp` (target); `call AdjustDetectionLight` (`__stdcall`, `ret 8`); `popad`; `jmp
0x5463F0`. Does NOT push `ebx`. No Detour needed.

**Threading:** `bUseThreadedAI=1` in the user's Oblivion.ini, and which thread runs detection is
UNVERIFIED — the adjuster must not walk cells or the scene graph; read a main-thread snapshot.
Render-side tree data ([[speedtree-shader-variants]]) is frustum-only, so it can't serve as the
shrub source.

**Verified 2026-09-25 (`TESReloaded/Core/TreeCover.cpp`):** arg 5 (light) is an INT 0–100 — `ftol`
of process vfunc 0x3AC, night-eye scaled and clamped at 0x5F6611–0x5F6629, stored at frame
`[esp+0x24]`. Arg 9 (target sneaking) is a BYTE at frame `+0x3C`, set from bool `sub_5E0550` (the
is-sneaking test: movement flags `& 0x400` and not `& 0x800`) at 0x5F6736/0x5F6747, forced to 0 at
0x5F679A when the caller's flag at `[esp+0x128]` is set; the upper three bytes of the pushed dword
are stale stack, so only the low byte is valid and the `(UInt8)` cast is required. `sub_5F3B50` is
the BOOT WEIGHT term, arg 7 (frame `+0x38`, 0x5F669E): 0 at Journeyman Sneak, else the equipped
boots' weight, 5 when the global at `0xB333B8` is set. Process vfunc 0x2C0 = `GetMovementFlags`
(matches Game.h; see Hook shape above for the confirmed stub). In-game, light 52 → 13 at full cover
(reduction 0.75).

**LOS (verified 2026-09-25, for tree cover LOS blocking):** `sub_5F2820` is the general actor LOS
test (FOV checks + Havok ray), thiscall `(1, target, 1, &reason, 0)`, `ret 0x14`, 12 callers; the
detection one is the `call` at **0x5F6647**. Its byte lands at frame `[esp+0x2c]` and feeds arg 2, the
out byte `[esi]` (0x5F6693) and the observer's detection-list update (process vfunc 0xA8 at
0x5F6950) — so wrapping that one call changes all three consistently. In the formula LOS=0 ZEROES the
visual term and scales the sound term by `fSneakSoundLosMult` (0xB36718). Other formula globals:
fSneakMaxDistance 0xB36708, fSneakExteriorDistanceMult 0xB36748, fSneakLightMult 0xB36738,
fSneakSoundsMult 0xB36740, fSneakRunningMult 0xB36720, fSneakTargetInCombatBonus 0xB366E8,
fSneakSkillMult 0xB36710, fSneakBaseValue 0xB36700. Inside the callee, arg n is at `[esp+0x14+4n]`. Wrapped in TreeCover.cpp (DetectionLineOfSightHook, __fastcall + ThisCall to the original); live-tested 2026-09-25 with hostile NPCs, TreeCoverLOSDepth = 64.

**Tree refs:** a TREE ref's root node (`Ref->GetNode()`) IS the `BSTreeNode` (vtable 0xA65854) —
~700–760 tree refs in a loaded exterior grid, never a wrapper. Shrub bounds ~185–450 radius were
seen in the play-test area, consistent with the wider 190–617 range measured in
[[speedtree-shader-variants]]; bounds are centred near the ground, so the shader's `(height/R)²`
bend is ~7% at crouched-torso height, so the cover ring is measured at full (crown) bend.
