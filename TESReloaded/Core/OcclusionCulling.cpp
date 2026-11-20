#include "OcclusionCulling.h"
#include "OcclusionCullMath.h"
#include <unordered_set>

using namespace OcclusionCullMath;

static const int	kRingSize			= 3;
static const UInt32	kNoQuery			= 0xFFFFFFFF;
static const UInt32	kRectVertexSize		= 16;
static const DWORD	kStatsIntervalMs	= 1000;

/// One queried reference: its (reference, root node) key and the index of its query in the slot.
struct QueryEntry {
	UInt64	Key;
	UInt32	Query;
};

/// One frame's queries: what they were issued for, which reference each tested, and the query pool
/// (grown on demand, kept across frames).
struct Slot {
	SlotMeta						Meta;
	std::vector<QueryEntry>			Entries;
	std::vector<IDirect3DQuery9*>	Queries;
};

/// A cullable reference in view this frame, found by the cell walk in BeginOcclusionCull.
struct Candidate {
	TESObjectREFR*	Ref;
	NiNode*			Node;
	UInt64			Key;
	NiBound*		Bound;
};

/// Per-far-pass counts, summed between stats lines.
struct Stats {
	UInt32	Frames;
	UInt32	Candidates;
	UInt32	AlwaysDrawn;
	UInt32	Queried;
	UInt32	Ready;
	UInt32	NotReady;
	UInt32	Hidden;
	UInt32	Culled;
	UInt32	Dropped;
	UInt32	Overflow;
};

static Slot							Ring[kRingSize];
static UInt32						Serial = 0;
static std::unordered_set<UInt64>	HiddenSet;
static std::vector<Candidate>		Candidates;
static std::vector<NiNode*>			CulledNodes;
static IDirect3DVertexBuffer9*		RectVB = NULL;
static UInt32						RectVBCapacity = 0;
static bool							IssueCameraValid = false;
static float						IssueCameraPos[3] = { 0.0f, 0.0f, 0.0f };
static bool							OverflowLogged = false;
static bool							DepthRowLogged = false;
static Stats						Totals = {};
static DWORD						StatsStart = 0;

static UInt64 MakeKey(TESObjectREFR* Ref, NiNode* Node) {
	return ((UInt64)(UInt32)Ref << 32) | (UInt32)Node;
}

static bool IsCandidateType(UInt8 TypeID) {
	switch (TypeID) {
	case TESForm::FormType::kFormType_Activator:
	case TESForm::FormType::kFormType_Apparatus:
	case TESForm::FormType::kFormType_Armor:
	case TESForm::FormType::kFormType_Book:
	case TESForm::FormType::kFormType_Clothing:
	case TESForm::FormType::kFormType_Container:
	case TESForm::FormType::kFormType_Door:
	case TESForm::FormType::kFormType_Ingredient:
	case TESForm::FormType::kFormType_Misc:
	case TESForm::FormType::kFormType_Stat:
	case TESForm::FormType::kFormType_Flora:
	case TESForm::FormType::kFormType_Furniture:
	case TESForm::FormType::kFormType_Weapon:
	case TESForm::FormType::kFormType_Ammo:
	case TESForm::FormType::kFormType_SoulGem:
	case TESForm::FormType::kFormType_Key:
	case TESForm::FormType::kFormType_AlchemyItem:
	case TESForm::FormType::kFormType_SigilStone:
		return true;
	default:
		return false;
	}
}

static NiCamera* SceneCamera() {
	return WorldSceneGraph ? WorldSceneGraph->camera : NULL;
}

/// The main camera as the far pass uses it: ApplyPass(PassFar) has already put the far pass's depth
/// range into the frustum, and it stays there until the shell's ApplyPass(PassNear).
static CullCamera BuildCamera(NiCamera* Camera) {
	CullCamera C;
	NiMatrix33* R = &Camera->m_worldTransform.rot;
	NiPoint3* P = &Camera->m_worldTransform.pos;
	NiFrustum* F = &Camera->Frustum;
	C.Pos[0] = P->x; C.Pos[1] = P->y; C.Pos[2] = P->z;
	for (int i = 0; i < 3; i++) {
		C.Forward[i] = R->data[i][0];
		C.Up[i] = R->data[i][1];
		C.Right[i] = R->data[i][2];
	}
	C.Left = F->Left; C.RightEdge = F->Right; C.Top = F->Top; C.Bottom = F->Bottom;
	C.Near = F->Near; C.Far = F->Far;
	return C;
}

static const void* CurrentWorldKey() {
	TESWorldSpace* World = Player->GetWorldSpace();
	return World ? (const void*)World : (const void*)Player->parentCell;
}

static void RestoreCulledNodes();

static void ReleaseResources() {
	RestoreCulledNodes();
	for (int s = 0; s < kRingSize; s++) {
		for (IDirect3DQuery9* Query : Ring[s].Queries) Query->Release();
		Ring[s].Queries.clear();
		Ring[s].Entries.clear();
		Ring[s].Meta.Valid = false;
	}
	if (RectVB) { RectVB->Release(); RectVB = NULL; }
	RectVBCapacity = 0;
	HiddenSet.clear();
	Candidates.clear();
	IssueCameraValid = false;
	OverflowLogged = false;
	DepthRowLogged = false;
}

static bool HoldsResources() {
	return RectVB || !Ring[0].Queries.empty() || !Ring[1].Queries.empty() || !Ring[2].Queries.empty() || !HiddenSet.empty();
}

/// Turns last frame's queries into HiddenSet. Returns false (and leaves HiddenSet empty) when those
/// results may not be used this frame.
static bool ReadPreviousResults(const float CameraPos[3], const void* WorldKey, Stats& Frame) {
	Slot& Previous = Ring[(Serial - 1) % kRingSize];
	HiddenSet.clear();
	if (!ResultsUsable(Previous.Meta, Serial, CameraPos, WorldKey, TheSettingManager->SettingsMain.Main.OcclusionCameraCutDistance)) return false;
	for (const QueryEntry& Entry : Previous.Entries) {
		DWORD Pixels = 0;
		HRESULT hr = Previous.Queries[Entry.Query]->GetData(&Pixels, sizeof(DWORD), 0);
		if (hr == S_OK) {
			Frame.Ready++;
			if (Pixels == 0) HiddenSet.insert(Entry.Key);
		}
		else if (hr == S_FALSE) Frame.NotReady++;
	}
	return true;
}

/// Appends one cell's candidates, counting the references that are always drawn instead. With Cull
/// set, a candidate hidden in the last far pass gets AppCulled on its root, remembered in CulledNodes.
/// Roots already AppCulled (disabled, or culled by the engine) are never candidates, so End never
/// clears a flag the engine set.
static void CollectCell(TESObjectCELL* Cell, const CullCamera& Camera, bool Cull, Stats& Frame) {
	if (!Cell) return;
	SettingsMainStruct::MainStruct* Settings = &TheSettingManager->SettingsMain.Main;
	bool HasWater = TheShaderManager->ShaderConst.HasWater;
	float WaterHeight = TheShaderManager->ShaderConst.Water.waterSettings.x;
	for (TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First; Entry; Entry = Entry->next) {
		TESObjectREFR* Ref = Entry->item;
		NiNode* Node;
		if (!Ref || Ref == Player || !(Node = Ref->GetNode())) continue;
		if (Node->m_flags & NiAVObject::kFlag_AppCulled) continue;
		if (!Ref->baseForm || !IsCandidateType(Ref->baseForm->formType)) continue;
		NiBound* Bound = Node->GetWorldBound();
		if (!Bound || Bound->Radius <= 0.0f) continue;
		float Center[3] = { Bound->Center.x, Bound->Center.y, Bound->Center.z };
		if (!SphereInView(Camera, Center, Bound->Radius)) continue;
		float D[3] = { Center[0] - Camera.Pos[0], Center[1] - Camera.Pos[1], Center[2] - Camera.Pos[2] };
		float DistToSurface = std::sqrt(Dot3(D, D)) - Bound->Radius;
		if (ClassifyCandidate(DistToSurface, Settings->OcclusionAlwaysDrawDistance, HasWater, Center[2] - Bound->Radius, WaterHeight) == Mode_AlwaysDraw) {
			Frame.AlwaysDrawn++;
			continue;
		}
		UInt64 Key = MakeKey(Ref, Node);
		Candidates.push_back({ Ref, Node, Key, Bound });
		if (!HiddenSet.count(Key)) continue;
		Frame.Hidden++;
		if (!Cull) continue;
		Node->m_flags |= NiAVObject::kFlag_AppCulled;
		CulledNodes.push_back(Node);
		Frame.Culled++;
	}
}

static void LogStats(const Stats& Frame) {
	Totals.Frames++;
	Totals.Candidates += Frame.Candidates;
	Totals.AlwaysDrawn += Frame.AlwaysDrawn;
	Totals.Ready += Frame.Ready;
	Totals.NotReady += Frame.NotReady;
	Totals.Hidden += Frame.Hidden;
	Totals.Culled += Frame.Culled;
	Totals.Dropped += Frame.Dropped;
	DWORD Now = GetTickCount();
	if (!StatsStart) StatsStart = Now;
	if (Now - StatsStart < kStatsIntervalMs) return;
	float N = (float)Totals.Frames;
	Logger::Log("[Occlusion] per far pass over %u: candidates %.0f, always drawn %.0f, queried %.0f, ready %.0f, not ready %.0f, hidden %.0f, culled %.0f; dropped frames %u, overflow frames %u",
		Totals.Frames, Totals.Candidates / N, Totals.AlwaysDrawn / N, Totals.Queried / N, Totals.Ready / N, Totals.NotReady / N,
		Totals.Hidden / N, Totals.Culled / N, Totals.Dropped, Totals.Overflow);
	Totals = {};
	StatsStart = Now;
}

void BeginOcclusionCull() {

	Candidates.clear();
	if (!TheSettingManager->SettingsMain.Main.OcclusionCulling) {
		if (HoldsResources()) ReleaseResources();
		return;
	}
	NiCamera* Camera = SceneCamera();
	if (!Player || !Tes || !Camera) return;

	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_OccBegin);
	UInt8 Debug = TheSettingManager->SettingsMain.Develop.OcclusionCullDebug;
	Stats Frame = {};
	CullCamera Cam = BuildCamera(Camera);
	const void* WorldKey = CurrentWorldKey();

	bool Cull = Debug != 3;
	Serial++;
	if (Debug != 2 && !ReadPreviousResults(Cam.Pos, WorldKey, Frame)) Frame.Dropped = 1;

	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++)
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++)
				CollectCell(Tes->gridCellArray->GetCell(x, y), Cam, Cull, Frame);
	}
	else {
		CollectCell(Player->parentCell, Cam, Cull, Frame);
	}

	Frame.Candidates = (UInt32)Candidates.size();
	FrameProfiler::Count(FrameProfiler::Cnt_OccCandidates, Frame.Candidates);
	FrameProfiler::Count(FrameProfiler::Cnt_OccHidden, Frame.Hidden);
	FrameProfiler::Count(FrameProfiler::Cnt_OccCulled, Frame.Culled);
	if (Debug) LogStats(Frame);

}

static void RestoreCulledNodes() {
	for (NiNode* Node : CulledNodes) Node->m_flags &= ~NiAVObject::kFlag_AppCulled;
	CulledNodes.clear();
}

void EndOcclusionCull() {
	if (CulledNodes.empty()) return;
	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_OccEnd);
	RestoreCulledNodes();
}

static bool EnsureRectBuffer(IDirect3DDevice9* Device, UInt32 Capacity) {
	if (RectVB && RectVBCapacity == Capacity) return true;
	if (RectVB) { RectVB->Release(); RectVB = NULL; }
	RectVBCapacity = 0;
	if (FAILED(Device->CreateVertexBuffer(Capacity * 4 * kRectVertexSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW, D3DPOOL_DEFAULT, &RectVB, NULL))) {
		RectVB = NULL;
		return false;
	}
	RectVBCapacity = Capacity;
	return true;
}

static void LogDepthRow(IDirect3DDevice9* Device, const CullCamera& Cam) {
	DepthRowLogged = true;
	D3DMATRIX Proj;
	if (FAILED(Device->GetTransform(D3DTS_PROJECTION, &Proj))) return;
	float Row33 = Cam.Far / (Cam.Far - Cam.Near);
	Logger::Log("[Occlusion] depth row: ours _33 %.6f _43 %.4f (near %.3f far %.1f), device _33 %.6f _43 %.4f",
		Row33, -Cam.Near * Row33, Cam.Near, Cam.Far, Proj._33, Proj._43);
}

void IssueOcclusionQueries() {

	SettingsMainStruct::MainStruct* Settings = &TheSettingManager->SettingsMain.Main;
	if (!Settings->OcclusionCulling || Candidates.empty()) return;
	NiCamera* Camera = SceneCamera();
	IDirect3DDevice9* Device = TheRenderManager->device;
	if (!Camera || !Device) return;

	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_OccIssue);
	Slot& Current = Ring[Serial % kRingSize];
	Current.Entries.clear();
	Current.Meta.Valid = false;

	UInt32 Capacity = Settings->OcclusionMaxQueries;
	if (!EnsureRectBuffer(Device, Capacity)) return;
	CullCamera Cam = BuildCamera(Camera);
	UInt8 Debug = TheSettingManager->SettingsMain.Develop.OcclusionCullDebug;
	if (Debug && !DepthRowLogged) LogDepthRow(Device, Cam);

	float Move = 0.0f;
	if (IssueCameraValid) {
		float D[3] = { Cam.Pos[0] - IssueCameraPos[0], Cam.Pos[1] - IssueCameraPos[1], Cam.Pos[2] - IssueCameraPos[2] };
		Move = min(std::sqrt(Dot3(D, D)), Settings->OcclusionCameraCutDistance);
	}
	memcpy(IssueCameraPos, Cam.Pos, sizeof(IssueCameraPos));
	IssueCameraValid = true;

	D3DVIEWPORT9 Viewport;
	if (FAILED(Device->GetViewport(&Viewport))) return;

	float* Vertex = NULL;
	if (FAILED(RectVB->Lock(0, 0, (void**)&Vertex, D3DLOCK_DISCARD)) || !Vertex) return;
	bool Overflow = false;
	for (const Candidate& C : Candidates) {
		if (Current.Entries.size() >= Capacity) { Overflow = true; break; }
		float Center[3] = { C.Bound->Center.x, C.Bound->Center.y, C.Bound->Center.z };
		QueryRect Rect;
		if (ProjectBoundRect(Cam, Center, C.Bound->Radius + Move, (float)Viewport.X, (float)Viewport.Y, (float)Viewport.Width, (float)Viewport.Height, &Rect) != Rect_Ok) continue;
		UInt32 Index = (UInt32)Current.Entries.size();
		if (Index == Current.Queries.size()) {
			IDirect3DQuery9* Query = NULL;
			if (FAILED(Device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &Query)) || !Query) break;
			Current.Queries.push_back(Query);
		}
		const float Corners[4][2] = { { Rect.X0, Rect.Y0 }, { Rect.X1, Rect.Y0 }, { Rect.X0, Rect.Y1 }, { Rect.X1, Rect.Y1 } };
		for (int v = 0; v < 4; v++) {
			*Vertex++ = Corners[v][0];
			*Vertex++ = Corners[v][1];
			*Vertex++ = Rect.Z;
			*Vertex++ = 1.0f;
		}
		Current.Entries.push_back({ C.Key, Index });
	}
	RectVB->Unlock();
	if (Overflow) {
		Totals.Overflow++;
		if (!OverflowLogged) {
			OverflowLogged = true;
			Logger::Log("[Occlusion] more than OcclusionMaxQueries (%u) references to query; the rest are drawn.", Capacity);
		}
	}
	if (Current.Entries.empty() || !TheShaderManager->CaptureDeviceState()) {
		Current.Entries.clear();
		return;
	}

	Device->SetVertexShader(NULL);
	Device->SetPixelShader(NULL);
	Device->SetFVF(D3DFVF_XYZRHW);
	Device->SetStreamSource(0, RectVB, 0, kRectVertexSize);
	Device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
	Device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
	Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	Device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	Device->SetRenderState(D3DRS_DEPTHBIAS, 0);
	Device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
	for (const QueryEntry& Entry : Current.Entries) {
		IDirect3DQuery9* Query = Current.Queries[Entry.Query];
		Query->Issue(D3DISSUE_BEGIN);
		Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, Entry.Query * 4, 2);
		Query->Issue(D3DISSUE_END);
	}
	TheShaderManager->CachedStateBlock->Apply();

	Current.Meta.Serial = Serial;
	memcpy(Current.Meta.CameraPos, Cam.Pos, sizeof(Current.Meta.CameraPos));
	Current.Meta.WorldKey = CurrentWorldKey();
	Current.Meta.Valid = true;
	Totals.Queried += (UInt32)Current.Entries.size();
	FrameProfiler::Count(FrameProfiler::Cnt_OccQueried, (unsigned int)Current.Entries.size());

}

void ResetOcclusionCull() {
	for (int s = 0; s < kRingSize; s++) {
		Ring[s].Entries.clear();
		Ring[s].Meta.Valid = false;
	}
	HiddenSet.clear();
	IssueCameraValid = false;
}

bool OcclusionCullFlagsHeld() {
	return !CulledNodes.empty();
}
