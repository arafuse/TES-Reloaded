#pragma once
#include <unordered_map>

/// Tracks LOD and full-model load transitions and publishes a per-geometry dither fade alpha.
/// Detection is by polling the engine grid arrays; see
/// docs/superpowers/specs/2026-08-20-lod-dither-fade-design.md.
class LODFadeManager {
public:
	LODFadeManager();

	enum {
		FadeDir_In	= 0,
		FadeDir_Out	= 1,
	};

	/// One in-flight transition. Root is the scene-graph node whose subtree fades.
	struct FadeRecord {
		NiAVObject*	Root;
		UInt8		Direction;
		float		StartTime;
		bool		Pinned;
		bool		Invert;
	};

	/// Advances all live fades and retires completed ones. Called once per frame.
	void			Update();

	/// Starts a fade. Returns NULL when the table is full, in which case the caller pops as before.
	FadeRecord*		AddFade(NiAVObject* Root, UInt8 Direction);

	/// Fade fraction for a record: 0 to 1 for a fade-in, 1 to 0 for a fade-out.
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

private:
	std::vector<FadeRecord>	Fades;
	UInt32					LiveCount;
	float					CurrentTime;

	std::vector<NiAVObject*>							PrevDistant;
	std::vector<NiAVObject*>							PrevLandLOD;
	std::unordered_map<NiAVObject*, FadeRecord*>		RootIndex;
	std::unordered_map<NiAVObject*, FadeRecord*>		GeomCache;
	bool												PrevValid;
	bool												FadeSetDirty;

	void			PollDistantGrid();
	void			PollLandLOD();

	void			Retire(UInt32 Index);
};
