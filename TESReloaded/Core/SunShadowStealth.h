#pragma once

/// Sun shadow stealth: while the player stands in exterior sun (or moon) shadow, the sun's share of
/// their light level is scaled down by as much as the shadow visibly darkens them, measured by a GPU
/// probe of the sun shadow maps at points on the player's body. Oblivion only.

/// Wraps the sun term in HighProcess_GetLightLevel (0x655FE0). Always installed; the scale it applies
/// stays 1.0 unless SettingsShadows.Exteriors.SunShadowStealth is on and the sun maps are probed.
void CreateSunShadowStealthHook();

/// Gates the feature, creates or releases the probe resources, and publishes 1.0 whenever the sun
/// maps can't be probed. Main thread, from ShaderManager::UpdateConstants.
void UpdateSunShadowStealth();

/// Publishes the scale from any finished probe, then renders this frame's probe. Main thread, from
/// RenderShadowsMidScene straight after the sun shadow apply, with that apply's device state bound.
/// Changes render target 0; the caller restores it.
void RenderSunShadowProbe(IDirect3DDevice9* Device);
