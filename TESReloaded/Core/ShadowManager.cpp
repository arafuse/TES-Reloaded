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
		Phase_PointClassify,   // point-light ref walk + per-slot classification (pure CPU)
		Phase_PointBake,       // point-light cube face draw submission
		Phase_PointTotal,      // whole RenderPointShadows (interiors AND exteriors)
		Phase_FrameTotal,      // exterior + interior for one frame
		Phase_COUNT
	};
	const char* const ShadowPhaseNames[Phase_COUNT] = {
		"BuildGeoItems", "Pass:Near", "Pass:Far", "Pass:Ortho", "Pass:Skin",
		"ExtTotal", "Int:Classify", "Int:CubeRender", "IntTotal",
		"Point:Classify", "Point:Bake", "PointTotal", "FrameTotal"
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
		// Point-light shadow cubes (unified interior/exterior path):
		Cnt_PointCandidates,    // lights passing the candidate filter this frame
		Cnt_PointSlotsActive,   // slots holding a light after assignment
		Cnt_PointRebakes,       // slot cubes (re)baked this frame (6 faces each)
		Cnt_COUNT
	};
	const char* const ShadowCounterNames[Cnt_COUNT] = {
		"DrawCalls", "DrawCalls:Cube", "DrawCalls:CubeActor", "RenderCalls:Cube", "CubeActorGeo",
		"InstancedDraws", "CubeLightsDrawn", "CubeLightsCached",
		"Dir:TerrainDraws", "Dir:ItemsInstanced", "Dir:ItemsImmNonInst", "Dir:ItemsImmAlpha",
		"Dir:Groups", "Dir:InstancedItems", "Dir:FallbackItems",
		"Point:Candidates", "Point:SlotsActive", "Point:Rebakes"
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

// Publish the deferred shadow bias constants. Two things are going on here:
//
// 1. The normal-offset values (.x/.y) are authored in shadow-map TEXELS, so they are scaled by each
//    cascade's world-units-per-texel (2 * Radius / Size). That keeps them correct if ShadowMapSize or
//    ShadowMapRadius is retuned. The legacy bias path wants the raw clip-space numbers instead, so the
//    scaling is skipped entirely when AdaptiveBias is off.
// 2. Bias is NOT weather-dependent, so the values come from the canonical Exteriors struct while only
//    the cascade geometry comes from the weather-selected one. This is also what makes live INI edits
//    take effect under the cloudy/precipitation tiers -- SelectExteriorShadowSettings returns a COPY
//    (ExteriorsAlt/ExteriorsPrecip, built at load), which the live-edit handlers never touch.
void ShadowManager::PublishShadowBiasConstants(SettingsShadowStruct::ExteriorsStruct* Selected) {
	auto& Ext = TheSettingManager->SettingsShadows.Exteriors;
	auto& Sm = TheShaderManager->ShaderConst.ShadowMap;

	float ScaleNear = 1.0f;
	float ScaleFar = 1.0f;
	if (Ext.AdaptiveBias) {
		if (Selected->ShadowMapSize[MapNear])
			ScaleNear = (2.0f * Selected->ShadowMapRadius[MapNear]) / (float)Selected->ShadowMapSize[MapNear];
		if (Selected->ShadowMapSize[MapFar])
			ScaleFar = (2.0f * Selected->ShadowMapRadius[MapFar]) / (float)Selected->ShadowMapSize[MapFar];
	}

	Sm.ShadowBiasDeferred.x = Ext.deferredNormBias * ScaleNear;
	Sm.ShadowBiasDeferred.y = Ext.deferredFarNormBias * ScaleFar;
	Sm.ShadowBiasDeferred.z = Ext.deferredConstBias;
	Sm.ShadowBiasDeferred.w = Ext.deferredFarConstBias;

	Sm.ShadowBiasAdaptive.x = Ext.BiasTerminatorWidth;
	Sm.ShadowBiasAdaptive.y = Ext.BiasMaxSlope;
	Sm.ShadowBiasAdaptive.z = Ext.AdaptiveBias ? 1.0f : 0.0f;
	// .w (sun-active flag) is deliberately NOT written here. It is owned by RenderExteriorShadows,
	// which sets it every exterior frame BEFORE its no-work early return -- this function only runs
	// on frames that already have sun, so writing .w here could never clear it.
}

// Startup publish. Uses the canonical Exteriors struct rather than SelectExteriorShadowSettings(),
// which depends on weather state that is not available this early.
void ShadowManager::InitShadowBiasConstants() {
	PublishShadowBiasConstants(&TheSettingManager->SettingsShadows.Exteriors);
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
	// Optional: instanced static shadow VS. If the binary is missing (not yet compiled), the
	// handle stays NULL and the renderer transparently falls back to the per-object path.
	ShadowMapInstancedVertex = new ShaderRecord();
	if (ShadowMapInstancedVertex->LoadShader("ShadowMapInstanced.vso")) Device->CreateVertexShader((const DWORD*)ShadowMapInstancedVertex->Function, &ShadowMapInstancedVertexShader);
}

void ShadowManager::CreateShadowMapSurfaces(IDirect3DDevice9* Device, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors) {
	// Zero-init all four map slots, then allocate them: MapOrtho (precipitation occlusion) first,
	// the directional sun maps (Near/Far/Skin) in the loop below. R32F color + D24S8 depth throughout.
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
		// The skin overlay is min-combined with the near map in the apply, which reuses the near texel size
		// (TESR_ShadowData.z) for the skin PCF kernel — so allocate skin at the NEAR resolution to keep the
		// kernel correctly scaled regardless of any [ExteriorsSkin] ShadowMapSize override. Texture (below) and
		// stored viewport share this Size, so they stay consistent.
		UINT Size = ShadowsExteriors->ShadowMapSize[m == ShadowMapTypeEnum::MapSkin ? ShadowMapTypeEnum::MapNear : m];
		if (!Size) continue;
		Device->CreateTexture(Size, Size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &ShadowMapTexture[m], NULL);
		ShadowMapTexture[m]->GetSurfaceLevel(0, &ShadowMapSurface[m]);
		Device->CreateDepthStencilSurface(Size, Size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowMapDepthSurface[m], NULL);
		ShadowMapViewPort[m] = { 0, 0, Size, Size, 0.0f, 1.0f };
	}

	// Previous-bake copies for the static crossfade. Allocated HERE, in the same function as the maps
	// they shadow, because EffectRecord::LoadTexture resolves a sampler's texture pointer ONCE at
	// effect load — the pointer must already exist by then, which is the same guarantee the main maps
	// rely on. Skipped entirely when the feature is off, so nothing pays the ~32 MB.
	ShadowMapTexturePrev[0] = ShadowMapTexturePrev[1] = NULL;
	ShadowMapSurfacePrev[0] = ShadowMapSurfacePrev[1] = NULL;
	StaticFadeReady = false;
	// CacheStaticShadows = 0 rebakes every frame, so the maps are never stale and nothing pops; the
	// rebake gate this feature installs would also silently turn caching back on there.
	if (ShadowsExteriors->FadeTime > 0.0f && ShadowsExteriors->CacheStaticShadows) {
		bool Ok = true;
		for (int m = ShadowMapTypeEnum::MapNear; m <= ShadowMapTypeEnum::MapFar; m++) {
			int r = m - ShadowMapTypeEnum::MapNear;
			UINT Size = ShadowsExteriors->ShadowMapSize[m];
			if (!Size) { Ok = false; break; }
			if (FAILED(Device->CreateTexture(Size, Size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &ShadowMapTexturePrev[r], NULL))) { Ok = false; break; }
			if (FAILED(ShadowMapTexturePrev[r]->GetSurfaceLevel(0, &ShadowMapSurfacePrev[r]))) { Ok = false; break; }
		}
		if (!Ok) {
			// A partial allocation must not leave a live D3D9 interface behind: StaticFadeReady is about
			// to go false, and TextureManager must never be able to hand a sampler a texture the feature
			// itself believes doesn't exist. Release whatever the loop above managed to create and NULL
			// it back out, so "failed" and "never attempted" are indistinguishable.
			for (int r = 0; r < 2; r++) {
				if (ShadowMapSurfacePrev[r]) { ShadowMapSurfacePrev[r]->Release(); ShadowMapSurfacePrev[r] = NULL; }
				if (ShadowMapTexturePrev[r]) { ShadowMapTexturePrev[r]->Release(); ShadowMapTexturePrev[r] = NULL; }
			}
		}
		StaticFadeReady = Ok;
		Logger::Log("[ShadowFade] previous-bake copies %s (FadeTime %.2fs)", Ok ? "allocated" : "FAILED - fading disabled", ShadowsExteriors->FadeTime);
	}
}

void ShadowManager::CreateCubeMapSurfaces(IDirect3DDevice9* Device, UINT CubeMapSize) {
	// One R32F cube per point-light slot. Faces render strictly sequentially with a per-face
	// depth clear, so a single shared depth-stencil surface serves every face of every cube.
	for (int i = 0; i < PointLightMax; i++) {
		ShadowCubeMapTexture[i] = NULL;
		for (int j = 0; j < 6; j++) ShadowCubeMapSurface[i][j] = NULL;
	}
	ShadowCubeMapDepthSurface = NULL;
	if (!TheSettingManager->SettingsShadows.Point.Enabled || !CubeMapSize) return;
	for (int i = 0; i < PointLightMax; i++) {
		if (FAILED(Device->CreateCubeTexture(CubeMapSize, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &ShadowCubeMapTexture[i], NULL))) {
			Logger::Log("ShadowManager: point cube map %d allocation FAILED (size %u)", i, CubeMapSize);
			ShadowCubeMapTexture[i] = NULL;
			continue;
		}
		for (int j = 0; j < 6; j++)
			ShadowCubeMapTexture[i]->GetCubeMapSurface((D3DCUBEMAP_FACES)j, 0, &ShadowCubeMapSurface[i][j]);
	}
	if (FAILED(Device->CreateDepthStencilSurface(CubeMapSize, CubeMapSize, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true, &ShadowCubeMapDepthSurface, NULL))) {
		Logger::Log("ShadowManager: point cube depth surface allocation FAILED (size %u)", CubeMapSize);
		ShadowCubeMapDepthSurface = NULL;
	}
}

ShadowManager::ShadowManager() {
	Logger::Log("Starting the shadows manager...");
	TheShadowManager = this;

	IDirect3DDevice9* Device = TheRenderManager->device;
	SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors = &TheSettingManager->SettingsShadows.Exteriors;

	InitShadowBiasConstants();

	// Shadow.Data.y is the shadow "darkness" multiplier, and it is only written on a DoSun frame
	// (RenderExteriorShadows). ShaderConst is a plain member that is never zeroed, so it must be
	// given a sane value here: the apply shader reads it as `darkness` on the very first exterior
	// frames after a load, and ShaderManager reads it as the LOWER bound of several std::clamp()
	// calls on ShadowLightDir.w -- where a garbage value above 1.0 would be undefined behavior.
	// 1.0 means "no darkening", the safe neutral default.
	TheShaderManager->ShaderConst.Shadow.Data.y = 1.0f;

	// Same reasoning for the sun-active flag: RenderExteriorShadows writes it on every worldspace
	// frame (which is also the only condition under which the apply effect runs), so nothing should
	// read it unwritten -- but since PublishShadowBiasConstants deliberately does not touch it, seed
	// it explicitly rather than leaving it as uninitialized memory. 0 = terminator ramp off.
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasAdaptive.w = 0.0f;

	UINT ShadowCubeMapSize = TheSettingManager->SettingsShadows.Point.ShadowCubeMapSize;

	CurrentCell = NULL;
	PointCurrentCell = NULL;
	PointSlotsActive = 0;
	PointSlotsShaded = 0;
	EnableStaticMaps = false;
	EnableStaticMapsFrameCount = 0;
	ShadowMapInstancedVertexShader = NULL;
	InstanceVB = NULL;
	InstanceVBCapacity = 0;
	InstanceGroupCount = 0;
	ShadowGeoCount = 0;
	CollectWorldSpace = false;
	CollectSkinnedOnly = false;
	CollectAnchor = D3DXVECTOR3(0.0f, 0.0f, 0.0f);

	StaticFadeT = 1.0f;
	StaticFadeLastMs = 0.0;
	ForceRebake = false;
	// Seeded to MapFar so the fair steady-state picker gives MapNear the first pick, matching this
	// codebase's existing near-first convention on the very first rebake.
	LastBakedRegion = MapFar;
	// Seeded for the same reason as Shadow.Data.y above: ShaderConst is a plain member that is never
	// zeroed, and 1.0 is the neutral "no crossfade" value the apply shader must see before the first
	// sun frame publishes anything.
	TheShaderManager->ShaderConst.ShadowMap.ShadowFadeData = D3DXVECTOR4(1.0f, 0.0f, 0.0f, 0.0f);

	for (int i = 0; i < 2; i++) {
		Regions[i].Valid = false; D3DXMatrixIdentity(&Regions[i].BakedViewProj); Regions[i].AnchorPos = D3DXVECTOR3(0.0f, 0.0f, 0.0f); Regions[i].BakedSunDir = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		D3DXMatrixIdentity(&Regions[i].PrevBakedViewProj); Regions[i].PrevAnchorPos = D3DXVECTOR3(0.0f, 0.0f, 0.0f); Regions[i].PrevValid = false;
	}

	LoadShadowShaders(Device);
	CreateShadowMapSurfaces(Device, ShadowsExteriors);
	CreateCubeMapSurfaces(Device, ShadowCubeMapSize);

	ShadowCubeMapViewPort = { 0, 0, ShadowCubeMapSize, ShadowCubeMapSize, 0.0f, 1.0f };
	for (int i = 0; i < PointLightMax; i++) {
		PointSlots[i].Light = NULL;
		PointSlots[i].BakedLightPos = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
		PointSlots[i].Checksum = 0.0;
		PointSlots[i].Valid = false;
		PointSlots[i].Intensity = 0.0f;
		PointSlots[i].LastFarPlane = 0.0f;
		PointSlots[i].LastLuminance = 0.0f;
		PointSlots[i].RetiringLight = NULL;
	}
	PointFadeLastMs = 0.0;

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

bool ShadowManager::InFrustum(D3DXPLANE* Frustum, NiGeometry* Geo) {

	NiBound* Bound = Geo->GetWorldBound();
	if (!Bound) return true; // Cannot cull without a bound; keep it.

	D3DXVECTOR3 Position = { Bound->Center.x - TheRenderManager->CameraPosition.x, Bound->Center.y - TheRenderManager->CameraPosition.y, Bound->Center.z - TheRenderManager->CameraPosition.z };
	for (int i = 0; i < 6; ++i) {
		if (D3DXPlaneDotCoord(&Frustum[i], &Position) <= -Bound->Radius) return false;
	}
	return true;

}

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


bool ShadowManager::InShadowFrustum(ShadowMapTypeEnum ShadowMapType, NiAVObject* Object) {

	float Distance = 0.0f;
	bool R = false;
	NiBound* Bound = Object->GetWorldBound();

	if (Bound) {
		// Cull in the same space the map's frustum lives in: anchor-relative during a world-anchored
		// bake (near/far cached maps), camera-relative otherwise (ortho). Must match the space the
		// terrain is subsequently drawn in (see Render()), or terrain is culled against the wrong frustum.
		float BaseX = CollectWorldSpace ? CollectAnchor.x : TheRenderManager->CameraPosition.x;
		float BaseY = CollectWorldSpace ? CollectAnchor.y : TheRenderManager->CameraPosition.y;
		float BaseZ = CollectWorldSpace ? CollectAnchor.z : TheRenderManager->CameraPosition.z;
		D3DXVECTOR3 Position = { Bound->Center.x - BaseX, Bound->Center.y - BaseY, Bound->Center.z - BaseZ };

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

// Flattens a ref's scene graph into renderable leaves for a cube bake. Both statics and actors go
// through this: a cube bake redraws everything it contains, so there is no reason to split them at
// draw time (the split exists only during classification, where actors use a wider radius).
// No water z-cull: submerged casters cast, matching the sun path (submerged-casters fix).
void ShadowManager::CollectCubeMapGeometry(NiAVObject* Object, std::vector<NiGeometry*>& Out) {

	if (Object && !(Object->m_flags & NiAVObject::kFlag_AppCulled)) {
		void* VFT = *(void**)Object;
		if (VFT == VFTNiNode || VFT == VFTBSFadeNode || VFT == VFTBSFaceGenNiNode || VFT == VFTBSTreeNode || VFT == VFTNiLODNode) {
			NiNode* Node = (NiNode*)Object;
			for (int i = 0; i < Node->m_children.end; i++) {
				CollectCubeMapGeometry(Node->m_children.data[i], Out);
			}
		}
		else if (VFT == VFTNiTriShape || VFT == VFTNiTriStrips) {
			NiGeometry* Geo = (NiGeometry*)Object;
			// Torch geometry is skipped by Render() anyway; drop it once here instead of
			// re-testing the name (and re-entering Render) for every one of the 6 cube faces.
			if (Geo->m_pcName && !memcmp(Geo->m_pcName, "Torch", 5)) return;
			// SpeedTree leaves are unusable here: they need billboarding the cube bake VS does not
			// implement, and Render()'s leaf path writes vertex constant c63 -- which is where the
			// cube VS keeps the light position. Trunks still cast.
			if (Geo->m_parent && Geo->m_parent->m_pcName && !memcmp(Geo->m_parent->m_pcName, "Leaves", 6)) return;
			if (Geo->shader) {
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
		// Terrain is drawn through this path with no precomputed matrix. In a world-anchored bake (near/far
		// cached maps) it must be placed anchor-relative like the statics (CreateD3DMatrixWorld), NOT
		// camera-relative — otherwise it lands offset by (Camera - Anchor) and its shadow slides across the
		// ground like a moving cloud as the camera drifts from the anchor. Ortho (CollectWorldSpace==false)
		// stays camera-relative as before.
		else if (CollectWorldSpace)
			CreateD3DMatrixWorld(&TheShaderManager->ShaderConst.ShadowMap.ShadowWorld, &Geo->m_worldTransform);
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

// Advance the static crossfade on REAL time, not game time. An accelerated timescale is one of the
// cases this fade exists to smooth, so the fade must still last FadeTime wall-clock seconds there.
void ShadowManager::AdvanceStaticFade() {
	double Now = TheFrameRateManager->GetPerformance(); // milliseconds since startup
	if (StaticFadeT >= 1.0f) { StaticFadeLastMs = Now; return; }
	double Delta = Now - StaticFadeLastMs;
	StaticFadeLastMs = Now;
	if (Delta <= 0.0) return; // clock hiccup / same-millisecond frame
	float FadeTime = TheSettingManager->SettingsShadows.Exteriors.FadeTime;
	if (FadeTime <= 0.0f) { StaticFadeT = 1.0f; return; }
	StaticFadeT += (float)(Delta / (1000.0 * (double)FadeTime));
	if (StaticFadeT > 1.0f) StaticFadeT = 1.0f;
}

// Snapshot BOTH cached maps and their matrices as the crossfade's source, then restart the clock.
// Both, even when only one region is about to rebake, for the reason given on StaticFadeT: the pair
// has to stay consistent or the near->far fallback mixes states across the cascade boundary.
void ShadowManager::BeginStaticCrossfade() {
	if (!StaticFadeEnabled()) return;
	IDirect3DDevice9* Device = TheRenderManager->device;
	for (int i = 0; i < 2; i++) {
		if (!Regions[i].Valid || !ShadowMapSurfacePrev[i]) { Regions[i].PrevValid = false; continue; }
		// The Prev surface is a D3DPOOL_DEFAULT render target with undefined contents until this blit
		// actually populates it. If it fails, leave PrevValid false rather than fading against
		// whatever garbage was left in VRAM reinterpreted as R32F depth.
		if (FAILED(Device->StretchRect(ShadowMapSurface[MapNear + i], NULL, ShadowMapSurfacePrev[i], NULL, D3DTEXF_NONE))) {
			Regions[i].PrevValid = false;
			continue;
		}
		Regions[i].PrevBakedViewProj = Regions[i].BakedViewProj;
		Regions[i].PrevAnchorPos     = Regions[i].AnchorPos;
		Regions[i].PrevValid         = true;
	}
	// Nothing to fade FROM unless both halves of the pair have history.
	StaticFadeT = (Regions[0].PrevValid && Regions[1].PrevValid) ? 0.0f : 1.0f;
}

// A rebake that follows a big jump — fast travel, coc, a load door — has no ground in common with the
// previous maps, so crossfading would drape the old location's shadows over the new one for FadeTime
// seconds. Snap instead. Each region is compared against ITS OWN radius, not a shared one: MapNear's
// radius (2048 by default) is far smaller than MapFar's (8192), and normal drift can never trip
// either -- near's rebake trigger is RebakeMarginNear * its radius (0.25 * 2048 = 512, well under
// 2048) and far's is RebakeMarginFar * its radius (0.5 * 8192 = 4096, under 8192) -- so a per-region
// test still only fires on an actual teleport, not on drift, while correctly catching a jump that
// clears near's smaller box but not far's larger one (e.g. a mine exit a few cells away).
void ShadowManager::SnapStaticFadeIfTeleported(SettingsShadowStruct::ExteriorsStruct* S) {
	if (!StaticFadeEnabled() || StaticFadeT >= 1.0f) return;
	for (int i = 0; i < 2; i++) {
		if (!Regions[i].PrevValid) { StaticFadeT = 1.0f; return; }
		D3DXVECTOR3 D = Regions[i].AnchorPos - Regions[i].PrevAnchorPos;
		float Len = D3DXVec3Length(&D);
		if (Len > S->ShadowMapRadius[MapNear + i]) { StaticFadeT = 1.0f; return; }
	}
}

// Publish the crossfade scalar and, only while one is in flight, the previous bake's sample matrices.
// The matrix math mirrors PublishCachedRegionSampleMatrix exactly — current clip -> camera-relative
// world, translated by (Camera - Anchor) to reach anchor-relative world, then the baked projection —
// but reads the Prev* fields.
void ShadowManager::PublishStaticFadeConstants() {
	auto& Sm = TheShaderManager->ShaderConst.ShadowMap; // as PublishShadowBiasConstants does
	Sm.ShadowFadeData.x = StaticFadeEnabled() ? StaticFadeT : 1.0f;
	if (Sm.ShadowFadeData.x >= 1.0f) return;
	for (int i = 0; i < 2; i++) {
		D3DXVECTOR3& A = Regions[i].PrevAnchorPos;
		D3DXMATRIX Trans;
		D3DXMatrixTranslation(&Trans, TheRenderManager->CameraPosition.x - A.x, TheRenderManager->CameraPosition.y - A.y, TheRenderManager->CameraPosition.z - A.z);
		Sm.ShadowCameraToLightPrev[i] = TheRenderManager->InvViewProjMatrix * Trans * Regions[i].PrevBakedViewProj;
	}
}

// A cached region is due for a rebake when it is invalid, the look-at focus has left the guard band
// the map was baked to cover, or the sun has rotated past the interval. Near uses a small margin
// (rebakes more often); far uses a large one (rarely).
bool ShadowManager::RegionNeedsRebake(ShadowMapTypeEnum ShadowMapType) {
	int r = ShadowMapType - MapNear;
	CachedRegion& Reg = Regions[r];
	if (!Reg.Valid) return true;

	// No rebake while a crossfade is in flight. A second rebake cannot be folded into one already
	// running: both buffers hold DEPTHS, and a lerp of two depths is meaningless, so there is nowhere
	// to put the partially faded state. Deferring by at most FadeTime is bounded staleness on top of
	// caching that is already deliberately stale. Forced bakes bypass this by not calling here.
	if (StaticFadeEnabled() && StaticFadeT < 1.0f) return false;

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
	// RIGID statics only. Skinned geometry (GeoData==NULL, drawn via RenderSkinnedGeo) is camera-relative
	// and would land in the wrong space in this anchor-relative bake, so it's routed to the per-frame
	// camera-relative overlay instead (along with actors).
	int w = 0;
	for (int i = 0; i < ShadowGeoCount; i++) if (!ShadowGeoPool[i].IsActor && ShadowGeoPool[i].GeoData != NULL) ShadowGeoPool[w++] = ShadowGeoPool[i];
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
	// Actors AND any skinned geometry (GeoData==NULL) are camera-relative (RenderSkinnedGeo) so they belong in
	// this camera-relative overlay, not the anchor-relative static bakes. CollectSkinnedOnly drops rigid
	// non-actor statics during the walk, so the pool comes back already limited to skinned + actor casters.
	CollectSkinnedOnly = true;
	BuildExteriorGeoItems(S, MapSkin);
	CollectSkinnedOnly = false;
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

// Append one cell's eligible shadow-casting refs to ShadowGeoPool. Node/flag/excluded/type/Forms
// eligibility and the ref-root frustum cull are resolved here; CollectExteriorGeo then walks the
// accepted sub-trees. A NULL cell is skipped, so callers can pass a grid slot without checking.
void ShadowManager::CollectCellGeo(TESObjectCELL* Cell, SettingsShadowStruct::FormsStruct* Forms, SettingsShadowStruct::ExcludedFormsList* ExcludedForms, bool HasWater, ShadowMapTypeEnum ShadowMapType) {
	if (!Cell) return;
	bool HasExcluded = ExcludedForms->size() > 0;
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

// Flatten every frustum-visible, Forms-allowed shadow-casting ref into ShadowGeoPool once per frame.
// World transforms, bounds, the water test, the per-geo leaf cull, the MinRadius cut, and instancing
// eligibility are all resolved here; RenderShadowMap then draws the flat list with no further culling.
//
// Like BuildPointGeoLists, the ONLY interior/exterior distinction is which ref list to walk. An
// interior flagged BehaveLikeExterior passes IsExteriorLike() and so runs these passes, but has no
// loaded exterior grid -- walking gridCellArray there collected nothing, leaving every map empty
// (precipitation was occluded by nothing at all in such cells).
void ShadowManager::BuildExteriorGeoItems(SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, ShadowMapTypeEnum ShadowMapType) {
	ShadowGeoCount = 0;
	SettingsShadowStruct::ExcludedFormsList* ExcludedForms = &ShadowsExteriors->ExcludedForms;
	bool HasWater = TheShaderManager->ShaderConst.HasWater;
	SettingsShadowStruct::FormsStruct* Forms = &ShadowsExteriors->Forms[ShadowMapType];
	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++)
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++)
				CollectCellGeo(Tes->gridCellArray->GetCell(x, y), Forms, ExcludedForms, HasWater, ShadowMapType);
	}
	else {
		CollectCellGeo(Player->parentCell, Forms, ExcludedForms, HasWater, ShadowMapType);
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

	// Overlay pass: only skinned geo (DrawViaSkin) and actors are drawn camera-relative here; skip rigid
	// non-actor statics now, before their bounds/matrix/instancing work, instead of collecting then discarding.
	if (CollectSkinnedOnly && !DrawViaSkin && !IsActorRef) return;

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
	if (!ShadowMapSurface[ShadowMapType]) return; // slot not allocated (e.g. ShadowMapSize 0) — nothing to render
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
	// Terrain lives in the exterior grid only, so an interior (incl. BehaveLikeExterior) has none to draw.
	if (!SkipTerrain && Player->GetWorldSpace()) {
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
	if (!Player->IsExteriorLike()) return false;
	return TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir.z > TheSettingManager->SettingsShadows.Exteriors.SunUpThreshold;
}

// Exterior shadow pass. Renders two independent, separately-gated things:
//  - Ortho depth map (DoOrtho): sampled by the precipitation effects (Rain/Snow/SnowAccumulation)
//    for occlusion; also publishes ShaderConst.ShadowMap.ShadowCameraToLight[MapOrtho]
//    (-> TESR_ShadowCameraToLightTransformOrtho) via SetupShadowMapMatrices.
//  - Directional sun maps MapNear/MapFar (DoSun): per-frame directional shadows, applied in image
//    space by ShadowsExteriors.fx, plus the MapSkin per-frame actor overlay (RenderActorOverlay).
// Point-light cube shadows are a separate pass (RenderPointShadows, driven from RenderShadowMaps).
void ShadowManager::RenderExteriorShadows() {
	if (!Player->IsExteriorLike()) return;
	bool DoOrtho = OrthoNeeded();
	bool DoSun   = SunShadowNeeded();
	// Published every exterior frame, including ones with no sun: the apply shader runs on a
	// broader condition than this function's shadow work, and its terminator ramp must switch
	// off when the maps stop updating (otherwise it darkens flat ground as the sun sets).
	TheShaderManager->ShaderConst.ShadowMap.ShadowBiasAdaptive.w = DoSun ? 1.0f : 0.0f;
	// The clock only advances on sun frames, so a gap would otherwise resume a stale crossfade
	// against maps from before the gap. The only gap that reaches this point is exterior-like with
	// the sun below SunUpThreshold (dusk/dawn/night) -- true interiors already returned above, at
	// the IsExteriorLike() check, so no bakes happen while this function is skipping its body and
	// there is nothing here for interiors to leave stale. (Even without this pin, the large real-time
	// delta accumulated across such a gap would clamp AdvanceStaticFade's clock straight to 1 on
	// resume; this just makes that explicit and immediate instead of one more frame of drift.)
	if (!DoSun) {
		StaticFadeT = 1.0f;
		Regions[0].PrevValid = Regions[1].PrevValid = false;
		TheShaderManager->ShaderConst.ShadowMap.ShadowFadeData.x = 1.0f;
	}
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
		// Occlusion-compare bias for the precipitation ray march, converted from world units to the
		// normalized ortho depth the map stores (the projection spans 2 * FarPlane). Read from the
		// canonical struct, not the selected copy: this is a precision knob, not a weather tier.
		float OrthoDepthRange = 2.0f * TheSettingManager->SettingsShadows.Exteriors.ShadowMapFarPlane;
		OrthoData->x = OrthoDepthRange > 0.0f ? TheSettingManager->SettingsShadows.Exteriors.OrthoOcclusionBias / OrthoDepthRange : 0.0f;
	}

	if (CurrentCell != Player->parentCell) {
		CurrentCell = Player->parentCell;
		// This used to clear Valid on both regions, which forced an immediate UNFADED bake — exactly
		// the cell load/unload pop the crossfade exists to remove. Route it through the normal rebake
		// path instead, so the outgoing maps survive as the fade's source. With fading off, the
		// force flag reaches the same two BakeStaticRegion calls the invalidation used to cause.
		ForceRebake = true;
	}

	if (DoSun) {
		D3DXVECTOR4* SunDir = &TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
		// A never-baked region (first frame / cell change) bakes immediately so it is never sampled unbaked
		// (its AnchorPos + surface would be garbage). Both may bake on a cell-change frame — a one-time cost
		// during an already-hitchy load. Once both are valid, steady-state refreshes pick whichever of
		// Near/Far is due and did NOT bake last (see LastBakedRegion); never two refreshes in one frame.
		if (!ShadowsExteriors->CacheStaticShadows) {
			// Caching disabled (INI opt-out): rebake BOTH static regions every frame. Slower — the full
			// scene walk + static draws happen every frame instead of on invalidation — but the shadow is
			// always fresh (no round-robin lag, no guard-band/sun-interval staleness).
			BakeStaticRegion(MapNear, ShadowsExteriors, SunDir);
			BakeStaticRegion(MapFar,  ShadowsExteriors, SunDir);
		}
		else {
			if (StaticFadeEnabled()) AdvanceStaticFade();
			if (!Regions[0].Valid || !Regions[1].Valid) {
				// First bake after load: there is no previous state, so nothing to fade from.
				if (!Regions[0].Valid) BakeStaticRegion(MapNear, ShadowsExteriors, SunDir);
				if (!Regions[1].Valid) BakeStaticRegion(MapFar,  ShadowsExteriors, SunDir);
				Regions[0].PrevValid = Regions[1].PrevValid = false;
				StaticFadeT = 1.0f;
				ForceRebake = false;
			}
			else if (ForceRebake) {
				// Cell change: both regions rebake together, as the old invalidation path did.
				ForceRebake = false;
				BeginStaticCrossfade();
				BakeStaticRegion(MapNear, ShadowsExteriors, SunDir);
				BakeStaticRegion(MapFar,  ShadowsExteriors, SunDir);
				LastBakedRegion = MapFar; // both baked; harmless which one "last" means here
				SnapStaticFadeIfTeleported(ShadowsExteriors);
			}
			else {
				bool NearDue = RegionNeedsRebake(MapNear);   // the fade gate lives inside; both are false while t < 1
				bool FarDue  = RegionNeedsRebake(MapFar);
				if (NearDue || FarDue) {
					// Prefer whichever region did NOT bake last. A near-first chain starves MapFar once the
					// fade gate is in play: the gate collapses far's evaluation windows to the single frame
					// per FadeTime where t reaches 1, and sustained movement keeps near due on exactly those
					// frames -- so far's anchor is abandoned and every distant shadow drops out mid-travel.
					ShadowMapTypeEnum Pick = (FarDue && (!NearDue || LastBakedRegion == MapNear)) ? MapFar : MapNear;
					BeginStaticCrossfade();
					BakeStaticRegion(Pick, ShadowsExteriors, SunDir);
					LastBakedRegion = Pick;
					SnapStaticFadeIfTeleported(ShadowsExteriors);
				}
			}
		}

		RenderActorOverlay(ShadowsExteriors, SunDir);

		// Per-frame sample matrices from the CURRENT camera (the cached maps may be stale/world-anchored).
		PublishCachedRegionSampleMatrix(MapNear);
		PublishCachedRegionSampleMatrix(MapFar);
		PublishStaticFadeConstants();

		ShadowData->y = ShadowsExteriors->Darkness;
		ShadowData->z = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapNear];
		ShadowData->w = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapFar];

		PublishShadowBiasConstants(ShadowsExteriors);
	}
}

// ---------------------------------------------------------------------------------------------
// Point-light shadows (unified interior/exterior). Up to PointLightMax lights each own a shadow
// cube map, baked camera-relative and cached until the slot is dirtied. Nothing here is gated on
// worldspace: interiors and exteriors run the identical path.
// ---------------------------------------------------------------------------------------------

void ShadowManager::CollectSceneLights() {
	SceneLights.clear();
	ShadowSceneNode* SceneNode = *(ShadowSceneNode**)kShadowSceneNode;
	if (!SceneNode) return;
	NiTList<ShadowSceneLight>::Entry* Entry = SceneNode->lights.start;
	while (Entry) {
		NiPointLight* Light = Entry->data->sourceLight;
		if (Light) {
			int distance = (int)Light->GetDistance(&Player->pos);
			SceneLights.emplace_back(distance, Light);
		}
		Entry = Entry->next;
	}
	// Order nearest-first; ties keep scene-graph order. Replaces a distance-keyed std::map
	// with back-probing collision handling (a node allocation + O(log n) probe per light).
	std::stable_sort(SceneLights.begin(), SceneLights.end(),
		[](const std::pair<int, NiPointLight*>& a, const std::pair<int, NiPointLight*>& b) { return a.first < b.first; });
}

// Warmup after a cell change: havok settles objects over the first frames, so the per-slot static
// checksum is unstable until then. Rebakes run unconditionally during warmup.
void ShadowManager::UpdateStaticMapsCounter() {
	if (!EnableStaticMaps) {
		if (EnableStaticMapsFrameCount < EnableStaticMapsFrameThreshold)
			EnableStaticMapsFrameCount++;
		else
			EnableStaticMaps = true;
	}
}

bool ShadowManager::PointShadowsNeeded() {
	if (!TheSettingManager->SettingsShadows.Point.Enabled) return false;
	if (!ShadowCubeMapTexture[0] || !ShadowCubeMapDepthSurface) return false; // allocation failed
	if (!Player || !Player->parentCell) return false;
	return true;
}

// A light qualifies for a cube slot if it casts, is not a magic effect light, and its radius is
// within the configured band. Distance is checked by the caller against the (already sorted)
// collection distance.
bool ShadowManager::IsPointLightCandidate(NiPointLight* Light, SettingsShadowStruct::PointStruct* Settings, bool TorchOnBeltEnabled) {
	if (!Light->CastShadows) return false;
	if (IsLightFromMagic(Light)) return false;
	float Radius = Light->Spec.r; // NiPointLight stores light radius in Spec.rgb
	if (Radius < Settings->LightRadiusMin || Radius > Settings->LightRadiusMax) return false;
	// EquipmentMode torch-on-belt: the player's carried torch (CanCarry == 2) is stowed, so its
	// shadow would be cast from inside the player's own body.
	if (TorchOnBeltEnabled && Light->CanCarry == 2) {
		HighProcessEx* Process = (HighProcessEx*)Player->process;
		if (Process && Process->OnBeltState == HighProcessEx::State::In) return false;
	}
	return true;
}

// Steps every slot's fade weight toward its target: 1 while the slot holds a light, 0 once it loses
// one. Runs before selection so a slot that finished fading out this frame is claimable immediately.
//
// The target deliberately ignores PointLightSlot::Valid. Valid is cleared on every exterior cell
// boundary crossing (see RenderPointShadows), which is a rebake trigger, not a change of occupant --
// fading on it would dip every point shadow to nothing and back each time the player walks across a
// cell line. Fade-IN is instead started explicitly, by SelectPointLights zeroing Intensity at the two
// points where a slot takes a new occupant.
void ShadowManager::AdvancePointFades() {
	double Now = TheFrameRateManager->GetPerformance(); // milliseconds since startup
	double Delta = Now - PointFadeLastMs;
	PointFadeLastMs = Now;
	float FadeTime = TheSettingManager->SettingsShadows.Point.FadeTime;
	// Feature off, or a clock hiccup / same-millisecond frame: snap to the target so a slot mid-fade
	// can never stall retired-but-visible, holding its slot forever.
	bool Snap = (FadeTime <= 0.0f || Delta <= 0.0);
	float Step = Snap ? 1.0f : (float)(Delta / (1000.0 * (double)FadeTime));

	for (int s = 0; s < PointLightMax; s++) {
		PointLightSlot& Slot = PointSlots[s];
		float Target = Slot.Light ? 1.0f : 0.0f;
		if (Slot.Intensity < Target) {
			Slot.Intensity += Step;
			if (Slot.Intensity > Target) Slot.Intensity = Target;
		}
		else if (Slot.Intensity > Target) {
			Slot.Intensity -= Step;
			if (Slot.Intensity < Target) Slot.Intensity = Target;
		}
		// A slot that has finished fading out is fully released: only now may its cube be discarded
		// and the slot reused.
		if (!Slot.Light && Slot.Intensity <= 0.0f) {
			Slot.Valid = false;
			Slot.RetiringLight = NULL;
		}
	}
}

// Assigns candidate lights to cube slots with stability: an incumbent keeps its slot as long as it
// is still a candidate (so its cached cube survives), empty slots take the nearest free candidates,
// and an incumbent is only evicted by a candidate substantially nearer than it (hysteresis), which
// stops slot thrash — and the rebake it would cost — when two lights are at similar distance.
//
// Losing a light does not free a slot immediately: the slot RETIRES (Light = NULL, cube frozen and
// still sampled) until AdvancePointFades takes its Intensity to 0. The single rule the whole fade
// rests on is that a slot never changes occupant while its Intensity is above 0 — otherwise a bake
// for the new light would overwrite the cube the old one is still fading out of.
void ShadowManager::SelectPointLights() {
	SettingsShadowStruct::PointStruct* Settings = &TheSettingManager->SettingsShadows.Point;
	SettingsMainStruct::EquipmentModeStruct* EquipmentModeSettings = &TheSettingManager->SettingsMain.EquipmentMode;
	bool TorchOnBeltEnabled = EquipmentModeSettings->Enabled && EquipmentModeSettings->TorchKey != 255;
	int MaxSlots = Settings->LightCount;
	if (MaxSlots > PointLightMax) MaxSlots = PointLightMax;
	if (MaxSlots < 1) MaxSlots = 1;

	CollectSceneLights();

	// Candidates, nearest first. SceneLights is sorted, so the scan stops at the distance cut.
	const int MaxCandidates = 16;
	struct Candidate { NiPointLight* Light; float Dist; };
	Candidate Candidates[MaxCandidates];
	bool Taken[MaxCandidates] = { false };
	int CandidateCount = 0;
	for (auto& [Distance, Light] : SceneLights) {
		if ((float)Distance > Settings->MaxDistance) break;
		if (CandidateCount >= MaxCandidates) break;
		if (!IsPointLightCandidate(Light, Settings, TorchOnBeltEnabled)) continue;
		Candidates[CandidateCount].Light = Light;
		Candidates[CandidateCount].Dist = (float)Distance;
		CandidateCount++;
	}
	ProfileCount(Cnt_PointCandidates, CandidateCount);

	// 1. Incumbents that are still candidates keep their slot (and their cached cube). A slot whose
	// light is absent from this frame's candidate set is released — its NiPointLight may already be
	// destroyed (cell unload), so the pointer must not be dereferenced again after this point.
	for (int s = 0; s < PointLightMax; s++) {
		if (s >= MaxSlots) { PointSlots[s].Light = NULL; continue; }
		NiPointLight* Incumbent = PointSlots[s].Light;
		if (!Incumbent) continue;
		int Found = -1;
		for (int c = 0; c < CandidateCount; c++) {
			if (!Taken[c] && Candidates[c].Light == Incumbent) { Found = c; break; }
		}
		// Retire rather than clear: Valid stays set so the frozen cube keeps being sampled, and
		// AdvancePointFades clears it once the fade completes. A light that comes back before then
		// (one hovering on the distance cut) simply reclaims the slot below with its cube intact.
		if (Found < 0) { PointSlots[s].Light = NULL; PointSlots[s].RetiringLight = Incumbent; }
		else Taken[Found] = true;
	}

	// 1b. A retiring slot whose own light is a candidate again reclaims it, cube and all, and its
	// Intensity ramps back up from wherever it had fallen to. Without this, a light sitting on the
	// distance cut fades fully out and back in, rebaking each time. RetiringLight is compared, never
	// dereferenced; a match means the address is in this frame's live candidate set.
	for (int s = 0; s < MaxSlots; s++) {
		if (PointSlots[s].Light || !PointSlots[s].RetiringLight) continue;
		for (int c = 0; c < CandidateCount; c++) {
			if (Taken[c] || Candidates[c].Light != PointSlots[s].RetiringLight) continue;
			Taken[c] = true;
			PointSlots[s].Light = Candidates[c].Light;
			PointSlots[s].RetiringLight = NULL;
			break;
		}
	}

	// 2. Fully free slots take the nearest unclaimed candidates. A retiring slot is not free.
	for (int s = 0; s < MaxSlots; s++) {
		if (PointSlots[s].Light || PointSlots[s].Intensity > 0.0f) continue;
		for (int c = 0; c < CandidateCount; c++) {
			if (Taken[c]) continue;
			Taken[c] = true;
			PointSlots[s].Light = Candidates[c].Light;
			PointSlots[s].Valid = false;     // new occupant: cube must be baked
			PointSlots[s].Intensity = 0.0f;  // and fade in from nothing
			break;
		}
	}

	// 3. Hysteresis eviction: a still-unclaimed candidate displaces the farthest incumbent only if
	// it is clearly nearer. Candidates are sorted, so once one fails the test no later one can pass.
	// The incumbent is only retired here, not replaced — the candidate claims the slot through step 2
	// on a later frame, once the fade-out has finished, giving a clean fade-out-then-in rather than a
	// swap-pop. Skipped entirely while any slot is already retiring, and stopped after retiring one:
	// otherwise the candidate, still unclaimed for the whole fade, would retire another slot every
	// frame until every light in the set had been evicted.
	bool AnyRetiring = false;
	for (int s = 0; s < PointLightMax; s++)
		if (!PointSlots[s].Light && PointSlots[s].Intensity > 0.0f) { AnyRetiring = true; break; }

	for (int c = 0; !AnyRetiring && c < CandidateCount; c++) {
		if (Taken[c]) continue;
		int Worst = -1;
		float WorstDist = -1.0f;
		for (int s = 0; s < MaxSlots; s++) {
			if (!PointSlots[s].Light) continue;
			float d = PointSlots[s].Light->GetDistance(&Player->pos);
			if (d > WorstDist) { WorstDist = d; Worst = s; }
		}
		if (Worst < 0) break;
		if (Candidates[c].Dist >= WorstDist * PointSlotEvictFactor) break;
		// RetiringLight is deliberately left NULL here: the evicted light is still a candidate, so
		// step 1b would reclaim it on the very next frame and the two would trade the slot forever.
		PointSlots[Worst].Light = NULL;
		break;
	}
}

// Publishes each slot's light position CAMERA-RELATIVE every frame (w = the cube's far plane).
// The apply shader reconstructs camera-relative positions too, so cube sampling needs no matrix —
// a cached cube stays correct as the camera moves because stored radial distance is origin-invariant.
// w == 0 marks an inactive slot for the shader. A retiring slot is NOT inactive: it publishes a real
// far plane from its cached values so its frozen cube keeps being sampled, and fades out through the
// luminance instead — which is what makes the shadow lose depth rather than shrink.
void ShadowManager::PublishPointLightConstants() {
	D3DXVECTOR4* Positions = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition;
	// One component per slot, so index it as a float array (D3DXVECTOR4 converts implicitly).
	float* Luminance = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightLuminance;
	// Diff/Dimmer are read nowhere else in this fork (only Spec.r, which stores the radius), so log
	// them once per slot occupant while shadow profiling is on: an unpopulated Diff or a zero Dimmer
	// would silently produce no point shadows at all, with nothing else to point at the cause.
	static NiPointLight* LastLoggedLight[PointLightMax] = { NULL };
	int Active = 0;
	int Shaded = 0;
	for (int s = 0; s < PointLightMax; s++) {
		PointLightSlot& Slot = PointSlots[s];
		NiPointLight* Light = Slot.Light;
		if (!Light) {
			// Retiring: the NiPointLight is gone but the cube is still live, so keep publishing from
			// the cached values with the fade applied. BakedLightPos is the right position to use —
			// drifting further than PointLightMoveEpsilon from it forces a rebake, which updates it,
			// so it tracks the light to within one unit right up to the moment the slot retired.
			if (Slot.Intensity > 0.0f) {
				Positions[s].x = Slot.BakedLightPos.x - TheRenderManager->CameraPosition.x;
				Positions[s].y = Slot.BakedLightPos.y - TheRenderManager->CameraPosition.y;
				Positions[s].z = Slot.BakedLightPos.z - TheRenderManager->CameraPosition.z;
				Positions[s].w = Slot.LastFarPlane;
				Luminance[s] = Slot.LastLuminance * Slot.Intensity;
				Shaded++;
			}
			else {
				Positions[s] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
				Luminance[s] = 0.0f;
			}
			LastLoggedLight[s] = NULL;
			continue;
		}
		NiPoint3* LightPos = &Light->m_worldTransform.pos;
		// Carried torches use a fixed far plane: their authored radius is large and washes the
		// cube's precision out over a range the torch never actually lights.
		float FarPlane = Light->CanCarry ? 257.0f : Light->Spec.r;
		Positions[s].x = LightPos->x - TheRenderManager->CameraPosition.x;
		Positions[s].y = LightPos->y - TheRenderManager->CameraPosition.y;
		Positions[s].z = LightPos->z - TheRenderManager->CameraPosition.z;
		Positions[s].w = FarPlane;
		// The shadow removes this light's own contribution, so the apply needs how bright the light
		// is — diffuse scaled by the dimmer, as the engine's own lighting shaders use it, reduced to
		// a single luma (Rec. 601). Brightness only, deliberately not colour: removing the light's
		// hue tinted shadows in a way that read wrong in game.
		// Cached unfaded, so a retiring slot fades out from the light's true brightness rather than
		// from whatever partial value it happened to be published at.
		Slot.LastFarPlane = FarPlane;
		Slot.LastLuminance = (0.299f * Light->Diff.r + 0.587f * Light->Diff.g + 0.114f * Light->Diff.b) * Light->Dimmer;
		Luminance[s] = Slot.LastLuminance * Slot.Intensity;
		if (ProfilingEnabled && LastLoggedLight[s] != Light) {
			LastLoggedLight[s] = Light;
			Logger::Log("[ShadowProfile] point slot %d: Diff=(%.3f, %.3f, %.3f) Dimmer=%.3f Radius=%.1f CanCarry=%d",
				s, Light->Diff.r, Light->Diff.g, Light->Diff.b, Light->Dimmer, Light->Spec.r, (int)Light->CanCarry);
		}
		Active++;
		Shaded++;
	}
	PointSlotsActive = Active;
	PointSlotsShaded = Shaded;
	ProfileCount(Cnt_PointSlotsActive, Active);
}

void ShadowManager::ClearCubeMapNodeLists() {
	for (int i = 0; i < PointLightMax; i++) {
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
	// Quantized to whole units: an idle NPC's bound center jitters continuously under its
	// breathing animation, and an exact sum would report the scene as changed every frame,
	// rebaking every cube forever. Whole units still catch any movement that shifts a shadow.
	Info.CenterSum = std::floor(B->Center.x) + std::floor(B->Center.y) + std::floor(B->Center.z);
	Info.IsPlayer = (Ref->refID == Player->refID);
	return Info;
}

// Tests one ref against one slot's light and files it under statics or actors. Actors (and any
// carried light) get a wider radius because they move: a caster just outside the strict radius
// can step inside it before the next rebake.
void ShadowManager::ClassifyRefForPointSlot(const RefLightInfo& Info, int Slot, double* Checksums) {
	NiPointLight* Light = PointSlots[Slot].Light;
	NiPoint3* LightPos = &Light->m_worldTransform.pos;
	float FarPlane = TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[Slot].w;
	float Radius = (Light->CanCarry || Info.IsActorType) ? FarPlane * 1.2f : FarPlane;

	if (Info.Node->GetDistance(LightPos) - Info.BoundRadius > Radius) return;
	if (Info.IsActorType) {
		// The player's own carried torch would shadow the player from inside their own mesh.
		if (Info.IsPlayer && Light->CanCarry) return;
		CubeMapActorMap[Slot].emplace_back(Info.Node);
	}
	else {
		CubeMapRefMap[Slot].emplace_back(Info.Node);
	}
	Checksums[Slot] += Info.CenterSum;
}

void ShadowManager::ClassifyCellForPointSlots(TESObjectCELL* Cell, double* Checksums) {
	if (!Cell) return;
	SettingsShadowStruct::PointStruct* Settings = &TheSettingManager->SettingsShadows.Point;
	TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First;
	while (Entry) {
		if (TESObjectREFR* Ref = GetRef(Entry->item, &Settings->Forms, &Settings->ExcludedForms)) {
			RefLightInfo Info = BuildRefLightInfo(Ref);
			for (int s = 0; s < PointLightMax; s++)
				if (PointSlots[s].Light) ClassifyRefForPointSlot(Info, s, Checksums);
		}
		Entry = Entry->next;
	}
}

// The ONLY place the point path distinguishes interiors from exteriors: which ref list to walk.
// Everything downstream (classification, baking, applying) is identical.
void ShadowManager::BuildPointGeoLists(double* Checksums) {
	ClearCubeMapNodeLists();
	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++)
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++)
				ClassifyCellForPointSlots(Tes->gridCellArray->GetCell(x, y), Checksums);
	}
	else {
		ClassifyCellForPointSlots(Player->parentCell, Checksums);
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
	RenderState->SetPixelShader(ShadowCubeMapPixelShader, false);
}

// A slot's cached cube is reused unless something it depends on changed.
bool ShadowManager::PointSlotNeedsRebake(int Slot, double Checksum) {
	PointLightSlot& Slot_ = PointSlots[Slot];
	if (!Slot_.Valid) return true;          // never baked, or reassigned to a different light
	if (!EnableStaticMaps) return true;     // post-cell-change warmup: havok still settling
	NiPoint3* P = &Slot_.Light->m_worldTransform.pos;
	D3DXVECTOR3 Pos(P->x, P->y, P->z);
	// Covers carried torches without special-casing them: a moving light is simply a moved light.
	if (D3DXVec3Length(&(Pos - Slot_.BakedLightPos)) > PointLightMoveEpsilon) return true;
	return Checksum != Slot_.Checksum;      // a caster within reach moved
}

// Renders one slot's shadow cube: 6 perspective faces from the light, storing normalized radial
// distance. Statics and actors are drawn together per face so the single shared depth-stencil is
// valid throughout (a separate later actor pass would have lost each face's depth).
void ShadowManager::BakePointCube(int Slot) {
	IDirect3DDevice9* Device = TheRenderManager->device;
	D3DXVECTOR4* BakeData = &TheShaderManager->ShaderConst.ShadowPoint.BakeData;
	D3DXVECTOR4* LightPos = &TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[Slot];
	ProfileCount(Cnt_PointRebakes);

	// Cube bakes are camera-relative (matching the skinned bone path, which always is).
	CollectWorldSpace = false;
	CurrentVertex = ShadowCubeMapVertex;
	CurrentPixel = ShadowCubeMapPixel;
	AlphaEnabled = TheSettingManager->SettingsShadows.Point.AlphaEnabled;
	TheShaderManager->ShaderConst.ShadowMap.ShadowCubeMapLightPosition = *LightPos;
	BakeData->z = LightPos->w; // far plane: the PS normalizes distance by this

	// Flatten the scene graph once and reuse across all 6 faces, with world matrices precomputed
	// (they don't vary per face). Skinned geometry gets NULL here and resolves inside Render().
	CubeMapGeoList.clear();
	for (NiNode* RefNode : CubeMapRefMap[Slot]) CollectCubeMapGeometry(RefNode, CubeMapGeoList);
	for (NiNode* RefNode : CubeMapActorMap[Slot]) CollectCubeMapGeometry(RefNode, CubeMapGeoList);
	CubeMapGeoWorld.resize(CubeMapGeoList.size());
	for (size_t g = 0; g < CubeMapGeoList.size(); g++)
		CreateD3DMatrix(&CubeMapGeoWorld[g], &CubeMapGeoList[g]->m_worldTransform);

	D3DXMATRIX Proj, View;
	D3DXMatrixPerspectiveFovRH(&Proj, D3DXToRadian(90.0f), 1.0f, 1.0f, LightPos->w);
	D3DXVECTOR3 Eye(LightPos->x, LightPos->y, LightPos->z);

	for (int Face = 0; Face < 6; Face++) {
		D3DXVECTOR3 At = Eye, Up;
		GetCubeFaceAtUp(Face, At, Up);
		D3DXMatrixLookAtRH(&View, &Eye, &At, &Up);
		TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj = View * Proj;
		Device->SetDepthStencilSurface(ShadowCubeMapDepthSurface);
		Device->SetRenderTarget(0, ShadowCubeMapSurface[Slot][Face]);
		// Clear to 1.0 = "nothing occludes out to the far plane"; the apply reads distances < 1.
		Device->Clear(0L, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DXCOLOR(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, 0L);
		if (CubeMapGeoList.empty()) continue;

		D3DXPLANE Frustum[6];
		GetFrustumPlanes(Frustum, &TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj);
		Device->BeginScene();
		SetupCubeMapRenderState();
		for (size_t g = 0; g < CubeMapGeoList.size(); g++)
			if (InFrustum(Frustum, CubeMapGeoList[g])) Render(CubeMapGeoList[g], BakeData, &CubeMapGeoWorld[g]);
		Device->EndScene();
	}

	// Record what this cube was baked against, so the next frame can tell whether it is stale.
	NiPoint3* P = &PointSlots[Slot].Light->m_worldTransform.pos;
	PointSlots[Slot].BakedLightPos = D3DXVECTOR3(P->x, P->y, P->z);
	PointSlots[Slot].Valid = true;
}

void ShadowManager::RenderPointShadows() {
	PointSlotsActive = 0;
	PointSlotsShaded = 0;
	if (!PointShadowsNeeded()) {
		// Publish zeroed slots so a stale set never keeps shadowing after the feature is disabled.
		// Fades are killed outright rather than run out: there is nothing to fade against once the
		// apply stops running, and a slot left mid-retirement would hold itself reserved forever.
		for (int s = 0; s < PointLightMax; s++) {
			PointSlots[s].Light = NULL;
			PointSlots[s].Valid = false;
			PointSlots[s].Intensity = 0.0f;
			PointSlots[s].RetiringLight = NULL;
			TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[s] = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightLuminance = D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
		// Pin the clock, so the gap while the feature was off does not arrive as one huge delta that
		// snaps the first slot straight to full instead of fading it in.
		PointFadeLastMs = TheFrameRateManager->GetPerformance();
		return;
	}
	ScopeTimer profile(Phase_PointTotal);

	// Cell change invalidates every cached cube (the whole ref set changed). Tracked separately
	// from CurrentCell, which belongs to the exterior-only sun path.
	if (PointCurrentCell != Player->parentCell) {
		PointCurrentCell = Player->parentCell;
		for (int s = 0; s < PointLightMax; s++) {
			PointSlots[s].Valid = false;
			// A retiring slot is fading out a cube baked from casters that just went away with the
			// cell, so end it here rather than let it drag the old cell's shadow into the new one.
			if (!PointSlots[s].Light) { PointSlots[s].Intensity = 0.0f; PointSlots[s].RetiringLight = NULL; }
		}
		EnableStaticMapsFrameCount = 0;
		EnableStaticMaps = false;
	}
	UpdateStaticMapsCounter();

	AdvancePointFades();
	SelectPointLights();
	PublishPointLightConstants();

	D3DXVECTOR4* PointData = &TheShaderManager->ShaderConst.ShadowPoint.PointData;
	// Dead since darkness became light-derived, but a stale compiled ShadowsPoint.fx still reads .y
	// as its darkness preshader (CompileShaders defaults off). 1.0 makes that case degrade to
	// "no point shadows" rather than an unwritten read.
	PointData->y = 1.0f;
	PointData->z = 1.0f / (float)TheSettingManager->SettingsShadows.Point.ShadowCubeMapSize;
	PointData->w = TheSettingManager->SettingsShadows.Point.Bias;

	if (!PointSlotsActive) return;

	// Classify every loaded ref against every occupied slot. This walk runs each frame even when
	// no cube rebakes, because it is what produces the checksums that decide whether one must.
	double Checksums[PointLightMax] = { 0.0 };
	{
		ScopeTimer profileClassify(Phase_PointClassify);
		BuildPointGeoLists(Checksums);
	}

	{
		ScopeTimer profileBake(Phase_PointBake);
		gCubeBucket = true;
		for (int s = 0; s < PointLightMax; s++) {
			if (!PointSlots[s].Light) continue;
			if (!PointSlotNeedsRebake(s, Checksums[s])) { ProfileCount(Cnt_CubeLightsCached); continue; }
			BakePointCube(s);
			PointSlots[s].Checksum = Checksums[s];
			ProfileCount(Cnt_CubeLightsDrawn);
		}
		gCubeBucket = false;
	}
}


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
		RenderPointShadows(); // unified point-light cubes: runs in interiors AND exteriors
	}
	Device->SetDepthStencilSurface(DepthSurface);
	ShadowProfileFrameEnd();
}


void ShadowManager::ResetIntervals() {
	GameTime = -1;
}


bool ShadowManager::IsLightFromMagic(NiPointLight* light) {
	//TODO: a better way to check this
	return light->m_parent && strstr(light->m_parent->m_pcName, "agic") != NULL;
}

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

// Look-at target and up vector for each cube face. The sampling convention in the apply shader
// (negating the light-vector Z) is matched to this table — change one and you must change both.
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
	// CalculateBoneMatrixes stamped SkinInstance->FrameID, and the game's per-frame cache would make
	// the later water-reflection pass reuse these shadow-pass bone matrices (floating actor
	// reflections). Invalidate the stamp so the game recomputes with its own transform.
	SkinInstance->FrameID = 0xFFFFFFFF;
}


// Three weather-driven darkness tiers (INI [Exteriors] Darkness/DarknessCloudy/DarknessPrecipitation):
// clear (Pleasant/None) is darkest, Cloudy is lighter, Rainy/Snow is lightest. weatherType is a bitmask
// (see TESWeather::WeatherType), so test with & rather than equality; precipitation takes priority over
// cloudy if a weather record somehow sets both bits.
SettingsShadowStruct::ExteriorsStruct* ShadowManager::SelectExteriorShadowSettings() {
	TESWeather* currentWeather = Tes->sky->firstWeather;
	UInt32 weatherType = currentWeather->weatherType;
	if (weatherType & (TESWeather::WeatherType::kType_Rainy | TESWeather::WeatherType::kType_Snow))
		return &TheSettingManager->SettingsShadows.ExteriorsPrecip;
	if (weatherType & TESWeather::WeatherType::kType_Cloudy)
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

	// Unified [Point] settings: torches behave the same in interiors and exteriors.
	SettingsShadowStruct::PointStruct* ShadowSettings = &TheSettingManager->SettingsShadows.Point;
	SettingsMainStruct::EquipmentModeStruct* EquipmentModeSettings = &TheSettingManager->SettingsMain.EquipmentMode;

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