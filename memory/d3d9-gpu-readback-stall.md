---
name: d3d9-gpu-readback-stall
description: MEASURED - LockRect on a LOCKABLE render target drains the whole queued frame even after its event query signals (~6 ms/frame); read GPU results back via GetRenderTargetData into a SYSTEMMEM surface and lock that instead
metadata:
  type: project
---

Never read GPU results back by locking a render target created with `Lockable = TRUE`. The event
query passing and `D3DLOCK_DONOTWAIT` do not help: on the user's NVIDIA driver the lock still waited
for everything queued in the frame so far. The sun shadow probe's 8-pixel readback cost
**5.93 ms/frame** this way (`Mid:ProbeRead`, CPU frame 10.66 ms → 4.87 ms with it gone), and the rain
probe used the same pattern. Fixed 2026-11-07 (commit f0edc0e).

The pattern that measured stall-free (`SunShadowStealth.cpp`, `RainProbe.cpp`):
1. render into a NON-lockable `D3DPOOL_DEFAULT` render target;
2. `GetRenderTargetData(target, sysmemSurface)` right after the draw (same size/format,
   `CreateOffscreenPlainSurface(..., D3DPOOL_SYSTEMMEM, ...)`);
3. `Issue(D3DISSUE_END)` on an event query AFTER that copy;
4. frames later, once `GetData` returns S_OK, `LockRect` the SYSTEMMEM surface (READONLY | DONOTWAIT).
Keep a ring (3 slots) so a slot is never reused while pending.

**Why:** a stall like this does not show up as GPU time. It reads as a huge CPU bucket in
FrameProfiler and serializes CPU and GPU. The tell is a ~one-call-per-frame bucket costing
milliseconds while issuing trivial work.

**How to apply:** any new GPU→CPU readback (probes, luminance, occlusion) follows steps 1–4. When a
FrameProfiler bucket is far too big for its work, add nested sub-buckets
(`Buck_Mid*` in FrameProfiler.h shows how) before guessing. Consumers: [[sun-shadow-probe-consumers]],
[[wet-actors]].
