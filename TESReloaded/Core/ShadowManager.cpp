#include <algorithm>
#include <cmath>
#include <list>
#include <string>
#include <thread>
#include <sstream>
#if defined(NEWVEGAS)
#define RenderStateArgs 0, 0
#define kRockParams 0x01200658
#define kRustleParams 0x01200668
#define kWindMatrixes 0x01200688
#define kShadowSceneNode 0x011F91C8
static const UInt32 kRenderShadowMapHook = 0x00870C39;
static const UInt32 kRenderShadowMapReturn = 0x00870C41;
static const UInt32 kAddCastShadowFlagHook = 0x0050DD06;
static const UInt32 kAddCastShadowFlagReturn = 0x0050DD0B;
static const UInt32 kEditorCastShadowFlagHook = 0x00000000;
static const UInt32 kEditorCastShadowFlagReturn = 0x00000000;
static const UInt32 kLeavesNodeName = 0x0066A115;
static const UInt32 kLeavesNodeNameReturn = 0x0066A11E;
static const void* VFTNiNode = (void*)0x0109B5AC;
static const void* VFTBSFadeNode = (void*)0x010A8F90;
static const void* VFTBSFaceGenNiNode = (void*)0x010660DC;
static const void* VFTBSTreeNode = (void*)0x010668E4;
static const void* VFTNiTriShape = (void*)0x0109D454;
static const void* VFTNiTriStrips = (void*)0x0109CD44;
static const void* VFTNiLODNode = (void*)0x00000000; // TODO: NewVegas/Skyrim NiLODNode vtable unknown; this fork builds OBLIVION only
#elif defined(OBLIVION)
#define RenderStateArgs 0
#define kRockParams 0x00B46778
#define kRustleParams 0x00B46788
#define kWindMatrixes 0x00B467B8
#define kShadowSceneNode 0x00B42F54
static const UInt32 kRenderShadowMapHook = 0x0040C919;
static const UInt32 kRenderShadowMapReturn = 0x0040C920;
static const UInt32 kAddCastShadowFlagHook = 0x004B1A25;
static const UInt32 kAddCastShadowFlagReturn = 0x004B1A2A;
static const UInt32 kEditorCastShadowFlagHook = 0x005498DD;
static const UInt32 kEditorCastShadowFlagReturn = 0x005498E3;
static const void* VFTNiNode = (void*)0x00A7E38C;
static const void* VFTBSFadeNode = (void*)0x00A3F944;
static const void* VFTBSFaceGenNiNode = (void*)0x00A64F5C;
static const void* VFTBSTreeNode = (void*)0x00A65854;
static const void* VFTNiTriShape = (void*)0x00A7ED5C;
static const void* VFTNiTriStrips = (void*)0x00A7F27C;
static const void* VFTNiLODNode = (void*)0x00A7F97C; // NiLODNode (NiSwitchNode/NiNode subclass) — holds LOD levels; must be recursed so LOD meshes cast shadows
#endif
#define ShadowMapObjectMinBound 10.0f
#define ShadowInstanceStride   48 // 3 float4 columns of the world matrix per instance
#define ShadowInstanceMinCount 4  // minimum group size worth batching via hardware instancing

#if defined(NEWVEGAS) || defined(OBLIVION)

// --- Lightweight per-phase CPU profiler (Develop.ProfileShadows) --------------
// Wall-clock (QueryPerformanceCounter) timing of the shadow phases, accumulated
// across frames and averaged to the log every ReportFrames frames. Phases that
// issue D3D9 draws measure CPU *submission* cost (driver call overhead), NOT GPU
// execution — which is exactly the signal we want for deciding which scene-graph
// traversal / cull / matrix work is worth moving off the render thread. The RAII
// scope does no QPC calls when disabled, so it is free in normal builds.
namespace {
	enum ShadowPhase {
		Phase_BuildGeoItems,   // BuildExteriorGeoItems: grid walk + flatten + matrices (pure CPU)
		Phase_PassNear,        // RenderShadowMap(MapNear):  cull + draw submission
		Phase_PassFar,         // RenderShadowMap(MapFar)
		Phase_PassOrtho,       // RenderShadowMap(MapOrtho)
		Phase_PassSkin,        // RenderShadowMap(MapSkin)
		Phase_ExtTotal,        // whole RenderExteriorShadows
		Phase_IntClassify,     // cube-map ref gathering + per-light classification (pure CPU)
		Phase_IntCubeRender,   // cube-map face draw submission
		Phase_IntTotal,        // whole RenderInteriorShadows
		Phase_FrameTotal,      // exterior + interior for one frame
		Phase_COUNT
	};
	const char* const ShadowPhaseNames[Phase_COUNT] = {
		"BuildGeoItems", "Pass:Near", "Pass:Far", "Pass:Ortho", "Pass:Skin",
		"ExtTotal", "Int:Classify", "Int:CubeRender", "IntTotal", "FrameTotal"
	};
	// Per-frame counters: how the submission cost breaks down (draws, batches, cache hits).
	enum ShadowCounter {
		Cnt_DrawCalls,        // DrawIndexedPrimitive calls issued (all shadow paths)
		Cnt_DrawCallsCube,    // ...of which issued by the cube/point-light path (statics + actors)
		Cnt_DrawCallsCubeActor, // ...of which the actor sub-path (RenderActorFaces, all 6 faces uncull'd)
		Cnt_RenderCallsCube,  // Render() invocations in the cube path (= STATIC meshes drawn, summed over 6 faces)
		Cnt_CubeActorGeo,     // RenderActor invocations (= actor sub-geometries drawn, ×6 faces each)
		Cnt_InstancedDraws,   // directional instanced-batch draws (each batches many objects)
		Cnt_CubeLightsDrawn,  // cube lights redrawn this frame (6 faces each)
		Cnt_CubeLightsCached, // cube lights skipped via the static-map tracker
		// Directional (Near/Far/Ortho/Skin) instancing diagnostics, summed over the 4 passes:
		Cnt_DirTerrainDraws,    // DrawIndexedPrimitive issued by terrain (never instanced)
		Cnt_DirItemsInstanced,  // ref items routed to AddInstance (instanceable, not alpha-excluded)
		Cnt_DirItemsImmNonInst, // ref items drawn immediately because NOT instanceable (skinned/leaf/no decl)
		Cnt_DirItemsImmAlpha,   // ref items drawn immediately because alpha-masked in an alpha pass
		Cnt_DirGroups,          // distinct instance groups formed (= unique repeated meshes)
		Cnt_DirInstancedItems,  // items actually covered by an instanced batch (group >= min, has decl)
		Cnt_DirFallbackItems,   // instanceable items drawn per-object anyway (group < min, or no decl)
		Cnt_COUNT
	};
	const char* const ShadowCounterNames[Cnt_COUNT] = {
		"DrawCalls", "DrawCalls:Cube", "DrawCalls:CubeActor", "RenderCalls:Cube", "CubeActorGeo",
		"InstancedDraws", "CubeLightsDrawn", "CubeLightsCached",
		"Dir:TerrainDraws", "Dir:ItemsInstanced", "Dir:ItemsImmNonInst", "Dir:ItemsImmAlpha",
		"Dir:Groups", "Dir:InstancedItems", "Dir:FallbackItems"
	};
	// When true, draw/Render counters attribute to the cube/point-light path (set around its submission).
	bool gCubeBucket = false;
	bool gCubeActorBucket = false; // narrower: the actor sub-path within the cube path
	bool gTerrainBucket = false;   // directional terrain submission (for Cnt_DirTerrainDraws)

	struct PhaseAccum { double Ms; UInt32 Calls; };
	PhaseAccum gPhase[Phase_COUNT] = {};
	UInt32     gCounter[Cnt_COUNT] = {};
	LONGLONG   gQpcFreq = 0;
	int        gFrames = 0;
	const int  ReportFrames = 600;
	bool       ProfilingEnabled = false;

	inline LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
	inline void ProfileCount(ShadowCounter c, UInt32 n = 1) { if (ProfilingEnabled) gCounter[c] += n; }

	struct ScopeTimer {
		ShadowPhase Phase;
		LONGLONG    Start;
		ScopeTimer(ShadowPhase p) : Phase(p), Start(ProfilingEnabled ? QpcNow() : 0) {}
		~ScopeTimer() {
			if (!ProfilingEnabled) return;
			gPhase[Phase].Ms += (double)(QpcNow() - Start) * 1000.0 / (double)gQpcFreq;
			gPhase[Phase].Calls++;
		}
	};

	void ShadowProfileFrameBegin() {
		ProfilingEnabled = TheSettingManager->SettingsMain.Develop.ProfileShadows != 0;
		if (ProfilingEnabled && gQpcFreq == 0) {
			LARGE_INTEGER f; QueryPerformanceFrequency(&f); gQpcFreq = f.QuadPart;
		}
	}

	void ShadowProfileFrameEnd() {
		if (!ProfilingEnabled || ++gFrames < ReportFrames) return;
		Logger::Log("[ShadowProfile] avg over %d frames  (ms/frame | ms/call | calls/frame)", gFrames);
		for (int i = 0; i < Phase_COUNT; i++) {
			double msFrame  = gPhase[i].Ms / gFrames;
			double msCall   = gPhase[i].Calls ? gPhase[i].Ms / gPhase[i].Calls : 0.0;
			double callRate = (double)gPhase[i].Calls / gFrames;
			Logger::Log("[ShadowProfile]   %-14s %8.4f | %8.4f | %6.2f", ShadowPhaseNames[i], msFrame, msCall, callRate);
			gPhase[i].Ms = 0.0; gPhase[i].Calls = 0;
		}
		Logger::Log("[ShadowProfile] counts (per frame):");
		for (int i = 0; i < Cnt_COUNT; i++) {
			Logger::Log("[ShadowProfile]   %-16s %9.2f", ShadowCounterNames[i], (double)gCounter[i] / gFrames);
			gCounter[i] = 0;
		}
		gFrames = 0;
	}
}

void ShadowManager::InitShadowBiasConstants() {
	auto& Ext = TheSettingManager->SettingsShadows.Exteriors;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasForward.x  = Ext.forwardNormBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasForward.y  = Ext.forwardFarNormBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasForward.z  = Ext.forwardConstBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasForward.w  = Ext.forwardFarConstBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasDeferred.x = Ext.deferredNormBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasDeferred.y = Ext.deferredFarNormBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasDeferred.z = Ext.deferredConstBias;
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasDeferred.w = Ext.deferredFarConstBias;
}

void ShadowManager::LoadShadowShaders(IDirect3DDevice9* Device) {
	ShadowMapVertex = new ShaderRecord();
	if (ShadowMapVertex->LoadShader("ShadowMap.vso")) Device->CreateVertexShader((const DWORD*)ShadowMapVertex->Function, &ShadowMapVertexShader);
	ShadowMapPixel = new ShaderRecord();
	if (ShadowMapPixel->LoadShader("ShadowMap.pso")) Device->CreatePixelShader((const DWORD*)ShadowMapPixel->Function, &ShadowMapPixelShader);
	ShadowCubeMapVertex = new ShaderRecord();
	if (ShadowCubeMapVertex->LoadShader("ShadowCubeMap.vso")) Device->CreateVertexShader((const DWORD*)ShadowCubeMapVertex->Function, &ShadowCubeMapVertexShader);
	ShadowCubeMapPixel = new ShaderRecord();
	if (ShadowCubeMapPixel->LoadShader("ShadowCubeMap.pso")) Device->CreatePixelShader((const DWORD*)ShadowCubeMapPixel->Function, &ShadowCubeMapPixelShader);
	ShadowCubeMapExteriorPixel = new ShaderRecord();
	if (ShadowCubeMapExteriorPixel->LoadShader("ShadowCubeMapExterior.pso")) Device->CreatePixelShader((const DWORD*)ShadowCubeMapExteriorPixel->Function, &ShadowCubeMapExteriorPixelShader);
	// Optional: instanced static shadow VS. If the binary is missing (not yet compiled), the
	// handle stays NULL and the renderer transparently falls back to the per-object path.
	ShadowMapInstancedVertex = new ShaderRecord();
	if (ShadowMapInstancedVertex->LoadShader("ShadowMapInstanced.vso")) Device->CreateVertexShader((const DWORD*)ShadowMapInstancedVertex->Function, &ShadowMapInstancedVertexShader);
}

void ShadowManager::CreateShadowMapSurfaces(IDirect3DDevice9* Device, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors) {
	// SHADOWS DISABLED: only the ortho map is still rendered (precipitation dependency). The
	// Near/Far/Skin maps (incl. 4096^2 Near/Skin) are left unallocated to reclaim VRAM.
	// TextureManager captures the NULL handle at shader-load and the bind path
	// (ShaderManager: `if (Value->Texture->Texture)`) skips NULL textures; the neutered
	// scene shaders never sample them anyway.
	for (int i = 0; i < 4; i++) {
		ShadowMapTexture[i] = NULL;
		ShadowMapSurface[i] = NULL;
		ShadowMapDepthSurface[i] = NULL;
		ShadowMapViewPort[i] = { 0, 0, 0, 0, 0.0f, 1.0f };
	}
	int Ortho = ShadowMapTypeEnum::MapOrtho;
	UINT ShadowMapSize = ShadowsExteriors->ShadowMapSize[Ortho];
	Device->CreateTexture(ShadowMapSize, ShadowMapSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &ShadowMapTexture[Ortho], NULL);
	ShadowMapTexture[Ortho]->GetSurfaceLevel(0, &ShadowMapSurface[Ortho]);
	Device->CreateDepthStencilSurface(ShadowMapSize, ShadowMapSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowMapDepthSurface[Ortho], NULL);
	ShadowMapViewPort[Ortho] = { 0, 0, ShadowMapSize, ShadowMapSize, 0.0f, 1.0f };

	// Directional sun maps (reimplemented). Near = crisp small radius, Far = wide coarse. Skin = the
	// per-frame actor overlay (Task 9), drawn against the near region's baked projection. R32F color
	// + D24S8 depth, matching the ortho map's formats.
	for (int m = ShadowMapTypeEnum::MapNear; m <= ShadowMapTypeEnum::MapSkin; m++) {
		if (m == ShadowMapTypeEnum::MapOrtho) continue; // allocated separately above
		UINT Size = ShadowsExteriors->ShadowMapSize[m];
		if (!Size) continue;
		Device->CreateTexture(Size, Size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &ShadowMapTexture[m], NULL);
		ShadowMapTexture[m]->GetSurfaceLevel(0, &ShadowMapSurface[m]);
		Device->CreateDepthStencilSurface(Size, Size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowMapDepthSurface[m], NULL);
		ShadowMapViewPort[m] = { 0, 0, Size, Size, 0.0f, 1.0f };
	}
}

void ShadowManager::CreateCubeMapSurfaces(IDirect3DDevice9* Device, UINT CubeMapSize) {
	// SHADOWS DISABLED: point-light cube maps are no longer generated; leave all 12 unallocated
	// to reclaim VRAM (12 * 6 faces of R32F + depth). TextureManager captures NULL and the bind
	// path skips it; the neutered scene shaders never sample these. Dead reference: original
	// allocation in the #if 0 block below.
	for (int i = 0; i < 12; i++) {
		ShadowCubeMapTexture[i] = NULL;
		for (int j = 0; j < 6; j++) {
			ShadowCubeMapSurface[i][j] = NULL;
			ShadowCubeMapDepthSurface[i][j] = NULL;
		}
	}
#if 0 // SHADOWS DISABLED: original cube-map allocation (dead reference)
	for (int i = 0; i < 12; i++) {
		Device->CreateCubeTexture(CubeMapSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &ShadowCubeMapTexture[i], NULL);
		for (int j = 0; j < 6; j++) {
			ShadowCubeMapTexture[i]->GetCubeMapSurface((D3DCUBEMAP_FACES)j, 0, &ShadowCubeMapSurface[i][j]);
			Device->CreateDepthStencilSurface(CubeMapSize, CubeMapSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowCubeMapDepthSurface[i][j], NULL);
		}
	}
#endif // SHADOWS DISABLED
}

ShadowManager::ShadowManager() {
	Logger::Log("Starting the shadows manager...");
	TheShadowManager = this;

	IDirect3DDevice9* Device = TheRenderManager->device;
	SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors = &TheSettingManager->SettingsShadows.Exteriors;
	SettingsShadowStruct::InteriorsStruct* ShadowsInteriors = &TheSettingManager->SettingsShadows.Interiors;
	SettingsShadowStruct::InteriorsStruct* ShadowsExteriorsPoint = &TheSettingManager->SettingsShadows.ExteriorsPoint;

	InitShadowBiasConstants();

	//TODO: should this setting be on it's own? choose smaller of two for now
	UINT ShadowCubeMapSize = min(ShadowsInteriors->ShadowCubeMapSize, ShadowsExteriorsPoint->ShadowCubeMapSize);

	CurrentCell = NULL;
	ShadowCubeMapState = ShadowCubeMapStateEnum::None;
	ShadowMapInstancedVertexShader = NULL;
	InstanceVB = NULL;
	InstanceVBCapacity = 0;
	InstanceGroupCount = 0;
	ShadowGeoCount = 0;
	CollectWorldSpace = false;
	CollectAnchor = D3DXVECTOR3(0.0f, 0.0f, 0.0f);

	for (int i = 0; i < 2; i++) { Regions[i].Valid = false; D3DXMatrixIdentity(&Regions[i].BakedViewProj); }

	LoadShadowShaders(Device);
	CreateShadowMapSurfaces(Device, ShadowsExteriors);
	CreateCubeMapSurfaces(Device, ShadowCubeMapSize);

	ShadowCubeMapViewPort = { 0, 0, ShadowCubeMapSize, ShadowCubeMapSize, 0.0f, 1.0f };
	ShadowCubeMapLights[12] = { NULL };

	ResetIntervals();
}

void ShadowManager::CreateD3DMatrix(D3DMATRIX* Matrix, NiTransform* Transform) {

	NiMatrix33* Rot = &Transform->rot;
	NiPoint3* Pos = &Transform->pos;
	float Scale = Transform->scale;

	Matrix->_11 = Rot->data[0][0] * Scale;
	Matrix->_12 = Rot->data[1][0] * Scale;
	Matrix->_13 = Rot->data[2][0] * Scale;
	Matrix->_14 = 0.0f;
	Matrix->_21 = Rot->data[0][1] * Scale;
	Matrix->_22 = Rot->data[1][1] * Scale;
	Matrix->_23 = Rot->data[2][1] * Scale;
	Matrix->_24 = 0.0f;
	Matrix->_31 = Rot->data[0][2] * Scale;
	Matrix->_32 = Rot->data[1][2] * Scale;
	Matrix->_33 = Rot->data[2][2] * Scale;
	Matrix->_34 = 0.0f;
	Matrix->_41 = Pos->x - TheRenderManager->CameraPosition.x;
	Matrix->_42 = Pos->y - TheRenderManager->CameraPosition.y;
	Matrix->_43 = Pos->z - TheRenderManager->CameraPosition.z;
	Matrix->_44 = 1.0f;

}

// World-space variant of CreateD3DMatrix (no camera subtraction) for cached bakes rendered against a
// world-anchored ShadowViewProj.
void ShadowManager::CreateD3DMatrixWorld(D3DMATRIX* Matrix, NiTransform* Transform) {
	NiMatrix33* Rot = &Transform->rot;
	NiPoint3* Pos = &Transform->pos;
	float Scale = Transform->scale;
	Matrix->_11 = Rot->data[0][0] * Scale; Matrix->_12 = Rot->data[1][0] * Scale; Matrix->_13 = Rot->data[2][0] * Scale; Matrix->_14 = 0.0f;
	Matrix->_21 = Rot->data[0][1] * Scale; Matrix->_22 = Rot->data[1][1] * Scale; Matrix->_23 = Rot->data[2][1] * Scale; Matrix->_24 = 0.0f;
	Matrix->_31 = Rot->data[0][2] * Scale; Matrix->_32 = Rot->data[1][2] * Scale; Matrix->_33 = Rot->data[2][2] * Scale; Matrix->_34 = 0.0f;
	// Anchor-relative translation (not absolute world) so cached-bake geometry stays near the origin.
	Matrix->_41 = Pos->x - CollectAnchor.x; Matrix->_42 = Pos->y - CollectAnchor.y; Matrix->_43 = Pos->z - CollectAnchor.z; Matrix->_44 = 1.0f;
}

void ShadowManager::GetShadowFrustum(ShadowMapTypeEnum ShadowMapType, D3DMATRIX* Matrix) {

	ShadowMapFrustum[ShadowMapType][PlaneNear].a = Matrix->_13;
	ShadowMapFrustum[ShadowMapType][PlaneNear].b = Matrix->_23;
	ShadowMapFrustum[ShadowMapType][PlaneNear].c = Matrix->_33;
	ShadowMapFrustum[ShadowMapType][PlaneNear].d = Matrix->_43;
	ShadowMapFrustum[ShadowMapType][PlaneFar].a = Matrix->_14 - Matrix->_13;
	ShadowMapFrustum[ShadowMapType][PlaneFar].b = Matrix->_24 - Matrix->_23;
	ShadowMapFrustum[ShadowMapType][PlaneFar].c = Matrix->_34 - Matrix->_33;
	ShadowMapFrustum[ShadowMapType][PlaneFar].d = Matrix->_44 - Matrix->_43;
	ShadowMapFrustum[ShadowMapType][PlaneLeft].a = Matrix->_14 + Matrix->_11;
	ShadowMapFrustum[ShadowMapType][PlaneLeft].b = Matrix->_24 + Matrix->_21;
	ShadowMapFrustum[ShadowMapType][PlaneLeft].c = Matrix->_34 + Matrix->_31;
	ShadowMapFrustum[ShadowMapType][PlaneLeft].d = Matrix->_44 + Matrix->_41;
	ShadowMapFrustum[ShadowMapType][PlaneRight].a = Matrix->_14 - Matrix->_11;
	ShadowMapFrustum[ShadowMapType][PlaneRight].b = Matrix->_24 - Matrix->_21;
	ShadowMapFrustum[ShadowMapType][PlaneRight].c = Matrix->_34 - Matrix->_31;
	ShadowMapFrustum[ShadowMapType][PlaneRight].d = Matrix->_44 - Matrix->_41;
	ShadowMapFrustum[ShadowMapType][PlaneTop].a = Matrix->_14 - Matrix->_12;
	ShadowMapFrustum[ShadowMapType][PlaneTop].b = Matrix->_24 - Matrix->_22;
	ShadowMapFrustum[ShadowMapType][PlaneTop].c = Matrix->_34 - Matrix->_32;
	ShadowMapFrustum[ShadowMapType][PlaneTop].d = Matrix->_44 - Matrix->_42;
	ShadowMapFrustum[ShadowMapType][PlaneBottom].a = Matrix->_14 + Matrix->_12;
	ShadowMapFrustum[ShadowMapType][PlaneBottom].b = Matrix->_24 + Matrix->_22;
	ShadowMapFrustum[ShadowMapType][PlaneBottom].c = Matrix->_34 + Matrix->_32;
	ShadowMapFrustum[ShadowMapType][PlaneBottom].d = Matrix->_44 + Matrix->_42;
	for (int i = 0; i < 6; ++i) {
		D3DXPLANE Plane(ShadowMapFrustum[ShadowMapType][i]);
		D3DXPlaneNormalize(&ShadowMapFrustum[ShadowMapType][i], &Plane);
	}

}

#if 0 // SHADOWS DISABLED: dead reference — cube/point-light frustum helpers (unused by ortho path)
void ShadowManager::GetFrustumPlanes(D3DXPLANE* Frustum, D3DXMATRIX* Matrix) {

	Frustum[PlaneNear].a   = Matrix->_13;
	Frustum[PlaneNear].b   = Matrix->_23;
	Frustum[PlaneNear].c   = Matrix->_33;
	Frustum[PlaneNear].d   = Matrix->_43;
	Frustum[PlaneFar].a    = Matrix->_14 - Matrix->_13;
	Frustum[PlaneFar].b    = Matrix->_24 - Matrix->_23;
	Frustum[PlaneFar].c    = Matrix->_34 - Matrix->_33;
	Frustum[PlaneFar].d    = Matrix->_44 - Matrix->_43;
	Frustum[PlaneLeft].a   = Matrix->_14 + Matrix->_11;
	Frustum[PlaneLeft].b   = Matrix->_24 + Matrix->_21;
	Frustum[PlaneLeft].c   = Matrix->_34 + Matrix->_31;
	Frustum[PlaneLeft].d   = Matrix->_44 + Matrix->_41;
	Frustum[PlaneRight].a  = Matrix->_14 - Matrix->_11;
	Frustum[PlaneRight].b  = Matrix->_24 - Matrix->_21;
	Frustum[PlaneRight].c  = Matrix->_34 - Matrix->_31;
	Frustum[PlaneRight].d  = Matrix->_44 - Matrix->_41;
	Frustum[PlaneTop].a    = Matrix->_14 - Matrix->_12;
	Frustum[PlaneTop].b    = Matrix->_24 - Matrix->_22;
	Frustum[PlaneTop].c    = Matrix->_34 - Matrix->_32;
	Frustum[PlaneTop].d    = Matrix->_44 - Matrix->_42;
	Frustum[PlaneBottom].a = Matrix->_14 + Matrix->_12;
	Frustum[PlaneBottom].b = Matrix->_24 + Matrix->_22;
	Frustum[PlaneBottom].c = Matrix->_34 + Matrix->_32;
	Frustum[PlaneBottom].d = Matrix->_44 + Matrix->_42;
	for (int i = 0; i < 6; ++i) {
		D3DXPLANE Plane(Frustum[i]);
		D3DXPlaneNormalize(&Frustum[i], &Plane);
	}

}

// 6-plane containment for a precomputed camera-relative bound (center + radius) against an
// arbitrary normalized frustum. Same test as InFrustum but takes the bound directly, so the
// per-light face frustums can be reused across an actor's skin partitions.
static bool BoundInFrustum6(const D3DXPLANE* Frustum, const D3DXVECTOR3& Center, float Radius) {
	for (int i = 0; i < 6; ++i)
		if (D3DXPlaneDotCoord(&Frustum[i], &Center) <= -Radius) return false;
	return true;
}

bool ShadowManager::InFrustum(D3DXPLANE* Frustum, NiGeometry* Geo) {

	NiBound* Bound = Geo->GetWorldBound();
	if (!Bound) return true; // Cannot cull without a bound; keep it.

	D3DXVECTOR3 Position = { Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y, Bound->Center.z - TheRenderManager->CameraPosition.z };
	for (int i = 0; i < 6; ++i) {
		if (D3DXPlaneDotCoord(&Frustum[i], &Position) <= -Bound->Radius) return false;
	}
	return true;

}

#endif // SHADOWS DISABLED

// True if the form type can ever cast shadows (independent of the per-map-type Forms filter).
static bool IsShadowCastableType(UInt8 TypeID) {
	switch (TypeID) {
	case TESForm::FormType::kFormType_Activator:
	case TESForm::FormType::kFormType_Apparatus:
	case TESForm::FormType::kFormType_Book:
	case TESForm::FormType::kFormType_Container:
	case TESForm::FormType::kFormType_Door:
	case TESForm::FormType::kFormType_Misc:
	case TESForm::FormType::kFormType_Stat:
	case TESForm::FormType::kFormType_Tree:
	case TESForm::FormType::kFormType_Furniture:
		return true;
	default:
		return TypeID >= TESForm::FormType::kFormType_NPC && TypeID <= TESForm::FormType::kFormType_LeveledCreature;
	}
}

// Whether the given Forms filter enables shadows for this form type.
static bool FormsAllows(SettingsShadowStruct::FormsStruct* Forms, UInt8 TypeID) {
	switch (TypeID) {
	case TESForm::FormType::kFormType_Activator: return Forms->Activators;
	case TESForm::FormType::kFormType_Apparatus: return Forms->Apparatus;
	case TESForm::FormType::kFormType_Book:      return Forms->Books;
	case TESForm::FormType::kFormType_Container: return Forms->Containers;
	case TESForm::FormType::kFormType_Door:      return Forms->Doors;
	case TESForm::FormType::kFormType_Misc:      return Forms->Misc;
	case TESForm::FormType::kFormType_Stat:      return Forms->Statics;
	case TESForm::FormType::kFormType_Tree:      return Forms->Trees;
	case TESForm::FormType::kFormType_Furniture: return Forms->Furniture;
	default:
		return TypeID >= TESForm::FormType::kFormType_NPC && TypeID <= TESForm::FormType::kFormType_LeveledCreature && Forms->Actors;
	}
}

#if 0 // SHADOWS DISABLED: dead reference — cube/point-light ref filters (unused by ortho path)
TESObjectREFR* ShadowManager::GetRef(TESObjectREFR* Ref, SettingsShadowStruct::FormsStruct* Forms, SettingsShadowStruct::ExcludedFormsList* ExcludedForms) {

	if (Ref && Ref->GetNode()) {
		TESForm* Form = Ref->baseForm;
		if (!(Ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows) && FormsAllows(Forms, Form->formType)) {
			if (!(ExcludedForms->size() > 0 && std::binary_search(ExcludedForms->begin(), ExcludedForms->end(), Form->refID)))
				return Ref;
		}
	}
	return NULL;

}

TESObjectREFR* ShadowManager::GetRefO(TESObjectREFR* Ref) {

	TESObjectREFR* r = NULL;

	if (Ref && Ref->GetNode()) {
		UInt8 TypeID = Ref->baseForm->formType;
		if (TypeID == TESForm::FormType::kFormType_Stat) r = Ref;
	}
	return r;

}

#endif // SHADOWS DISABLED

bool ShadowManager::InShadowFrustum(ShadowMapTypeEnum ShadowMapType, NiAVObject* Object) {

	float Distance = 0.0f;
	bool R = false;
	NiBound* Bound = Object->GetWorldBound();

	if (Bound) {
		D3DXVECTOR3 Position = { Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y, Bound->Center.z - TheRenderManager->CameraPosition.z };

		R = true;
		for (int i = 0; i < 6; ++i) {
			Distance = D3DXPlaneDotCoord(&ShadowMapFrustum[ShadowMapType][i], &Position);
			if (Distance <= -Bound->Radius) {
				R = false;
				break;
			}
		}
		if (ShadowMapType == MapFar && R) { // Ensures to not be fully in the near frustum
			for (int i = 0; i < 6; ++i) {
				Distance = D3DXPlaneDotCoord(&ShadowMapFrustum[MapNear][i], &Position);
				if (Distance <= -Bound->Radius || std::fabs(Distance) < Bound->Radius) {
					R = false;
					break;
				}
			}
			R = !R;
		}
	}
	return R;

}

// Ref-root cull: exact behaviour of InShadowFrustum (incl. the MapFar near-frustum
// exclusion) but on a precomputed camera-relative center+radius. Keeps which refs
// participate in which map identical to the per-candidate test it replaces.
bool ShadowManager::RootInShadowFrustum(ShadowMapTypeEnum ShadowMapType, const D3DXVECTOR3& Center, float Radius) {
	for (int i = 0; i < 6; ++i)
		if (D3DXPlaneDotCoord(&ShadowMapFrustum[ShadowMapType][i], &Center) <= -Radius) return false;
	if (ShadowMapType == MapFar) { // Ensures to not be fully in the near frustum
		// "Fully inside near" is a short-circuit AND over the 6 near planes. Near and far share
		// the same view (only the ortho extents differ), so the two depth planes are identical to
		// far's — already satisfied here and effectively never decisive. Test the 4 (much tighter)
		// side planes first so the early-out fires on the first plane for the common case of an
		// object laterally outside the small near cascade. Result is identical; only order changes.
		static const int NearPlaneOrder[6] = { PlaneLeft, PlaneRight, PlaneTop, PlaneBottom, PlaneNear, PlaneFar };
		bool fullyInNear = true;
		for (int k = 0; k < 6; ++k) {
			float Distance = D3DXPlaneDotCoord(&ShadowMapFrustum[MapNear][NearPlaneOrder[k]], &Center);
			if (Distance <= -Radius || std::fabs(Distance) < Radius) { fullyInNear = false; break; }
		}
		if (fullyInNear) return false;
	}
	return true;
}

// Leaf cull: plain 6-plane containment against the map's frustum (no near-exclusion). Only
// removes sub-geometry fully outside the light frustum, which contributes nothing.
bool ShadowManager::LeafInShadowFrustum(ShadowMapTypeEnum ShadowMapType, const D3DXVECTOR3& Center, float Radius) {
	for (int i = 0; i < 6; ++i)
		if (D3DXPlaneDotCoord(&ShadowMapFrustum[ShadowMapType][i], &Center) <= -Radius) return false;
	return true;
}

#if 0 // SHADOWS DISABLED: dead reference — cube/point-light scene-graph collectors (unused by ortho path)
void ShadowManager::RenderObjectPoint(NiAVObject* Object, D3DXVECTOR4* ShadowData, bool HasWater) {

	if (Object && !(Object->m_flags & NiAVObject::kFlag_AppCulled)) {
		void* VFT = *(void**)Object;
		if (VFT == VFTNiNode || VFT == VFTBSFadeNode || VFT == VFTBSFaceGenNiNode || VFT == VFTBSTreeNode) {
			NiNode* Node = (NiNode*)Object;
			for (int i = 0; i < Node->m_children.end; i++) {
				RenderObjectPoint(Node->m_children.data[i], ShadowData, HasWater);
			}
		}
		else if (VFT == VFTNiTriShape || VFT == VFTNiTriStrips) {
			NiGeometry* Geo = (NiGeometry*)Object;
			if (Geo->shader) {
				if (Geo->skinInstance || !HasWater || (HasWater && Geo->GetWorldBound()->Center.z > TheShaderManager->ShaderConst.Water.waterSettings.x)) {
					NiGeometryBufferData* GeoData = Geo->geomData->BuffData;
					if (GeoData) {
						Render(Geo, ShadowData);
					}
					else if (Geo->skinInstance && Geo->skinInstance->SkinPartition && Geo->skinInstance->SkinPartition->Partitions) {
						GeoData = Geo->skinInstance->SkinPartition->Partitions[0].BuffData;
						if (GeoData) Render(Geo, ShadowData);
					}
				}
			}
		}
	}

}

void ShadowManager::CollectCubeMapGeometry(NiAVObject* Object, bool HasWater, std::vector<NiGeometry*>& Out) {

	if (Object && !(Object->m_flags & NiAVObject::kFlag_AppCulled)) {
		void* VFT = *(void**)Object;
		if (VFT == VFTNiNode || VFT == VFTBSFadeNode || VFT == VFTBSFaceGenNiNode || VFT == VFTBSTreeNode) {
			NiNode* Node = (NiNode*)Object;
			for (int i = 0; i < Node->m_children.end; i++) {
				CollectCubeMapGeometry(Node->m_children.data[i], HasWater, Out);
			}
		}
		else if (VFT == VFTNiTriShape || VFT == VFTNiTriStrips) {
			NiGeometry* Geo = (NiGeometry*)Object;
			// Torch geometry is skipped by Render() anyway; drop it once here instead of
			// re-testing the name (and re-entering Render) for every one of the 6 cube faces.
			if (Geo->shader && !(Geo->m_pcName && !memcmp(Geo->m_pcName, "Torch", 5))) {
				if (Geo->skinInstance || !HasWater || (HasWater && Geo->GetWorldBound()->Center.z > TheShaderManager->ShaderConst.Water.waterSettings.x)) {
					if (Geo->geomData->BuffData) {
						Out.emplace_back(Geo);
					}
					else if (Geo->skinInstance && Geo->skinInstance->SkinPartition && Geo->skinInstance->SkinPartition->Partitions) {
						if (Geo->skinInstance->SkinPartition->Partitions[0].BuffData) Out.emplace_back(Geo);
					}
				}
			}
		}
	}

}

void ShadowManager::RenderObjectPointActor(NiAVObject* Object, D3DXVECTOR4* ShadowData, bool HasWater, int lightIndex) {

	if (Object && !(Object->m_flags & NiAVObject::kFlag_AppCulled)) {
		void* VFT = *(void**)Object;
		if (VFT == VFTNiNode || VFT == VFTBSFadeNode || VFT == VFTBSFaceGenNiNode || VFT == VFTBSTreeNode) {
			NiNode* Node = (NiNode*)Object;
			for (int i = 0; i < Node->m_children.end; i++) {
				RenderObjectPointActor(Node->m_children.data[i], ShadowData, HasWater, lightIndex);
			}
		}
		else if (VFT == VFTNiTriShape || VFT == VFTNiTriStrips) {
			NiGeometry* Geo = (NiGeometry*)Object;
			if (Geo->shader) {
				if (Geo->skinInstance || !HasWater || (HasWater && Geo->GetWorldBound()->Center.z > TheShaderManager->ShaderConst.Water.waterSettings.x)) {
					NiGeometryBufferData* GeoData = Geo->geomData->BuffData;
					if (GeoData) {
						RenderActor(Geo, ShadowData, lightIndex);
					}
					else if (Geo->skinInstance && Geo->skinInstance->SkinPartition && Geo->skinInstance->SkinPartition->Partitions) {
						GeoData = Geo->skinInstance->SkinPartition->Partitions[0].BuffData;
						if (GeoData) RenderActor(Geo, ShadowData, lightIndex);
					}
				}
			}
		}
	}

}

#endif // SHADOWS DISABLED

void ShadowManager::RenderTerrain(NiAVObject* Object, ShadowMapTypeEnum ShadowMapType, D3DXVECTOR4* ShadowData) {

	if (Object && !(Object->m_flags & NiAVObject::kFlag_AppCulled)) {
		void* VFT = *(void**)Object;
		if (VFT == VFTNiNode) {
			NiNode* Node = (NiNode*)Object;
			if (InShadowFrustum(ShadowMapType, Node)) {
				for (int i = 0; i < Node->m_children.end; i++) {
					RenderTerrain(Node->m_children.data[i], ShadowMapType, ShadowData);
				}
			}
		}
		else if (VFT == VFTNiTriShape || VFT == VFTNiTriStrips) {
			Render((NiGeometry*)Object, ShadowData);
		}
	}

}

void ShadowManager::Render(NiGeometry* Geo, D3DXVECTOR4* ShadowData, const D3DMATRIX* PrecomputedWorld) {

	NiGeometryData* ModelData = Geo->geomData;
	NiGeometryBufferData* GeoData = ModelData->BuffData;
	NiSkinInstance* SkinInstance = Geo->skinInstance;
	NiD3DShaderDeclaration* ShaderDeclaration = Geo->shader->ShaderDeclaration;

	if (Geo->m_pcName && !memcmp(Geo->m_pcName, "Torch", 5)) return;
	if (gCubeBucket) ProfileCount(Cnt_RenderCallsCube);

	ShadowData->x = 0.0f;
	ShadowData->y = 0.0f;
	if (GeoData) {
		// Reuse the matrix computed once per light for cube faces; otherwise build it now.
		if (PrecomputedWorld)
			TheShaderManager->ShaderConst.ShadowMap.ShadowWorld = *PrecomputedWorld;
		else
			CreateD3DMatrix(&TheShaderManager->ShaderConst.ShadowMap.ShadowWorld, &Geo->m_worldTransform);
		if (Geo->m_parent->m_pcName && !memcmp(Geo->m_parent->m_pcName, "Leaves", 6)) {
			SetupSpeedTreeLeafShader(Geo, ShadowData);
		} else {
			BSShaderProperty* LProp = (BSShaderProperty*)Geo->GetProperty(NiProperty::PropertyType::kType_Lighting);
			if (!LProp || !LProp->IsLightingProperty()) return;
			if (AlphaEnabled) SetupAlphaTexture(Geo, LProp, ShadowData);
		}
		TheRenderManager->PackGeometryBuffer(GeoData, ModelData, SkinInstance, ShaderDeclaration);
		SetupGeoStreams(GeoData);
		CurrentVertex->SetCT();
		CurrentPixel->SetCT();
		DrawGeoArrays(GeoData, GeoData->PrimitiveType, GeoData->VertCount);
	} else {
		RenderSkinnedGeo(Geo, ShadowData);
	}

}

#if 0 // SHADOWS DISABLED: dead reference — cube/point-light actor draw (unused by ortho path)
void ShadowManager::RenderActor(NiGeometry* Geo, D3DXVECTOR4* ShadowData, int lightIndex) {
	NiGeometryData* ModelData = Geo->geomData;
	NiGeometryBufferData* GeoData = ModelData->BuffData;
	NiSkinInstance* SkinInstance = Geo->skinInstance;
	NiD3DShaderDeclaration* ShaderDeclaration = Geo->shader->ShaderDeclaration;

	if (Geo->m_pcName && !memcmp(Geo->m_pcName, "Torch", 5)) return;

	// Per-face cull: mark which of the 6 cube faces this geo's world bound overlaps (against the
	// face frustums precomputed once per light in ComputeCubeFaceFrusta). RenderActorFaces then
	// draws only those faces; previously every geo was drawn to all 6 faces unconditionally. A
	// geo that overlaps no face contributes nothing, so skip it entirely.
	int VisibleFaces = 0;
	NiBound* Bound = Geo->GetWorldBound();
	if (Bound) {
		D3DXVECTOR3 Center = { Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y, Bound->Center.z - TheRenderManager->CameraPosition.z };
		for (int Face = 0; Face < 6; Face++) {
			CubeActorFaceVisible[Face] = BoundInFrustum6(CubeFaceFrustum[Face], Center, Bound->Radius);
			if (CubeActorFaceVisible[Face]) VisibleFaces++;
		}
	} else {
		for (int Face = 0; Face < 6; Face++) CubeActorFaceVisible[Face] = true; // no bound: keep all faces (matches InFrustum)
		VisibleFaces = 6;
	}
	if (VisibleFaces == 0) return;
	ProfileCount(Cnt_CubeActorGeo);

	ShadowData->x = 0.0f;
	ShadowData->y = 0.0f;
	if (GeoData) {
		CreateD3DMatrix(&TheShaderManager->ShaderConst.ShadowMap.ShadowWorld, &Geo->m_worldTransform);
		BSShaderProperty* LProp = (BSShaderProperty*)Geo->GetProperty(NiProperty::PropertyType::kType_Lighting);
		if (!LProp || !LProp->IsLightingProperty()) return;
		if (AlphaEnabled) SetupAlphaTexture(Geo, LProp, ShadowData);
		TheRenderManager->PackGeometryBuffer(GeoData, ModelData, SkinInstance, ShaderDeclaration);
		SetupGeoStreams(GeoData);
		RenderActorFaces(GeoData, GeoData->PrimitiveType, GeoData->VertCount, lightIndex);
	} else {
		RenderActorSkinnedGeo(Geo, ShadowData, lightIndex);
	}
}

#endif // SHADOWS DISABLED

void ShadowManager::SetupShadowMapMatrices(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, D3DXVECTOR3* At, D3DXVECTOR4* ShadowLightDir) {
	float FarPlane = ShadowsExteriors->ShadowMapFarPlane;
	float Radius = ShadowsExteriors->ShadowMapRadius[ShadowMapType];
	D3DXVECTOR3 Up(0.0f, 0.0f, 1.0f);
	D3DXMATRIX View, Proj;
	D3DXVECTOR3 Eye;
	Eye.x = At->x - FarPlane * ShadowLightDir->x * -1;
	Eye.y = At->y - FarPlane * ShadowLightDir->y * -1;
	Eye.z = At->z - FarPlane * ShadowLightDir->z * -1;
	D3DXMatrixLookAtRH(&View, &Eye, At, &Up);
	D3DXMatrixOrthoRH(&Proj, 2.0f * Radius, (1 + ShadowLightDir->z) * Radius, 0.0f, 2.0f * FarPlane);
	TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj = View * Proj;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight[ShadowMapType] = TheRenderManager->InvViewProjMatrix * TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj;
	BillboardRight = { View._11, View._21, View._31, 0.0f };
	BillboardUp    = { View._12, View._22, View._32, 0.0f };
	GetShadowFrustum(ShadowMapType, &TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj);
}

// Anchor-relative variant of SetupShadowMapMatrices for the cached directional regions. The light matrix
// is built with the snapped anchor at the ORIGIN (not absolute world coords) so all bake/sample math stays
// near zero — using absolute world coords (~1e5) here destroys float32 precision and flickers shadow edges.
// The anchor is snapped to the shadow-map texel grid so reused/rebaked maps don't shimmer.
void ShadowManager::SetupCachedRegionMatrices(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, D3DXVECTOR4* SunDir) {
	float FarPlane = ShadowsExteriors->ShadowMapFarPlane;
	float Radius   = ShadowsExteriors->ShadowMapRadius[ShadowMapType];
	int   Size     = ShadowsExteriors->ShadowMapSize[ShadowMapType];
	float TexelWorld = (2.0f * Radius) / (float)Size;

	D3DXVECTOR3 Anchor = LookAtPosition; // world-space; maintained by ComputeExteriorLookAt
	Anchor.x = floorf(Anchor.x / TexelWorld) * TexelWorld;
	Anchor.y = floorf(Anchor.y / TexelWorld) * TexelWorld;
	Anchor.z = floorf(Anchor.z / TexelWorld) * TexelWorld;

	// Anchor-relative: look-at at the origin, eye up the sun direction. Geometry is drawn relative to Anchor
	// (CollectAnchor below), and the apply re-bases the receiver to Anchor-relative before this matrix.
	D3DXVECTOR3 Up(0.0f, 0.0f, 1.0f);
	D3DXVECTOR3 AtRel(0.0f, 0.0f, 0.0f);
	D3DXVECTOR3 Eye(FarPlane * SunDir->x, FarPlane * SunDir->y, FarPlane * SunDir->z);
	D3DXMATRIX View, Proj;
	D3DXMatrixLookAtRH(&View, &Eye, &AtRel, &Up);
	D3DXMatrixOrthoRH(&Proj, 2.0f * Radius, (1 + SunDir->z) * Radius, 0.0f, 2.0f * FarPlane);
	D3DXMATRIX ViewProj = View * Proj;

	int r = ShadowMapType - MapNear;
	Regions[r].BakedViewProj = ViewProj;
	Regions[r].AnchorPos     = Anchor;
	Regions[r].BakedSunDir   = *SunDir;
	Regions[r].Valid         = true;
	CollectAnchor = Anchor; // this bake's geometry + cull centers are drawn relative to the anchor

	// The bake renders geometry with anchor-relative matrices against this ViewProj (Task 9). Publish it as
	// the current pass's ShadowViewProj so RenderShadowMap's vertex path uses it; set the culling frustum
	// + billboard vectors as SetupShadowMapMatrices does.
	TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj = ViewProj;
	BillboardRight = { View._11, View._21, View._31, 0.0f };
	BillboardUp    = { View._12, View._22, View._32, 0.0f };
	GetShadowFrustum(ShadowMapType, &TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj);
}

// Recompute the apply-pass sample matrix from the CURRENT camera each frame. BakedViewProj is
// world->light and InvViewProjMatrix is current-clip->world, so the product maps current screen depth
// into the cached (possibly stale) light space correctly.
void ShadowManager::PublishCachedRegionSampleMatrix(ShadowMapTypeEnum ShadowMapType) {
	int r = ShadowMapType - MapNear;
	// The map is baked ANCHOR-relative (origin at Regions[r].AnchorPos) to keep coordinates small.
	// InvViewProjMatrix maps current clip -> camera-relative world (world - Camera). Translate by
	// (Camera - Anchor) to reach anchor-relative world, then project with the baked matrix. Camera and Anchor
	// are both near the player, so this stays small and precise.
	D3DXVECTOR3& A = Regions[r].AnchorPos;
	D3DXMATRIX Trans;
	D3DXMatrixTranslation(&Trans, TheRenderManager->CameraPosition.x - A.x, TheRenderManager->CameraPosition.y - A.y, TheRenderManager->CameraPosition.z - A.z);
	TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight[ShadowMapType] = TheRenderManager->InvViewProjMatrix * Trans * Regions[r].BakedViewProj;
}

// A cached region is due for a rebake when it is invalid, the look-at focus has left the guard band
// the map was baked to cover, or the sun has rotated past the interval. Near uses a small margin
// (rebakes more often); far uses a large one (rarely).
bool ShadowManager::RegionNeedsRebake(ShadowMapTypeEnum ShadowMapType) {
	int r = ShadowMapType - MapNear;
	CachedRegion& Reg = Regions[r];
	if (!Reg.Valid) return true;

	SettingsShadowStruct::ExteriorsStruct* S = SelectExteriorShadowSettings();
	float Radius = S->ShadowMapRadius[ShadowMapType];
	float MarginFrac = (ShadowMapType == MapNear) ? S->RebakeMarginNear : S->RebakeMarginFar;
	D3DXVECTOR3 Drift = LookAtPosition - Reg.AnchorPos;
	if (D3DXVec3Length(&Drift) > Radius * MarginFrac) return true;

	D3DXVECTOR4* Sun = &TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
	float SunDot = Sun->x * Reg.BakedSunDir.x + Sun->y * Reg.BakedSunDir.y + Sun->z * Reg.BakedSunDir.z;
	if (SunDot < S->RebakeSunInterval) return true;

	return false;
}

// Rebake a cached region's STATIC depth only (Near or Far): world-anchored matrices, world-space
// collection, statics filtered out of the pool (actors are dynamic and draw via the per-frame
// overlay instead), then drawn into the persistent ShadowMapSurface[ShadowMapType].
void ShadowManager::BakeStaticRegion(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* S, D3DXVECTOR4* SunDir) {
	SetupCachedRegionMatrices(ShadowMapType, S, SunDir);
	CollectWorldSpace = true;
	BuildExteriorGeoItems(S, ShadowMapType);
	int w = 0; // statics only (actors draw in the per-frame overlay)
	for (int i = 0; i < ShadowGeoCount; i++) if (!ShadowGeoPool[i].IsActor) ShadowGeoPool[w++] = ShadowGeoPool[i];
	ShadowGeoCount = w;
	RenderShadowMap(ShadowMapType, S, &LookAtPosition, SunDir, &TheShaderManager->ShaderConst.Shadow.Data);
	CollectWorldSpace = false;
}

// Per-frame actor overlay (MapSkin): actors only, terrain skipped (statics/terrain are already covered by
// the cached near/far bakes). Camera-relative (matches RenderSkinnedGeo), with its own sample matrix
// (TESR_ShadowCameraToLightTransformSkin) min-combined with the cached static term in the apply shader.
void ShadowManager::RenderActorOverlay(SettingsShadowStruct::ExteriorsStruct* S, D3DXVECTOR4* SunDir) {
	// The overlay is redrawn every frame (not cached), so draw it CAMERA-RELATIVE — matching the space
	// RenderSkinnedGeo produces for skinned actors (and the Stage-1 directional path, which cast actors
	// correctly). It gets its OWN sample matrix (ShadowCameraToLight[MapSkin] ->
	// TESR_ShadowCameraToLightTransformSkin), which the apply shader min-combines with the cached
	// anchor-relative near-static map. CollectWorldSpace stays FALSE (camera-relative collect + draw).
	D3DXVECTOR3 At;
	At.x = LookAtPosition.x - TheRenderManager->CameraPosition.x;
	At.y = LookAtPosition.y - TheRenderManager->CameraPosition.y;
	At.z = LookAtPosition.z - TheRenderManager->CameraPosition.z;
	SetupShadowMapMatrices(MapSkin, S, &At, SunDir); // camera-relative; publishes ShadowCameraToLight[MapSkin] + frustum + ShadowViewProj
	BuildExteriorGeoItems(S, MapSkin);
	int w = 0; // actors only
	for (int i = 0; i < ShadowGeoCount; i++) if (ShadowGeoPool[i].IsActor) ShadowGeoPool[w++] = ShadowGeoPool[i];
	ShadowGeoCount = w;
	RenderShadowMap(MapSkin, S, &At, SunDir, &TheShaderManager->ShaderConst.Shadow.Data, /*SkipTerrain=*/true);
}

static const float MinRadii[4] = { 9.0f, 100.0f, 100.0f, 0.0f }; // Near, Far, Ortho, Skin

void ShadowManager::RenderShadowMapCellTerrain(TESObjectCELL* Cell, ShadowMapTypeEnum ShadowMapType, D3DXVECTOR4* ShadowData) {
	NiNode* CellNode = Cell->niNode;
	for (int i = 2; i < 6; i++) {
		NiNode* TerrainNode = (NiNode*)CellNode->m_children.data[i];
		if (TerrainNode->m_children.end) RenderTerrain(TerrainNode->m_children.data[0], ShadowMapType, ShadowData);
	}
}

// Flatten every ortho-frustum-visible, Forms-allowed shadow-casting ref in the loaded grid into
// ShadowGeoPool once per frame. Node/flag/excluded/type/Forms eligibility, the ref-root frustum cull,
// world transforms, bounds, the water test, the per-geo leaf cull, the MinRadius cut, and instancing
// eligibility are all resolved here; RenderShadowMap then draws the flat list with no further culling.
void ShadowManager::BuildExteriorGeoItems(SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, ShadowMapTypeEnum ShadowMapType) {
	ShadowGeoCount = 0;
	SettingsShadowStruct::ExcludedFormsList* ExcludedForms = &ShadowsExteriors->ExcludedForms;
	bool HasExcluded = ExcludedForms->size() > 0;
	bool HasWater = TheShaderManager->ShaderConst.HasWater;
	SettingsShadowStruct::FormsStruct* Forms = &ShadowsExteriors->Forms[ShadowMapType];
	for (UInt32 x = 0; x < *SettingGridsToLoad; x++) {
		for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
			TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y);
			if (!Cell) continue;
			TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First;
			for (; Entry; Entry = Entry->next) {
				TESObjectREFR* Ref = Entry->item;
				NiNode* Node;
				if (!Ref || !(Node = Ref->GetNode()) || (Ref->flags & TESForm::FormFlags::kFormFlags_NotCastShadows)) continue;
				TESForm* Form = Ref->baseForm;
				UInt8 TypeID = Form->formType;
				if (!IsShadowCastableType(TypeID)) continue;
				if (!FormsAllows(Forms, TypeID)) continue;
				if (HasExcluded && std::binary_search(ExcludedForms->begin(), ExcludedForms->end(), Form->refID)) continue;
				NiBound* RootBound = Node->GetWorldBound();
				if (!RootBound) continue;
				D3DXVECTOR3 RootCenter;
				if (CollectWorldSpace) { RootCenter.x = RootBound->Center.x - CollectAnchor.x; RootCenter.y = RootBound->Center.y - CollectAnchor.y; RootCenter.z = RootBound->Center.z - CollectAnchor.z; }
				else { RootCenter.x = RootBound->Center.x - TheRenderManager->CameraPosition.x; RootCenter.y = RootBound->Center.y - TheRenderManager->CameraPosition.y; RootCenter.z = RootBound->Center.z - TheRenderManager->CameraPosition.z; }
				if (!RootInShadowFrustum(ShadowMapType, RootCenter, RootBound->Radius)) continue; // whole-subtree cull
				bool IsActorRef = (TypeID >= TESForm::FormType::kFormType_NPC && TypeID <= TESForm::FormType::kFormType_LeveledCreature); // used by the Stage 2 static/dynamic split
				CollectExteriorGeo(Node, HasWater, ShadowMapType, IsActorRef);
			}
		}
	}
}

// Recursive collector: walks one ref's sub-tree and appends drawable geometry to ShadowGeoPool.
// Mirrors the old RenderObjectInstanced traversal/filters, but precomputes and stores per-geo
// state instead of drawing. Anything Render()/RenderSkinnedGeo() would have drawn is collected;
// anything they would have skipped (torch, no shader, submerged, no lighting property on opaque
// statics, no usable buffer) is dropped here.
void ShadowManager::CollectExteriorGeo(NiAVObject* Object, bool HasWater, ShadowMapTypeEnum ShadowMapType, bool IsActorRef) {
	if (!Object || (Object->m_flags & NiAVObject::kFlag_AppCulled)) return;
	void* VFT = *(void**)Object;
	if (VFT == VFTNiNode || VFT == VFTBSFadeNode || VFT == VFTBSFaceGenNiNode || VFT == VFTBSTreeNode || VFT == VFTNiLODNode) {
		NiNode* Node = (NiNode*)Object; // NiLODNode derives from NiNode, so children access is valid; the drawable (active) LOD casts, others are filtered by AppCull/NotDrawable
		for (int i = 0; i < Node->m_children.end; i++)
			CollectExteriorGeo(Node->m_children.data[i], HasWater, ShadowMapType, IsActorRef);
		return;
	}
	if (VFT != VFTNiTriShape && VFT != VFTNiTriStrips) return;

	NiGeometry* Geo = (NiGeometry*)Object;
	if (Geo->m_pcName && !memcmp(Geo->m_pcName, "Torch", 5)) return; // skipped by Render() anyway
	if (!Geo->shader) return;

	NiBound* Bound = Geo->GetWorldBound();
	if (!Bound) return; // no bound: can't cull/place; the ref-root test already requires one
	// Submerged casters are intentionally NOT dropped: pre-water depth shadows underwater receivers, so
	// submerged geometry must cast (a caster resting on a submerged base then grounds correctly instead of
	// peter-panning). Skinned casters were already exempt (6d662023); this applies the same to statics.
	// HasWater is still propagated through the recursion but no longer gates collection here.

	// Per-pass cuts, applied at collection: drop sub-MinRadius geo and anything outside this pass's
	// frustum. Center is reused for the stored item below.
	if (Bound->Radius < MinRadii[ShadowMapType]) return;
	D3DXVECTOR3 Center;
	if (CollectWorldSpace) { Center.x = Bound->Center.x - CollectAnchor.x; Center.y = Bound->Center.y - CollectAnchor.y; Center.z = Bound->Center.z - CollectAnchor.z; }
	else { Center.x = Bound->Center.x - TheRenderManager->CameraPosition.x; Center.y = Bound->Center.y - TheRenderManager->CameraPosition.y; Center.z = Bound->Center.z - TheRenderManager->CameraPosition.z; }
	if (!LeafInShadowFrustum(ShadowMapType, Center, Bound->Radius)) return;

	// Resolve the buffer Render() will use: model buffer (static path), else first skin
	// partition (RenderSkinnedGeo path, signalled by storing GeoData = NULL on the item).
	NiGeometryBufferData* ModelBuff = Geo->geomData->BuffData;
	bool DrawViaSkin = false;
	if (!ModelBuff) {
		if (!(Geo->skinInstance && Geo->skinInstance->SkinPartition && Geo->skinInstance->SkinPartition->Partitions
			&& Geo->skinInstance->SkinPartition->Partitions[0].BuffData)) return; // not drawable
		DrawViaSkin = true;
	}

	bool BaseInstanceable = false;
	bool HasAlphaMask = false;
	if (!DrawViaSkin) {
		bool IsLeaf = Geo->m_parent && Geo->m_parent->m_pcName && !memcmp(Geo->m_parent->m_pcName, "Leaves", 6);
		if (!IsLeaf) {
			// Opaque static path: Render() early-outs without a lighting property, so drop it here.
			BSShaderProperty* LProp = (BSShaderProperty*)Geo->GetProperty(NiProperty::PropertyType::kType_Lighting);
			if (!LProp || !LProp->IsLightingProperty()) return;
			NiAlphaProperty* AProp = (NiAlphaProperty*)Geo->GetProperty(NiProperty::PropertyType::kType_Alpha);
			HasAlphaMask = AProp && (AProp->flags & (NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK | NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK));
			BaseInstanceable = ModelBuff->VertexDeclaration && !Geo->skinInstance; // FVF-only / skinned excluded
		}
		// SpeedTree leaves draw via SetupSpeedTreeLeafShader and are never instanced.
	}

	if (ShadowGeoCount == (int)ShadowGeoPool.size()) ShadowGeoPool.emplace_back();
	ShadowGeoItem& Item = ShadowGeoPool[ShadowGeoCount++];
	Item.Geo = Geo;
	Item.GeoData = ModelBuff; // NULL => Render() takes the skinned path and ignores World
	Item.Center = Center;
	Item.Radius = Bound->Radius;
	Item.BaseInstanceable = BaseInstanceable;
	Item.HasAlphaMask = HasAlphaMask;
	Item.IsActor = IsActorRef;
	if (!DrawViaSkin) { if (CollectWorldSpace) CreateD3DMatrixWorld(&Item.World, &Geo->m_worldTransform); else CreateD3DMatrix(&Item.World, &Geo->m_worldTransform); }
}

// Append a pool item to its mesh's instance group (keyed by the shared NiGeometryBufferData),
// reusing the pooled index vector across passes.
void ShadowManager::AddInstance(NiGeometryBufferData* GeoData, int ItemIndex) {
	auto it = InstanceGroupIndex.find(GeoData);
	int idx;
	if (it == InstanceGroupIndex.end()) {
		idx = InstanceGroupCount++;
		if ((size_t)idx == InstancePool.size()) InstancePool.emplace_back();
		InstancePool[idx].GeoData = GeoData;
		InstancePool[idx].ItemIdx.clear(); // reuse capacity from a previous pass
		InstancePool[idx].Decl = GetInstancedDeclaration(GeoData); // resolve once per group
		InstanceGroupIndex.emplace(GeoData, idx);
	} else {
		idx = it->second;
	}
	InstancePool[idx].ItemIdx.emplace_back(ItemIndex);
}

// Build (and cache, keyed by the source declaration) a vertex declaration that appends a
// per-instance stream carrying the three world-matrix columns as TEXCOORD5/6/7.
IDirect3DVertexDeclaration9* ShadowManager::GetInstancedDeclaration(NiGeometryBufferData* GeoData) {
	IDirect3DVertexDeclaration9* Src = GeoData->VertexDeclaration;
	if (!Src) return NULL;
	auto it = InstancedDeclCache.find(Src);
	if (it != InstancedDeclCache.end()) return it->second;

	D3DVERTEXELEMENT9 Elems[MAXD3DDECLLENGTH + 4];
	UINT Num = 0;
	if (FAILED(Src->GetDeclaration(Elems, &Num)) || Num == 0 || Num > MAXD3DDECLLENGTH) {
		InstancedDeclCache[Src] = NULL;
		return NULL;
	}
	UINT Term = Num - 1; // index of the existing D3DDECL_END terminator
	WORD InstStream = (WORD)GeoData->StreamCount;
	for (int k = 0; k < 3; k++) {
		Elems[Term + k].Stream     = InstStream;
		Elems[Term + k].Offset     = (WORD)(k * 16);
		Elems[Term + k].Type       = D3DDECLTYPE_FLOAT4;
		Elems[Term + k].Method     = D3DDECLMETHOD_DEFAULT;
		Elems[Term + k].Usage      = D3DDECLUSAGE_TEXCOORD;
		Elems[Term + k].UsageIndex = (BYTE)(5 + k);
	}
	Elems[Term + 3] = D3DDECL_END();

	IDirect3DVertexDeclaration9* Combined = NULL;
	if (FAILED(TheRenderManager->device->CreateVertexDeclaration(Elems, &Combined))) Combined = NULL;
	InstancedDeclCache[Src] = Combined;
	return Combined;
}

bool ShadowManager::EnsureInstanceVB(UINT InstanceCount) {
	if (InstanceCount == 0) return false;
	if (InstanceVB && InstanceVBCapacity >= InstanceCount) return true;
	if (InstanceVB) { InstanceVB->Release(); InstanceVB = NULL; }
	UINT Cap = InstanceVBCapacity ? InstanceVBCapacity : 256;
	while (Cap < InstanceCount) Cap *= 2;
	if (FAILED(TheRenderManager->device->CreateVertexBuffer(Cap * ShadowInstanceStride, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &InstanceVB, NULL))) {
		InstanceVB = NULL;
		InstanceVBCapacity = 0;
		return false;
	}
	InstanceVBCapacity = Cap;
	return true;
}

void ShadowManager::DrawInstancedGroup(NiGeometryBufferData* GeoData, std::vector<int>& ItemIdx, IDirect3DVertexDeclaration9* Decl) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	UINT Count = (UINT)ItemIdx.size();

	if (!Decl) return;

	// Pack each instance's precomputed camera-relative world matrix as 3 columns into the stream.
	float* Data = NULL;
	if (FAILED(InstanceVB->Lock(0, Count * ShadowInstanceStride, (void**)&Data, D3DLOCK_DISCARD))) return;
	for (UINT i = 0; i < Count; i++) {
		const D3DMATRIX& m = ShadowGeoPool[ItemIdx[i]].World;
		float* d = Data + i * 12;
		d[0] = m._11; d[1] = m._21; d[2]  = m._31; d[3]  = m._41;
		d[4] = m._12; d[5] = m._22; d[6]  = m._32; d[7]  = m._42;
		d[8] = m._13; d[9] = m._23; d[10] = m._33; d[11] = m._43;
	}
	InstanceVB->Unlock();

	WORD InstStream = (WORD)GeoData->StreamCount;
	for (UInt32 s = 0; s < GeoData->StreamCount; s++) {
		Device->SetStreamSource(s, GeoData->VBChip[s]->VB, 0, GeoData->VertexStride[s]);
		Device->SetStreamSourceFreq(s, D3DSTREAMSOURCE_INDEXEDDATA | Count);
	}
	Device->SetStreamSource(InstStream, InstanceVB, 0, ShadowInstanceStride);
	Device->SetStreamSourceFreq(InstStream, D3DSTREAMSOURCE_INSTANCEDATA | 1);
	Device->SetIndices(GeoData->IB);
	RenderState->SetVertexDeclaration(Decl, false);

	DrawGeoArrays(GeoData, GeoData->PrimitiveType, GeoData->VertCount);
	ProfileCount(Cnt_InstancedDraws);

	// Restore default (non-instanced) stream frequencies for subsequent draws.
	for (UInt32 s = 0; s < GeoData->StreamCount; s++) Device->SetStreamSourceFreq(s, 1);
	Device->SetStreamSourceFreq(InstStream, 1);
	Device->SetStreamSource(InstStream, NULL, 0, 0);
}

void ShadowManager::FlushInstanceGroups(D3DXVECTOR4* ShadowData) {
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	ProfileCount(Cnt_DirGroups, (UInt32)InstanceGroupCount); // unique repeated meshes this pass

	// Size the instance buffer once for the largest batchable group.
	UINT MaxGroup = 0;
	for (int i = 0; i < InstanceGroupCount; i++)
		if (InstancePool[i].ItemIdx.size() >= ShadowInstanceMinCount && InstancePool[i].ItemIdx.size() > MaxGroup) MaxGroup = (UINT)InstancePool[i].ItemIdx.size();
	bool CanInstance = MaxGroup > 0 && EnsureInstanceVB(MaxGroup);

	// Pass 1: draw batchable groups with the instanced shader (opaque, ShadowData x=y=0).
	bool InstancedSet = false;
	if (CanInstance) {
		for (int i = 0; i < InstanceGroupCount; i++) {
			InstanceGroup& Group = InstancePool[i];
			if (Group.ItemIdx.size() < ShadowInstanceMinCount || !Group.Decl) continue;
			if (!InstancedSet) {
				ShadowData->x = 0.0f;
				ShadowData->y = 0.0f;
				RenderState->SetVertexShader(ShadowMapInstancedVertexShader, false);
				RenderState->SetPixelShader(ShadowMapPixelShader, false);
				ShadowMapInstancedVertex->SetCT();
				ShadowMapPixel->SetCT();
				InstancedSet = true;
			}
			DrawInstancedGroup(Group.GeoData, Group.ItemIdx, Group.Decl);
			ProfileCount(Cnt_DirInstancedItems, (UInt32)Group.ItemIdx.size());
		}
	}

	// Pass 2: everything not instanced (small groups, or buffers without a usable declaration).
	CurrentVertex = ShadowMapVertex;
	CurrentPixel  = ShadowMapPixel;
	RenderState->SetVertexShader(ShadowMapVertexShader, false);
	RenderState->SetPixelShader(ShadowMapPixelShader, false);
	for (int i = 0; i < InstanceGroupCount; i++) {
		InstanceGroup& Group = InstancePool[i];
		bool Instanced = CanInstance && Group.ItemIdx.size() >= ShadowInstanceMinCount && Group.Decl;
		if (Instanced) continue;
		ProfileCount(Cnt_DirFallbackItems, (UInt32)Group.ItemIdx.size());
		for (int idx : Group.ItemIdx) Render(ShadowGeoPool[idx].Geo, ShadowData, &ShadowGeoPool[idx].World);
	}
}

void ShadowManager::RenderShadowMap(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, D3DXVECTOR3* At, D3DXVECTOR4* ShadowLightDir, D3DXVECTOR4* ShadowData, bool SkipTerrain) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	ScopeTimer profile((ShadowPhase)(Phase_PassNear + ShadowMapType)); // enum order matches Near/Far/Ortho/Skin

	AlphaEnabled = ShadowsExteriors->AlphaEnabled[ShadowMapType];
	// Matrices/frustum are set up by the caller (RenderExteriorShadows) before geometry collection,
	// so the pool is already culled to this map's frustum; do not recompute here.
	if (!ShadowMapSurface[ShadowMapType]) return; // only MapOrtho is allocated in the dummied-out build
	Device->SetRenderTarget(0, ShadowMapSurface[ShadowMapType]);
	Device->SetDepthStencilSurface(ShadowMapDepthSurface[ShadowMapType]);
	Device->SetViewport(&ShadowMapViewPort[ShadowMapType]);
	Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
	if (!ShadowsExteriors->Enabled[ShadowMapType]) return;

	RenderState->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
	RenderState->SetRenderState(D3DRS_ZWRITEENABLE, 1, RenderStateArgs);
	RenderState->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE, RenderStateArgs);
	RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, 0, RenderStateArgs);
	RenderState->SetVertexShader(ShadowMapVertexShader, false);
	RenderState->SetPixelShader(ShadowMapPixelShader, false);
	Device->BeginScene();
	if (!SkipTerrain) {
		gTerrainBucket = true;
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++)
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++)
				if (TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y))
					RenderShadowMapCellTerrain(Cell, ShadowMapType, ShadowData);
		gTerrainBucket = false;
	}
	bool UseInstancing = ShadowsExteriors->UseInstancing && ShadowMapInstancedVertexShader;
	if (UseInstancing) { InstanceGroupIndex.clear(); InstanceGroupCount = 0; }

	// Pool is already culled to this map's frustum and Forms-filtered (BuildExteriorGeoItems), so just
	// draw: batch instanceable opaque statics, draw everything else immediately.
	for (int i = 0; i < ShadowGeoCount; i++) {
		ShadowGeoItem& Item = ShadowGeoPool[i];
		if (UseInstancing && Item.BaseInstanceable && !(AlphaEnabled && Item.HasAlphaMask)) {
			AddInstance(Item.GeoData, i);
			ProfileCount(Cnt_DirItemsInstanced);
		} else {
			Render(Item.Geo, ShadowData, Item.GeoData ? &Item.World : NULL);
			if (!Item.BaseInstanceable) ProfileCount(Cnt_DirItemsImmNonInst);
			else ProfileCount(Cnt_DirItemsImmAlpha);
		}
	}
	if (UseInstancing) FlushInstanceGroups(ShadowData);
	Device->EndScene();
}

// The ortho depth map's only consumers are the precipitation effects (Rain/Snow/SnowAccumulation),
// which sample it for occlusion. Rebuild it only while one of them is still contributing: rain/snow
// intensity ramps (RainData.x / SnowData.x) or the snow-accumulation amount (Params.w), which keeps
// decreasing for a while after snow stops. If both effects are disabled in the INI nothing can ever
// sample the map, so skip unconditionally. Values are maintained in ShaderManager::UpdateConstants;
// reading them here may be one frame stale relative to that update, which is invisible for a depth
// occlusion map across weather ramps.
bool ShadowManager::OrthoNeeded() {
	SettingsMainStruct::EffectsStruct* Effects = &TheSettingManager->SettingsMain.Effects;
	if (!Effects->Precipitations && !Effects->SnowAccumulation) return false;
	if (Effects->Precipitations &&
		(TheShaderManager->ShaderConst.Precipitations.RainData.x > 0.0f ||
		 TheShaderManager->ShaderConst.Precipitations.SnowData.x > 0.0f)) return true;
	if (Effects->SnowAccumulation &&
		TheShaderManager->ShaderConst.SnowAccumulation.Params.w > 0.0f) return true;
	return false;
}

// Directional sun shadows only make sense in an exterior worldspace with the sun above the
// horizon. ShadowLightDir.z is the sun's vertical component (published by ShaderManager); below a
// small threshold (dusk/dawn/night) there is effectively no sun to cast shadows, so skip the whole
// pass and pay nothing. UsePostProcessing gates on the feature being enabled at all.
bool ShadowManager::SunShadowNeeded() {
	if (!TheSettingManager->SettingsShadows.Exteriors.UsePostProcessing) return false;
	if (!TheShaderManager->isFullyInitialized) return false;
	if (!Player->GetWorldSpace()) return false;
	return TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir.z > TheSettingManager->SettingsShadows.Exteriors.SunUpThreshold;
}

// Exterior shadow pass. Renders two independent, separately-gated things:
//  - Ortho depth map (DoOrtho): sampled by the precipitation effects (Rain/Snow/SnowAccumulation)
//    for occlusion; also publishes ShaderConst.ShadowMap.ShadowCameraToLight[MapOrtho]
//    (-> TESR_ShadowCameraToLightTransformOrtho) via SetupShadowMapMatrices.
//  - Directional sun maps MapNear/MapFar (DoSun): reimplemented per-frame directional shadows,
//    applied in image space by ShadowsExteriors.fx.
// The Skin map, point-light cube maps, and interior shadows remain dummied out (dead reference in
// the #if 0 block below) pending later work.
void ShadowManager::RenderExteriorShadows() {
	if (!Player->GetWorldSpace()) return;
	bool DoOrtho = OrthoNeeded();
	bool DoSun   = SunShadowNeeded();
	if (!DoOrtho && !DoSun) return;
	ScopeTimer profile(Phase_ExtTotal);

	SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors = SelectExteriorShadowSettings();
	D3DXVECTOR4* ShadowData = &TheShaderManager->ShaderConst.Shadow.Data;
	D3DXVECTOR4* OrthoData  = &TheShaderManager->ShaderConst.Shadow.OrthoData;
	D3DXVECTOR4  OrthoDir   = D3DXVECTOR3(0.05f, 0.05f, 1.0f);

	CurrentVertex = ShadowMapVertex;
	CurrentPixel  = ShadowMapPixel;

	D3DXVECTOR3 At, SkinAt;
	ComputeExteriorLookAt(At, SkinAt, ShadowsExteriors);

	if (DoOrtho) {
		// Matrices/frustum first: collection culls geometry against ShadowMapFrustum[MapOrtho], so the
		// frustum must exist before the walk. SetupShadowMapMatrices also publishes
		// ShadowCameraToLight[MapOrtho] (-> TESR_ShadowCameraToLightTransformOrtho) and Billboard vectors.
		SetupShadowMapMatrices(MapOrtho, ShadowsExteriors, &At, &OrthoDir);

		{ ScopeTimer profileBuild(Phase_BuildGeoItems); BuildExteriorGeoItems(ShadowsExteriors, MapOrtho); }

		RenderShadowMap(MapOrtho, ShadowsExteriors, &At, &OrthoDir, ShadowData);

		OrthoData->z = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapOrtho];
	}

	if (CurrentCell != Player->parentCell) {
		CurrentCell = Player->parentCell;
		Regions[0].Valid = Regions[1].Valid = false;
	}

	if (DoSun) {
		D3DXVECTOR4* SunDir = &TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
		// Round-robin: rebake near if due; else far if due; never both in one frame.
		if (RegionNeedsRebake(MapNear))      BakeStaticRegion(MapNear, ShadowsExteriors, SunDir);
		else if (RegionNeedsRebake(MapFar))  BakeStaticRegion(MapFar,  ShadowsExteriors, SunDir);

		RenderActorOverlay(ShadowsExteriors, SunDir);

		// Per-frame sample matrices from the CURRENT camera (the cached maps may be stale/world-anchored).
		PublishCachedRegionSampleMatrix(MapNear);
		PublishCachedRegionSampleMatrix(MapFar);

		ShadowData->y = ShadowsExteriors->Darkness;
		ShadowData->z = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapNear];
		ShadowData->w = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapFar];
	}
}

#if 0 // SHADOWS DISABLED: dead reference — point-light cube maps, interior shadows, and the old directional exterior path (Near/Far/Skin + interval). Replaced by the ortho-only RenderExteriorShadows above.
void ShadowManager::RenderShadowCubeMapExt(NiPointLight** Lights, int LightIndex, float radiusScan, SettingsShadowStruct::InteriorsStruct* ShadowSettings, D3DXVECTOR4* ShadowData) {
	double StaticValues[12] = { 0 };
	bool forceRedrawMap[12] = { false };

	{
		ScopeTimer profile(Phase_IntClassify);
		ClearCubeMapNodeLists();
		for (UInt32 x = 0; x < *SettingGridsToLoad - 1; x++) {
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
				if (TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y)) {
					TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First;
					while (Entry) {
						if (TESObjectREFR* Ref = GetRef(Entry->item, &ShadowSettings->Forms, &ShadowSettings->ExcludedForms)) {
							RefLightInfo Info = BuildRefLightInfo(Ref);
							for (int L = 0; L <= LightIndex; L++)
								ClassifyRefForLight(Info, Lights, L, radiusScan, CubeMapRefMap, CubeMapActorMap, StaticValues, forceRedrawMap);
						}
						Entry = Entry->next;
					}
				}
			}
		}
		UpdateStaticTrackers(LightIndex, StaticValues, forceRedrawMap);
	}
	{
		ScopeTimer profile(Phase_IntCubeRender);
		gCubeBucket = true;
		RenderShadowCubeMap(LightIndex, CubeMapRefMap, ShadowData, ShadowSettings->Enabled);
		gCubeActorBucket = true;
		RenderShadowCubeMapActor(LightIndex, CubeMapActorMap, ShadowData, ShadowSettings->Enabled);
		gCubeActorBucket = false;
		gCubeBucket = false;
	}
}

void ShadowManager::RenderShadowCubeMapInt(NiPointLight** Lights, int LightIndex, float radiusScan, SettingsShadowStruct::InteriorsStruct* ShadowSettings, D3DXVECTOR4* ShadowData) {
	double StaticValues[12] = { 0 };
	bool forceRedrawMap[12] = { false };
	TList<TESObjectREFR>::Entry* Entry = &Player->parentCell->objectList.First;

	{
		ScopeTimer profile(Phase_IntClassify);
		ClearCubeMapNodeLists();
		while (Entry) {
			if (TESObjectREFR* Ref = GetRef(Entry->item, &ShadowSettings->Forms, &ShadowSettings->ExcludedForms)) {
				RefLightInfo Info = BuildRefLightInfo(Ref);
				for (int L = 0; L <= LightIndex; L++)
					ClassifyRefForLight(Info, Lights, L, radiusScan, CubeMapRefMap, CubeMapActorMap, StaticValues, forceRedrawMap);
			}
			Entry = Entry->next;
		}
		UpdateStaticTrackers(LightIndex, StaticValues, forceRedrawMap);
	}
	{
		ScopeTimer profile(Phase_IntCubeRender);
		gCubeBucket = true;
		RenderShadowCubeMap(LightIndex, CubeMapRefMap, ShadowData, ShadowSettings->Enabled);
		gCubeActorBucket = true;
		RenderShadowCubeMapActor(LightIndex, CubeMapActorMap, ShadowData, ShadowSettings->Enabled);
		gCubeActorBucket = false;
		gCubeBucket = false;
	}
}

void ShadowManager::RenderShadowCubeMapFakeInt(int LightIndex, SettingsShadowStruct::InteriorsStruct* ShadowSettings, D3DXVECTOR4* ShadowData) {
	D3DXVECTOR3 At, Eye;
	D3DXVECTOR4* ShadowLightDir = &TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;

	if (!FakeExtShadowLightDirSet) {
		if (FakeExtShadowLightDirCnt > 4) {
			FakeExtShadowLightDir = TheRenderManager->CameraPosition;
			FakeExtShadowLightDirSet = true;
		}
		FakeExtShadowLightDirCnt++;
	}

	Eye.x = ((ShadowLightDir->x * 4000) + FakeExtShadowLightDir.x) - TheRenderManager->CameraPosition.x;
	Eye.y = ((ShadowLightDir->y * 4000) + FakeExtShadowLightDir.y) - TheRenderManager->CameraPosition.y;
	Eye.z = ((ShadowLightDir->z * 4000) + FakeExtShadowLightDir.z) - TheRenderManager->CameraPosition.z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[LightIndex].x = Eye.x;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[LightIndex].y = Eye.y;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[LightIndex].z = Eye.z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[LightIndex].w = 15000;

	ClearCubeMapNodeLists();
	TList<TESObjectREFR>::Entry* Entry = &Player->parentCell->objectList.First;
	while (Entry) {
		if (TESObjectREFR* Ref = GetRef(Entry->item, &ShadowSettings->Forms, &ShadowSettings->ExcludedForms)) {
			CubeMapRefMap[LightIndex].emplace_back(Ref->GetNode());
		}
		Entry = Entry->next;
	}
	RenderShadowCubeMap(LightIndex, CubeMapRefMap, ShadowData, ShadowSettings->Enabled);
}


void ShadowManager::RenderShadowCubeMap(int LightIndex, std::vector<NiNode*>* refMap, D3DXVECTOR4* ShadowData, bool enabled) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DXMATRIX View, Proj;
	D3DXVECTOR3 Eye, At, Up;

	bool HasWater = TheShaderManager->ShaderConst.HasWater;
	for (int L = 0; L <= LightIndex; L++) {
		SetShadowCubeMapRegisters(L);
		if (ShadowCubeMapStaticTracker[L] && EnableStaticMaps) { ProfileCount(Cnt_CubeLightsCached); continue; }
		ProfileCount(Cnt_CubeLightsDrawn);

		// Walk the scene graph once per light to gather renderable geometry, then reuse
		// the flat list across all 6 cube faces instead of re-traversing per face.
		CubeMapGeoList.clear();
		if (enabled)
			for (NiNode* RefNode : refMap[L])
				CollectCubeMapGeometry(RefNode, HasWater, CubeMapGeoList);

		// World transforms are constant across the 6 faces, so build each geo's camera-relative
		// matrix once here and reuse it per face instead of rebuilding it inside Render() 6x.
		CubeMapGeoWorld.resize(CubeMapGeoList.size());
		for (size_t g = 0; g < CubeMapGeoList.size(); g++)
			CreateD3DMatrix(&CubeMapGeoWorld[g], &CubeMapGeoList[g]->m_worldTransform);

		float FarPlane = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[L].w;
		D3DXMatrixPerspectiveFovRH(&Proj, D3DXToRadian(90.0f), 1.0f, 1.0f, FarPlane);
		for (int Face = 0; Face < 6; Face++) {
			At.x = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[L].x;
			At.y = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[L].y;
			At.z = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[L].z;
			Eye = At;
			GetCubeFaceAtUp(Face, At, Up);
			D3DXMatrixLookAtRH(&View, &Eye, &At, &Up);
			TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj = View * Proj;
			Device->SetDepthStencilSurface(ShadowCubeMapDepthSurface[L][Face]);
			Device->SetRenderTarget(0, ShadowCubeMapSurface[L][Face]);
			Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
			if (enabled) {
				// Cull geometry against this face's 90-degree frustum; each object
				// typically overlaps only 1-2 of the 6 faces.
				D3DXPLANE Frustum[6];
				GetFrustumPlanes(Frustum, &TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj);
				Device->BeginScene();
				SetupCubeMapRenderState();
				for (size_t g = 0; g < CubeMapGeoList.size(); g++)
					if (InFrustum(Frustum, CubeMapGeoList[g])) Render(CubeMapGeoList[g], ShadowData, &CubeMapGeoWorld[g]);
				Device->EndScene();
			}
		}
	}
}

void ShadowManager::RenderShadowCubeMapActor(int LightIndex, std::vector<NiNode*>* refMap, D3DXVECTOR4* ShadowData, bool enabled) {
	IDirect3DDevice9* Device = TheRenderManager->device;

	// araf Cap this due exterior shadows hack
	for (int L = 0; L <= 1; L++) {
		SetShadowCubeMapRegisters(L);
		if (ShadowCubeMapStaticTracker[L] && EnableStaticMaps) continue;

		if (enabled) {
			ComputeCubeFaceFrusta(L); // 6 face view-projections + frustums, reused to cull every actor geo
			Device->BeginScene();
			SetupCubeMapRenderState();
			for (NiNode* RefNode : refMap[L])
				RenderObjectPointActor(RefNode, ShadowData, TheShaderManager->ShaderConst.HasWater, L);
			Device->EndScene();
		}
	}
}

//TODO: rename
void ShadowManager::RenderExteriorShadows() {
	if (!Player->GetWorldSpace()) return;
	ScopeTimer profile(Phase_ExtTotal);

	SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors = SelectExteriorShadowSettings();

	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DXVECTOR4* ShadowData     = &TheShaderManager->ShaderConst.Shadow.Data;
	D3DXVECTOR4* ShadowSkinData = &TheShaderManager->ShaderConst.Shadow.ShadowSkinData;
	D3DXVECTOR4* OrthoData      = &TheShaderManager->ShaderConst.Shadow.OrthoData;
	D3DXVECTOR4* ShadowLightDir = &TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
	D3DXVECTOR4 OrthoDir        = D3DXVECTOR3(0.05f, 0.05f, 1.0f);

	CurrentVertex = ShadowMapVertex;
	CurrentPixel  = ShadowMapPixel;
	ClearShadowCubeMaps(Device, -1, ShadowCubeMapStateEnum::Exterior);

	D3DXVECTOR3 At, SkinAt;
	ComputeExteriorLookAt(At, SkinAt, ShadowsExteriors);
	AdjustShadowLightDir(ShadowLightDir);

	{ ScopeTimer profileBuild(Phase_BuildGeoItems); BuildExteriorGeoItems(ShadowsExteriors); }

	if (TheSettingManager->SettingsShadows.Exteriors.UseIntervalUpdate && TheShaderManager->isFullyInitialized) {
		D3DXVECTOR4 ShadowLightDirInterval;
		if (!UpdateShadowLightDirInterval(ShadowLightDir, ShadowLightDirInterval)) return;
		RenderShadowMap(MapNear, ShadowsExteriors, &At,     &ShadowLightDirInterval, ShadowData);
		RenderShadowMap(MapFar,  ShadowsExteriors, &At,     &ShadowLightDirInterval, ShadowData);
		RenderShadowMap(MapSkin, ShadowsExteriors, &SkinAt, &ShadowLightDirInterval, ShadowData);
	} else {
		RenderShadowMap(MapNear, ShadowsExteriors, &At,     ShadowLightDir, ShadowData);
		RenderShadowMap(MapFar,  ShadowsExteriors, &At,     ShadowLightDir, ShadowData);
		RenderShadowMap(MapSkin, ShadowsExteriors, &SkinAt, ShadowLightDir, ShadowData);
	}

	RenderShadowMap(MapOrtho, ShadowsExteriors, &At, &OrthoDir, ShadowData);

	ShadowData->y    = ShadowsExteriors->Darkness;
	ShadowData->z    = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapNear];
	ShadowData->w    = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapFar];
	OrthoData->z     = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapOrtho];
	ShadowSkinData->z = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapSkin];
}

//TODO: rename, doesn't apply solely to interiors
void ShadowManager::RenderInteriorShadows() {
	ScopeTimer profile(Phase_IntTotal);
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DXVECTOR4* ShadowData = &TheShaderManager->ShaderConst.ShadowCube.Data;

	CurrentVertex = ShadowCubeMapVertex;
	SettingsShadowStruct::InteriorsStruct* ShadowSettings = SelectInteriorShadowSettings();

	if (CurrentCell != Player->parentCell) HandleCellChange();
	UpdateStaticMapsCounter();

	AlphaEnabled = ShadowSettings->AlphaEnabled;

	NiPointLight* ShadowCastLights[12]  = { NULL };
	NiPointLight* ShadowCullLights[24]  = { NULL };
	NiPointLight* GeneralPointLights[2] = { NULL };
	int ShadowCastLightIndex = -1, ShadowCullLightIndex = -1, GeneralPointLightIndex = -1;

	if (ShadowLightPointSettings->bEnabled) {
		// araf Int shadows in 'behaves as exterior' cells for Immersive Interiors
		/*
		if (!(Player->parentCell->flags0 & Player->parentCell->kFlags0_BehaveLikeExterior && ShadowLightDir->z > 0.01f)) {
		*/
			FakeExtShadowLightDirSet = false;
			FakeExtShadowLightDirCnt = 0;
			GetShadowSceneLights(ShadowCastLights, ShadowCullLights, GeneralPointLights, ShadowCastLightIndex, ShadowCullLightIndex, GeneralPointLightIndex, ShadowLightPointSettings);
			SetAllShadowCastLightPos(ShadowCastLights, ShadowCastLightIndex);
			SetAllShadowCullLightPos(ShadowCullLights, ShadowCullLightIndex);
			if (Player->GetWorldSpace()) {
				RenderShadowCubeMapExt(ShadowCastLights, ShadowCastLightIndex, ShadowLightPointSettings->fShadowObjectScanRadius, ShadowSettings, ShadowData);
				SetAllGeneralLightPos(GeneralPointLights, GeneralPointLightIndex);
			} else {
				RenderShadowCubeMapInt(ShadowCastLights, ShadowCastLightIndex, ShadowLightPointSettings->fShadowObjectScanRadius, ShadowSettings, ShadowData);
				ClearShadowCubeMaps(Device, ShadowCastLightIndex, ShadowCubeMapStateEnum::Interior);
				GeneralPointLightIndex = -1;
			}
			ShadowData->y = ShadowSettings->Darkness;
		/*
		else {
			ShadowCastLightIndex = 0;
			RenderShadowCubeMapFakeInt(ShadowCastLightIndex, ShadowSettings, ShadowData);
			ClearShadowCubeMaps(Device, ShadowCastLightIndex, ShadowCubeMapStateEnum::Interior);
			ShadowData->y = 2.0f;
		}
		*/
	} else {
		if (Player->GetWorldSpace()) {
			GetShadowSceneLights(ShadowCastLights, ShadowCullLights, GeneralPointLights, ShadowCastLightIndex, ShadowCullLightIndex, GeneralPointLightIndex, ShadowLightPointSettings);
			SetAllShadowCastLightPos(ShadowCastLights, ShadowCastLightIndex);
			SetAllShadowCullLightPos(ShadowCullLights, ShadowCullLightIndex);
			SetAllGeneralLightPos(GeneralPointLights, GeneralPointLightIndex);
		}
		ShadowCastLightIndex = -1;
	}

	if (ShadowCastLightIndex   < ShadowCubeLightCount)    ClearShadowCubeMaps(Device, ShadowCastLightIndex);
	if (ShadowCullLightIndex   < ShadowCubeCullLightCount) ClearShadowCubeLightCullRegister(ShadowCullLightIndex);
	if (GeneralPointLightIndex < GeneralPointLightCount)   ClearGeneralPointLightRegister(GeneralPointLightIndex);

	ShadowCubeLightCount     = ShadowCastLightIndex;
	ShadowCubeCullLightCount = ShadowCullLightIndex;
	GeneralPointLightCount   = GeneralPointLightIndex;

	ShadowData->z = 1.0f / (float)ShadowSettings->ShadowCubeMapSize;
}

#endif // SHADOWS DISABLED

void ShadowManager::RenderShadowMaps() {
	Global->RenderShadowMaps(); //Window reflections seem to be rendered here

	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* DepthSurface = NULL;

#if defined(OBLIVION)
	// This part "creates" a fake canopy map only one time to avoid random canopy shadows if i forgot to replace a shader.
	// By now i cannot disable the canopy map pass in Oblivion.ini otherwise the game changes the shaders used for the rendering.
	NiRenderedTexture* CanopyMap = *(NiRenderedTexture**)0x00B4310C;
	if (!CanopyMap) {
		NiRenderedTexture* (__cdecl * CreateNiRenderedTexture)(UInt32, UInt32, NiRenderer*, NiTexture::FormatPrefs*) = (NiRenderedTexture * (__cdecl*)(UInt32, UInt32, NiRenderer*, NiTexture::FormatPrefs*))0x0072A9B0;
		void(__cdecl * SetTextureCanopyMap)(NiRenderedTexture*) = (void(__cdecl*)(NiRenderedTexture*))0x00441850;
		NiTexture::FormatPrefs FP = { NiRenderedTexture::PixelLayout::kPixelLayout_TrueColor32, NiRenderedTexture::AlphaFormat::kAlpha_Smooth, NiRenderedTexture::MipMapFlag::kMipMap_Default };
		SetTextureCanopyMap(CreateNiRenderedTexture(1, 1, TheRenderManager, &FP));
	}
#endif

	ShadowProfileFrameBegin();
	Device->GetDepthStencilSurface(&DepthSurface);
	TheRenderManager->SetupSceneCamera();
	{
		ScopeTimer profile(Phase_FrameTotal);
		RenderExteriorShadows();
		// RenderInteriorShadows(); // SHADOWS DISABLED: interior/point-light path dummied out
	}
	Device->SetDepthStencilSurface(DepthSurface);
	ShadowProfileFrameEnd();
}

#if 0 // SHADOWS DISABLED: dead reference — shadow/cube map clears + light-position register setters (unused by ortho path)
void ShadowManager::ClearShadowMap(IDirect3DDevice9* Device) {
	Device->SetRenderTarget(0, ShadowMapSurface[MapNear]);
	Device->SetDepthStencilSurface(ShadowMapDepthSurface[MapNear]);
	Device->SetViewport(&ShadowMapViewPort[MapNear]);
	Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
	Device->SetRenderTarget(0, ShadowMapSurface[MapFar]);
	Device->SetDepthStencilSurface(ShadowMapDepthSurface[MapFar]);
	Device->SetViewport(&ShadowMapViewPort[MapFar]);
	Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
	Device->SetRenderTarget(0, ShadowMapSurface[MapOrtho]);
	Device->SetDepthStencilSurface(ShadowMapDepthSurface[MapOrtho]);
	Device->SetViewport(&ShadowMapViewPort[MapOrtho]);
	Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
	Device->SetRenderTarget(0, ShadowMapSurface[MapSkin]);
	Device->SetDepthStencilSurface(ShadowMapDepthSurface[MapSkin]);
	Device->SetViewport(&ShadowMapViewPort[MapSkin]);
	Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
}

void ShadowManager::ClearShadowCubeMaps(IDirect3DDevice9* Device, int From, ShadowCubeMapStateEnum NewState) {
	if (ShadowCubeMapState != NewState) {
		ClearShadowMap(Device);
		ClearShadowCubeMaps(Device, From);
		ShadowCubeMapState = NewState;
	}
}

void ShadowManager::ClearShadowCubeLightRegister(int From) {

	int del1 = (12 - (From + 1)) * sizeof(D3DXVECTOR4);
	int del2 = (12 - (From + 1)) * sizeof(float);
	memset(TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition + ((From + 1)), 0, del1);
	memset(ShadowCubeMapStaticValue + ((From + 1)), 0, del2);
}

void ShadowManager::ClearShadowCubeLightCullRegister(int From) {

	int del = (24 - (From + 1)) * 16;
	memset(TheShaderManager->ShaderConst.ShadowMap.ShadowCullLightPosition + ((From + 1)), 0, del);
}

void ShadowManager::ClearGeneralPointLightRegister(int From) {

	int del = (2 - (From + 1)) * 16;
	memset(TheShaderManager->ShaderConst.PointLights.LightPosition + ((From + 1)), 0, del);
}

void ShadowManager::ClearShadowCubeMaps(IDirect3DDevice9* Device, int From) {

	ClearShadowCubeLightRegister(From);
	for (int L = From + 1; L < 12; L++) {
		for (int Face = 0; Face < 6; Face++) {
			Device->SetRenderTarget(0, ShadowCubeMapSurface[L][Face]);
			Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DXCOLOR(1.0f, 0.25f, 0.25f, 0.55f), 1.0f, 0L);
		}
	}
}

int ShadowManager::GetShadowSceneLights(NiPointLight** ShadowCastLights, NiPointLight** ShadowCullLights, NiPointLight** GeneralPointLights, int& shadowCastLightIndex, int& shadowCullLightIndex, int& GeneralPointLightIndex, SettingsShadowPointLightsStruct* ShadowSettings) {
	SettingsMainStruct::EquipmentModeStruct* EquipmentModeSettings = &TheSettingManager->SettingsMain.EquipmentMode;
	bool TorchOnBeltEnabled = EquipmentModeSettings->Enabled && EquipmentModeSettings->TorchKey != 255;
	int shadowCastIndex = -1, shadowCullIndex = -1, LightIndex = -1;

	CollectSceneLights();

	for (auto& [key, Light] : SceneLights) { // nearest light first
		if (LightIndex < 1) GeneralPointLights[++LightIndex] = Light;
		bool CastShadow = true;
		if (TorchOnBeltEnabled && Light->CanCarry == 2) {
			HighProcessEx* Process = (HighProcessEx*)Player->process;
			if (Process->OnBeltState == HighProcessEx::State::In) CastShadow = false;
		}
		if (CategorizeSceneLight(Light, shadowCastIndex, shadowCullIndex, ShadowCastLights, ShadowCullLights, ShadowSettings, CastShadow)) break;
	}

	shadowCastLightIndex  = shadowCastIndex;
	shadowCullLightIndex  = shadowCullIndex;
	GeneralPointLightIndex = LightIndex;
	return shadowCastIndex;
}

void ShadowManager::SetAllShadowCastLightPos(NiPointLight** Lights, int LightIndex) {
	for (int L = 0; L <= LightIndex; L++) {
		SetShadowCastLightPos(Lights, L);
	}
}

void ShadowManager::SetShadowCastLightPos(NiPointLight** Lights, int index) {
	D3DXVECTOR3 Eye;
	NiPoint3* LightPos = &Lights[index]->m_worldTransform.pos;

	float FarPlane = Lights[index]->Spec.r; // Light radius is stored in Spec.r, Spec.g and Spec.b for NiPointLight
	if (Lights[index]->CanCarry) FarPlane = 257.00f; // Set torch shadow to a fixed value to obtain a better effect
	Eye.x = LightPos->x - TheRenderManager->CameraPosition.x;
	Eye.y = LightPos->y - TheRenderManager->CameraPosition.y;
	Eye.z = LightPos->z - TheRenderManager->CameraPosition.z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].x = Eye.x;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].y = Eye.y;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].z = Eye.z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].w = FarPlane;
}

void ShadowManager::SetAllShadowCullLightPos(NiPointLight** Lights, int LightIndex) {
	for (int L = 0; L <= LightIndex; L++) {
		SetShadowCullLightPos(Lights, L);
	}
}

void ShadowManager::SetShadowCullLightPos(NiPointLight** Lights, int index) {
	D3DXVECTOR3 Eye;
	NiPoint3* LightPos = &Lights[index]->m_worldTransform.pos;
	float avgDiff = ((Lights[index]->Diff.r + Lights[index]->Diff.g + Lights[index]->Diff.b) / 3.0f);
	float FarPlane = Lights[index]->Spec.r * avgDiff;
	Eye.x = LightPos->x - TheRenderManager->CameraPosition.x;
	Eye.y = LightPos->y - TheRenderManager->CameraPosition.y;
	Eye.z = LightPos->z - TheRenderManager->CameraPosition.z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCullLightPosition[index].x = Eye.x;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCullLightPosition[index].y = Eye.y;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCullLightPosition[index].z = Eye.z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCullLightPosition[index].w = FarPlane;
}

void ShadowManager::SetAllGeneralLightPos(NiPointLight** Lights, int LightIndex) {
	for (int L = 0; L <= LightIndex; L++) {
		SetGeneralLightPos(Lights, L);
	}
}

void ShadowManager::SetGeneralLightPos(NiPointLight** Lights, int index) {
	NiPoint3* LightPos = &Lights[index]->m_worldTransform.pos;
	NiColor* LightColor = &Lights[index]->Diff;
	float FarPlane = Lights[index]->Spec.r; // Light radius is stored in Spec.r, Spec.g and Spec.b for NiPointLight
	TheShaderManager->ShaderConst.PointLights.LightPosition[index].x = LightPos->x;
	TheShaderManager->ShaderConst.PointLights.LightPosition[index].y = LightPos->y;
	TheShaderManager->ShaderConst.PointLights.LightPosition[index].z = LightPos->z;
	TheShaderManager->ShaderConst.PointLights.LightPosition[index].w = FarPlane;
	TheShaderManager->ShaderConst.PointLights.LightColor[index].x = LightColor->r;
	TheShaderManager->ShaderConst.PointLights.LightColor[index].y = LightColor->g;
	TheShaderManager->ShaderConst.PointLights.LightColor[index].z = LightColor->b;
}

void ShadowManager::SetShadowCubeMapRegisters(int index) {
	TheShaderManager->ShaderConst.ShadowMap.ShadowCubeMapLightPosition.x = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].x;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCubeMapLightPosition.y = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].y;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCubeMapLightPosition.z = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].z;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCubeMapLightPosition.w = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].w;
	TheShaderManager->ShaderConst.ShadowCube.Data.z = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[index].w;
}

#endif // SHADOWS DISABLED

void ShadowManager::ResetIntervals() {
	GameTime = -1;
}

void ShadowManager::LoadShadowLightPointSettings() {

	if (Player->GetWorldSpace()) {
		ShadowLightPointSettings = TheSettingManager->GetSettingsShadowPointLight(Player->GetWorldSpace()->GetEditorName());
		if (!ShadowLightPointSettings) ShadowLightPointSettings = TheSettingManager->GetSettingsShadowPointLight("DefaultExterior");
	}
	else {
		ShadowLightPointSettings = TheSettingManager->GetSettingsShadowPointLight(Player->parentCell->GetEditorName());
		if (!ShadowLightPointSettings) ShadowLightPointSettings = TheSettingManager->GetSettingsShadowPointLight("DefaultInterior");
	}
}

#if 0 // SHADOWS DISABLED: dead reference — point-light magic test (unused by ortho path)
bool ShadowManager::IsLightFromMagic(NiPointLight* light) {
	//TODO: a better way to check this
	return light->m_parent && strstr(light->m_parent->m_pcName, "agic") != NULL;
}

#endif // SHADOWS DISABLED

void ShadowManager::SetupGeoStreams(NiGeometryBufferData* GeoData) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	for (UInt32 i = 0; i < GeoData->StreamCount; i++)
		Device->SetStreamSource(i, GeoData->VBChip[i]->VB, 0, GeoData->VertexStride[i]);
	Device->SetIndices(GeoData->IB);
	if (GeoData->FVF)
		RenderState->SetFVF(GeoData->FVF, false);
	else
		RenderState->SetVertexDeclaration(GeoData->VertexDeclaration, false);
}

void ShadowManager::DrawGeoArrays(NiGeometryBufferData* GeoData, D3DPRIMITIVETYPE PrimitiveType, UINT VertCount) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	int StartIndex = 0;
	for (UInt32 i = 0; i < GeoData->NumArrays; i++) {
		int PrimitiveCount = GeoData->ArrayLengths ? (int)GeoData->ArrayLengths[i] - 2 : GeoData->TriCount;
		Device->DrawIndexedPrimitive(PrimitiveType, GeoData->BaseVertexIndex, 0, VertCount, StartIndex, PrimitiveCount);
		StartIndex += PrimitiveCount + 2;
		ProfileCount(Cnt_DrawCalls);
		if (gCubeBucket) ProfileCount(Cnt_DrawCallsCube);
		if (gCubeActorBucket) ProfileCount(Cnt_DrawCallsCubeActor);
		if (gTerrainBucket) ProfileCount(Cnt_DirTerrainDraws);
	}
}

#if 0 // SHADOWS DISABLED: dead reference — cube-face orientation helper (unused by ortho path)
void ShadowManager::GetCubeFaceAtUp(int Face, D3DXVECTOR3& At, D3DXVECTOR3& Up) {
	switch (Face) {
	case D3DCUBEMAP_FACE_POSITIVE_X: At += D3DXVECTOR3( 1.0f,  0.0f,  0.0f); Up = D3DXVECTOR3(0.0f,  1.0f,  0.0f); break;
	case D3DCUBEMAP_FACE_NEGATIVE_X: At += D3DXVECTOR3(-1.0f,  0.0f,  0.0f); Up = D3DXVECTOR3(0.0f,  1.0f,  0.0f); break;
	case D3DCUBEMAP_FACE_POSITIVE_Y: At += D3DXVECTOR3( 0.0f,  1.0f,  0.0f); Up = D3DXVECTOR3(0.0f,  0.0f,  1.0f); break;
	case D3DCUBEMAP_FACE_NEGATIVE_Y: At += D3DXVECTOR3( 0.0f, -1.0f,  0.0f); Up = D3DXVECTOR3(0.0f,  0.0f, -1.0f); break;
	case D3DCUBEMAP_FACE_POSITIVE_Z: At += D3DXVECTOR3( 0.0f,  0.0f, -1.0f); Up = D3DXVECTOR3(0.0f,  1.0f,  0.0f); break;
	case D3DCUBEMAP_FACE_NEGATIVE_Z: At += D3DXVECTOR3( 0.0f,  0.0f,  1.0f); Up = D3DXVECTOR3(0.0f,  1.0f,  0.0f); break;
	}
}

#endif // SHADOWS DISABLED

void ShadowManager::SetupSpeedTreeLeafShader(NiGeometry* Geo, D3DXVECTOR4* ShadowData) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	NiVector4* RockParams = (NiVector4*)kRockParams;
	NiVector4* RustleParams = (NiVector4*)kRustleParams;
	NiVector4* WindMatrixes = (NiVector4*)kWindMatrixes;
	SpeedTreeLeafShaderProperty* STProp = (SpeedTreeLeafShaderProperty*)Geo->GetProperty(NiProperty::PropertyType::kType_Lighting);
	BSTreeNode* Node = (BSTreeNode*)Geo->m_parent->m_parent;
	NiDX9SourceTextureData* Texture = (NiDX9SourceTextureData*)Node->TreeModel->LeavesTexture->rendererData;
	ShadowData->x = 2.0f;
	Device->SetVertexShaderConstantF(63, (float*)&BillboardRight, 1);
	Device->SetVertexShaderConstantF(64, (float*)&BillboardUp, 1);
	Device->SetVertexShaderConstantF(65, (float*)RockParams, 1);
	Device->SetVertexShaderConstantF(66, (float*)RustleParams, 1);
	Device->SetVertexShaderConstantF(67, (float*)WindMatrixes, 16);
	Device->SetVertexShaderConstantF(83, STProp->leafData->leafBase, 48);
	RenderState->SetTexture(0, Texture->dTexture);
	RenderState->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
	RenderState->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
	RenderState->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT, false);
	RenderState->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT, false);
	RenderState->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT, false);
}

void ShadowManager::SetupAlphaTexture(NiGeometry* Geo, BSShaderProperty* LProp, D3DXVECTOR4* ShadowData) {
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	NiAlphaProperty* AProp = (NiAlphaProperty*)Geo->GetProperty(NiProperty::PropertyType::kType_Alpha);
	if (AProp->flags & NiAlphaProperty::AlphaFlags::ALPHA_BLEND_MASK || AProp->flags & NiAlphaProperty::AlphaFlags::TEST_ENABLE_MASK) {
		if (NiTexture* Texture = *((BSShaderPPLightingProperty*)LProp)->textures[0]) {
			ShadowData->y = 1.0f;
			RenderState->SetTexture(0, Texture->rendererData->dTexture);
			RenderState->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP, false);
			RenderState->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP, false);
			RenderState->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT, false);
			RenderState->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT, false);
			RenderState->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT, false);
		}
	}
}

void ShadowManager::RenderSkinnedGeo(NiGeometry* Geo, D3DXVECTOR4* ShadowData) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiGeometryData* ModelData = Geo->geomData;
	NiSkinInstance* SkinInstance = Geo->skinInstance;
	NiD3DShaderDeclaration* ShaderDeclaration = Geo->shader->ShaderDeclaration;
	NiSkinPartition* SkinPartition = SkinInstance->SkinPartition;
	D3DPRIMITIVETYPE PrimitiveType = (SkinPartition->Partitions[0].Strips == 0) ? D3DPT_TRIANGLELIST : D3DPT_TRIANGLESTRIP;
	ShadowData->x = 1.0f;
	TheRenderManager->CalculateBoneMatrixes(SkinInstance, &Geo->m_worldTransform);
	if (SkinInstance->SkinToWorldWorldToSkin) memcpy(&TheShaderManager->ShaderConst.ShadowMap.ShadowWorld, SkinInstance->SkinToWorldWorldToSkin, 0x40);
	for (UInt32 p = 0; p < SkinPartition->PartitionsCount; p++) {
		int StartRegister = 9;
		NiSkinPartition::Partition* Partition = &SkinPartition->Partitions[p];
		for (int i = 0; i < Partition->Bones; i++) {
			UInt16 NewIndex = (Partition->pBones == NULL) ? i : Partition->pBones[i];
			Device->SetVertexShaderConstantF(StartRegister, ((float*)SkinInstance->BoneMatrixes) + (NewIndex * 3 * 4), 3);
			StartRegister += 3;
		}
		NiGeometryBufferData* GeoData = Partition->BuffData;
		TheRenderManager->PackSkinnedGeometryBuffer(GeoData, ModelData, SkinInstance, Partition, ShaderDeclaration);
		SetupGeoStreams(GeoData);
		CurrentVertex->SetCT();
		CurrentPixel->SetCT();
		DrawGeoArrays(GeoData, PrimitiveType, Partition->Vertices);
	}
}

#if 0 // SHADOWS DISABLED: dead reference — cube actor frusta/draw + interior light classification & selection (unused by ortho path)
// Build the 6 cube-face view-projections and their frustums for a light, once per light. The
// light position and far plane are constant across all of the light's actor geos, so this is
// hoisted out of the per-geo/per-partition draw path (where View*Proj used to be rebuilt every
// face of every partition). Matrices are byte-identical to the old per-face construction.
void ShadowManager::ComputeCubeFaceFrusta(int lightIndex) {
	float FarPlane = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[lightIndex].w;
	D3DXMATRIX Proj;
	D3DXMatrixPerspectiveFovRH(&Proj, D3DXToRadian(90.0f), 1.0f, 1.0f, FarPlane);
	D3DXVECTOR3 LightPos(
		TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[lightIndex].x,
		TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[lightIndex].y,
		TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[lightIndex].z);
	for (int Face = 0; Face < 6; Face++) {
		D3DXVECTOR3 Eye = LightPos, At = LightPos, Up;
		GetCubeFaceAtUp(Face, At, Up);
		D3DXMATRIX View;
		D3DXMatrixLookAtRH(&View, &Eye, &At, &Up);
		CubeFaceViewProj[Face] = View * Proj;
		GetFrustumPlanes(CubeFaceFrustum[Face], &CubeFaceViewProj[Face]);
	}
}

void ShadowManager::RenderActorFaces(NiGeometryBufferData* GeoData, D3DPRIMITIVETYPE PrimitiveType, UINT VertCount, int lightIndex) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	for (int Face = 0; Face < 6; Face++) {
		if (!CubeActorFaceVisible[Face]) continue; // geo's bound doesn't reach this face
		TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj = CubeFaceViewProj[Face];
		Device->SetDepthStencilSurface(ShadowCubeMapDepthSurface[lightIndex][Face]);
		Device->SetRenderTarget(0, ShadowCubeMapSurface[lightIndex][Face]);
		CurrentVertex->SetCT();
		CurrentPixel->SetCT();
		DrawGeoArrays(GeoData, PrimitiveType, VertCount);
	}
}

void ShadowManager::RenderActorSkinnedGeo(NiGeometry* Geo, D3DXVECTOR4* ShadowData, int lightIndex) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiGeometryData* ModelData = Geo->geomData;
	NiSkinInstance* SkinInstance = Geo->skinInstance;
	NiD3DShaderDeclaration* ShaderDeclaration = Geo->shader->ShaderDeclaration;
	NiSkinPartition* SkinPartition = SkinInstance->SkinPartition;
	D3DPRIMITIVETYPE PrimitiveType = (SkinPartition->Partitions[0].Strips == 0) ? D3DPT_TRIANGLELIST : D3DPT_TRIANGLESTRIP;
	ShadowData->x = 1.0f;
	TheRenderManager->CalculateBoneMatrixes(SkinInstance, &Geo->m_worldTransform);
	if (SkinInstance->SkinToWorldWorldToSkin) memcpy(&TheShaderManager->ShaderConst.ShadowMap.ShadowWorld, SkinInstance->SkinToWorldWorldToSkin, 0x40);
	for (UInt32 p = 0; p < SkinPartition->PartitionsCount; p++) {
		int StartRegister = 9;
		NiSkinPartition::Partition* Partition = &SkinPartition->Partitions[p];
		for (int i = 0; i < Partition->Bones; i++) {
			UInt16 NewIndex = (Partition->pBones == NULL) ? i : Partition->pBones[i];
			Device->SetVertexShaderConstantF(StartRegister, ((float*)SkinInstance->BoneMatrixes) + (NewIndex * 3 * 4), 3);
			StartRegister += 3;
		}
		NiGeometryBufferData* GeoData = Partition->BuffData;
		TheRenderManager->PackSkinnedGeometryBuffer(GeoData, ModelData, SkinInstance, Partition, ShaderDeclaration);
		SetupGeoStreams(GeoData);
		RenderActorFaces(GeoData, PrimitiveType, Partition->Vertices, lightIndex);
	}
}

void ShadowManager::SetupCubeMapRenderState() {
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	RenderState->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE, RenderStateArgs);
	RenderState->SetRenderState(D3DRS_ZWRITEENABLE, 1, RenderStateArgs);
	RenderState->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE, RenderStateArgs);
	RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, 0, RenderStateArgs);
	Device->SetViewport(&ShadowCubeMapViewPort);
	RenderState->SetVertexShader(ShadowCubeMapVertexShader, false);
	if (Player->GetWorldSpace())
		RenderState->SetPixelShader(ShadowCubeMapExteriorPixelShader, false);
	else
		RenderState->SetPixelShader(ShadowCubeMapPixelShader, false);
}

void ShadowManager::ClearCubeMapNodeLists() {
	for (int i = 0; i < 12; i++) {
		CubeMapRefMap[i].clear();
		CubeMapActorMap[i].clear();
	}
}

ShadowManager::RefLightInfo ShadowManager::BuildRefLightInfo(TESObjectREFR* Ref) {
	RefLightInfo Info;
	Info.Node = Ref->GetNode();
	UInt8 TypeID = Ref->baseForm->formType;
	Info.IsActorType = (TypeID >= TESForm::FormType::kFormType_NPC && TypeID <= TESForm::FormType::kFormType_LeveledCreature);
	Info.BoundRadius = Info.Node->GetWorldBoundRadius();
	NiBound* B = Ref->niNode->GetWorldBound();
	Info.CenterSum = (double)B->Center.x + (double)B->Center.y + (double)B->Center.z;
	Info.IsPlayer = (Ref->refID == Player->refID);
	return Info;
}

void ShadowManager::ClassifyRefForLight(const RefLightInfo& Info, NiPointLight** Lights, int L, float radiusScan, std::vector<NiNode*>* refMap, std::vector<NiNode*>* actorMap, double* StaticValues, bool* forceRedrawMap) {
	NiPoint3* LightPos = &Lights[L]->m_worldTransform.pos;
	float FarPlane = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[L].w;
	float radius = (Lights[L]->CanCarry || Info.IsActorType) ? FarPlane * 1.2f : FarPlane * radiusScan;

	if (Info.Node->GetDistance(LightPos) - Info.BoundRadius <= radius) {
		if (Lights[L]->CanCarry) forceRedrawMap[L] = true;
		if (Info.IsActorType) {
			// araf Exclude torches on the player, bugs in IFPV
			// TODO: Exclude player's own torch
			if (!Info.IsPlayer || !Lights[L]->CanCarry)
				actorMap[L].emplace_back(Info.Node);
		} else {
			refMap[L].emplace_back(Info.Node);
		}
		StaticValues[L] += Info.CenterSum;
	}
}

void ShadowManager::UpdateStaticTrackers(int LightIndex, double* StaticValues, bool* forceRedrawMap) {
	for (int i = 0; i <= LightIndex; i++) {
		if (StaticValues[i] == ShadowCubeMapStaticValue[i]) {
			ShadowCubeMapStaticTracker[i] = !forceRedrawMap[i];
		} else {
			ShadowCubeMapStaticTracker[i] = false;
			ShadowCubeMapStaticValue[i] = StaticValues[i];
		}
	}
}

#endif // SHADOWS DISABLED

SettingsShadowStruct::ExteriorsStruct* ShadowManager::SelectExteriorShadowSettings() {
	TESWeather* currentWeather = Tes->sky->firstWeather;
	if (currentWeather->weatherType != TESWeather::WeatherType::kType_Pleasant && currentWeather->weatherType != TESWeather::WeatherType::kType_None)
		return &TheSettingManager->SettingsShadows.ExteriorsAlt;
	return &TheSettingManager->SettingsShadows.Exteriors;
}

void ShadowManager::ComputeExteriorLookAt(D3DXVECTOR3& At, D3DXVECTOR3& SkinAt, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors) {
	NiNode* PlayerNode = Player->GetNode();
	SkinAt.x = PlayerNode->m_worldTransform.pos.x - TheRenderManager->CameraPosition.x;
	SkinAt.y = PlayerNode->m_worldTransform.pos.y - TheRenderManager->CameraPosition.y;
	SkinAt.z = PlayerNode->m_worldTransform.pos.z - TheRenderManager->CameraPosition.z;
	At.x = LookAtPosition.x - TheRenderManager->CameraPosition.x;
	At.y = LookAtPosition.y - TheRenderManager->CameraPosition.y;
	At.z = LookAtPosition.z - TheRenderManager->CameraPosition.z;
	D3DXVECTOR3 newPos(PlayerNode->m_worldTransform.pos.x, PlayerNode->m_worldTransform.pos.y, PlayerNode->m_worldTransform.pos.z);
	if (D3DXVec3Length(&(newPos - LookAtPosition)) > ShadowsExteriors->ShadowMapRadius[MapNear] / 2.0f)
		LookAtPosition = newPos;
}

#if 0 // SHADOWS DISABLED: dead reference — directional sun-dir adjust/interval + interior cell/scene-light helpers (unused by ortho path)
void ShadowManager::AdjustShadowLightDir(D3DXVECTOR4*& ShadowLightDir) {
	if (ShadowLightDir->z < 0.0f && TheShaderManager->ShaderConst.DayPhase == Dusk)
		ShadowLightDir = &TheShaderManager->ShaderConst.MasserDir;
	else if (ShadowLightDir->z < 0.0f && TheShaderManager->ShaderConst.DayPhase == Dawn)
		ShadowLightDir = &TheShaderManager->ShaderConst.SunDir;
	if (ShadowLightDir->z < 0.3f)
		ShadowLightDir->z = 0.3f;
}

bool ShadowManager::UpdateShadowLightDirInterval(D3DXVECTOR4* ShadowLightDir, D3DXVECTOR4& ShadowLightDirInterval) {
	float NewGameTime = TheShaderManager->ShaderConst.GameTime.y;
	if (NewGameTime < GameTime) NewGameTime += 24.0f;

	float gameTimeDiff = NewGameTime - GameTime;
	if (gameTimeDiff > 0.10f) {
		if (GameTime > 0) {
			if (!(gameTimeDiff > 0.15f)) {
				UpdateShadowLightDir = true;
				UpdateTargetTime = NewGameTime + 0.025f;
				ShadowLightDirNew = *ShadowLightDir;
				GameTime = NewGameTime;
			} else {
				TheShaderManager->isFullyInitialized = false;
				return false;
			}
		} else {
			ShadowLightDirInterval = *ShadowLightDir;
			ShadowLightDirOld = ShadowLightDirInterval;
			UpdateShadowLightDir = false;
			GameTime = NewGameTime;
		}
	}

	if (ShadowLightDir->z > 0.985f) UpdateShadowLightDir = false;

	if (UpdateShadowLightDir) {
		float newTime = NewGameTime - GameTime;
		float targetTime = UpdateTargetTime - GameTime;
		float t = newTime / targetTime;
		ShadowLightDirInterval.x = std::lerp(ShadowLightDirOld.x, ShadowLightDirNew.x, t);
		ShadowLightDirInterval.y = std::lerp(ShadowLightDirOld.y, ShadowLightDirNew.y, t);
		ShadowLightDirInterval.z = std::lerp(ShadowLightDirOld.z, ShadowLightDirNew.z, t);
		if (newTime >= targetTime) {
			UpdateShadowLightDir = false;
			ShadowLightDirOld = ShadowLightDirInterval;
			if (UpdateTargetTime >= 24.0f) GameTime = 0.0f;
		}
	} else {
		ShadowLightDirInterval = ShadowLightDirOld;
	}
	return true;
}

SettingsShadowStruct::InteriorsStruct* ShadowManager::SelectInteriorShadowSettings() {
	if (Player->GetWorldSpace()) {
		TESWeather* currentWeather = Tes->sky->firstWeather;
		CurrentPixel = ShadowCubeMapExteriorPixel;
		if (currentWeather->weatherType != TESWeather::WeatherType::kType_Pleasant && currentWeather->weatherType != TESWeather::WeatherType::kType_None)
			return &TheSettingManager->SettingsShadows.ExteriorsPointAlt;
		return &TheSettingManager->SettingsShadows.ExteriorsPoint;
	}
	CurrentPixel = ShadowCubeMapPixel;
	return &TheSettingManager->SettingsShadows.Interiors;
}

void ShadowManager::HandleCellChange() {
	ShadowCubeMapState = ShadowCubeMapStateEnum::None;
	CurrentCell = Player->parentCell;
	LoadShadowLightPointSettings();
	EnableStaticMapsFrameCount = 0;
	EnableStaticMaps = false;
}

void ShadowManager::UpdateStaticMapsCounter() {
	if (!EnableStaticMaps) {
		if (EnableStaticMapsFrameCount < EnableStaticMapsFrameThreshold)
			EnableStaticMapsFrameCount++;
		else
			EnableStaticMaps = true;
	}
}

void ShadowManager::CollectSceneLights() {
	SceneLights.clear();
	ShadowSceneNode* SceneNode = *(ShadowSceneNode**)kShadowSceneNode;
	NiTList<ShadowSceneLight>::Entry* Entry = SceneNode->lights.start;
	while (Entry) {
		NiPointLight* Light = Entry->data->sourceLight;
		int distance = (int)Light->GetDistance(&Player->pos);
		SceneLights.emplace_back(distance, Light);
		Entry = Entry->next;
	}
	// Order nearest-first; ties keep scene-graph order. Replaces a distance-keyed std::map
	// with back-probing collision handling (a node allocation + O(log n) probe per light).
	std::stable_sort(SceneLights.begin(), SceneLights.end(),
		[](const std::pair<int, NiPointLight*>& a, const std::pair<int, NiPointLight*>& b) { return a.first < b.first; });
}

bool ShadowManager::CategorizeSceneLight(NiPointLight* Light, int& shadowCastIndex, int& shadowCullIndex, NiPointLight** ShadowCastLights, NiPointLight** ShadowCullLights, SettingsShadowPointLightsStruct* ShadowSettings, bool CastShadow) {
	if (IsLightFromMagic(Light) && shadowCullIndex < (ShadowSettings->iShadowCullLightPoints - 1)) {
		Light->CanCarry = 1;
		ShadowCullLights[++shadowCullIndex] = Light;
	} else if (Light->CastShadows && CastShadow) {
		if (Light->Spec.r >= ShadowSettings->fShadowLightRadiusMin && Light->Spec.r <= ShadowSettings->fShadowLightRadiusMax
				&& shadowCastIndex < (ShadowSettings->iShadowLightPoints - 1)
				&& Player->GetNode()->GetDistance(&Light->m_worldTransform.pos) < 4000.0f) {
			ShadowCastLights[++shadowCastIndex] = Light;
		} else if (Light->Spec.r >= ShadowSettings->fShadowCullLightRadiusMin && Light->Spec.r <= ShadowSettings->fShadowCullLightRadiusMax
				&& shadowCullIndex < (ShadowSettings->iShadowCullLightPoints - 1)) {
			ShadowCullLights[++shadowCullIndex] = Light;
		}
	}
	return (shadowCastIndex + shadowCullIndex) == (ShadowSettings->iShadowLightPoints + ShadowSettings->iShadowCullLightPoints) - 1;
}

#endif // SHADOWS DISABLED

static __declspec(naked) void RenderShadowMapHook() {

	__asm
	{
		pushad
		mov		ecx, TheShadowManager
		call	ShadowManager::RenderShadowMaps
		popad
		jmp		kRenderShadowMapReturn
	}

}

void AddCastShadowFlag(TESObjectREFR* Ref, TESObjectLIGH* Light, NiPointLight* LightPoint) {

	SettingsShadowStruct::InteriorsStruct* ShadowSettings;
	SettingsMainStruct::EquipmentModeStruct* EquipmentModeSettings = &TheSettingManager->SettingsMain.EquipmentMode;

	if (Player->GetWorldSpace()) {
		ShadowSettings = &TheSettingManager->SettingsShadows.ExteriorsPoint;
	}
	else {
		ShadowSettings = &TheSettingManager->SettingsShadows.Interiors;
	}

	if (Light->lightFlags & TESObjectLIGH::LightFlags::kLightFlags_CanCarry) {
		LightPoint->CastShadows = ShadowSettings->TorchesCastShadows;
		LightPoint->CanCarry = 1;
		if (EquipmentModeSettings->Enabled) {
			if (Ref == Player) {
				if (Player->isThirdPerson) {
					if (Player->firstPersonSkinInfo->LightForm == Light) LightPoint->CastShadows = 0;
				}
				else {
					if (Player->ActorSkinInfo->LightForm == NULL && Player->firstPersonSkinInfo->LightForm == Light) LightPoint->CastShadows = 0;
				}
				LightPoint->CanCarry = 2;
			}
		}
	}
	else {
		LightPoint->CastShadows = !(Light->flags & TESForm::FormFlags::kFormFlags_NotCastShadows);
		LightPoint->CanCarry = 0;
	}

}

static __declspec(naked) void AddCastShadowFlagHook() {

	__asm
	{
#if defined(OBLIVION)
		mov     ecx, [esp + 0x158]
		pushad
		push	esi
		push	edi
		push	ecx
		call	AddCastShadowFlag
		pop		ecx
		pop		edi
		pop		esi
		popad
		pop		ecx
		pop		edi
		pop		esi
		pop		ebp
		pop		ebx
#elif defined(NEWVEGAS)
		mov     esi, [ebp + 0x008]
		pushad
		mov		ecx, [ebp - 0x164]
		push	eax
		push	ecx
		push	esi
		call	AddCastShadowFlag
		pop		esi
		pop		ecx
		pop		eax
		popad
		pop		ecx
		pop		esi
		mov     ecx, [ebp - 0x34]
#endif
		jmp		kAddCastShadowFlagReturn
	}

}

#if defined(NEWVEGAS)
void LeavesNodeName(BSTreeNode* TreeNode) {

	TreeNode->m_children.data[1]->SetName("Leaves");

}

static __declspec(naked) void LeavesNodeNameHook() {

	__asm
	{
		pushad
		push	eax
		call	LeavesNodeName
		pop		eax
		popad
		jmp		kLeavesNodeNameReturn
	}

}
#endif

void CreateShadowsHook() {

	WriteRelJump(kRenderShadowMapHook, (UInt32)RenderShadowMapHook);
	WriteRelJump(kAddCastShadowFlagHook, (UInt32)AddCastShadowFlagHook);

#if defined(NEWVEGAS)
	WriteRelJump(kLeavesNodeName, (UInt32)LeavesNodeNameHook);
#elif defined(OBLIVION)
	// This part "disables" the canopy map pass but values are anyway passed to the shaders (they are not used when OR shadows).
	// By now i cannot disable the canopy map pass in Oblivion.ini otherwise the game changes the shaders used for the rendering.
	WriteRelJump(0x0040D637, 0x0040D655); //Avoid tree canopy shadows rendering
	WriteRelJump(0x004425F7, 0x00442621); //Skip canopy map deinitialization (the game disposes/recreates the map every cell changed)
	WriteRelJump(0x004446FB, 0x00444723); //Skip canopy map deinitialization (the game disposes/recreates the map every cell changed)
	WriteRelJump(0x00444CCF, 0x00444CF9); //Skip canopy map deinitialization (the game disposes/recreates the map every cell changed)
	WriteRelJump(0x0055F5C9, 0x0055F5ED); //Skip canopy map deinitialization (the game disposes/recreates the map every cell changed)
#endif
}

void EditorCastShadowFlag(HWND Window, TESForm* Form) {

	if (Window && Form) {
		SetDlgItemTextA(Window, 0x697, "Does Not Cast Shadows");
		SetWindowPos(GetDlgItem(Window, 0x697), HWND_BOTTOM, 0, 0, 140, 15, SWP_NOMOVE | SWP_NOZORDER);
	}

}

static __declspec(naked) void EditorCastShadowFlagHook() {

	__asm
	{
		pushad
		push	eax
		push	edi
		call	EditorCastShadowFlag
		pop		edi
		pop		eax
		popad
		jmp		kEditorCastShadowFlagReturn
	}

}

void CreateEditorShadowsHook() {

	WriteRelJump(kEditorCastShadowFlagHook, (UInt32)EditorCastShadowFlagHook);

}
#elif defined(SKYRIM)
ShadowManager::ShadowManager() {

	Logger::Log("Starting the shadows manager...");
	TheShadowManager = this;

}
#endif