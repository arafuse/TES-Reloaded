#pragma once

/// Grass cover for sneak detection: tall grass around the sneaking player lowers the light level fed to
/// Calc_DetectionLevel and can block observers' line of sight. TreeCover.cpp's detection hooks consume
/// it. Oblivion only.

/// Rescans the tall grass around the sneaking player and publishes their cover and the nearby clumps.
/// Main thread, once per frame.
void UpdateGrassCover();

/// Returns the sneaking player's grass cover this frame, 0-1; 0 when the scan is off. Any thread.
float GetPlayerGrassCover();

/// Returns the weighted depth of tall grass, world units, on the sight line from the eye point to the
/// player's torso this frame; 0 without a snapshot. Any thread.
float GetGrassSightDepth(float EyeX, float EyeY, float EyeZ);
