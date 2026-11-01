#include "LODFadeManager.h"
#include <cmath>
#include <cstdlib>

static const double	kPendingTimeoutMs	= 8000.0;
static const double	kCensusDelayMs		= 30000.0;
static const float	kCellSize			= 4096.0f;
static const UInt32	kMaxPending			= 256;
static const UInt32	kMaxParentDepth		= 10;
static const UInt32	kCensusMaxDepth		= 12;

LODFadeManager::LODFadeManager() {

	Logger::Log("Starting the LOD fade manager...");
	TheLODFadeManager = this;

	DitherSeed = 0.0f;
	CensusArmed = TheSettingManager->SettingsMain.Develop.LogLODFade != 0;
	CensusCount = 0;
	CensusOverflow = 0;
	CensusStartMs = 0.0;
	DistantRef = NULL;
	PrevValid = false;
	ResolveLogged = false;
	MissingLogged = false;
	NowMs = 0.0;

}

/// True when Object's runtime type derives from NiNode, decided by walking the NiRTTI chain.
/// Oblivion's NiObject has no GetAsNiNode slot, and a VFT whitelist misclassifies BSFadeNode and
/// friends; a broken chain answers "not a node", which only makes an arrival count as a leaf.
bool LODFadeManager::IsNiNodeType(NiAVObject* Object) {

	if (!Object) return false;

	NiRTTI* Type = Object->GetType();
	for (UInt32 Depth = 0; Type && Depth < 16; Depth++) {
		if (Type->name && !strcmp(Type->name, "NiNode")) return true;
		Type = Type->parent;
	}
	return false;

}

void LODFadeManager::Update() {

	if (!Player || !Tes) {
		Reset();
		return;
	}

	NowMs = TheFrameRateManager->GetPerformance();
	DitherSeed += 0.618034f;
	if (DitherSeed >= 1.0f) DitherSeed -= 1.0f;

	NiNode* Resolved = ResolveDistantRef();
	if (Resolved != DistantRef) {
		Reset();
		DistantRef = Resolved;
	}
	if (!DistantRef) return;

	PollDistantRef();
	DrainPending();
	RetireFinished();

	if (CensusArmed) {
		if (CensusStartMs == 0.0 && Player->IsExteriorLike()) CensusStartMs = NowMs;
		if (CensusStartMs != 0.0 && NowMs - CensusStartMs >= kCensusDelayMs) ReportCensus();
	}

}

/// Tes->LODRoot is misnamed: it is the LandLOD node, and its parent holds LandLOD, DistantRefLOD and
/// LODWaterRoot. Resolved by name every frame (three children), so a rebuilt container is picked up.
NiNode* LODFadeManager::ResolveDistantRef() {

	NiNode* LandLOD = Tes->LODRoot;
	NiNode* Container = LandLOD ? LandLOD->m_parent : NULL;
	NiNode* Found = NULL;

	if (Container) {
		NiTArray<NiAVObject*>& Children = Container->m_children;
		UInt32 Slots = Children.end < Children.capacity ? Children.end : Children.capacity;
		for (UInt32 i = 0; i < Slots && !Found; i++) {
			NiAVObject* Child = Children.data[i];
			if (Child && Child->m_pcName && !strcmp(Child->m_pcName, "DistantRefLOD")) Found = (NiNode*)Child;
		}
	}

	if (Found && !ResolveLogged && TheSettingManager->SettingsMain.Develop.LogLODFade) {
		ResolveLogged = true;
		Logger::Log("[LODFade] DistantRefLOD resolved name=%s children=%d", Found->m_pcName, Found->m_children.numObjs);
	}
	if (!Found && !MissingLogged) {
		MissingLogged = true;
		Logger::Log("[LODFade] DistantRefLOD not found; distant LOD will not fade until it is");
	}
	return Found;

}

void LODFadeManager::Reset() {

	Fades.clear();
	RootIndex.clear();
	GeometryCache.clear();
	Pending.clear();
	LiveChildren.clear();
	ScanChildren.clear();
	PrevValid = false;
	DistantRef = NULL;

}

/// Set-membership diff of DistantRefLOD's children. Scanned to `end`, not `numObjs`: removing a child
/// NULLs its slot without compacting. A large churn (teleport, fast travel) is adopted without fading.
void LODFadeManager::PollDistantRef() {

	ScanChildren.clear();
	NiTArray<NiAVObject*>& Children = DistantRef->m_children;
	UInt32 Slots = Children.end < Children.capacity ? Children.end : Children.capacity;
	if (ScanChildren.bucket_count() < Slots) ScanChildren.reserve(Slots);
	for (UInt32 i = 0; i < Slots; i++) {
		if (NiAVObject* Child = Children.data[i]) ScanChildren.insert(Child);
	}

	UInt32 Arrived = 0;
	UInt32 Departed = 0;
	for (NiAVObject* Child : ScanChildren) if (!LiveChildren.count(Child)) Arrived++;
	for (NiAVObject* Child : LiveChildren) if (!ScanChildren.count(Child)) Departed++;

	if (Departed) DropDeparted(ScanChildren);

	UInt32 Count = (UInt32)ScanChildren.size();
	if (!PrevValid || Arrived + Departed > Count / 4) {
		if (PrevValid && TheSettingManager->SettingsMain.Develop.LogLODFade)
			Logger::Log("[LODFade] discontinuity: %u arrived, %u departed of %u, not fading", Arrived, Departed, Count);
	}
	else if (Arrived) {
		for (NiAVObject* Child : ScanChildren) {
			if (!LiveChildren.count(Child)) AddArrival(Child);
		}
	}

	LiveChildren.swap(ScanChildren);
	PrevValid = true;

}

void LODFadeManager::DropDeparted(const std::unordered_set<NiAVObject*>& Live) {

	bool Dropped = false;
	for (size_t i = 0; i < Fades.size(); ) {
		if (Live.count(Fades[i].Root)) {
			i++;
			continue;
		}
		Fades[i] = Fades.back();
		Fades.pop_back();
		Dropped = true;
	}
	if (Dropped) RebuildIndex();

	for (auto it = Pending.begin(); it != Pending.end(); ) {
		if (Live.count(it->first)) ++it;
		else it = Pending.erase(it);
	}

}

void LODFadeManager::AddArrival(NiAVObject* Node) {

	if (IsReady(Node)) TryStartFade(Node);
	else if (Pending.size() < kMaxPending) Pending.emplace(Node, NowMs);

}

/// Drawable now: a valid world bound, and either a leaf or a node that already has children. The
/// engine sometimes attaches an empty container and streams the mesh in afterwards.
bool LODFadeManager::IsReady(NiAVObject* Node) {

	if (Node->m_kWorldBound.Radius <= 0.0f) return false;
	return !IsNiNodeType(Node) || ((NiNode*)Node)->m_children.numObjs != 0;

}

/// Starts a fade only for far-edge arrivals. The ring next to the loaded grid is LOD replacing a cell
/// that just unloaded; its full models are already gone, so fading it in would open a hole.
void LODFadeManager::TryStartFade(NiAVObject* Node) {

	NiPoint3& Center = Node->m_kWorldBound.Center;
	int dx = abs((int)floorf(Center.x / kCellSize) - (int)floorf(Player->pos.x / kCellSize));
	int dy = abs((int)floorf(Center.y / kCellSize) - (int)floorf(Player->pos.y / kCellSize));
	int Ring = dx > dy ? dx : dy;
	int MinRing = (int)(*SettingGridsToLoad / 2) + 2;
	bool Log = TheSettingManager->SettingsMain.Develop.LogLODFade != 0;
	const char* Name = (Node->m_pcName && Node->m_pcName[0]) ? Node->m_pcName : "?";

	if (Ring < MinRing) {
		if (Log) Logger::Log("[LODFade] skip %s@%08X ring=%d min=%d", Name, (UInt32)Node, Ring, MinRing);
		return;
	}
	if (RootIndex.count(Node)) return;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) {
		if (Log) Logger::Log("[LODFade] table full, %s@%08X pops", Name, (UInt32)Node);
		return;
	}

	Fades.push_back({ Node, NowMs });
	RebuildIndex();
	if (Log) Logger::Log("[LODFade] fade %s@%08X ring=%d live=%u", Name, (UInt32)Node, Ring, (UInt32)Fades.size());

}

void LODFadeManager::DrainPending() {

	for (auto it = Pending.begin(); it != Pending.end(); ) {
		NiAVObject* Node = it->first;
		if (!LiveChildren.count(Node) || NowMs - it->second > kPendingTimeoutMs) {
			it = Pending.erase(it);
			continue;
		}
		if (IsReady(Node)) {
			TryStartFade(Node);
			it = Pending.erase(it);
			continue;
		}
		++it;
	}

}

void LODFadeManager::RetireFinished() {

	bool Retired = false;
	for (size_t i = 0; i < Fades.size(); ) {
		if (GetAlpha(Fades[i]) < 1.0f) {
			i++;
			continue;
		}
		Fades[i] = Fades.back();
		Fades.pop_back();
		Retired = true;
	}
	if (Retired) RebuildIndex();

}

/// Any change to Fades invalidates every cached index, so the geometry cache is rebuilt lazily.
void LODFadeManager::RebuildIndex() {

	RootIndex.clear();
	for (UInt32 i = 0; i < Fades.size(); i++) RootIndex[Fades[i].Root] = i;
	GeometryCache.clear();

}

float LODFadeManager::GetAlpha(const FadeRecord& Record) const {

	double Duration = TheSettingManager->SettingsMain.LODFade.FadeTime * 1000.0;
	if (Duration <= 0.0) return 1.0f;

	double t = (NowMs - Record.StartMs) / Duration;
	if (t <= 0.0) return 0.0f;
	if (t >= 1.0) return 1.0f;
	return (float)t;

}

float LODFadeManager::GetFadeRemaining(NiAVObject* Geometry) {

	if (Fades.empty() || !Geometry) return 0.0f;

	int Index = -1;
	auto Cached = GeometryCache.find(Geometry);
	if (Cached != GeometryCache.end()) {
		Index = Cached->second;
	}
	else {
		NiAVObject* Node = Geometry;
		for (UInt32 Depth = 0; Node && Depth < kMaxParentDepth; Depth++) {
			auto Root = RootIndex.find(Node);
			if (Root != RootIndex.end()) {
				Index = (int)Root->second;
				break;
			}
			Node = Node->m_parent;
		}
		GeometryCache[Geometry] = Index;
	}

	if (Index < 0) return 0.0f;
	return 1.0f - GetAlpha(Fades[Index]);

}

void LODFadeManager::NoteCensusDraw(NiAVObject* Geometry, const char* PixelShaderName, const char* VertexShaderName, bool PixelHasProgram) {

	if (CensusStartMs == 0.0 || !DistantRef || !Geometry) return;

	NiAVObject* Child = NULL;
	NiAVObject* Node = Geometry;
	for (UInt32 Depth = 0; Node && Depth < kCensusMaxDepth; Depth++) {
		if (Node->m_parent == DistantRef) {
			Child = Node;
			break;
		}
		Node = Node->m_parent;
	}
	if (!Child) return;

	bool Tree = Child->m_pcName && !strcmp(Child->m_pcName, "LOD Trees");
	for (UInt32 i = 0; i < CensusCount; i++) {
		CensusEntry& Entry = Census[i];
		if (Entry.PixelShader == PixelShaderName && Entry.VertexShader == VertexShaderName && Entry.Tree == Tree) {
			Entry.Draws++;
			return;
		}
	}
	if (CensusCount >= kCensusMax) {
		CensusOverflow++;
		return;
	}
	Census[CensusCount++] = { PixelShaderName, VertexShaderName, PixelHasProgram, Tree, 1 };

}

void LODFadeManager::ReportCensus() {

	CensusArmed = false;
	Logger::Log("[LODFade] census: %u shader pairs drew under DistantRefLOD in %.0f s (overflow %u)",
		CensusCount, kCensusDelayMs / 1000.0, CensusOverflow);
	for (UInt32 i = 0; i < CensusCount; i++) {
		CensusEntry& Entry = Census[i];
		Logger::Log("[LODFade] census ps=%s vs=%s replaced=%d tree=%d draws=%u",
			Entry.PixelShader ? Entry.PixelShader : "(unnamed)",
			Entry.VertexShader ? Entry.VertexShader : "(unnamed)",
			Entry.PixelHasProgram ? 1 : 0, Entry.Tree ? 1 : 0, Entry.Draws);
	}

}
