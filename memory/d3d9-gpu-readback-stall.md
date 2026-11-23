---
name: d3d9-gpu-readback-stall
description: Never LockRect GPU results (DXVK ignores DONOTWAIT and waits for the copy's command list, ~2.5 ms worst frame; on native NVIDIA a LOCKABLE RT drains the whole frame, ~6 ms); read small probe results as occlusion-query pixel counts instead (clip failing pixels, one query per answer)
metadata:
  type: project
---

**Read small GPU answers back as occlusion-query pixel counts, never by locking a surface.** Both
probes do this (`SunShadowStealth.cpp`, `RainProbe.cpp`), and so does [[occlusion-culling]]:
1. the probe shader `clip()`s every pixel whose test fails, so the surviving pixel count is the answer;
2. each answer gets its own `D3DQUERYTYPE_OCCLUSION` query and its own run of pixels, set with a
   per-answer `SetViewport` + `DrawPrimitive` (the EffectQuad's ±1 positions cover any viewport;
   `SetRenderTarget` resets the viewport when the old target is restored);
3. frames later, `GetData(..., 0)` on each query; any S_FALSE means "not yet, try next frame".
Keep a ring (3 slots) so a slot is never reused while pending.
- A fraction becomes thresholds: the sun probe gives each point 16 pixels, pixel j survives when
  visibility > (j+0.5)/16, and count/16 is the visibility (quantized to 1/16). The rain probe only
  needs "any exposed point", so one query per body and count > 0.
- Cost: `Mid:ProbeRead` ~0.002 ms avg, 0.004 ms worst frame (a sysmem LockRect path cost 0.067 avg,
  2.58 ms worst on DXVK 3.1.1).

Why the alternatives stall (DXVK 3.1.1 source):
- `LockImage` unconditionally clears `D3DLOCK_DONOTWAIT` ("Games like Beyond Good and Evil break").
  For a SYSTEMMEM surface flagged by `GetRenderTargetData` it flushes, syncs the CS thread and waits
  until the command list holding the copy retires, which can be later than an event query after
  the copy signals (d3d9 `+0x1595A3`, `SleepConditionVariableSRW`).
- On native NVIDIA D3D9, locking a `Lockable = TRUE` render target drains everything queued so far
  even after its event query signals (~5.9 ms/frame).
- `D3D9Query::GetData` never syncs; occlusion results come from `vkGetQueryPoolResults` without WAIT.
- Ending an EVENT query mid-scene is a strong flush hint (`ConsiderFlush`); ending an occlusion query
  is not.
- Nested occlusion queries are exact in DXVK (`restartQueries` splits segments, each query sums its
  own). The engine runs its own around some accumulator draws (CreateQuery type 9 at 0x7AA1AB,
  0x7AE409, 0x7AE927; Begin → draw → End per item, spin on GetData at 0x7AE8B0), so a probe drawn
  inside one adds its pixels to the engine's count, like our full-screen apply passes already do.

**Why:** a stall like this does not show up as GPU time. It reads as a huge CPU bucket in
FrameProfiler and serializes CPU and GPU. The tell is a ~one-call-per-frame bucket costing
milliseconds while issuing trivial work, or a tiny average with a multi-ms max.

**How to apply:** any new GPU→CPU readback (probes, luminance, occlusion) follows steps 1–3. If it
truly needs float data, expect DXVK to block on the lock and budget for it. When a FrameProfiler
bucket is far too big for its work, add nested sub-buckets (`Buck_Mid*` in FrameProfiler.h shows
how) before guessing. Consumers: [[sun-shadow-probe-consumers]], [[wet-actors]].
