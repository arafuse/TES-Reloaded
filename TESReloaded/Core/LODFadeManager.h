#pragma once
#include <unordered_map>

/// Tracks LOD and full-model load transitions and publishes a per-geometry dither fade alpha.
/// Detection is by polling the engine grid arrays; see
/// docs/superpowers/specs/2026-08-20-lod-dither-fade-design.md.
class LODFadeManager {
public:
	LODFadeManager();

	/// One in-flight transition. Root is the scene-graph node whose subtree fades. Every fade is a
	/// rising alpha; a departing node uses Invert so its coverage falls as the alpha rises, which is
	/// what makes it exactly complementary to a partner fade-in sharing the same StartTime. WasCulled
	/// records the cull flag Pin found so Unpin can restore the exact prior state rather than assume it.
	struct FadeRecord {
		NiAVObject*	Root;
		float		StartTime;
		bool		Pinned;
		bool		Invert;
		bool		WasCulled;
	};

	/// Advances all live fades and retires completed ones. Called once per frame.
	void			Update();

	/// Starts a fade. Returns NULL when the table is full, in which case the caller pops as before.
	FadeRecord*		AddFade(NiAVObject* Root);

	/// Fade fraction for a record: 0 to 1, rising. Invert is applied by the shader, not here.
	float			GetAlpha(FadeRecord* Record);

	/// True when at least one fade is in flight. Gates all per-draw work.
	bool			AnyFadesLive() { return LiveCount > 0; }

	/// Maps a drawn geometry to the fade it belongs to by walking m_parent to a registered root.
	/// The answer is cached for the duration of the fade episode, misses included.
	FadeRecord*		ResolveGeometry(NiAVObject* Geometry);

	/// Seed published in TESR_GEOM_FadeParams.y to animate the dither pattern per frame.
	float			DitherSeed;

	/// Set for one frame after the last fade retires, so every covered shader is reset to opaque.
	bool			FadeResetPending;

	/// True until TESR_GEOM_FadeParams has been written at least once. D3D9 seeds pixel shader float
	/// constants to ZERO, so an unwritten c110 gives alpha 0 and clips every pixel of every covered
	/// draw. The draw hook must publish opaque once even with the feature disabled.
	bool			NeedsOpaquePublish;

	/// Keeps a departing node alive and drawn for the fade duration by un-culling it and taking a
	/// reference. Safe to call only on a node a shadow-copy slot still holds a reference to, which is
	/// what guarantees the pointer is live here. Returns false if the node has already been detached
	/// from the graph, in which case the caller must not start a fade for it — re-attachment is a
	/// separate, conditional task.
	bool			Pin(FadeRecord* Record);

	/// Releases a pin, restoring the cull flag to the state Pin found it in (not unconditionally
	/// clearing it) and dropping the reference taken by Pin.
	void			Unpin(FadeRecord* Record);

private:
	std::vector<FadeRecord>	Fades;
	UInt32					LiveCount;
	float					CurrentTime;

	// INVARIANT: these three shadow copies OWN one reference to every non-NULL entry they hold. The
	// reference is taken when a slot is filled, not when a departure is later detected -- by then the
	// engine may already have dropped its last reference, and Pin would be reading freed memory.
	// Every path that overwrites or clears a slot must release exactly once; use AssignSlot and
	// ReleaseSlots rather than writing a slot directly.
	std::vector<NiAVObject*>							PrevDistant;
	std::vector<NiAVObject*>							PrevLandLOD;
	std::vector<NiAVObject*>							PrevCell;
	std::unordered_map<NiAVObject*, FadeRecord*>		RootIndex;
	std::unordered_map<NiAVObject*, FadeRecord*>		GeomCache;
	bool												PrevValid;
	bool												FadeSetDirty;

	void			PollDistantGrid();
	void			PollLandLOD();
	void			PollCellGrid();

	void			AssignSlot(NiAVObject*& Slot, NiAVObject* Node);
	void			ReleaseSlots(std::vector<NiAVObject*>& Slots);

	void			Retire(UInt32 Index);
};
