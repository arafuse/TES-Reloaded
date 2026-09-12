#pragma once

/// Enforces a minimum real-time weather transition (Main.WeatherMinTransitionTime seconds).
/// Wraps the engine's per-frame transition update and rate-limits Sky::weatherPercent, restoring
/// the previous weather when the engine snaps (ForceWeather, game-time jumps, low transDelta).
/// Save loads and teleports still snap. Each intervention is logged with its cause. Oblivion only.
void CreateWeatherSmoothingHook();
