#pragma once

/// Sun shadow stealth: a GPU probe of the sun shadow maps at points on the player's body measures how
/// much of the player stands in exterior sun (or moon) shadow. Scripts read it via
/// IsPlayerInSunShadow, and with SettingsShadows.Exteriors.SunShadowStealth on, the sun's share of the
/// player's light level is scaled down by as much as the shadow visibly darkens them. Oblivion only.

/// Wraps the sun term in HighProcess_GetLightLevel (0x655FE0). Always installed; it leaves the term
/// alone unless SettingsShadows.Exteriors.SunShadowStealth is on and the sun maps are probed.
void CreateSunShadowStealthHook();

/// Arms the probe while the sun maps are live, creates its resources, and publishes the defaults
/// (scale 1.0, not in shadow) whenever the maps can't be probed. Main thread, from
/// ShaderManager::UpdateConstants.
void UpdateSunShadowStealth();

/// Publishes the scale from any finished probe, then renders this frame's probe. Main thread, from
/// RenderShadowsMidScene straight after the sun shadow apply, with that apply's device state bound.
/// Restores render target 0 and the depth-stencil itself before returning.
void RenderSunShadowProbe(IDirect3DDevice9* Device);

/// True if at least half of the player's probe points were in sun or moon shadow at the last
/// readback, which trails rendering by a frame or two. False whenever the sun maps can't be probed
/// (interiors, light below the horizon, post-process exterior shadows off).
bool IsPlayerInSunShadow();
