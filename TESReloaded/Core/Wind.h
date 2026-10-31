#pragma once

/// World-space wind shared by rain and grass so they agree on direction. Oblivion only stores a wind
/// speed per weather, never a direction, so the direction is chosen here: each new weather picks a
/// heading up to 90 degrees from the last, the wind turns toward it over real time, and it drifts
/// slowly with the game clock.

/// Points the engine's grass sway axis (TallGrassShader, hardcoded world north) at the wind, and drives
/// the grass sway phase from game frame time instead of the game clock, so the sway speed no longer
/// follows the timescale. Always installed.
void CreateWindHook();

/// Advances the wind and writes it to ShaderConst.WindData as (direction x, direction y, speed 0-1, 0),
/// where the direction is the unit horizontal vector the wind blows toward, and advances the grass sway
/// clock by the frame time (0 in menus) at the speed of timescale 30 times Grass.ini SwaySpeed. Main
/// thread, once per frame in exteriors, after the weather wind speed is blended.
void UpdateWind(ShaderConstants& ShaderConst, TESWeather* CurrentWeather);

/// The direction the wind blows toward, in degrees measured like GetAngle Z (0 = north, 90 = east),
/// in [0, 360). 0 until the first exterior frame; interiors keep the last exterior value.
float GetWindHeading();

/// The wind as stored in the OBSE co-save, so a loaded game resumes the wind it was saved with instead
/// of rolling a new one. Its layout is the record format: change it only with a new record version.
struct WindState {
	float	Heading;		///< Radians, the heading the wind is turning from
	float	Target;			///< Radians, the heading it is turning toward
	UInt32	WeatherRefID;	///< Weather the target was picked for, so reloading in it does not turn the wind
	float	DirectionX;		///< Last direction written to WindData, drift included, so interiors report it
	float	DirectionY;
};

/// Fills State and returns true once the wind has a direction; false before the first exterior frame.
bool GetWindState(WindState& State);

/// Restores a saved wind, with WeatherRefID already resolved against the current load order (0 if the
/// weather's plugin is gone, which turns the wind on the next exterior frame). NULL forgets the wind,
/// so the next exterior frame picks a fresh heading as on a first load.
void SetWindState(const WindState* State);
