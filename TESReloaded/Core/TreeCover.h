#pragma once

/// Foliage cover for sneak detection: while the player sneaks inside small SpeedTree trees and shrubs, or
/// in tall grass (GrassCover.h), the light level fed to Calc_DetectionLevel is scaled down, and observers
/// whose sight line crosses enough of that foliage lose line of sight. Oblivion only.

/// Patches the only call to Calc_DetectionLevel and the detection call to the actor LOS test. Always
/// installed; gated at runtime on SettingsGrass.TreeCover, GrassCover, TreeCoverBlockLOS and
/// GrassCoverBlockLOS.
void CreateTreeCoverHook();

/// Rescans nearby small trees and publishes the player's cover and the shrubs containing them for the
/// detection hooks. Main thread, once per frame.
void UpdateTreeCover();
