---
name: animation-transition-engine
description: "How ActorAnimData starts/stops sequences and picks blend times (PlaySequence 0x474530, StopSequence 0x470FC0, +0xC4 no-blend flag); the AnimationBlending hooks now live in ProtoPlugin"
metadata:
  type: reference
---

**Engine fact:** PlaySequence `0x474530` blends for max(KF `Blend:` key / 30, `fAnimationDefaultBlend`
0.1 s), divided by `fAnimationMult` at `0x4748D3`, then crossfades or blends from the pose.
StopSequence `0x470FC0` (thiscall: slot, easeOut) has 59 call references that nearly all pass 0, so
overlays snap back. The ActorAnimData `+0xC4` byte marks the next start as a deliberate no-blend
(resets, AI package changes, knockdown). `animSequences[5]` is at +0xA0.

**The hooks live in ProtoPlugin** ([[protoplugin-sibling]]): `ProtoPlugin/AnimationBlending.cpp`,
`[Animation]` in `ProtoPlugin.ini` (`fMinBlendTime` 0.25, `fActionMinBlendTime` 0.1,
`fStopBlendTime` 0.2, `bLogTransitions`). The full write-up (blend selection, slot fan-out,
instant-stop call sites) is ProtoPlugin's `memory/module-animationblending.md`. OR's
`[AnimationBlending]` section and `[Develop] LogAnimationBlending` are gone; leftover lines in an old
INI are ignored.

**How to apply:** never patch `0x4748D3`-`0x4748E1` or the entry of `0x470FC0` from OR.

Related: [[skinning-bone-matrix-cache]], [[sneak-detection-formula]].
