#include "LODFadeManager.h"

LODFadeManager::LODFadeManager() {

	TheLODFadeManager = this;
	Fades.clear();
	LiveCount = 0;
	CurrentTime = 0.0f;
	DitherSeed = 0.0f;
	PrevValid = false;

}

LODFadeManager::FadeRecord* LODFadeManager::AddFade(NiAVObject* Root, UInt8 Direction) {

	if (!Root) return NULL;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) return NULL;

	FadeRecord Record;
	Record.Root = Root;
	Record.Direction = Direction;
	Record.StartTime = CurrentTime;
	Record.Pinned = false;
	Fades.push_back(Record);
	LiveCount = Fades.size();

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] start root=%08X dir=%d live=%d", (UInt32)Root, Direction, LiveCount);

	return &Fades.back();

}

float LODFadeManager::GetAlpha(FadeRecord* Record) {

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	if (FadeTime <= 0.0f) return 1.0f;

	float t = (CurrentTime - Record->StartTime) / FadeTime;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return Record->Direction == FadeDir_In ? t : 1.0f - t;

}

void LODFadeManager::Retire(UInt32 Index) {

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] retire root=%08X", (UInt32)Fades[Index].Root);

	Fades.erase(Fades.begin() + Index);

}

/// Detects distant-LOD slots gaining a node (stream-in) by diffing the grid against the previous frame.
/// A large fraction of slots changing at once (teleport/fast-travel) is a discontinuity and is suppressed.
void LODFadeManager::PollDistantGrid() {

	GridDistantArray* Grid = Tes->gridDistantArray;
	if (!Grid || !Grid->grid || !Grid->size) return;

	UInt32 Slots = Grid->size * Grid->size;
	if (PrevDistant.size() != Slots) {
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
		for (UInt32 i = 0; i < Slots; i++) PrevDistant[i] = (NiAVObject*)Grid->grid[i].unk04;
		return;
	}

	for (UInt32 i = 0; i < Slots; i++) {
		NiAVObject* Node = (NiAVObject*)Grid->grid[i].unk04;
		if (Node != PrevDistant[i]) {
			if (Node && !RootIndex.count(Node)) AddFade(Node, FadeDir_In);
			PrevDistant[i] = Node;
		}
	}

}

/// Detects LandLOD (terrain LOD) quadrants gaining a new node by diffing the child array against
/// the previous frame. A size change (e.g. quadrant count changing) resets tracking without fading.
void LODFadeManager::PollLandLOD() {

	NiNode* LandLOD = Tes->LODRoot;
	if (!LandLOD) return;

	UInt32 Count = LandLOD->m_children.numObjs;
	if (PrevLandLOD.size() != Count) {
		PrevLandLOD.assign(Count, NULL);
		return;
	}

	for (UInt32 i = 0; i < Count; i++) {
		NiAVObject* Node = LandLOD->m_children.data[i];
		if (Node != PrevLandLOD[i]) {
			if (Node && !RootIndex.count(Node)) AddFade(Node, FadeDir_In);
			PrevLandLOD[i] = Node;
		}
	}

}

/// Advances all live fades, retires completed ones, then polls the grids for new stream-in
/// transitions. RootIndex is rebuilt wholesale afterward since AddFade/Retire invalidate pointers.
void LODFadeManager::Update() {

	CurrentTime = (float)GetTickCount() * 0.001f;
	DitherSeed = (float)(GetTickCount() & 0xFFFF);

	// Player and Tes are NULL at the main menu; every per-frame hook that touches them must guard.
	if (!Player || !Tes) {
		Fades.clear();
		RootIndex.clear();
		LiveCount = 0;
		PrevValid = false;
		return;
	}

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	for (SInt32 i = (SInt32)Fades.size() - 1; i >= 0; i--) {
		if (CurrentTime - Fades[i].StartTime >= FadeTime) Retire(i);
	}

	PollDistantGrid();
	PollLandLOD();
	PrevValid = true;

	RootIndex.clear();
	for (UInt32 i = 0; i < Fades.size(); i++) RootIndex[Fades[i].Root] = &Fades[i];
	LiveCount = Fades.size();

}
