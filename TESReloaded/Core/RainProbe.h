#pragma once

/// Rain exposure from the precipitation occlusion map. While rain falls, a GPU probe tests points on
/// the player's body, and on up to RainProbeMaxActors nearby NPCs, each pushed out along the
/// wind-slanted fall line to the body's surface, so the answer matches the rain that is drawn.
/// Oblivion only. Answers trail rendering by a frame or two.

/// Most NPCs the probe tests in one frame, besides the player. RainProbe.fx.hlsl sizes its point
/// array for this (8 player points + 2 per NPC = 56).
static const int RainProbeMaxActors = 24;

/// One NPC for the probe to test: its reference ID, foot position and scale.
struct RainProbeActor {
	UInt32		RefID;
	NiPoint3	Position;
	float		Scale;
};

/// Arms or disarms the probe for this frame, creating or releasing its resources while
/// SettingsPrecipitations.Rain.IsRainHittingPlayer or WetActors.Enabled is on, and publishes "not
/// exposed" whenever rain can't reach anyone. Main thread, from ShaderManager::UpdateConstants.
void UpdateRainProbe();

/// Publishes the result of any finished probe, then renders this frame's probe. Main thread, from
/// ShaderManager::RenderEffects straight after the rain effect, with the effect quad bound. Restores
/// render target 0 and the depth-stencil itself before returning.
void RenderRainProbe(IDirect3DDevice9* Device);

/// True if falling rain reached any probe point on the player at the last readback. False whenever
/// SettingsPrecipitations.Rain.IsRainHittingPlayer is off. For the OBSE command.
bool IsRainHittingPlayer();

/// True while rain is drawn this frame: the Precipitations effect is on with its rain effect loaded,
/// the player is in an exterior-like cell, and the weather's rain ramp is above 0.
bool IsRainFalling();

/// True once the armed probe has produced an answer since it was last discarded or failed, so its answers can be used.
bool IsRainProbeWorking();

/// The player's answer at the last readback, whatever the IsRainHittingPlayer switch says.
bool IsRainReachingPlayer();

/// Sets the NPCs the next probe tests, nearest first; only the first RainProbeMaxActors are used.
/// Main thread, before RenderRainProbe in the same frame.
void SetRainProbeActors(const RainProbeActor* Actors, int Count);

/// Drops pending probes and the last answers, so nothing measured before a load or new game is used.
/// Main thread, from the co-save load and new-game callbacks.
void ResetRainProbe();

/// Writes an NPC's answer from the last readback that tested it to *Exposed and returns true; returns
/// false if that readback did not test this NPC.
bool GetRainProbeActorExposure(UInt32 RefID, bool* Exposed);
