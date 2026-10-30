#pragma once

/// Actor wetness for the wet-actors look (Oblivion only). Every tracked actor has a wetness from 0
/// (dry) to 1 (soaked), keyed by reference ID: it rises while rain reaches the actor, falls otherwise,
/// jumps to 1 while swimming, and falls by Precipitations.ini [WetActors] DryPerSkippedHour for every
/// game hour skipped by waiting, sleeping or fast travel. Base-pass actor shaders read it per draw.

/// One saved actor wetness, as stored in the co-save.
struct WetActorRecord {
	UInt32	RefID;
	float	Wetness;
};

/// Advances every actor's wetness by this frame's unpaused real time and skipped game hours, rescans
/// loaded NPCs about five times a second, hands the rain probe its NPCs, and rebuilds the draw lookup.
/// Main thread, once per frame, from ShaderManager::UpdateConstants after UpdateRainProbe.
void UpdateWetActors();

/// The wetness of the actor that owns this geometry, found by walking up its parents to an actor
/// root. 0 for geometry not under a wet actor, and whenever WetActors is off. Main thread, per draw.
float GetDrawWetness(NiAVObject* Geometry);

/// Appends every actor with a wetness above 0. For the co-save.
void GetWetActorRecords(std::vector<WetActorRecord>& Records);

/// Replaces all wetness with these records (NULL or 0 clears it) and restarts the clocks, so the jump
/// to a loaded save's game time is not counted as skipped hours. For the co-save.
void SetWetActorRecords(const WetActorRecord* Records, UInt32 Count);
