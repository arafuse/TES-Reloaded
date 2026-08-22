#pragma once
#include <unordered_map>
#include <unordered_set>

/// Tracks LOD and full-model load transitions and publishes a per-geometry dither fade alpha.
/// Detection is by polling the loaded-cell grid and the LOD scene-graph nodes; see
/// docs/superpowers/specs/2026-08-20-lod-dither-fade-design.md.
class LODFadeManager {
public:
	LODFadeManager();

	/// One in-flight transition. Root is the scene-graph node whose subtree fades. Every fade is a
	/// rising alpha; a departing node uses Invert so its coverage falls as the alpha rises, which is
	/// what makes it exactly complementary to a partner fade-in sharing the same StartTime. WasCulled
	/// records the cull flag Pin found so Unpin can restore the exact prior state rather than assume it.
	/// Holder is the plugin-owned node a re-attached departure was hung under, or NULL when the pin was
	/// a plain un-cull; Unpin uses it to decide whether a detach is owed. Tier is a string literal
	/// naming the poller that emitted the record, carried only so every log line can be attributed.
	struct FadeRecord {
		NiAVObject*	Root;
		float		StartTime;
		bool		Pinned;
		bool		Invert;
		bool		WasCulled;
		NiNode*		Holder;
		const char*	Tier;
	};

	/// Advances all live fades and retires completed ones. Called once per frame.
	void			Update();

	/// Starts a fade. Returns NULL when the table is full, in which case the caller pops as before.
	/// Tier must be a string literal; it is stored by pointer and used only for logging.
	FadeRecord*		AddFade(NiAVObject* Root, const char* Tier);

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

	/// DIAGNOSTIC ONLY -- no invariant depends on any of these five and nothing but logging reads them.
	/// Per-frame draw-path counters, incremented by the LODFade block in
	/// RenderHook::TrackSetupShaderPrograms and reset at the top of Update() where the line is emitted.
	/// DrawCovered counts every draw whose pixel shader declares TESR_GEOM_FadeParams, before any other
	/// condition; DrawGated those that passed the full gate; DrawResolved those ResolveGeometry matched.
	/// DrawMinAlpha is the smallest Params.x actually published, starting at 2.0 so "nothing published"
	/// is distinguishable from "published 1.0". ResolveMissWanted is the arming flag for the one-shot
	/// ancestry sample; the draw hook tests it inline so a missed draw costs one bool load.
	UInt32			DrawCovered;
	UInt32			DrawGated;
	UInt32			DrawResolved;
	float			DrawMinAlpha;
	bool			ResolveMissWanted;

	/// DIAGNOSTIC ONLY. Captures one sample of the m_parent walk from a geometry that failed to resolve,
	/// as plain values so Update() can print it without dereferencing pointers later. Call only when
	/// ResolveMissWanted is true; it disarms itself, so the walk runs at most once per frame.
	void			NoteResolveMiss(NiAVObject* Geometry);

	/// DIAGNOSTIC ONLY. Records one covered draw against the pixel shader that issued it, and for the
	/// first draw of each distinct shader captures that geometry's m_parent chain to the top of the
	/// graph, by name and address. Answers the one question the counters cannot: WHICH shaders the
	/// covered draws belong to, and WHERE in the scene graph their geometry lives.
	/// Only ever called while CoveredCensusWanted is true.
	void			NoteCoveredDraw(const char* ShaderName, NiAVObject* Geometry, bool Resolved);

	/// DIAGNOSTIC ONLY. False once the per-shader covered-draw census has printed. The draw hook tests
	/// it inline, so after the single report every covered draw pays one bool load and no call.
	bool			CoveredCensusWanted;

	/// Non-mutating test for whether Pin would succeed, so a departure that cannot be held never gets a
	/// fade record at all. Parent is the node the departing node was attached to when the shadow copy
	/// last observed it. Logs the decline, which is the diagnostic this feature is measured by.
	bool			CanPin(NiAVObject* Node, NiNode* Parent, const char* Tier);

	/// Keeps a departing node alive and drawn for the fade duration. A node still in the graph is
	/// simply un-culled; a node the engine has already detached -- which is what every departure turns
	/// out to be -- is re-attached under a plugin-owned holder hanging off Parent, the node it was last
	/// observed under, so it keeps rendering in exactly the context it always did. Safe to call only on
	/// a node a shadow-copy slot still holds a reference to, which is what guarantees the pointer is
	/// live here. Returns false without touching anything if neither path is available.
	bool			Pin(FadeRecord* Record, NiNode* Parent);

	/// Releases a pin, detaching the node from its holder if it had one, restoring the cull flag to the
	/// state Pin found it in (not unconditionally clearing it) and dropping the reference taken by Pin.
	void			Unpin(FadeRecord* Record);

private:
	std::vector<FadeRecord>	Fades;
	UInt32					LiveCount;
	float					CurrentTime;

	// One shadow-copy entry: a remembered node plus the parent it was attached to when last observed.
	// The parent is captured while the node is still IN the graph, because by the time a departure is
	// detected the engine has already NULLed m_parent and the original parent is otherwise
	// unrecoverable.
	//
	// Used by the LandLOD tier alone, and Parent carries NO reference. AssignSlot refreshes it on
	// EVERY poll, so it is never more than one poll old. That tier observes its nodes through a child
	// array, so a node's presence in the poll and a live m_parent are the same fact, and a container
	// that dies takes all its children with it -- mass churn the discontinuity guard suppresses before
	// any pin runs. Refcount traffic that high in the graph would be risk for nothing.
	struct SlotEntry {
		NiAVObject*	Node;
		NiNode*		Parent;
	};

	// INVARIANT: these two shadow copies OWN one reference to every non-NULL node they hold. The
	// reference is taken when a slot is filled, not when a departure is later detected -- by then the
	// engine may already have dropped its last reference, and Pin would be reading freed memory.
	// Every path that overwrites or clears a slot must release exactly once; use AssignSlot,
	// ResyncSlots and ReleaseSlots rather than writing a slot directly. SlotEntry::Parent is NOT
	// covered by it and owns nothing; see SlotEntry.
	//
	// PrevDistant is keyed by NODE, not by index: DistantRefLOD's child array is compacted and its
	// slots reused as distant cells stream, so an index-keyed diff reports every slot changed on every
	// frame. Membership diffing is immune to reordering and compaction. The mapped value is the
	// remembered parent, which is the same role SlotEntry::Parent plays for the vector.
	std::unordered_map<NiAVObject*, NiNode*>			PrevDistant;
	std::vector<SlotEntry>								PrevLandLOD;
	std::unordered_map<NiAVObject*, FadeRecord*>		RootIndex;
	std::unordered_map<NiAVObject*, FadeRecord*>		GeomCache;
	bool												PrevValid;
	bool												FadeSetDirty;

	// Per-frame scratch for the distant membership diff. Owns NO references -- ownership moves into
	// PrevDistant via ResyncSlots, which SWAPS rather than copies, so on return this holds the previous
	// frame's stale membership and is rebuilt from scratch next poll. Never pass it to ReleaseSlots.
	// Kept as a member so the swap recycles both maps' buckets instead of reallocating ~2075 nodes.
	std::unordered_map<NiAVObject*, NiNode*>			CurDistant;

	// The loaded-cell tier, keyed by CELL and mapped to the container node that currently holds it.
	//
	// Keyed by cell because CellInfo::niNode is a PERSISTENT PER-SLOT CONTAINER, not a per-cell node:
	// measured in game, all 25 GridEntry::cell pointers change on a boundary crossing while every
	// niNode and every CellInfo* stays identical. A niNode-keyed diff therefore watches a field that
	// structurally cannot change and detects nothing, which is exactly what four sessions showed. The
	// grid also re-indexes wholesale rather than shifting a row, so a slot-keyed diff is useless here
	// too; set membership over cells is immune to the re-index by construction.
	//
	// PrevCellSet OWNS one reference on the container in each entry -- the mapped value, never the
	// key -- on the same contract as the shadow copies above, so a container cannot be freed while a
	// fade names it. Move it only through ResyncCells and ReleaseCells. The TESObjectCELL* key is an
	// opaque identity token: it is compared and never dereferenced, and no reference is taken on it.
	//
	// CurCellSet is the per-frame scratch, with the same swap discipline (and the same warning) as
	// CurDistant: after ResyncCells it holds the previous poll's stale membership and owns nothing.
	std::unordered_map<TESObjectCELL*, NiNode*>			PrevCellSet;
	std::unordered_map<TESObjectCELL*, NiNode*>			CurCellSet;

	// Plugin-owned holder nodes, keyed by the original parent each one hangs under. Keyed rather than
	// global because render context follows position in the graph: a distant LOD node under
	// DistantRefLOD binds the DISTLOD shaders and a LandLOD quadrant under LandLOD binds the terrain
	// LOD shaders, so a single shared holder would make one of the two draw wrongly or not at all.
	//
	// Holders are never destroyed. A pinned node is a child of its holder for the duration of a fade,
	// so releasing a holder a live pin still points at would be a use-after-free in Unpin. HolderNodes
	// keeps every holder ever created, both as the O(1) exclusion test the pollers need and as the
	// owner of the reference that keeps those pointers from dangling.
	std::unordered_map<NiNode*, NiNode*>				Holders;
	std::unordered_set<NiAVObject*>						HolderNodes;

	// Cached DistantRefLOD node, revalidated against the live child list every poll so a stale pointer
	// is never dereferenced. DistantRefLogged keeps the resolve diagnostic to a single line.
	NiNode*												DistantRef;
	bool												DistantRefLogged;

	// Latches for the one-shot cell-grid and LandLOD population diagnostics in PollCellGrid and
	// PollLandLOD, each printing once per session to prove their node pointers are actually non-NULL.
	bool												CellGridLogged;
	bool												LandLODLogged;

	// DIAGNOSTIC ONLY: the captured resolve-miss sample and its latches. Stored as plain values, never
	// as pointers to dereference, so printing it a frame later is safe.
	bool												ResolveMissPending;
	bool												ResolveMissLogged;
	UInt32												MissGeom;
	UInt32												MissParents[3];
	UInt32												MissDepth;
	float												LastDrawLogTime;

	// DIAGNOSTIC ONLY: one-shot subtree census latches, one per tier, so the bounded read-only walk in
	// RunRootCensus runs at most once per tier per session. The three tiers pick their roots by
	// completely different routes, so a per-tier latch separates "universal" from "tier-specific".
	bool												CensusDistantLogged;
	bool												CensusCellLogged;
	bool												CensusLandLODLogged;

	// DIAGNOSTIC ONLY: the per-shader covered-draw census. Name is the ShaderRecord's own string
	// pointer and rows are matched by pointer alone -- two records sharing a spelling merely produce
	// two rows, which changes nothing about the read-off. Chain is the ancestry of the FIRST geometry
	// seen for that shader, captured at draw time while the pointers are guaranteed live and stored as
	// text so printing it a frame later dereferences nothing.
	struct CoveredShaderEntry {
		const char*	Name;
		UInt32		Draws;
		UInt32		Resolved;
		char		Chain[320];
	};
	static const UInt32								kCoveredShaderMax = 24;
	CoveredShaderEntry								CoveredShaders[kCoveredShaderMax];
	UInt32											CoveredShaderCount;
	UInt32											CoveredOverflow;
	bool											CoveredLogged;

	NiNode*			ResolveDistantRef();
	void			PollDistantRef();
	void			PollLandLOD();
	void			PollCellGrid();

	NiNode*			GetHolder(NiNode* Parent);
	void			RefreshHolder(NiNode* Holder);

	/// True when Node is one of our own holders. The pollers must skip these or a holder attached to a
	/// node they watch registers as an arrival and starts a fade that attaches another holder.
	bool			IsHolder(NiAVObject* Node) { return !HolderNodes.empty() && HolderNodes.count(Node) != 0; }

	void			AssignSlot(SlotEntry& Entry, NiAVObject* Node);
	void			ReleaseSlots(std::vector<SlotEntry>& Slots);
	void			ReleaseSlots(std::unordered_map<NiAVObject*, NiNode*>& Slots);
	void			ResyncSlots(std::unordered_map<NiAVObject*, NiNode*>& Slots, std::unordered_map<NiAVObject*, NiNode*>& Current);

	void			ReleaseCells(std::unordered_map<TESObjectCELL*, NiNode*>& Cells);
	void			ResyncCells(std::unordered_map<TESObjectCELL*, NiNode*>& Cells, std::unordered_map<TESObjectCELL*, NiNode*>& Current);

	void			Retire(UInt32 Index);

	// DIAGNOSTIC ONLY: takes no references, dereferences nothing without a NULL test and mutates nothing
	// but its own latch. Walks Root downward once per tier per session and reports how many nodes and
	// how many drawable leaves actually live under a fade root, which nothing has ever verified.
	// It then walks back UP from that same leaf under ResolveGeometry's exact rule, so a m_parent chain
	// that does not mirror the child arrays shows up as upReached=0 on a node known to be under Root.
	void			RunRootCensus(NiAVObject* Root, const char* Tier);

	// True when Object's NiRTTI chain reaches NiNode. Oblivion's NiObject carries no GetAsNiNode slot --
	// that virtual exists only in the other two GameNi.h blocks -- and a vtable whitelist would silently
	// misclassify derived node types, so the RTTI chain is the only safe test available here.
	static bool		IsNiNodeType(NiAVObject* Object);
};
