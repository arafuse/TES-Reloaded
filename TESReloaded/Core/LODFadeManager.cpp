#include "LODFadeManager.h"

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

}

/// Overwrites one shadow-copy slot, moving reference ownership with it: the incoming node gains a
/// reference and the outgoing node loses the one the slot held. Every write to a Prev* slot goes
/// through here so the ownership invariant declared in the header holds on every path.
void LODFadeManager::AssignSlot(NiAVObject*& Slot, NiAVObject* Node) {

	if (Slot == Node) return;
	if (Node) InterlockedIncrement(&Node->m_uiRefCount);
	if (Slot && !InterlockedDecrement(&Slot->m_uiRefCount)) Slot->Destructor(true);
	Slot = Node;

}

/// Releases every reference a shadow-copy vector owns and empties it. Used by the resync paths and
/// the main-menu guard, both of which abandon the whole vector at once.
void LODFadeManager::ReleaseSlots(std::vector<NiAVObject*>& Slots) {

	for (UInt32 i = 0; i < Slots.size(); i++) {
		NiAVObject* Node = Slots[i];
		if (Node && !InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);
	}
	Slots.clear();

}

LODFadeManager::FadeRecord* LODFadeManager::AddFade(NiAVObject* Root) {

	if (!Root) return NULL;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) return NULL;

	FadeRecord Record;
	Record.Root = Root;
	Record.StartTime = CurrentTime;
	Record.Pinned = false;
	Record.Invert = false;
	Record.WasCulled = false;
	Fades.push_back(Record);
	LiveCount = Fades.size();
	FadeSetDirty = true;

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] start root=%08X live=%d", (UInt32)Root, LiveCount);

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
		Logger::Log("[LODFade] retire root=%08X pinned=%d", (UInt32)Fades[Index].Root, Fades[Index].Pinned ? 1 : 0);

	if (Fades[Index].Pinned) Unpin(&Fades[Index]);

	FadeSetDirty = true;
	Fades.erase(Fades.begin() + Index);

}

/// Keeps a departing node alive and drawn for the fade duration. Only the un-cull path is
/// implemented: if the node is already detached from the scene graph, this logs and declines
/// rather than re-attaching it, since re-attachment touches live scene-graph lifetimes and is a
/// separate, conditional task. The log line is the measurement that decides whether that task is
/// ever needed. Callers must still hold the shadow-copy slot's reference when calling this, so the
/// node is guaranteed live and the m_parent test below is a real attached-vs-detached test rather
/// than a read of freed memory.
bool LODFadeManager::Pin(FadeRecord* Record) {

	NiAVObject* Node = Record->Root;
	if (!Node) return false;

	if (!Node->m_parent) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] pin declined, node already detached root=%08X", (UInt32)Node);
		return false;
	}

	// Still in the graph: un-culling is all that is needed, and it touches no lifetime but the
	// refcount. The reference stops the engine freeing the node while we are still drawing it.
	// Record the flag as we found it -- Unpin restores exactly this rather than assuming it was set.
	InterlockedIncrement(&Node->m_uiRefCount);
	Record->WasCulled = (Node->m_flags & NiAVObject::kFlag_AppCulled) != 0;
	Node->m_flags &= (UInt16)~NiAVObject::kFlag_AppCulled;
	Record->Pinned = true;

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] pin root=%08X", (UInt32)Node);

	return true;

}

/// Releases a pin taken by Pin. Restores the cull flag to the exact state Pin recorded before
/// dropping the reference -- a node the engine had never culled must not be left un-culled forever,
/// and a node the engine had already culled must not be forced visible by this override.
void LODFadeManager::Unpin(FadeRecord* Record) {

	NiAVObject* Node = Record->Root;
	if (!Node || !Record->Pinned) return;

	Record->Pinned = false;
	if (Record->WasCulled) Node->m_flags |= (UInt16)NiAVObject::kFlag_AppCulled;
	if (!InterlockedDecrement(&Node->m_uiRefCount)) Node->Destructor(true);

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] unpin root=%08X", (UInt32)Node);

}

/// Detects distant-LOD slots gaining a node (stream-in) by diffing the grid against the previous frame.
/// A large fraction of slots changing at once (teleport/fast-travel) is a discontinuity and is suppressed.
void LODFadeManager::PollDistantGrid() {

	GridDistantArray* Grid = Tes->gridDistantArray;
	if (!Grid || !Grid->grid || !Grid->size) return;

	UInt32 Slots = Grid->size * Grid->size;
	if (PrevDistant.size() != Slots) {
		ReleaseSlots(PrevDistant);
		PrevDistant.assign(Slots, NULL);
		PrevValid = false;
	}

	// Count first, so a teleport is suppressed before any fade is started rather than after.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = (NiAVObject*)Grid->grid[i].unk04;
		if (Node != PrevDistant[i]) Changed++;
	}

	bool Discontinuity = !PrevValid || Changed > (Slots / 4);
	if (Discontinuity) {
		if (PrevValid && TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] distant discontinuity: %d of %d slots changed, suppressed", Changed, Slots);
		for (UInt32 i = 0; i < Slots; i++) AssignSlot(PrevDistant[i], (NiAVObject*)Grid->grid[i].unk04);
		return;
	}

	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = (NiAVObject*)Grid->grid[i].unk04;
		if (Node == PrevDistant[i]) continue;

		// Departure and arrival are independent tests, not if/else: a slot can swap straight from one
		// non-NULL node to a different one without ever passing through NULL, and both halves of that
		// swap have to fade. Both AddFade calls land in this Update() so they share a StartTime.
		if (PrevDistant[i] && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
			// Departing node fades out via the inverted rising alpha, not a separate declining-alpha
			// direction. See FadeRecord::Invert. Pin takes its own reference before the slot below
			// drops the one that kept this pointer valid.
			FadeRecord* Out = AddFade(PrevDistant[i]);
			if (Out) {
				Out->Invert = true;
				if (!Pin(Out)) Retire((UInt32)(Fades.size() - 1));
			}
		}
		if (Node && !RootIndex.count(Node)) AddFade(Node);
		AssignSlot(PrevDistant[i], Node);
	}

}

/// Detects LandLOD (terrain LOD) quadrants gaining a new node by diffing the child array against
/// the previous frame. A size change resyncs to the actual current pointers rather than NULL,
/// since stamping NULL would make every already-loaded quadrant look newly-populated next poll.
void LODFadeManager::PollLandLOD() {

	NiNode* LandLOD = Tes->LODRoot;
	if (!LandLOD) return;

	UInt32 Count = LandLOD->m_children.numObjs;
	if (PrevLandLOD.size() != Count) {
		ReleaseSlots(PrevLandLOD);
		PrevLandLOD.assign(Count, NULL);
		for (UInt32 i = 0; i < Count; i++) AssignSlot(PrevLandLOD[i], LandLOD->m_children.data[i]);
		return;
	}

	// Count first, so a teleport is suppressed before any fade is started rather than after.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Count; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (Node != PrevLandLOD[i]) Changed++;
	}

	if (Changed > (Count / 2)) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] landlod discontinuity: %d of %d slots changed, suppressed", Changed, Count);
		for (UInt32 i = 0; i < Count; i++) AssignSlot(PrevLandLOD[i], LandLOD->m_children.data[i]);
		return;
	}

	for (UInt32 i = 0; i < Count; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (Node == PrevLandLOD[i]) continue;

		// The out-fade is independent of the arrival, not gated on it: a quadrant replaced by NULL, or
		// replaced while the incoming node is already mid-fade, still has to fade out rather than pop.
		if (PrevLandLOD[i] && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
			// The old quadrant runs on the new one's rising alpha with Invert set (Ruling F14), so the
			// two together cover exactly 100% throughout. Same StartTime as the partner (both AddFade
			// calls land in this Update()), so no cross-record link is needed.
			FadeRecord* Out = AddFade(PrevLandLOD[i]);
			if (Out) {
				Out->Invert = true;
				if (!Pin(Out)) Retire((UInt32)(Fades.size() - 1));
			}
		}
		if (Node && !RootIndex.count(Node)) AddFade(Node);
		AssignSlot(PrevLandLOD[i], Node);
	}

}

/// Detects loaded-cell slots gaining or losing a cell by diffing Tes->gridCellArray against the
/// previous frame. Cell gains fade the cell's full models in; cell losses hold the departing full
/// models via Pin while they fade out on the inverted rising alpha, same shape as the distant grid.
void LODFadeManager::PollCellGrid() {

	GridCellArray* Grid = Tes->gridCellArray;
	if (!Grid || !Grid->grid || !Grid->size) return;

	UInt32 Dim = Grid->size;
	UInt32 Slots = Dim * Dim;
	if (PrevCell.size() != Slots) {
		ReleaseSlots(PrevCell);
		PrevCell.assign(Slots, NULL);
		for (UInt32 i = 0; i < Slots; i++) {
			GridCellArray::GridEntry* Entry = &Grid->grid[i];
			AssignSlot(PrevCell[i], Entry->info ? (NiAVObject*)Entry->info->niNode : NULL);
		}
		return;
	}

	// Crossing one boundary shifts Dim slots and a corner shifts 2*Dim-1, so 2*Dim sits one above
	// the worst legitimate case and far below a full reload of Dim squared.
	UInt32 Changed = 0;
	for (UInt32 i = 0; i < Slots; i++) {
		GridCellArray::GridEntry* Entry = &Grid->grid[i];
		NiAVObject* Node = Entry->info ? (NiAVObject*)Entry->info->niNode : NULL;
		if (Node != PrevCell[i]) Changed++;
	}

	if (Changed > Dim * 2) {
		if (TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] cell discontinuity: %d of %d slots changed, suppressed", Changed, Slots);
		for (UInt32 i = 0; i < Slots; i++) {
			GridCellArray::GridEntry* Entry = &Grid->grid[i];
			AssignSlot(PrevCell[i], Entry->info ? (NiAVObject*)Entry->info->niNode : NULL);
		}
		return;
	}

	for (UInt32 i = 0; i < Slots; i++) {
		GridCellArray::GridEntry* Entry = &Grid->grid[i];
		NiAVObject* Node = Entry->info ? (NiAVObject*)Entry->info->niNode : NULL;
		if (Node == PrevCell[i]) continue;

		// Departure and arrival are independent tests, not if/else, so a slot that swaps one cell
		// node straight for another fades both halves instead of popping the outgoing one.
		if (PrevCell[i] && TheSettingManager->SettingsMain.LODFade.PinDeparting) {
			// Cell lost: hold the departing full models while the LOD fades back in, via the
			// inverted rising alpha rather than a declining-alpha fade-out (Ruling F14).
			FadeRecord* Out = AddFade(PrevCell[i]);
			if (Out) {
				Out->Invert = true;
				if (!Pin(Out)) Retire((UInt32)(Fades.size() - 1));
			}
		}
		if (Node && !RootIndex.count(Node)) {
			// Cell gained: full models fade in. The paired LOD node is pinned by the distant
			// poller's own slot change in the same frame, so no cross-poller lookup is needed.
			AddFade(Node);
		}
		AssignSlot(PrevCell[i], Node);
	}

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
		// The shadow copies own a reference to every non-NULL entry; abandoning them without
		// releasing would leak one reference per occupied grid slot.
		ReleaseSlots(PrevDistant);
		ReleaseSlots(PrevLandLOD);
		ReleaseSlots(PrevCell);
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

	PollDistantGrid();
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
