---
name: light-cluster-merge
description: "LightMerge.cpp: groups of matching carried torches become one leader at sqrt(N) brightness; followers hidden by refusing them in 0x7D2EA0; NPC light level re-adds them; measured 34.4 → 21.8 ms in the guard-torch repro"
metadata:
  type: project
---

`TESReloaded/Core/LightMerge.{h,cpp}` + engine-free `LightMergeMath.h` (harness `Tools/Tests/Run-LightMergeMathTest.ps1`). INI `[LightMerge]` (Enabled, JoinDistance 192, LeaveDistance 256, FadeTime, BrightnessExponent 0.5, Color/RadiusTolerance); off by default.

- **Inputs**: scene-node point lights (`+0xFC`) whose source NiPointLight has `CanCarry`, refcount > 1, not a magic light, not under the player's nodes. Lights match on diffuse color and radius (the guard repro had a mod torch r 1280 and vanilla r 512, which never merge with each other).
- **Per frame** (RenderHook::TrackRender): `ApplyFrame` clusters (hysteresis, time-based fade) and writes Diff ×scale, and the leader's world pos (centroid) and Spec.r (+reach); `RestoreFrame` puts everything back, so the update phase and saves see real values. Depth counter: the save thumbnail re-enters the render.
- **Hiding**: fully absorbed followers are refused in the engine's light-reach test 0x7D2EA0 (see [[engine-light-attach-addresses]]). The hidden set lives from one ApplyFrame to the next (lock-free array, count written last) because the update-phase walk runs outside the render.
- **Gameplay**: the player's light level sums the scene node's own list (0x7C6570), so hidden torches still count. An NPC's sums the lights linked to its property (process +0x184), so a Detour on 0x655FE0 + a probe on 0x7D31B0 add each unsummed hidden follower's falloff back from a main-thread snapshot, then re-clamp. See [[actor-light-level-formula]]. Open: if threaded AI runs detection during the render, the player path can see a leader's render-time values.
- **Shadows**: ShadowManager casts a leader's point shadow from its real torch (`TruePosition`), widens the far plane by `ShadowReach`, skips `IsHidden` lights.
- **Measured** (10 guard torches after load, town, CPU-bound): unmerged 34.4 ms/frame; Join 128 → 4 lights, 24.6 ms; Join 192 → 3 lights, 21.8 ms; torches stowed ~10.5 ms. User confirmed visuals (brightness, no pop, no stale light, shadows).
- **Debug**: `[Develop] LightMergeDebug = 1` logs 300 frames after each load/cell change (per-frame costs, reach tests/refused, NPC light fixes) and a per-light dump every 30 frames; `= 2` also runs the sampler from load until the scene's lights go out.
