#pragma once

/// Tree cover for sneak detection: while the player sneaks inside small SpeedTree trees and shrubs,
/// the light level fed to Calc_DetectionLevel is scaled down. Oblivion only.

/// Patches the only call to Calc_DetectionLevel. Always installed; gated at runtime on
/// SettingsGrass.TreeCover.
void CreateTreeCoverHook();

/// Rescans nearby small trees and publishes the player's cover for the detection hook. Main thread,
/// once per frame.
void UpdateTreeCover();
