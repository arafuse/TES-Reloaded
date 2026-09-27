---
name: moon-shadows-no-dusk-fade
description: Gameplay effects driven by sun shadows must NOT fade at dawn/dusk or night; the moon casts shadows (ShadowLightDir switches to MasserDir), so shade matters at any hour
metadata:
  type: feedback
---

Don't tie shadow-driven gameplay to the apply shader's `ShadowLightDir.w` contrast ramp (dawn/dusk
`SunAmount` ramp, then `MasserAmount` at night). The user corrected sun shadow stealth
(2026-09-27, commit 9652283) when its scale followed that ramp: "The effect should not fade out at
dusk because we have moonlight and moon shadows."

**Why:** with `[Main] DirectionalLightOverride = 1` (the default), the shadow light hands over from
the sun to Masser, and moon shadows are part of the intended night look. A stealth or visibility
effect that fades with `.w` silently disables itself at night.

**How to apply:** use the shadow Darkness (`ShaderConst.Shadow.Data.y`) directly, e.g.
`lerp(1, D, shadowed)`. Gate only on the maps being live (`ShadowBiasAdaptive.w`) and the light being
above the horizon (`ShadowLightDir.z > 0`), never on `.w`. See [[actor-light-level-formula]] and
[[weather-transition-engine]].
