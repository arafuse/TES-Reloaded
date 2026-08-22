#include "LODFadeManager.h"

// DIAGNOSTIC ONLY. Bounds on the one-shot root census walk. It runs on the render thread, so the cap
// is on total visits as well as on depth; the stack array is sized by the visit cap.
static const UInt32 kCensusMaxVisits = 512;
static const UInt32 kCensusMaxDepth = 8;

/// DIAGNOSTIC ONLY. Writes Node's m_parent chain into Buffer as `name@address` hops, leaf first,
/// separated by '<'. Bounded in both depth and buffer length, and every hop is NULL-tested before it
/// is followed. Addresses are included so a chain can be compared directly against the root=%08X the
/// fade start lines print. Buffer is always NUL-terminated, empty when Node is NULL.
/// DIAGNOSTIC ONLY. Bit position for a tier's one-shot latches; 3 for anything unrecognised, which
/// no caller passes and which every latch mask treats as "already logged".
static UInt32 TierIndex(const char* Tier) {

	if (!Tier) return 3;
	if (!strcmp(Tier, "distant")) return 0;
	if (!strcmp(Tier, "cell")) return 1;
	if (!strcmp(Tier, "landlod")) return 2;
	return 3;

}

static void FormatAncestry(NiAVObject* Node, char* Buffer, UInt32 Size) {

	UInt32 Used = 0;
	Buffer[0] = 0;
	for (UInt32 Depth = 0; Node && Depth < 10; Depth++) {
		char Hop[64];
		const char* Name = (Node->m_pcName && Node->m_pcName[0]) ? Node->m_pcName : "?";
		sprintf(Hop, "%s%.24s@%08X", Used ? "<" : "", Name, (UInt32)Node);
		UInt32 Len = strlen(Hop);
		if (Used + Len >= Size) break;
		memcpy(Buffer + Used, Hop, Len);
		Used += Len;
		Node = (NiAVObject*)Node->m_parent;
	}
	Buffer[Used] = 0;

}

LODFadeManager::LODFadeManager() {

	TheLODFadeManager = this;
	Fades.clear();
	LiveCount = 0;
	CurrentTime = 0.0f;
	DitherSeed = 0.0f;
	PrevValid = false;
	FadeSetDirty = false;
	FadeResetPending = false;
	NeedsOpaquePublish = true;
	DistantRef = NULL;
	DistantRefLogged = false;
	CellGridLogged = false;
	LandLODLogged = false;
	DrawCovered = 0;
	DrawGated = 0;
	DrawResolved = 0;
	DrawMinAlpha = 2.0f;
	ResolveMissWanted = false;
	ResolveMissPending = false;
	ResolveMissLogged = false;
	MissGeom = 0;
	MissParents[0] = MissParents[1] = MissParents[2] = 0;
	MissDepth = 0;
	LastDrawLogTime = 0.0f;
	CensusLogged = 0;
	PinChainLogged = 0;
	CoveredCensusWanted = true;
	CoveredShaderCount = 0;
	CoveredOverflow = 0;
	CoveredLogged = false;

}

/// DIAGNOSTIC ONLY. True when Object's runtime type derives from NiNode, decided by walking the
/// NiRTTI{name,parent} chain returned by NiObject::GetType(). Oblivion's NiObject exposes no
/// GetAsNiNode slot -- that virtual is present only in the Fallout and Skyrim blocks of GameNi.h --
/// and a hardcoded VFT whitelist would silently misclassify BSFadeNode, NiBillboardNode and every
/// other derived container, so the RTTI chain is the only safe "is this a node" test here. The chain
/// walk is depth-bounded and NULL-guarded at every hop; a broken chain answers "not a node", which
/// only ever stops the census descending and can never produce a bad cast.
bool LODFadeManager::IsNiNodeType(NiAVObject* Object) {

	if (!Object) return false;

	NiRTTI* Type = Object->GetType();
	for (UInt32 Depth = 0; Type && Depth < 16; Depth++) {
		if (Type->name && !strcmp(Type->name, "NiNode")) return true;
		Type = Type->parent;
	}
	return false;

}

/// DIAGNOSTIC ONLY. Answers the question nothing in this feature has ever verified: does a fade root
/// actually contain any drawable geometry? We diff and fade CONTAINER nodes, and if the geometry the
/// renderer submits does not live beneath them then no parent walk from a drawn geometry can ever
/// reach one, which is exactly the resolved=0 signature the draw counters report.
///
/// Runs at most once per TIER per session -- the three tiers pick their roots by completely different
/// routes, so "geoms=0 on one tier" and "geoms=0 everywhere" are different diagnoses. Read-only
/// throughout: it takes no reference, writes nothing but its own latch, and never dereferences a
/// pointer it has not NULL-tested. Bounded at 512 visits and depth 8 because it runs on the render
/// thread; the bounds are deliberately small, since the question is "any geometry at all", not "how
/// much". Iterative rather than recursive so the bound is on total work rather than only on depth.
void LODFadeManager::RunRootCensus(NiAVObject* Root, const char* Tier, bool Departing) {

	if (!Root || !Tier) return;
	if (!TheSettingManager->SettingsMain.Develop.LogLODFade) return;

	// The latch sits INSIDE the log gate, same discipline as DistantRefLogged, so enabling logging
	// later in a session still gets each tier its lines. Keyed by direction as well as tier: a
	// departure is already detached when it is detected and an arrival is not, so one shared latch
	// would report only whichever happened to come first.
	UInt32 Bit = 1 << (TierIndex(Tier) + (Departing ? 3 : 0));
	if (TierIndex(Tier) > 2 || (CensusLogged & Bit)) return;
	CensusLogged |= Bit;

	// The root's OWN position in the graph, in the same name@address form the covered-draw census
	// prints for drawn geometry. The two are directly comparable: a root that never appears on any
	// covered chain cannot be an ancestor of anything drawn through a fade-carrying shader, whatever
	// its subtree contains. culled is the root's AppCulled bit, since a culled root draws nothing.
	char RootChain[320];
	FormatAncestry(Root, RootChain, sizeof(RootChain));
	Logger::Log("[LODFade] root chain: tier=%s dir=%s culled=%d %s",
		Tier, Departing ? "out" : "in", (Root->m_flags & NiAVObject::kFlag_AppCulled) ? 1 : 0, RootChain);

	struct CensusEntry { NiAVObject* Node; UInt32 Depth; };
	CensusEntry Stack[kCensusMaxVisits];
	UInt32 Top = 0;
	UInt32 Visited = 0;
	UInt32 Nodes = 0;
	UInt32 Geoms = 0;
	UInt32 CulledGeoms = 0;
	UInt32 CulledNodes = 0;
	UInt32 MaxDepth = 0;
	NiAVObject* FirstGeom = NULL;

	Stack[Top].Node = Root;
	Stack[Top].Depth = 0;
	Top++;

	while (Top && Visited < kCensusMaxVisits) {
		Top--;
		NiAVObject* Node = Stack[Top].Node;
		UInt32 Depth = Stack[Top].Depth;
		if (!Node) continue;
		Visited++;
		if (Depth > MaxDepth) MaxDepth = Depth;

		// A leaf -- anything whose RTTI chain does not reach NiNode -- is drawable geometry as far as this
		// census is concerned, and its address is directly comparable with the geom= value in a draw miss.
		if (!IsNiNodeType(Node)) {
			Geoms++;
			if (Node->m_flags & NiAVObject::kFlag_AppCulled) CulledGeoms++;
			if (!FirstGeom) FirstGeom = Node;
			continue;
		}

		Nodes++;
		if (Node->m_flags & NiAVObject::kFlag_AppCulled) CulledNodes++;
		if (Depth >= kCensusMaxDepth) continue;

		// Scanned to `end` clamped to `capacity`, never to numObjs: removal NULLs a slot without
		// compacting it, so numObjs is a live-child count and not a slot bound. NULL entries are skipped.
		NiNode* Branch = (NiNode*)Node;
		UInt32 Slots = Branch->m_children.data ? Branch->m_children.end : 0;
		if (Slots > Branch->m_children.capacity) Slots = Branch->m_children.capacity;
		for (UInt32 i = 0; i < Slots && Top < kCensusMaxVisits; i++) {
			NiAVObject* Child = Branch->m_children.data[i];
			if (!Child) continue;
			Stack[Top].Node = Child;
			Stack[Top].Depth = Depth + 1;
			Top++;
		}
	}

	// culledGeoms/culledNodes are the other half of "is this root drawable": a subtree that is entirely
	// AppCulled at the moment the transition is detected explains resolved=0 without any graph fault,
	// because the renderer never descends into it and no draw can carry the fade's alpha.
	Logger::Log("[LODFade] root census: tier=%s dir=%s root=%08X depth=%d nodes=%d geoms=%d culledNodes=%d culledGeoms=%d firstGeom=%08X firstGeomParent=%08X",
		Tier, Departing ? "out" : "in", (UInt32)Root, MaxDepth, Nodes, Geoms, CulledNodes, CulledGeoms,
		(UInt32)FirstGeom, FirstGeom ? (UInt32)FirstGeom->m_parent : 0);

	// The decisive half: walk back UP from the geometry the descent just found, under EXACTLY
	// ResolveGeometry's rule -- the node itself is tested before any step, and the cap is the same 16 --
	// so this is that function's own verdict on a node the descent proved is under Root.
	UInt32 UpReached = 0;
	UInt32 UpDepth = 0;
	UInt32 Up[3] = { 0, 0, 0 };
	if (FirstGeom) {
		NiAVObject* Node = FirstGeom;
		for (; Node && UpDepth < 16; UpDepth++) {
			if (Node == Root) { UpReached = 1; break; }
			Node = (NiAVObject*)Node->m_parent;
		}
	}

	// The chain shape itself, captured separately so a walk that terminated early still reports what
	// it saw. Each hop is NULL-tested before it is followed.
	NiAVObject* Ancestor = FirstGeom ? (NiAVObject*)FirstGeom->m_parent : NULL;
	for (UInt32 i = 0; i < 3 && Ancestor; i++) {
		Up[i] = (UInt32)Ancestor;
		Ancestor = (NiAVObject*)Ancestor->m_parent;
	}

	// selfRoot answers the landlod shape, where root and geometry are the same object, so
	// ResolveGeometry's self-check would match at depth 0. inIndex is RootIndex as it stands RIGHT NOW,
	// which is the PREVIOUS frame's index: this runs from AddFade, inside the poll phase, and Update()
	// rebuilds RootIndex only afterwards. Read it as "was this a root last frame", not as the lookup
	// the next draw will do.
	Logger::Log("[LODFade] census updown: tier=%s dir=%s root=%08X firstGeom=%08X upReached=%d upDepth=%d up1=%08X up2=%08X up3=%08X selfRoot=%d inIndex=%d",
		Tier, Departing ? "out" : "in", (UInt32)Root, (UInt32)FirstGeom, UpReached, UpDepth, Up[0], Up[1], Up[2],
		(FirstGeom && FirstGeom == Root) ? 1 : 0, FirstGeom ? (UInt32)RootIndex.count(FirstGeom) : 0);

}

/// DIAGNOSTIC ONLY. Records the frame's first failed parent walk -- the geometry, its first three
/// ancestors and the depth reached -- so Update() can print the shape of the hierarchy the draw hook
/// actually saw. Values are copied out here, while the pointers are known live, and never
/// dereferenced again. Disarms itself, so at most one walk runs per frame and none once printed.
void LODFadeManager::NoteResolveMiss(NiAVObject* Geometry) {

	ResolveMissWanted = false;
	if (!Geometry) return;

	ResolveMissPending = true;
	MissGeom = (UInt32)Geometry;
	MissParents[0] = MissParents[1] = MissParents[2] = 0;
	MissDepth = 0;

	NiAVObject* Node = (NiAVObject*)Geometry->m_parent;
	for (UInt32 Depth = 0; Node && Depth < 16; Depth++) {
		if (Depth < 3) MissParents[Depth] = (UInt32)Node;
		MissDepth = Depth + 1;
		Node = (NiAVObject*)Node->m_parent;
	}

}

/// DIAGNOSTIC ONLY. Tallies one covered draw under its pixel shader and, the first time a shader is
/// seen, records that draw's geometry ancestry as text. Mutates nothing but its own census fields and
/// dereferences only pointers the draw hook is holding live at this instant.
void LODFadeManager::NoteCoveredDraw(const char* ShaderName, NiAVObject* Geometry, bool Resolved) {

	// Rows are matched on the ShaderRecord's own string pointer; see the header for why that is enough.
	const char* Name = ShaderName ? ShaderName : "(unnamed)";
	for (UInt32 i = 0; i < CoveredShaderCount; i++) {
		if (CoveredShaders[i].Name == Name) {
			CoveredShaders[i].Draws++;
			if (Resolved) CoveredShaders[i].Resolved++;
			return;
		}
	}
	if (CoveredShaderCount >= kCoveredShaderMax) {
		CoveredOverflow++;
		return;
	}

	CoveredShaderEntry& Entry = CoveredShaders[CoveredShaderCount++];
	Entry.Name = Name;
	Entry.Draws = 1;
	Entry.Resolved = Resolved ? 1 : 0;
	Entry.Chain[0] = 0;

	// The measurement: the ancestry of the first geometry this shader drew, leaf first, each hop as
	// name@address so it can be compared directly against the root=%08X the fade logs print. If no
	// covered shader's chain passes through a node a poller chose as a root, the roots and the drawn
	// geometry live in different parts of the graph and no alpha could ever reach them.
	FormatAncestry(Geometry, Entry.Chain, sizeof(Entry.Chain));

}

/// Overwrites one shadow-copy slot, moving reference ownership with it: the incoming node gains a
/// reference and the outgoing node loses the one the slot held. Every write to a Prev* slot goes
/// through here so the ownership invariant declared in the header holds on every path.
///
/// The remembered parent is refreshed on every poll, so it is never more than one poll old and needs
/// no reference of its own: a container that dies inside that window takes all of its children with
/// it, which the poller sees as a discontinuity and suppresses before any pin is attempted.
///
/// ORDERING: the node field takes the new reference before releasing the old, so re-assigning a slot
/// to what it already holds cannot destruct it mid-swap. The parent is then re-read from the node
/// that survived that swap, so it can never be read out of freed memory.
void LODFadeManager::AssignSlot(SlotEntry& Entry, NiAVObject* Node) {

	if (Entry.Node != Node) {
		if (Node) InterlockedIncrement(&Node->m_uiRefCount);
		if (Entry.Node && !InterlockedDecrement(&Entry.Node->m_uiRefCount)) Entry.Node->Destructor(true);
		Entry.Node = Node;
	}

	Entry.Parent = Node ? Node->m_parent : NULL;

}

/// Releases every reference a shadow-copy vector owns and empties it. Used by the resync paths and
/// the main-menu guard, both of which abandon the whole vector at once. SlotEntry::Parent owns
/// nothing, so only the node needs releasing.
void LODFadeManager::ReleaseSlots(std::vector<SlotEntry>& Slots) {

	for (UInt32 i = 0; i < Slots.size(); i++) {
		NiAVObject* Node = Slots[i].Node;
		if (Node && !InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);
	}
	Slots.clear();

}

/// Releases every reference the distant shadow copy owns and empties it. Same contract as the vector
/// overload; used by the main-menu guard, which abandons the whole map at once.
void LODFadeManager::ReleaseSlots(std::unordered_map<NiAVObject*, NiNode*>& Slots) {

	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = Slots.begin(); it != Slots.end(); ++it) {
		NiAVObject* Node = it->first;
		if (Node && !InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);
	}
	Slots.clear();

}

/// Moves the shadow map onto the current frame's membership, carrying reference ownership with it:
/// every newly-present node gains a reference and every departed node loses the one the map held.
/// Releases run last, so a departing node destructed here is one nothing else still holds -- callers
/// that pin departures must have taken their own reference before calling this. The swap also carries
/// the freshly-read parents across, which is how the distant tier gets its per-poll parent refresh.
///
/// WARNING: Current is SWAPPED, not copied, so it does not survive the call. On return it holds the
/// PREVIOUS frame's membership -- stale pointers that own nothing and may already be destructed. It
/// must be cleared or overwritten before its next use, and must never be passed to ReleaseSlots,
/// which would release references this map no longer owns. The swap is what keeps this O(1): a
/// copy-assign here was ~2075 node allocations per frame on the render thread. All of the refcount
/// work above completes before the swap, so the ordering contract is unaffected by it.
void LODFadeManager::ResyncSlots(std::unordered_map<NiAVObject*, NiNode*>& Slots, std::unordered_map<NiAVObject*, NiNode*>& Current) {

	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = Current.begin(); it != Current.end(); ++it) {
		if (!Slots.count(it->first)) InterlockedIncrement(&it->first->m_uiRefCount);
	}
	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = Slots.begin(); it != Slots.end(); ++it) {
		NiAVObject* Node = it->first;
		if (!Current.count(Node) && !InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);
	}
	Slots.swap(Current);

}

LODFadeManager::FadeRecord* LODFadeManager::AddFade(NiAVObject* Root, const char* Tier, bool Departing) {

	if (!Root) return NULL;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) return NULL;

	FadeRecord Record;
	Record.Root = Root;
	Record.StartTime = CurrentTime;
	Record.Pinned = false;
	Record.Invert = false;
	Record.WasCulled = false;
	Record.Holder = NULL;
	Record.Tier = Tier;
	Fades.push_back(Record);
	LiveCount = Fades.size();
	FadeSetDirty = true;

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] %s start root=%08X live=%d", Tier, (UInt32)Root, LiveCount);

	// DIAGNOSTIC ONLY, and self-latching per tier: the first fade of each tier reports what is actually
	// underneath its root. Read-only, so it cannot disturb the record just pushed.
	RunRootCensus(Root, Tier, Departing);

	return &Fades.back();

}

float LODFadeManager::GetAlpha(FadeRecord* Record) {

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	if (FadeTime <= 0.0f) return 1.0f;

	float t = (CurrentTime - Record->StartTime) / FadeTime;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return t;

}

/// Retires a fade, releasing its pin first if one is held. Called both from the normal completion
/// path and the hard timeout, so a pin can never outlive its record.
void LODFadeManager::Retire(UInt32 Index) {

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] %s retire root=%08X pinned=%d", Fades[Index].Tier, (UInt32)Fades[Index].Root, Fades[Index].Pinned ? 1 : 0);

	if (Fades[Index].Pinned) Unpin(&Fades[Index]);

	FadeSetDirty = true;
	Fades.erase(Fades.begin() + Index);

}

/// Points a holder at its parent's world transform and gives it a bound the culler cannot reject.
/// NiNode::New leaves a zero world bound, and a zero-radius sphere fails the frustum test, which would
/// cull the holder and everything pinned under it -- an invisible fade-out, the exact failure this
/// path exists to fix. Unioning the children's bounds would be tighter, but a holder is transient and
/// holds a handful of nodes at most, and every child still carries its own correct bound and is culled
/// on that, so the only cost of the loose bound is one extra node visit per frame.
///
/// Called for every live holder once per Update(), not only at pin time: this fork's LOD roots are
/// anchor- and camera-relative, so a parent that moves during the one-second fade would otherwise
/// leave the holder holding a stale snapshot of its transform.
void LODFadeManager::RefreshHolder(NiNode* Holder) {

	NiNode* Parent = Holder->m_parent;
	if (!Parent) return;

	// Identity local transform, set explicitly rather than trusting what NiNode::New leaves behind: a
	// zero scale here would collapse every pinned node to a point.
	NiTransform* Local = &Holder->m_localTransform;
	for (UInt32 r = 0; r < 3; r++) {
		for (UInt32 c = 0; c < 3; c++) Local->rot.data[r][c] = (r == c) ? 1.0f : 0.0f;
	}
	Local->pos.x = 0.0f;
	Local->pos.y = 0.0f;
	Local->pos.z = 0.0f;
	Local->scale = 1.0f;

	// With an identity local transform the holder's world transform is exactly its parent's, so a
	// pinned child's already-computed world transform stays correct whether or not the engine ever
	// runs an update pass over this subtree.
	Holder->m_worldTransform = Parent->m_worldTransform;
	// Big enough that the frustum test can never reject it, small enough not to poison a union: if the
	// engine ever recomputes DistantRefLOD's own bound from its children via UpdateWorldBound, a 1e9
	// radius would propagate up the graph and persist there long after the holder has emptied.
	Holder->m_kWorldBound.Center = Holder->m_worldTransform.pos;
	Holder->m_kWorldBound.Radius = 1.0e6f;
	Holder->m_combinedBounds = Holder->m_kWorldBound;

}

/// Returns the plugin-owned holder hanging under Parent, creating and attaching it on first use.
/// Holders are keyed by parent because render context follows position in the scene graph; see the
/// header. Returns NULL -- declining the pin -- when Parent is NULL, when the allocation fails, or
/// when the cached holder has been orphaned.
///
/// An orphaned holder is the signature of a DESTRUCTED parent, since ~NiNode NULLs its children's
/// m_parent, and it is the strongest evidence this function ever gets that Parent is freed memory --
/// the creation path below validates Parent not at all. So a mismatch declines rather than attaching a
/// fresh holder to a pointer just concluded to be dead. The stale entry is deliberately RETAINED as a
/// tombstone: erasing it would make the check single-use, and a second departure for the same parent
/// in the same poll would then miss the map and walk straight into the creation path -- three
/// dereferences of the freed parent, one of them a write. Several distant nodes can depart from the
/// same DistantRefLOD parent in one poll and still sit under the discontinuity threshold, so that
/// sequence is reachable, not theoretical. The trade is that if
/// Parent's address is later recycled by a live node, that node can never get a holder and its
/// departures pop silently, which is the fail-safe direction.
///
/// Holder references are never released here either -- a live pin may still name a holder, and
/// destructing one under a pin would fault in Unpin.
///
/// The holder is APPENDED (AddObject FirstAvail = 0), not dropped into the first free slot the way
/// every other AddObject call site in this fork does. Those attach to skeleton nodes, where filling a
/// hole is harmless. Here the parent can be a fixed-arity engine-managed array: a LandLOD holder is
/// created precisely because a quadrant departed, and a slot going non-NULL to NULL is what a
/// departure IS, so FirstAvail = 1 would put a bare NiNode at that quadrant's index. If the engine
/// indexes those dozen children positionally, or casts them to a terrain-LOD type, that is type
/// confusion and it faults. Nothing else in this fork walks LODRoot's children, so it cannot be
/// settled statically; appending costs nothing and cannot collide.
NiNode* LODFadeManager::GetHolder(NiNode* Parent) {

	if (!Parent) return NULL;

	std::unordered_map<NiNode*, NiNode*>::iterator Found = Holders.find(Parent);
	if (Found != Holders.end()) {
		// Dereferences only the holder, which we own, so this is safe even once the key is freed.
		if (Found->second->m_parent != Parent) return NULL;
		RefreshHolder(Found->second);
		return Found->second;
	}

	NiNode* Holder = (NiNode*)MemoryAlloc(sizeof(NiNode));
	if (!Holder) return NULL;
	Holder->New(8);
	Holder->SetName("ORLODFadeHolder");
	InterlockedIncrement(&Holder->m_uiRefCount);
	// FirstAvail = 0 appends; the 1 used everywhere else in this fork would drop a bare NiNode into
	// the hole a departing LandLOD quadrant just left, at that quadrant's own index. See the doc above.
	Parent->AddObject(Holder, 0);
	RefreshHolder(Holder);
	Holders[Parent] = Holder;
	HolderNodes.insert(Holder);

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] holder created under parent=%08X name=%s", (UInt32)Parent, Parent->m_pcName ? Parent->m_pcName : "(unnamed)");

	return Holder;

}

/// Non-mutating viability test, run BEFORE AddFade so a departure that cannot be held never gets a
/// record. Testing after the fact created and destroyed a record in the same call, churning
/// FadeSetDirty and LiveCount and emitting a start/decline/retire triple per departure.
bool LODFadeManager::CanPin(NiAVObject* Node, NiNode* Parent, const char* Tier) {

	if (!Node) return false;
	if (Node->m_parent) return true;

	if (!Parent) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] %s pin declined, no parent remembered root=%08X", Tier, (UInt32)Node);
		return false;
	}

	return true;

}

/// Keeps a departing node alive and drawn for the fade duration. Callers must still hold the shadow
/// copy's reference when calling this, so the node is guaranteed live and the m_parent test below is a
/// real attached-vs-detached test rather than a read of freed memory.
///
/// Measured in game before the re-attach path existed: 43 declines and 0 successful pins. A departure
/// is only detected one poll AFTER the engine removed the child, and removal NULLs m_parent, so the
/// un-cull path alone can never fire for a departure and the fade-out half never ran. Parent is the
/// node the shadow copy last saw this one attached to, which is the only way to recover where it
/// belonged; see AssignSlot for why that pointer is safe to dereference here.
bool LODFadeManager::Pin(FadeRecord* Record, NiNode* Parent) {

	NiAVObject* Node = Record->Root;
	if (!Node) return false;

	if (!Node->m_parent) {
		// Already detached: re-attach under a holder hanging off the original parent, so the node
		// renders in exactly the context it always did -- a distant LOD node under DistantRefLOD binds
		// the DISTLOD shaders, a LandLOD quadrant under LandLOD binds the terrain LOD shaders.
		NiNode* Holder = GetHolder(Parent);
		if (!Holder) {
			if (TheSettingManager->SettingsMain.Develop.LogLODFade)
				Logger::Log("[LODFade] %s pin declined, holder unavailable root=%08X", Record->Tier, (UInt32)Node);
			return false;
		}
		InterlockedIncrement(&Node->m_uiRefCount);
		Record->WasCulled = (Node->m_flags & NiAVObject::kFlag_AppCulled) != 0;
		Node->m_flags &= (UInt16)~NiAVObject::kFlag_AppCulled;
		Holder->AddObject(Node, 1);
		Record->Holder = Holder;
		Record->Pinned = true;
		RefreshHolder(Holder);

		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] %s pin root=%08X mode=reattach", Record->Tier, (UInt32)Node);

		// DIAGNOSTIC ONLY, once per tier: the pinned node's ancestry AFTER the re-attach. A departure is
		// detached when it is detected, so its detection-time census can say nothing about whether the
		// pin put it back somewhere the renderer traverses. A chain that ends at the holder instead of
		// reaching WorldRoot Node means the holder itself is not in the graph and nothing under a pin
		// can ever draw.
		UInt32 PinBit = 1 << TierIndex(Record->Tier);
		if (TheSettingManager->SettingsMain.Develop.LogLODFade && TierIndex(Record->Tier) <= 2 && !(PinChainLogged & PinBit)) {
			PinChainLogged |= PinBit;
			char PinChain[320];
			FormatAncestry(Node, PinChain, sizeof(PinChain));
			Logger::Log("[LODFade] pin chain: tier=%s holder=%08X %s", Record->Tier, (UInt32)Holder, PinChain);
		}

		return true;
	}

	// Still in the graph: un-culling is all that is needed, and it touches no lifetime but the
	// refcount. The reference stops the engine freeing the node while we are still drawing it.
	// Record the flag as we found it -- Unpin restores exactly this rather than assuming it was set.
	InterlockedIncrement(&Node->m_uiRefCount);
	Record->WasCulled = (Node->m_flags & NiAVObject::kFlag_AppCulled) != 0;
	Node->m_flags &= (UInt16)~NiAVObject::kFlag_AppCulled;
	Record->Pinned = true;

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] %s pin root=%08X mode=uncull", Record->Tier, (UInt32)Node);

	return true;

}

/// Releases a pin taken by Pin. Restores the cull flag to the exact state Pin recorded before
/// dropping the reference -- a node the engine had never culled must not be left un-culled forever,
/// and a node the engine had already culled must not be forced visible by this override.
///
/// Order is load-bearing throughout: the decrement can hit zero and destruct the node, so every read
/// and every write to it -- the flag restore and the detach alike -- has to happen first.
///
/// The detach is unconditional, even when the engine has re-attached the node elsewhere meanwhile.
/// Skipping it in that case is the obvious resurrection guard and it is wrong: it would leave the node
/// in the holder's child array forever, drawn and referenced every frame. The owner is read before the
/// call and restored after it, since RemoveObject NULLs m_parent.
///
/// The out parameter starts NULL rather than aliasing the node, and that is the SAFE initialisation
/// rather than merely the tidier one: the slot is a smart pointer's, so assigning through it releases
/// whatever it previously held. The aliased form used elsewhere in this fork is the risky one. NULL
/// also distinguishes "child not found" for free.
///
/// SETTLED BY MEASUREMENT: RemoveObject TRANSFERS the child array's reference into the out parameter;
/// it does not release it. 35 re-attached pins in game all logged "refcount 2 -> 2", unchanged across
/// the call. The two references at that point are Pin's own and the holder array's; the transferred
/// one lands in Removed, which is discarded, so the re-attach path owes an extra decrement or every
/// pinned node and its whole subtree leaks. That decrement is the one below inside the Holder branch.
///
/// This confirmed the signature argument -- NiAVObject** RemovedChild is the address of an
/// NiAVObjectPtr's raw slot, and Gamebryo's DetachChild AddRefs into that out-slot before releasing
/// the array slot, netting zero. It also confirms EquipmentManager.cpp:636 was correctly discarded as
/// evidence: its Object->Destructor(1) bypasses refcounting entirely and fits either reading.
///
/// RefBefore/RefAfter and the log line are KEPT as the regression test for exactly this. They bracket
/// RemoveObject alone, not the extra decrement, so the correct reading remains "refcount 2 -> 2" on a
/// re-attach and "0 -> 0" on an un-cull, which takes neither branch. Do not "fix" those numbers.
void LODFadeManager::Unpin(FadeRecord* Record) {

	NiAVObject* Node = Record->Root;
	if (!Node || !Record->Pinned) return;

	Record->Pinned = false;
	if (Record->WasCulled) Node->m_flags |= (UInt16)NiAVObject::kFlag_AppCulled;

	UInt32 RefBefore = 0;
	UInt32 RefAfter = 0;
	if (Record->Holder) {
		// Detach unconditionally even if the engine re-attached this node meanwhile, then put its own
		// attachment back -- RemoveObject NULLs m_parent. See the doc comment for why skipping is worse.
		NiNode* Owner = Node->m_parent;
		NiAVObject* Removed = NULL;
		RefBefore = Node->m_uiRefCount;
		Record->Holder->RemoveObject(&Removed, Node);
		RefAfter = Node->m_uiRefCount;
		if (Owner != Record->Holder) Node->m_parent = Owner;
		// Drop the reference RemoveObject transferred into Removed, which we discard. Cannot reach zero
		// and so needs no Destructor: Pin's own reference is still held and is released just below.
		InterlockedDecrement(&Node->m_uiRefCount);
		Record->Holder = NULL;
	}

	if (!InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] %s unpin root=%08X refcount %u -> %u", Record->Tier, (UInt32)Node, RefBefore, RefAfter);

}

/// Resolves the DistantRefLOD scene-graph node, the container of every distant static/tree LOD node.
/// Tes->LODRoot is misnamed -- it points at "LandLOD" -- so the real container is its parent, whose
/// children are LandLOD, DistantRefLOD and LODWaterRoot. The container holds only those three, so the
/// cached pointer is revalidated by identity against the live child list on every call rather than
/// simply returned: that costs three pointer compares and guarantees a pointer the engine has since
/// swapped out is never dereferenced.
///
/// This replaced a poller that read Tes->gridDistantArray and treated DistantGridEntry::unk04 as the
/// per-cell NiNode*. That layout was never verified -- Game.h labels all four entry fields unk* -- and
/// in-game logging showed all 4225 slots differing every frame, so nothing was ever detected and the
/// poller was writing a refcount into ~4225 arbitrary addresses per frame. Do not reintroduce it
/// without first re-deriving DistantGridEntry's layout.
NiNode* LODFadeManager::ResolveDistantRef() {

	NiNode* LandLOD = Tes->LODRoot;
	if (!LandLOD) return NULL;

	NiNode* Container = LandLOD->m_parent;
	if (!Container) return NULL;

	NiNode* Found = NULL;
	UInt32 Slots = Container->m_children.end;
	if (Slots > Container->m_children.capacity) Slots = Container->m_children.capacity;
	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Child = Container->m_children.data[i];
		if (!Child) continue;
		if (Child == DistantRef) return DistantRef;
		if (Child->m_pcName && !strcmp(Child->m_pcName, "DistantRefLOD")) Found = (NiNode*)Child;
	}

	DistantRef = Found;

	// Prove the traversal found the node it assumed before the feature trusts it. Roughly 2075
	// children is the expected shape; a wildly different count, or no line at all, means the walk is
	// wrong and must be re-derived rather than silently detecting nothing for another session.
	// The latch sits INSIDE the log gate so it means "logged once", not "resolved once": if
	// LogLODFade is switched on after the first resolve, the line still gets its one chance to print.
	if (Found && !DistantRefLogged && TheSettingManager->SettingsMain.Develop.LogLODFade) {
		DistantRefLogged = true;
		Logger::Log("[LODFade] DistantRefLOD resolved name=%s children=%d", Found->m_pcName, Found->m_children.numObjs);
	}

	return Found;

}

/// Detects distant LOD nodes streaming in and out by diffing DistantRefLOD's children against the
/// previous frame as a SET. The diff is deliberately not index-keyed: the engine compacts and reuses
/// child slots as distant cells stream, so a positional diff reports the whole array changed every
/// frame. A large fraction of the children churning at once (teleport/fast-travel) is a discontinuity
/// and is suppressed.
void LODFadeManager::PollDistantRef() {

	NiNode* Node = ResolveDistantRef();
	if (!Node) return;

	// Scanned to `end`, not `numObjs`: removing a distant cell NULLs its slot and decrements numObjs
	// without compacting, so numObjs is a count of live children and not a slot bound. Capacity is the
	// hard clamp. NULL slots are simply skipped -- membership, not position, is what the diff keys on.
	// Our own holders are skipped too, or attaching one would register as an arrival, start a fade for
	// it, and pin the holder under a second holder.
	CurDistant.clear();
	UInt32 Slots = Node->m_children.end;
	if (Slots > Node->m_children.capacity) Slots = Node->m_children.capacity;
	// Sized once on first population; thereafter ResyncSlots swaps the two maps, so both keep their
	// buckets and the per-frame rebuild never rehashes.
	if (Slots && CurDistant.bucket_count() < Slots) CurDistant.reserve(Slots);
	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Child = Node->m_children.data[i];
		if (!Child || IsHolder(Child)) continue;
		// The parent is captured here, while the child is still attached. By the time a departure is
		// detected the engine has already NULLed m_parent, so this is the only chance to record it.
		CurDistant[Child] = Child->m_parent;
	}
	UInt32 Count = (UInt32)CurDistant.size();

	// Count first, so a teleport is suppressed before any fade is started rather than after.
	UInt32 Changed = 0;
	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = CurDistant.begin(); it != CurDistant.end(); ++it) {
		if (!PrevDistant.count(it->first)) Changed++;
	}
	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = PrevDistant.begin(); it != PrevDistant.end(); ++it) {
		if (!CurDistant.count(it->first)) Changed++;
	}

	// An empty previous set is the first population: resync silently rather than dissolving in the
	// entire world. Count is the live child count, so the quarter test scales with the loaded world
	// and a Count of 0 (interior) makes any churn at all a discontinuity, which is the safe answer.
	bool Discontinuity = !PrevValid || PrevDistant.empty() || Changed > (Count / 4);
	if (Discontinuity) {
		if (PrevValid && !PrevDistant.empty() && TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] distantref discontinuity: %d of %d changed, suppressed", Changed, Count);
		ResyncSlots(PrevDistant, CurDistant);
		return;
	}

	// Departures run first and are fully consumed before any arrival: AddFade returns a pointer into
	// Fades, which the next push_back would reallocate out from under Out.
	if (TheSettingManager->SettingsMain.LODFade.PinDeparting) {
		for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = PrevDistant.begin(); it != PrevDistant.end(); ++it) {
			if (CurDistant.count(it->first)) continue;
			// Departing node fades out via the inverted rising alpha, not a separate declining-alpha
			// direction. See FadeRecord::Invert. Pin takes its own reference before ResyncSlots below
			// drops the one that kept this pointer valid. Viability is tested before the record is
			// created so an unpinnable departure never churns the fade table.
			if (!CanPin(it->first, it->second, "distant")) continue;
			FadeRecord* Out = AddFade(it->first, "distant", true);
			if (Out) {
				Out->Invert = true;
				if (!Pin(Out, it->second)) Retire((UInt32)(Fades.size() - 1));
			}
		}
	}

	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = CurDistant.begin(); it != CurDistant.end(); ++it) {
		if (!PrevDistant.count(it->first) && !RootIndex.count(it->first)) AddFade(it->first, "distant", false);
	}

	ResyncSlots(PrevDistant, CurDistant);

}

/// Detects LandLOD (terrain LOD) quadrants gaining a new node by diffing the child array against
/// the previous frame. A slot-count change resyncs to the actual current pointers rather than NULL,
/// since stamping NULL would make every already-loaded quadrant look newly-populated next poll.
///
/// This diff stays POSITIONAL, unlike the distant one: LandLOD holds a fixed dozen quadrants that the
/// engine replaces in place, and slot identity is what pairs an outgoing quadrant with its incoming
/// replacement so the two share a StartTime. A NULL slot is therefore a meaningful value here, not an
/// entry to skip -- a slot going non-NULL to NULL is exactly the departure signal.
void LODFadeManager::PollLandLOD() {

	NiNode* LandLOD = Tes->LODRoot;
	if (!LandLOD) return;

	// Bounded by `end`, not `numObjs`, for the same reason as the distant poller: numObjs counts live
	// children, so NULLing a quadrant in place would shrink it, truncate the tail, and trip the
	// resync below -- silently discarding the very transition this function exists to catch.
	// Capacity is the hard clamp, and covers a zero-capacity (NULL data) array by making Slots 0.
	UInt32 Slots = LandLOD->m_children.end;
	if (Slots > LandLOD->m_children.capacity) Slots = LandLOD->m_children.capacity;

	// One-shot diagnostic: proves the child array is actually populated with real NiNode pointers
	// before the tier's silence is trusted as "no LandLOD quadrant changed". The latch sits INSIDE the
	// log gate, same discipline as DistantRefLogged, so enabling logging later still gets the line.
	if (!LandLODLogged && TheSettingManager->SettingsMain.Develop.LogLODFade) {
		LandLODLogged = true;
		UInt32 Populated = 0;
		for (UInt32 i = 0; i < Slots; i++) {
			NiAVObject* Child = LandLOD->m_children.data[i];
			if (Child && !IsHolder(Child)) Populated++;
		}
		Logger::Log("[LODFade] landlod children=%d populated=%d", Slots, Populated);
	}

	// Also the first-population path (PrevLandLOD empty), which must resync silently rather than fade
	// in every quadrant at once. An appended holder grows the array and takes this path exactly once,
	// costing one missed poll; thereafter it reads as a permanently empty trailing slot.
	if (PrevLandLOD.size() != Slots) {
		ReleaseSlots(PrevLandLOD);
		SlotEntry Empty;
		Empty.Node = NULL;
		Empty.Parent = NULL;
		PrevLandLOD.assign(Slots, Empty);
		for (UInt32 i = 0; i < Slots; i++) {
			NiAVObject* Child = LandLOD->m_children.data[i];
			AssignSlot(PrevLandLOD[i], IsHolder(Child) ? NULL : Child);
		}
		return;
	}

	// Count first, so a teleport is suppressed before any fade is started rather than after.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (IsHolder(Node)) Node = NULL;
		if (Node != PrevLandLOD[i].Node) Changed++;
	}

	// Half the SLOTS, not half the live children: the diff is positional, so the denominator has to be
	// the number of positions being compared or a sparse array would lower the bar for suppression.
	if (Changed > (Slots / 2)) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] landlod discontinuity: %d of %d slots changed, suppressed", Changed, Slots);
		for (UInt32 i = 0; i < Slots; i++) {
			NiAVObject* Child = LandLOD->m_children.data[i];
			AssignSlot(PrevLandLOD[i], IsHolder(Child) ? NULL : Child);
		}
		return;
	}

	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (IsHolder(Node)) Node = NULL;

		if (Node != PrevLandLOD[i].Node) {
			// The out-fade is independent of the arrival, not gated on it: a quadrant replaced by NULL, or
			// replaced while the incoming node is already mid-fade, still has to fade out rather than pop.
			if (PrevLandLOD[i].Node && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
				// The old quadrant runs on the new one's rising alpha with Invert set (Ruling F14), so the
				// two together cover exactly 100% throughout. Same StartTime as the partner (both AddFade
				// calls land in this Update()), so no cross-record link is needed.
				if (CanPin(PrevLandLOD[i].Node, PrevLandLOD[i].Parent, "landlod")) {
					NiNode* GoneParent = PrevLandLOD[i].Parent;
					FadeRecord* Out = AddFade(PrevLandLOD[i].Node, "landlod", true);
					if (Out) {
						Out->Invert = true;
						if (!Pin(Out, GoneParent)) Retire((UInt32)(Fades.size() - 1));
					}
				}
			}
			if (Node && !RootIndex.count(Node)) AddFade(Node, "landlod", false);
		}

		// Unconditional, so an unchanged slot still gets its remembered parent refreshed this poll.
		AssignSlot(PrevLandLOD[i], Node);
	}

}

/// Detects loaded cells arriving by diffing the CHILDREN OF Tes->ObjectLODRoot against the previous
/// poll as a set of nodes. An arriving child is one loaded cell's worth of full models, and it fades
/// in. Structurally identical to PollDistantRef, and it shares that tier's slot plumbing.
///
/// IT WATCHES THE RENDER GRAPH, NOT THE CELL GRID, and that is the whole point. The original version
/// diffed GridCellArray::CellInfo::niNode and never produced a visible fade in five sessions. The
/// covered-draw census settled why: that field resolves to `Water Node`, a child of WaterRoot, and it
/// is AppCulled at the moment a cell arrives -- the renderer never descends into it, so no draw could
/// ever carry the alpha. Every loaded object actually measured, landscape and doors and creatures and
/// the player alike, hangs under Tes->ObjectLODRoot through one direct child per cell. Those children
/// are what this now diffs, so the nodes it fades are by construction ancestors of drawn geometry.
/// The CellInfo layout in Game.h was never more than a guess and nothing here depends on it now.
///
/// FADE-IN ONLY. Departures are deliberately ignored: a cell unloading is the full model going away
/// as its distant LOD arrives, and the distant tier already fades that pair from its own side.
/// Do not reintroduce Pin, a remembered parent or a holder here.
void LODFadeManager::PollCellGrid() {

	NiNode* Root = Tes->ObjectLODRoot;
	if (!Root || !Root->m_children.data) return;

	// Scanned to `end` clamped to `capacity`, never to numObjs: removing a cell NULLs its slot without
	// compacting, so numObjs is a live-child count and not a slot bound. Our own holders are skipped,
	// or attaching one would register as an arrival and start a fade for it.
	CurCellSet.clear();
	UInt32 Slots = Root->m_children.end;
	if (Slots > Root->m_children.capacity) Slots = Root->m_children.capacity;
	if (Slots && CurCellSet.bucket_count() < Slots) CurCellSet.reserve(Slots);
	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Child = Root->m_children.data[i];
		if (!Child || IsHolder(Child)) continue;
		CurCellSet[Child] = Child->m_parent;
	}
	UInt32 Count = (UInt32)CurCellSet.size();

	// One-shot diagnostic, so a silent tier can be told apart from an empty one before its silence is
	// read as "no boundary crossed". Latch sits INSIDE the log gate, same discipline as elsewhere.
	if (!CellGridLogged && TheSettingManager->SettingsMain.Develop.LogLODFade) {
		CellGridLogged = true;
		Logger::Log("[LODFade] cell root=%08X slots=%d populated=%d", (UInt32)Root, Slots, Count);
	}

	// Count first, so a teleport is suppressed before any fade is started rather than after. Crossing
	// one boundary brings in a row of uGridsToLoad cells out of that many squared, so a quarter of the
	// live count sits well above the worst legitimate case and far below a full reload.
	UInt32 Arrived = 0;
	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = CurCellSet.begin(); it != CurCellSet.end(); ++it) {
		if (!PrevCellSet.count(it->first)) Arrived++;
	}

	// An empty previous set is the first population: resync silently rather than fading in every
	// loaded cell at once. A Count of 0 makes any churn a discontinuity, which is the safe answer.
	bool Discontinuity = !PrevValid || PrevCellSet.empty() || Arrived > (Count / 4);
	if (Discontinuity) {
		if (PrevValid && !PrevCellSet.empty() && TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] cell discontinuity: %d of %d arrived, suppressed", Arrived, Count);
		ResyncSlots(PrevCellSet, CurCellSet);
		return;
	}

	for (std::unordered_map<NiAVObject*, NiNode*>::iterator it = CurCellSet.begin(); it != CurCellSet.end(); ++it) {
		if (!PrevCellSet.count(it->first) && !RootIndex.count(it->first)) AddFade(it->first, "cell", false);
	}

	ResyncSlots(PrevCellSet, CurCellSet);

}

/// Advances all live fades, retires completed ones, then polls the grids for new stream-in
/// transitions. RootIndex is rebuilt wholesale afterward since AddFade/Retire invalidate pointers.
void LODFadeManager::Update() {

	CurrentTime = (float)GetTickCount() * 0.001f;

	// Golden-ratio (2^32/phi) advance in integer space, one step per ~16 ms, so the shader's
	// frac(hash + seed) actually moves. The multiply must NOT be done in float: at real uptimes
	// (float)GetTickCount() is large enough that a float32 ulp already exceeds 0.25, which would
	// quantise the seed to a handful of levels; an integer wrap keeps the full fraction.
	DitherSeed = (float)((GetTickCount() >> 4) * 2654435769u) * (1.0f / 4294967296.0f);

	// DIAGNOSTIC ONLY: report and reset the draw-path counters the render hook accumulated over the
	// frame just past. Throttled to one line a second and only emitted while a fade was live, so idle
	// play stays silent. Read-off: covered=0 means the constant never reached HasFadeParams (a CreateCT
	// / shader-record binding problem, not a manager one); covered>0 with gated=0 means the gate
	// condition is wrong; gated>0 with resolved=0 means ResolveGeometry never matches, i.e. the nodes
	// we fade are not ancestors of the nodes that get drawn; resolved>0 with minAlpha near 1.0 means the
	// alpha computation; resolved>0 with minAlpha<1.0 means the publish chain is fine and the fault is
	// in the shader or the constant register. rootIdx is RootIndex.size() read BEFORE the rebuild at the
	// bottom of this function, so it is the index the frame just measured actually resolved against:
	// rootIdx=0 while live>0 means the rebuild is the fault and the scene graph is not involved at all.
	bool LogFade = TheSettingManager->SettingsMain.Develop.LogLODFade;
	if (LogFade && LiveCount > 0 && CurrentTime - LastDrawLogTime >= 1.0f) {
		LastDrawLogTime = CurrentTime;
		// in/out split the live fades by direction. Both directions have to work for the feature to be
		// visible, but they fail for different reasons -- an arrival is a node already in the graph, a
		// departure only draws if Pin put it back somewhere the renderer traverses -- so "in=0 always"
		// and "out=0 always" are completely different bugs and the totals alone cannot tell them apart.
		UInt32 FadesIn = 0;
		UInt32 FadesOut = 0;
		for (UInt32 i = 0; i < Fades.size(); i++) Fades[i].Invert ? FadesOut++ : FadesIn++;
		Logger::Log("[LODFade] draw: covered=%d gated=%d resolved=%d minAlpha=%.3f live=%d in=%d out=%d rootIdx=%d",
			DrawCovered, DrawGated, DrawResolved, DrawMinAlpha, LiveCount, FadesIn, FadesOut, (UInt32)RootIndex.size());
	}
	// The captured ancestry sample is printed only when the whole frame resolved nothing while a fade
	// was live, which is the case that says the fade roots are not in the drawn geometry's ancestry.
	if (ResolveMissPending) {
		if (LogFade && !ResolveMissLogged && DrawResolved == 0 && LiveCount > 0) {
			ResolveMissLogged = true;
			Logger::Log("[LODFade] resolve miss: geom=%08X parents=%08X,%08X,%08X depth=%d",
				MissGeom, MissParents[0], MissParents[1], MissParents[2], MissDepth);
		}
		ResolveMissPending = false;
	}

	// DIAGNOSTIC ONLY, once per session: the per-shader breakdown of the covered draws, with the
	// scene-graph ancestry of the first geometry each shader drew. Held until a frame that had both a
	// live fade and covered draws, so the resolved column is measured against a frame where a match
	// was actually possible. Read-off: if no chain contains the root=%08X of a fade start line, the
	// pollers are picking nodes that are not ancestors of anything drawn through a covered shader --
	// the roots are wrong. If a chain does contain one, the fault is in RootIndex or the cache, not in
	// the graph. And if the LOD shaders (DISTLOD*, terrain) are absent from the table entirely, those
	// draws never reach this hook and the clip is on the wrong shaders.
	if (LogFade && !CoveredLogged && LiveCount > 0 && DrawCovered > 0) {
		CoveredLogged = true;
		CoveredCensusWanted = false;
		for (UInt32 i = 0; i < CoveredShaderCount; i++) {
			Logger::Log("[LODFade] covered shader: %s draws=%d resolved=%d chain=%s",
				CoveredShaders[i].Name, CoveredShaders[i].Draws, CoveredShaders[i].Resolved, CoveredShaders[i].Chain);
		}
		Logger::Log("[LODFade] covered shader: (total) names=%d overflow=%d", CoveredShaderCount, CoveredOverflow);
	}

	DrawCovered = 0;
	DrawGated = 0;
	DrawResolved = 0;
	DrawMinAlpha = 2.0f;
	ResolveMissWanted = LogFade && !ResolveMissLogged;

	// Defensive only: ShaderManager::UpdateConstants already dereferences Tes->sky and
	// Player->parentCell before it calls Update(), so this branch cannot currently be reached and is
	// NOT what keeps the main menu safe. It is kept correct in case that call order ever changes --
	// Fades.clear() invalidates every FadeRecord* GeomCache holds, so the cache is dropped here too,
	// since this early return would otherwise skip the FadeSetDirty-gated clear at the end.
	if (!Player || !Tes) {
		// Every live pin must be released here too, or its InterlockedIncrement leaks permanently
		// and its cull flag stays cleared forever with nothing left tracking the node.
		for (UInt32 i = 0; i < Fades.size(); i++) {
			if (Fades[i].Pinned) Unpin(&Fades[i]);
		}
		Fades.clear();
		RootIndex.clear();
		GeomCache.clear();
		// The shadow copies own a reference to every non-NULL entry -- the node for the first two, the
		// mapped container for the cell set; abandoning them without releasing would leak one
		// reference per occupied grid slot.
		ReleaseSlots(PrevDistant);
		ReleaseSlots(PrevLandLOD);
		ReleaseSlots(PrevCellSet);
		// Tes is gone, so the cached DistantRefLOD pointer is meaningless and must be re-resolved.
		// Both scratch maps hold stale post-swap pointers that own nothing, so they are only cleared.
		CurDistant.clear();
		CurCellSet.clear();
		DistantRef = NULL;
		// Every holder's parent went with the scene graph, so the parent-keyed lookup is meaningless.
		// HolderNodes and its references are deliberately kept: they are what stops those pointers
		// dangling, and a holder is never freed while a pin might still name it.
		Holders.clear();
		FadeSetDirty = false;
		LiveCount = 0;
		FadeResetPending = false;
		TheShaderManager->ShaderConst.LODFade.Params.x = 1.0f;
		TheShaderManager->ShaderConst.LODFade.Params.z = 0.0f;
		NeedsOpaquePublish = true;
		PrevValid = false;
		return;
	}

	// A pin held open by a partner fade is retired only on completion or the 2x hard timeout below,
	// which is a pure safety net in case a partner never completes.
	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	for (SInt32 i = (SInt32)Fades.size() - 1; i >= 0; i--) {
		float Elapsed = CurrentTime - Fades[i].StartTime;
		// GetTickCount wraps at 49.7 days of uptime. Without this, Elapsed goes hugely negative and
		// neither the completion test nor the hard timeout can ever fire again: every fade freezes at
		// its current alpha and every pin leaks permanently.
		if (Elapsed < 0.0f) { Retire(i); continue; }
		bool HardTimeout = Elapsed >= FadeTime * 2.0f;
		bool Complete = Elapsed >= FadeTime;
		if (HardTimeout || Complete) Retire(i);
	}

	// Holders re-track their parents every frame, not just at pin time: this fork's LOD roots are
	// anchor- and camera-relative. RefreshHolder no-ops on an orphaned holder, the only unsafe case.
	for (std::unordered_map<NiNode*, NiNode*>::iterator it = Holders.begin(); it != Holders.end(); ++it) {
		RefreshHolder(it->second);
	}

	PollDistantRef();
	PollCellGrid();
	PollLandLOD();
	PrevValid = true;

	// Rebuilt wholesale because AddFade/Retire reallocate and shift Fades. Through the poll phase
	// above, RootIndex therefore holds DANGLING FadeRecord* values; that is safe only because the
	// pollers touch it through count()/find() on the key alone and no draw is interleaved with them.
	UInt32 PrevLive = LiveCount;
	RootIndex.clear();
	for (UInt32 i = 0; i < Fades.size(); i++) RootIndex[Fades[i].Root] = &Fades[i];
	LiveCount = Fades.size();

	// GeomCache is only valid while RootIndex is unchanged. AddFade/Retire may both fire in the
	// same frame leaving LiveCount unchanged but RootIndex's contents different, so the cache is
	// invalidated on the FadeSetDirty flag rather than on a LiveCount comparison.
	if (FadeSetDirty) {
		GeomCache.clear();
		FadeSetDirty = false;
	}

	FadeResetPending = (LiveCount == 0 && PrevLive > 0);

	// A shader that declares both TESR_GEOM_Toggles and TESR_GEOM_FadeParams (e.g. skin) can call
	// SetPerGeomCT() from a foreign call site outside the LODFade gate, republishing whatever
	// LODFade.Params currently holds. With no fade in flight that value must always be opaque, so a
	// foreign publish can never leave stale stippling behind.
	if (LiveCount == 0) {
		TheShaderManager->ShaderConst.LODFade.Params.x = 1.0f;
		TheShaderManager->ShaderConst.LODFade.Params.z = 0.0f;
	}

}

/// Maps a drawn geometry to the fade it belongs to by walking m_parent to a registered root.
/// The answer is cached for the duration of the fade episode, misses included.
LODFadeManager::FadeRecord* LODFadeManager::ResolveGeometry(NiAVObject* Geometry) {

	std::unordered_map<NiAVObject*, FadeRecord*>::iterator Cached = GeomCache.find(Geometry);
	if (Cached != GeomCache.end()) return Cached->second;

	FadeRecord* Found = NULL;
	NiAVObject* Node = Geometry;
	for (UInt32 Depth = 0; Node && Depth < 16; Depth++) {
		std::unordered_map<NiAVObject*, FadeRecord*>::iterator Root = RootIndex.find(Node);
		if (Root != RootIndex.end()) {
			Found = Root->second;
			break;
		}
		Node = (NiAVObject*)Node->m_parent;
	}

	// Misses are cached too - a miss is the common case and the walk must not repeat per frame.
	GeomCache[Geometry] = Found;
	return Found;

}
