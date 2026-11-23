---
name: temporal-ao
description: "Temporal AO (AmbientOcclusionTemporal.fx) costs 0.665 vs legacy 0.394 ms GPU at 3072x1728 (+0.22 ms chain); TAA-style reprojection works as-is (prevClip.w is positive view depth); count frames per effect chain, never by the engine BeginFrame counter"
metadata:
  type: project
---

`AmbientOcclusion.ini` `TemporalEnabled` (per section, shipped on) swaps the legacy AO for
`AmbientOcclusionTemporal.fx`. That effect is the legacy estimator with per-pixel, per-frame noise
(IGN + an R2 offset of `frame % 64`, as in XeGTAO), accumulated over up to 32 frames in a ping-pong
history, then put through the legacy blur and combine. `ShaderManager::RenderAmbientOcclusionTemporal`
drives the `Compute` passes into its own targets; `Combine` runs through the effect chain. It is
independent of the TAA effect.

Verified in game (DXVK, 3072x1728):
- **Reprojection:** TAA.fx's reprojection (`getWorldPos` × `TESR_PrevWorldViewProjectionTransform`)
  works without changes. `prevClip.w` is positive view depth, so relative-depth rejection works.
  While moving, ~89% of pixels pass every history check; only the depth test fails, at disocclusions.
- **Effect orders:** accumulation saturates under both `RenderEffectsBeforeHdr` orders and both
  `EffectChainPingPong` paths. Menu toggles, load doors and fast travel neither crash nor show stale AO.
- **Cost:** `ProfileEffects = 1`, AO bucket ~0.665 ms temporal vs ~0.394 ms legacy; whole chain 1.72
  vs 1.51 ms. The effect profiler's "Effects" count reads +1 with temporal on, because it counts the
  compute passes and the combine as two effects.

**Frame index:** `ShaderManager::EffectChainFrame`, bumped once per `RenderEffects`. Never the engine
counter at `vertexBufferMgr+0x3C`: it steps ~36 per displayed frame, which resets the history every
frame ([[skinning-bone-matrix-cache]]). `EffectChainFrame` pauses during menus and loading screens,
so a wall-clock gap above 250 ms also resets the history. The persistent AO targets never clip: sky
writes 1 ([[discard-illegal-in-chained-effects]]).

Debugging tools:
- **Debug defines:** `#define viewhistory 1` (history frame count as grayscale) and `viewao`. Flip
  the define in the `.hlsl` itself: with `CompileShaders = 1` the game recompiles from source at
  startup, which overwrites an fxc `/D` build.
- Split-screen debug output read back from the OR screenshot ([[or-screenshot-dds-diagnostic]];
  post-HDR captures are 8-bit BGRA, not FP16).

Related: [[effect-quad-uv-and-aspect-bugs]] (AO's focal length uses the integer-divided aspect).
