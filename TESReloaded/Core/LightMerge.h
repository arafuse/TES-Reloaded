#pragma once

// --- Light cluster merge ([LightMerge]) ---------------------------------------------------------
// Groups of NPCs carrying the same light (guards with torches) stack overlapping lights: each one
// costs the engine a pass on every object it touches and, while it moves, a whole-scene walk
// (0x7C5F60) to rebuild its geometry list, and their sum lights surfaces unnaturally bright. This
// module merges each cluster of matching carried lights into one leader light at sqrt(N) brightness.
// Design: docs/superpowers/specs/2026-11-23-light-cluster-merge-design.md.

/// Installs the engine light-pass instrument (always) and the merge hooks. Called from Main.cpp when
/// [LightMerge] Enabled or [Develop] LightMergeDebug is set.
void CreateLightMergeHook();

namespace LightMerge {

	/// RenderHook::TrackRender entry, before OR's shadow maps and the engine cull: detects loads and
	/// cell changes and applies this frame's merge.
	void ApplyFrame();

	/// RenderHook::TrackRender exit: restores every light value ApplyFrame changed and publishes the
	/// frame's light counters (and the LightMergeDebug row).
	void RestoreFrame();

	/// True when the light is a fully absorbed follower this frame and is kept out of the engine's
	/// light assignment. Valid between ApplyFrame and RestoreFrame; false otherwise.
	bool IsHidden(const NiPointLight* Light);

	/// True when the light leads a merged cluster this frame; Out receives its real world position
	/// (the light itself sits at the cluster's centre until RestoreFrame).
	bool TruePosition(const NiPointLight* Light, NiPoint3* Out);

	/// How much further a cluster leader reaches than its own radius this frame; 0 for any other light.
	float ShadowReach(const NiPointLight* Light);
}
