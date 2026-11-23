---
name: animation-transition-engine
description: "RE'd how ActorAnimData starts/stops sequences and picks blend times (PlaySequence 0x474530, StopSequence 0x470FC0, +0xC4 no-blend flag, fAnimationDefaultBlend); what AnimationBlending.cpp hooks"
metadata:
  type: reference
---

Static disassembly; the hook built on it is play-tested (the user found the defaults — 0.25 s start
floor, 0.2 s stop ease-out — a clear improvement).

- **ActorAnimData slots**: `animSequences[5]` at +0xA0. Slot arg 5 = all slots (fans out 4,0,1,2 then 3), 6 = slots 1-3; PlaySequence maps 5→0, 6→3.
- **PlaySequence 0x474530** (thiscall: newSeq, UInt16 groupKey, int slot; ret 0xC). Blend = `fAnimationDefaultBlend` (0xB06538, 0.1 s), or max(old, new TESAnimGroup+0x21 "Blend:" text key)/30 if set; menu mode → `fMenuModeAnimBlend` (0xB06540); +0xC4 flag → 0; then ÷ `fAnimationMult` (0xB06530) at 0x4748D3. Then: ≤0.01 → hard Activate; old seq still Animating → NiControllerManager CrossFade 0x6C4000; otherwise BlendFromPose 0x6C5C70 (the `__TempBlendSequence__`), except culled actors (hard, 0x470B20). An old seq NOT in state Animating (mid-transition) is first killed with fade 0, then the new one blends from the pose, so interrupted blends are still smooth.
- **StopSequence 0x470FC0** (thiscall: slot, easeOut; ret 8) → NiControllerSequence::Deactivate 0x6C9CB0, then clears the slot. ~55 callers, nearly all pass 0. Slot-5 stops are resets (TESObjectREFR_GenerateNiNode, EvaluatePackage, knockdown).
- **ActorAnimData +0xC4 byte** = "next sequence starts with no blend"; the engine sets it right after deliberate snaps (AI package reset 0x6021C2/0x6022B1, knockdown 0x655755...) and PlaySequence clears it at 0x474A76. Struct in Game.h still calls it `float unkC4`.
- TESAnimGroup +0x20 is parsed from the `m:` text key, +0x21 from `Blend:` (parser ~0x51B790). Group IDs 0-42 (table count 0x2B).
- A similar blend-out helper 0x4733A0 (group Blend key or default) has 4 callers, and it also routes through StopSequence.

**OR's hook** (`TESReloaded/Core/AnimationBlending.cpp`, INI `[AnimationBlending]`): an inline jump at 0x4748D3 floors nonzero blends; a Detour on 0x470FC0 eases out single-slot stops, skipping slot 5, the +0xC4 flag, the internal fan-out calls, and the PlaySequence/knockdown return addresses. Both sites are byte-checked first because Blockhead/OpenOBMovement are loaded too. `Develop.LogAnimationBlending` logs each change.

Related: [[skinning-bone-matrix-cache]], [[sneak-detection-formula]].
