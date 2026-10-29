---
name: sun-shadow-probe-consumers
description: The SunShadowStealth GPU probe feeds TWO consumers (stealth light scaling and OBSE IsPlayerInSunShadow, opcode 0x3007); it runs whenever the sun maps are live, NOT only with the SunShadowStealth setting on; don't re-gate it on that setting
metadata:
  type: project
---

`SunShadowStealth.cpp` draws an 8-point probe of the exterior sun shadow maps (4 heights × 2 sides,
each pushed sunward out of a body capsule) in `RenderShadowsMidScene` right after the sun apply, and
reads it back through an event-query ring with no stall. Results trail rendering by 1–2 frames.

Since 2026-10-28 (commit b638891) it has two consumers:
- **Sun shadow stealth:** the sun-term hook at 0x6561FD scales the player's sun term by
  `lerp(1, Darkness, shadowed)`. The hook reads `[Exteriors] SunShadowStealth` live, and that setting
  gates ONLY this scaling.
- **OBSE `IsPlayerInSunShadow`:** returns 1 when the shadowed fraction is at least
  `kInShadowThreshold` (0.5). It ignores Darkness, so it answers "is the player geometrically in
  shade", even in weather where shadows are faint. It returns 0 whenever `SunMapsProbeable()` fails
  (interior, light below the horizon, `UsePostProcessing` off, missing `Shadows\ShadowProbe.fx`).
  It is registered after `GetWeatherWindDirection`, so it takes opcode 0x3007. Opcodes are assigned
  from base 0x3000 in registration order, so new commands must be appended.

**Why:** before b638891 the probe ran only with the setting on, so the command would have read 0
for every user with stealth off. The probe is one 8×1 draw per frame, so it now always runs.

**How to apply:** don't move the probe back behind the SunShadowStealth setting, and don't release
its resources when stealth is toggled off. The watchdog in `UpdateSunShadowStealth` resets both
results after ~30 frames without a readback, whether or not a slot is pending. Moon shadows count,
per [[moon-shadows-no-dusk-fade]]. The rain probe follows the same pattern
([[engine-wind-has-no-direction]]). Light-term RE: [[actor-light-level-formula]].
