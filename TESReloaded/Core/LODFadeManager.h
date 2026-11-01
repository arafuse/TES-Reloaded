#pragma once

#include <unordered_map>
#include <unordered_set>

/// Dithers newly streamed distant LOD in at the far edge of the distant grid.
///
/// Update() diffs DistantRefLOD's children once per frame and starts a timed fade for each far-edge
/// arrival. The draw hook asks GetFadeRemaining() for every covered draw and uploads the answer as
/// TESR_GEOM_FadeParams.x, which LODFade.hlsl turns into a screen-space dither clip.
/// Nothing here reference-counts or writes engine objects: roots are raw pointers, trusted only while
/// they are in the current frame's child set.
class LODFadeManager {
public:
	LODFadeManager();

	/// Once per frame, before any draw: resolves DistantRefLOD, diffs its children, starts and retires fades.
	void		Update();

	/// Remaining fade for a drawn geometry: 1 - alpha under a live fade root, otherwise 0 (opaque).
	float		GetFadeRemaining(NiAVObject* Geometry);

	/// Counts one draw by shader pair when Geometry sits under DistantRefLOD. Called only while CensusArmed.
	void		NoteCensusDraw(NiAVObject* Geometry, const char* PixelShaderName, const char* VertexShaderName, bool PixelHasProgram);

	/// Per-frame dither seed in [0, 1).
	float		DitherSeed;

	/// True while the one-shot shader census is collecting (Develop.LogLODFade only).
	bool		CensusArmed;

private:
	struct FadeRecord {
		NiAVObject*	Root;
		double		StartMs;
	};

	struct CensusEntry {
		const char*	PixelShader;
		const char*	VertexShader;
		bool		PixelHasProgram;
		bool		Tree;
		UInt32		Draws;
	};

	static const UInt32 kCensusMax = 64;

	static bool	IsNiNodeType(NiAVObject* Object);
	NiNode*		ResolveDistantRef();
	void		Reset();
	void		PollDistantRef();
	void		DropDeparted(const std::unordered_set<NiAVObject*>& Live);
	void		AddArrival(NiAVObject* Node);
	bool		IsReady(NiAVObject* Node);
	void		TryStartFade(NiAVObject* Node);
	void		DrainPending();
	void		RetireFinished();
	void		RebuildIndex();
	float		GetAlpha(const FadeRecord& Record) const;
	void		ReportCensus();

	std::vector<FadeRecord>						Fades;
	std::unordered_map<NiAVObject*, UInt32>		RootIndex;		// fade root -> index into Fades
	std::unordered_map<NiAVObject*, int>		GeometryCache;	// drawn geometry -> index into Fades, -1 = none
	std::unordered_set<NiAVObject*>				LiveChildren;	// DistantRefLOD's children as of the last poll
	std::unordered_set<NiAVObject*>				ScanChildren;	// scratch set for the current poll
	std::unordered_map<NiAVObject*, double>		Pending;		// unready arrival -> first-seen ms
	NiNode*										DistantRef;
	bool										PrevValid;
	bool										ResolveLogged;
	bool										MissingLogged;
	double										NowMs;

	CensusEntry									Census[kCensusMax];
	UInt32										CensusCount;
	UInt32										CensusOverflow;
	double										CensusStartMs;
};
