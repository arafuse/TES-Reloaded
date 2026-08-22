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
	// HOW LONG Parent MAY BE STALE DIFFERS BY TIER, and so does whether it carries a reference:
	//
	// - LandLOD (AssignSlot with StickyParent false) refreshes Parent on EVERY poll, so it is never
	//   more than one poll old, and it carries NO reference. That tier observes its nodes through a
	//   child array, so a node's presence in the poll and a live m_parent are the same fact, and a
	//   container that dies takes all its children with it -- mass churn the discontinuity guard
	//   suppresses before any pin runs. Refcount traffic that high in the graph would be risk for
	//   nothing.
	// - The cell tier (StickyParent true) keeps the last NON-NULL answer instead, because it observes
	//   Grid->grid[i].info->niNode independently of the scene graph; see AssignSlot. That pointer can
	//   therefore outlive one poll, which is exactly what the no-reference argument above rests on, so
	//   the sticky tier OWNS a reference on Parent and ParentOwned records it. Owning it is the only
	//   thing that makes a sticky pointer safe to dereference in Pin.
	//
	// ParentOwned is per entry rather than per tier so no release site has to know which vector it is
	// looking at. Every path that replaces or drops Parent must go through AssignSlot or ReleaseParent.
	struct SlotEntry {
		NiAVObject*	Node;
		NiNode*		Parent;
		bool		ParentOwned;
	};

	// INVARIANT: these three shadow copies OWN one reference to every non-NULL node they hold. The
	// reference is taken when a slot is filled, not when a departure is later detected -- by then the
	// engine may already have dropped its last reference, and Pin would be reading freed memory.
	// Every path that overwrites or clears a slot must release exactly once; use AssignSlot,
	// ResyncSlots and ReleaseSlots rather than writing a slot directly. The same rule applies
	// independently to SlotEntry::Parent wherever ParentOwned is set; see SlotEntry.
	//
	// PrevDistant is keyed by NODE, not by index: DistantRefLOD's child array is compacted and its
	// slots reused as distant cells stream, so an index-keyed diff reports every slot changed on every
	// frame. Membership diffing is immune to reordering and compaction. The mapped value is the
	// remembered parent, which is the same role SlotEntry::Parent plays for the two vectors.
	std::unordered_map<NiAVObject*, NiNode*>			PrevDistant;
	std::vector<SlotEntry>								PrevLandLOD;
	std::vector<SlotEntry>								PrevCell;
	std::unordered_map<NiAVObject*, FadeRecord*>		RootIndex;
	std::unordered_map<NiAVObject*, FadeRecord*>		GeomCache;
	bool												PrevValid;
	bool												FadeSetDirty;

	// Per-frame scratch for the distant membership diff. Owns NO references -- ownership moves into
	// PrevDistant via ResyncSlots, which SWAPS rather than copies, so on return this holds the previous
	// frame's stale membership and is rebuilt from scratch next poll. Never pass it to ReleaseSlots.
	// Kept as a member so the swap recycles both maps' buckets instead of reallocating ~2075 nodes.
	std::unordered_map<NiAVObject*, NiNode*>			CurDistant;

	// Plugin-owned holder nodes, keyed by the original parent each one hangs under. Keyed rather than
	// global because render context follows position in the graph: a distant LOD node under
	// DistantRefLOD binds the DISTLOD shaders and a cell node under the loaded-object root draws as an
	// ordinary object, so a single shared holder would make one of the two draw wrongly or not at all.
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

	NiNode*			ResolveDistantRef();
	void			PollDistantRef();
	void			PollLandLOD();
	void			PollCellGrid();

	NiNode*			GetHolder(NiNode* Parent);
	void			RefreshHolder(NiNode* Holder);

	/// True when Node is one of our own holders. The pollers must skip these or a holder attached to a
	/// node they watch registers as an arrival and starts a fade that attaches another holder.
	bool			IsHolder(NiAVObject* Node) { return !HolderNodes.empty() && HolderNodes.count(Node) != 0; }

	void			AssignSlot(SlotEntry& Entry, NiAVObject* Node, bool StickyParent);
	void			ReleaseParent(SlotEntry& Entry);
	void			ReleaseSlots(std::vector<SlotEntry>& Slots);
	void			ReleaseSlots(std::unordered_map<NiAVObject*, NiNode*>& Slots);
	void			ResyncSlots(std::unordered_map<NiAVObject*, NiNode*>& Slots, std::unordered_map<NiAVObject*, NiNode*>& Current);

	void			Retire(UInt32 Index);
};
