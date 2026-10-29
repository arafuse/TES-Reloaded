---
name: sun-shadow-probe-consumers
description: The SunShadowStealth GPU probe feeds TWO consumers (stealth light scaling and OBSE IsPlayerInSunShadow, opcode 0x3007), each with its own Shadows.ini [Exteriors] switch; the probe runs while EITHER is on, and each consumer checks only its own
metadata:
  type: project
---

`SunShadowStealth.cpp` draws an 8-point probe of the exterior sun shadow maps (4 heights × 2 sides,
each pushed sunward out of a body capsule) in `RenderShadowsMidScene` right after the sun apply, and
reads it back through an event-query ring with no stall. Results trail rendering by 1–2 frames.

Since 2026-10-28 it has two consumers, each with its own `[Exteriors]` switch in Shadows.ini. The
probe runs (and holds its resources) while EITHER switch is on, and is released when both are off:
- **Sun shadow stealth** (`SunShadowStealth`): the sun-term hook at 0x6561FD scales the player's sun
  term by `lerp(1, Darkness, shadowed)`. The hook reads the switch live.
- **OBSE `IsPlayerInSunShadow`** (`IsPlayerInSunShadow`, default 0; off, the command returns 0 even if
  stealth keeps the probe running): returns 1 when the shadowed fraction is at least
  `kInShadowThreshold` (0.5). It ignores Darkness, so it answers "is the player geometrically in
  shade", even in weather where shadows are faint. It returns 0 whenever `SunMapsProbeable()` fails
  (interior, light below the horizon, `UsePostProcessing` off, missing `Shadows\ShadowProbe.fx`).
  It is registered after `GetWeatherWindDirection`, so it takes opcode 0x3007. Opcodes are assigned
  from base 0x3000 in registration order, so new commands must be appended.

**Why:** the user wanted both player probes (this one and `IsRainHittingPlayer`'s) opt-in, so they
cost nothing when no script needs them.

**How to apply:** a new consumer gets its own switch and joins `ProbeWanted()`; never gate one
consumer's output on another consumer's switch. The watchdog in `UpdateSunShadowStealth` resets both
results after ~30 frames without a readback, whether or not a slot is pending. Moon shadows count,
per [[moon-shadows-no-dusk-fade]]. The rain probe follows the same pattern
([[engine-wind-has-no-direction]]). Light-term RE: [[actor-light-level-formula]].
