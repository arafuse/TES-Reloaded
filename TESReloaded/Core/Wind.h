#pragma once

/// World-space wind shared by rain and grass so they agree on direction. Oblivion only stores a wind
/// speed per weather, never a direction, so the direction is chosen here: each new weather picks a
/// heading up to 90 degrees from the last, the wind turns toward it over real time, and it drifts
/// slowly with the game clock.

/// Points the engine's grass sway axis (TallGrassShader, hardcoded world north) at the wind. Always
/// installed.
void CreateWindHook();

/// Advances the wind and writes it to ShaderConst.WindData as (direction x, direction y, speed 0-1, 0),
/// where the direction is the unit horizontal vector the wind blows toward. Main thread, once per frame
/// in exteriors, after the weather wind speed is blended.
void UpdateWind(ShaderConstants& ShaderConst, TESWeather* CurrentWeather);

/// The direction the wind blows toward, in degrees measured like GetAngle Z (0 = north, 90 = east),
/// in [0, 360). 0 until the first exterior frame; interiors keep the last exterior value.
float GetWindHeading();
