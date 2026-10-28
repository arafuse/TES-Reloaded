---
name: engine-wind-has-no-direction
description: "Oblivion has NO wind direction anywhere: grass sways on a hardcoded world +Y axis, SpeedTree wind matrices are zero-mean sin/cos wobbles in tree-local space; only wind SPEED exists (Sky+0xC0). Addresses for overriding both."
metadata:
  node_type: memory
  type: project
  originSessionId: 43d8c7eb-ca8c-4b8d-b6c4-e659d55005e6
  modified: 2026-10-28T00:02:14.984Z
---

Disassembly findings (2026-10-26), asked while scoping directional rain. You can't derive a wind
direction from vegetation animation, because the engine never has one. The Oblivion WTHR record
carries only `windSpeed` (Skyrim added a direction).

- **Speed:** `Sky::windSpeed` (+0xC0) is copied to 0xB2C670 in sub_53FF90 (0x540052). It drives both
  grass and trees. Our `ShaderConst.currentwindSpeed` is the same value lerped across transitions.
- **Grass `WindData` (c4)** is the global block 0xB460B0..BC. z (0xB460B8) = lerp(GrassWindMagnitudeMin
  0xB46064, Max 0xB46068, 0xB2C670), set in sub_7C78D0 at 0x7C7CC5. w (0xB460BC) = phase, in sub_7E8E10
  (TallGrassShader slot 10, 0xA92058): `fmod(t / 3600 x 2pi x Rate, 2pi)`, Rate = `[[property+0x9C]+0x30]`
  (esi at the call, sway cycles per game hour), t = `call [0xB42D78](0, 1)` at 0x7E90AE. 0xB42D78 is the
  engine clock sub_405290(bool frame, bool day): (1,0) = frame seconds 0xB33E9C (0 in menu mode, the
  TES_OnIdle world delta, which SpeedTree wind uses), (0,1) = GameHour x 3600. So vanilla grass sways
  at a speed proportional to the timescale and jumps at midnight.
  **x/y (0xB460B0/B4) are written by sub_7E6710 (0x7E6829/0x7E6833): the world vector (0,1,0) is
  multiplied by the inverse of the patch's world rotation×scale and normalised**, giving world NORTH in
  model space. The VS applies `sin(...) * WindData.z * WindData.xy`, a symmetric swing along the
  north–south axis with zero mean and no downwind lean.
- **SpeedTree:** sub_55E060 builds the 4 global wind matrices (`kWindMatrixes` 0xB467B8, via
  sub_7F1760 → D3DXMatrixRotationYawPitchRoll). The angles are `sin/cos(phase) × amplitude` (table
  0xB12538: 0.15/0.17, 0.25/0.15, …) × strength. That is a zero-mean wobble in each tree's own model
  space. The palette is shared by all trees, so a world-space lean can't be put in the matrices.

**Built (2026-10-26), `TESReloaded/Core/Wind.cpp`:** we invent the direction and push it into
everything that should agree.
- `UpdateWind` writes `ShaderConst.WindData` (`TESR_WindData`) = (dir x, dir y, currentwindSpeed, 0).
  Each new weather turns the heading by up to ±90°, chased over real time (τ 20 s), with ±0.25 rad of
  game-hour drift.
- Grass: `CreateWindHook` rel-calls 0x7E6807 (the D3DXVec3TransformNormal of (0,1,0) in sub_7E6710,
  TallGrassShader vtable slot 12 at 0xA92060) and substitutes the wind axis. The downwind lean is
  `Grass/Includes/GrassWind.hlsl` (`sin*0.6 + 0.5`), hardcoded because grass has no free constant
  ([[grass-vs-constant-budget]]).
- Rain.fx and Snow.fx rotate the view ray by `WindSlantRotation` (`Precipitations/PrecipitationWind.hlsl`,
  Rodrigues: wind-tilted axis onto +Z, folded into the preshader) before `cylindrical()`. Tilt =
  `TESR_PrecipitationSlant` (x = `[Rain] Slant`, y = `[Snow] Slant`) × wind speed.
- Occlusion matches the slant: `ShadowManager::SetupOrthoMapMatrices` renders the ortho map top-down,
  SHEARED by drift = slant × speed × wind dir (not rotated), so the texel grid stays world-aligned and
  zero wind is exactly vertical. The old map used a near-vertical (0.05,0.05,1) lookAt to dodge the
  up=Z degeneracy, which biased occlusion about 4° toward NE. There is one map for all precipitation:
  `GetOrthoShear` blends the rain and snow slants by RainData.x/SnowData.x, and SnowAccumulation's
  per-frame mask follows the snow slant, so clear patches slide if the wind turns after snowfall.
- OBSE `IsRainHittingPlayer` (`RainProbe.cpp`, `Precipitations/RainProbe.fx`): an 8-point GPU probe of
  the ortho map, drawn right after RainEffect in RenderEffects and read back via an event-query ring
  (the SunShadowStealth pattern). Points are pushed up the rain fall line out of a body capsule. It is
  registered LAST in AddCommands (opcodes come from base 0x3000 in registration order, so append only).
- OBSE `GetWeatherWindDirection` returns `GetWindHeading()`: the heading the wind blows TOWARD, in
  degrees like GetAngle Z (`atan2(x, y)`, 0 = north, 90 = east). That is not the meteorological
  "from" convention.
- Grass sway clock (`CreateWindHook`): the 0x7E90AE call (6 bytes, NOP the 6th) goes to a naked thunk
  that passes esi; it returns a double-precision clock wrapped to one cycle of that Rate. The clock
  advances in `UpdateWind` by frame seconds x 30 (reference timescale) x Grass.ini `SwaySpeed`.
- Not done: trees (would need the per-tree hooks in [[speedtree-shader-variants]], displaced
  identically in every multipass variant).
