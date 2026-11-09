---
name: d3d9-gpu-readback-stall
description: MEASURED - never LockRect GPU results on DXVK (it ignores DONOTWAIT and waits for the copy's command list, ~2.5 ms worst frame; a LOCKABLE RT drained the whole frame, ~6 ms); read small probe results as occlusion-query pixel counts instead (clip failing pixels, one query per answer)
metadata:
  type: project
---

**Read small GPU answers back as occlusion-query pixel counts, never by locking a surface.** Since
commit 89b5525 (2026-11-09) both probes do this (`SunShadowStealth.cpp`, `RainProbe.cpp`):
1. the probe shader `clip()`s every pixel whose test fails, so the surviving pixel count is the answer;
2. each answer gets its own `D3DQUERYTYPE_OCCLUSION` query and its own run of pixels, set with a
   per-answer `SetViewport` + `DrawPrimitive` (the EffectQuad's ±1 positions cover any viewport;
   `SetRenderTarget` resets the viewport when the old target is restored);
3. frames later, `GetData(..., 0)` on each query; any S_FALSE means "not yet, try next frame".
Keep a ring (3 slots) so a slot is never reused while pending.
- A fraction becomes thresholds: the sun probe gives each point 16 pixels, pixel j survives when
  visibility > (j+0.5)/16, and count/16 is the visibility (quantized to 1/16). The rain probe only
  needs "any exposed point", so one query per body and count > 0.
- Result: `Mid:ProbeRead` avg 0.067 → 0.002 ms, worst frame 2.58 → 0.004 ms (DXVK 3.1.1, 600 frames).
  Verified in game 2026-11-09: LogSunShadowStealth steps in sixteenths through a shade edge
  (1.00 → mixed 0.25-0.88 → 0.00) with the right scale, and rain occlusion looked correct.

Why DXVK makes this the only stall-free path (DXVK 3.1.1 source):
- `LockImage` unconditionally clears `D3DLOCK_DONOTWAIT` ("Games like Beyond Good and Evil break").
  For a SYSTEMMEM surface flagged by `GetRenderTargetData` it flushes, syncs the CS thread and waits
  until the command list holding the copy retires, which can be later than an event query after
  the copy signals. The sampler put the old ~2.5 ms worst frame there (d3d9 `+0x1595A3`,
  `SleepConditionVariableSRW`).
- `D3D9Query::GetData` never syncs; occlusion results come from `vkGetQueryPoolResults` without WAIT.
- Ending an EVENT query mid-scene is a strong flush hint (`ConsiderFlush`); ending an occlusion query
  is not, so the new path also drops a per-frame early submit.
- Nested occlusion queries are exact in DXVK (`restartQueries` splits segments, each query sums its
  own). The engine runs its own around some accumulator draws (CreateQuery type 9 at 0x7AA1AB,
  0x7AE409, 0x7AE927; Begin → draw → End per item, spin on GetData at 0x7AE8B0), so a probe drawn
  inside one adds its pixels to the engine's count, like our full-screen apply passes already do.

History: on native NVIDIA D3D9, locking a `Lockable = TRUE` render target drained everything queued so
far even after its event query signalled (5.93 ms/frame; fixed 2026-11-07 in f0edc0e with a
GetRenderTargetData → SYSTEMMEM copy). That copy path is what DXVK then stalled on.

**Why:** a stall like this does not show up as GPU time. It reads as a huge CPU bucket in
FrameProfiler and serializes CPU and GPU. The tell is a ~one-call-per-frame bucket costing
milliseconds while issuing trivial work, or a tiny average with a multi-ms max.

**How to apply:** any new GPU→CPU readback (probes, luminance, occlusion) follows steps 1–3. If it
truly needs float data, expect DXVK to block on the lock and budget for it. When a FrameProfiler
bucket is far too big for its work, add nested sub-buckets (`Buck_Mid*` in FrameProfiler.h shows
how) before guessing. Consumers: [[sun-shadow-probe-consumers]], [[wet-actors]].
