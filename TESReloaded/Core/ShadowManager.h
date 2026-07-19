#pragma once
#include <list>
#include <vector>
#include <map>
#include <unordered_map>

class ShadowManager { // Never disposed
public:
	ShadowManager();

	enum ShadowMapTypeEnum {
		MapNear		= 0,
		MapFar		= 1,
		MapOrtho	= 2,
		MapSkin     = 3,
	};
	// Compile-time cap on simultaneous shadow-casting point lights (cube maps allocated,
	// sampler slots in ShadowsPoint.fx, TESR_ShadowLightPosition0..N-1 constants).
	// [Point] LightCount can lower the active count at runtime but never exceed this.
	static const int PointLightMax = 4;
	enum ShadowCubeMapStateEnum {
		None		   = 0,
		Exterior	   = 1,
		Exterior_Night = 2,
		Interior	   = 3
	};
	enum PlaneEnum {
		PlaneNear	= 0,
		PlaneFar	= 1,
		PlaneLeft	= 2,
		PlaneRight	= 3,
		PlaneTop	= 4,
		PlaneBottom	= 5,
	};

	// Per-ref data needed when classifying a ref against each point light. Computed once per
	// ref (independent of the light) so the per-light loop only does the distance test.
	struct RefLightInfo {
		NiNode*	Node;
		double	CenterSum;
		float	BoundRadius;
		bool	IsActorType;
		bool	IsPlayer;
	};

	void					CreateD3DMatrix(D3DMATRIX* Matrix, NiTransform* Transform);
	void					CreateD3DMatrixWorld(D3DMATRIX* Matrix, NiTransform* Transform);
	void					GetShadowFrustum(ShadowMapTypeEnum ShadowMapType, D3DMATRIX* Matrix);
	bool					InShadowFrustum(ShadowMapTypeEnum ShadowMapType, NiAVObject* Object);
	// Camera-relative frustum tests using a precomputed bound center+radius (no GetWorldBound /
	// camera subtraction; done once at collection). Root variant ports InShadowFrustum exactly
	// (incl. the MapFar near-frustum exclusion); Leaf variant is plain 6-plane containment.
	bool					RootInShadowFrustum(ShadowMapTypeEnum ShadowMapType, const D3DXVECTOR3& Center, float Radius);
	bool					LeafInShadowFrustum(ShadowMapTypeEnum ShadowMapType, const D3DXVECTOR3& Center, float Radius);
	void					GetFrustumPlanes(D3DXPLANE* Frustum, D3DXMATRIX* Matrix);
	bool					InFrustum(D3DXPLANE* Frustum, NiGeometry* Geo);
	TESObjectREFR*			GetRef(TESObjectREFR* Ref, SettingsShadowStruct::FormsStruct* Forms, SettingsShadowStruct::ExcludedFormsList* ExcludedForms);
	TESObjectREFR*			GetRefO(TESObjectREFR* Ref);
	void					AddInstance(NiGeometryBufferData* GeoData, int ItemIndex);
	IDirect3DVertexDeclaration9* GetInstancedDeclaration(NiGeometryBufferData* GeoData);
	bool					EnsureInstanceVB(UINT InstanceCount);
	void					DrawInstancedGroup(NiGeometryBufferData* GeoData, std::vector<int>& ItemIdx, IDirect3DVertexDeclaration9* Decl);
	void					FlushInstanceGroups(D3DXVECTOR4* ShadowData);
	void					RenderObjectPoint(NiAVObject* Node, D3DXVECTOR4* ShadowData, bool HasWater);
	void					CollectCubeMapGeometry(NiAVObject* Object, bool HasWater, std::vector<NiGeometry*>& Out);
	void					RenderObjectPointActor(NiAVObject* Node, D3DXVECTOR4* ShadowData, bool HasWater, int lightIndex);
	void					RenderTerrain(NiAVObject* Object, ShadowMapTypeEnum ShadowMapType, D3DXVECTOR4* ShadowData);
	void					Render(NiGeometry* Geo, D3DXVECTOR4* ShadowData, const D3DMATRIX* PrecomputedWorld = NULL);
	void					RenderActor(NiGeometry* Geo, D3DXVECTOR4* ShadowData, int lightIndex);
	void					RenderShadowMap(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, D3DXVECTOR3* At, D3DXVECTOR4* SunDir, D3DXVECTOR4* ShadowData, bool SkipTerrain = false);
	void					RenderShadowCubeMapExt(NiPointLight** Lights, int LightIndex, float radiusLimit, SettingsShadowStruct::InteriorsStruct* ShadowsExteriors, D3DXVECTOR4* ShadowData);
	void					RenderShadowCubeMapInt(NiPointLight** Lights, int LightIndex, float radiusLimit, SettingsShadowStruct::InteriorsStruct* ShadowsInteriors, D3DXVECTOR4* ShadowData);
	void                    RenderShadowCubeMapFakeInt(int LightIndex, SettingsShadowStruct::InteriorsStruct* ShadowsInteriors, D3DXVECTOR4* ShadowData);
	void                    RenderShadowCubeMap(int LightIndex, std::vector<NiNode*>* refMap, D3DXVECTOR4* ShadowData, bool enabled);
	void                    RenderShadowCubeMapActor(int LightIndex, std::vector<NiNode*>* refMap, D3DXVECTOR4* ShadowData, bool enabled);
	void					RenderExteriorShadows();
	bool					OrthoNeeded();
	bool					SunShadowNeeded();
	void					RenderInteriorShadows();
	void					RenderShadowMaps();
	void					ClearShadowMap(IDirect3DDevice9* Device);
	void					ClearShadowCubeMaps(IDirect3DDevice9* Device, int From, ShadowCubeMapStateEnum NewState);
	void					ClearShadowCubeMaps(IDirect3DDevice9* Device, int LightIndex);
	void					ClearShadowCubeLightRegister(int From);
	void					ClearShadowCubeLightCullRegister(int From);
	void					ClearGeneralPointLightRegister(int From);
	int                     GetShadowSceneLights(NiPointLight** ShadowCastLights, NiPointLight** ShadowCullLights, NiPointLight** GeneralPointLights, int& ShadowCastLightIndex, int& ShadowCullLightIndex, int& GeneralPointLightIndex, SettingsShadowPointLightsStruct* ShadowSettings);
	void                    SetAllShadowCastLightPos(NiPointLight** Lights, int LightIndex);
	void                    SetShadowCastLightPos(NiPointLight** Lights, int index);
	void                    SetAllShadowCullLightPos(NiPointLight** Lights, int LightIndex);
	void                    SetShadowCullLightPos(NiPointLight** Lights, int index);
	void					SetAllGeneralLightPos(NiPointLight** Lights, int LightIndex);
	void					SetGeneralLightPos(NiPointLight** Lights, int index);
	void                    SetShadowCubeMapRegisters(int index);
	void					ResetIntervals();
	void					LoadShadowLightPointSettings();
	bool					IsLightFromMagic(NiPointLight* Light);

	// --- Helpers ---
	void					SetupGeoStreams(NiGeometryBufferData* GeoData);
	void					DrawGeoArrays(NiGeometryBufferData* GeoData, D3DPRIMITIVETYPE PrimitiveType, UINT VertCount);
	void					GetCubeFaceAtUp(int Face, D3DXVECTOR3& At, D3DXVECTOR3& Up);
	void					SetupSpeedTreeLeafShader(NiGeometry* Geo, D3DXVECTOR4* ShadowData);
	void					SetupAlphaTexture(NiGeometry* Geo, BSShaderProperty* LProp, D3DXVECTOR4* ShadowData);
	void					RenderSkinnedGeo(NiGeometry* Geo, D3DXVECTOR4* ShadowData);
	void					RenderActorFaces(NiGeometryBufferData* GeoData, D3DPRIMITIVETYPE PrimitiveType, UINT VertCount, int lightIndex);
	void					RenderActorSkinnedGeo(NiGeometry* Geo, D3DXVECTOR4* ShadowData, int lightIndex);
	void					ComputeCubeFaceFrusta(int lightIndex);
	void					SetupShadowMapMatrices(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, D3DXVECTOR3* At, D3DXVECTOR4* ShadowLightDir);
	void					SetupCachedRegionMatrices(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, D3DXVECTOR4* SunDir);
	void					PublishCachedRegionSampleMatrix(ShadowMapTypeEnum ShadowMapType);
	bool					RegionNeedsRebake(ShadowMapTypeEnum ShadowMapType);
	void					BakeStaticRegion(ShadowMapTypeEnum ShadowMapType, SettingsShadowStruct::ExteriorsStruct* S, D3DXVECTOR4* SunDir);
	void					RenderActorOverlay(SettingsShadowStruct::ExteriorsStruct* S, D3DXVECTOR4* SunDir);
	void					RenderShadowMapCellTerrain(TESObjectCELL* Cell, ShadowMapTypeEnum ShadowMapType, D3DXVECTOR4* ShadowData);
	void					BuildExteriorGeoItems(SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors, ShadowMapTypeEnum ShadowMapType);
	void					CollectExteriorGeo(NiAVObject* Object, bool HasWater, ShadowMapTypeEnum ShadowMapType, bool IsActorRef);
	void					SetupCubeMapRenderState();
	RefLightInfo			BuildRefLightInfo(TESObjectREFR* Ref);
	void					ClassifyRefForLight(const RefLightInfo& Info, NiPointLight** Lights, int L, float radiusScan, std::vector<NiNode*>* refMap, std::vector<NiNode*>* actorMap, double* StaticValues, bool* forceRedrawMap);
	void					ClearCubeMapNodeLists();
	void					UpdateStaticTrackers(int LightIndex, double* StaticValues, bool* forceRedrawMap);
	SettingsShadowStruct::ExteriorsStruct* SelectExteriorShadowSettings();
	void					ComputeExteriorLookAt(D3DXVECTOR3& At, D3DXVECTOR3& SkinAt, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors);
	void					AdjustShadowLightDir(D3DXVECTOR4*& ShadowLightDir);
	bool					UpdateShadowLightDirInterval(D3DXVECTOR4* ShadowLightDir, D3DXVECTOR4& ShadowLightDirInterval);
	SettingsShadowStruct::InteriorsStruct* SelectInteriorShadowSettings();
	void					HandleCellChange();
	void					UpdateStaticMapsCounter();
	void					InitShadowBiasConstants();
	void					LoadShadowShaders(IDirect3DDevice9* Device);
	void					CreateShadowMapSurfaces(IDirect3DDevice9* Device, SettingsShadowStruct::ExteriorsStruct* ShadowsExteriors);
	void					CreateCubeMapSurfaces(IDirect3DDevice9* Device, UINT CubeMapSize);
	void					CollectSceneLights();
	bool					CategorizeSceneLight(NiPointLight* Light, int& shadowCastIndex, int& shadowCullIndex, NiPointLight** ShadowCastLights, NiPointLight** ShadowCullLights, SettingsShadowPointLightsStruct* ShadowSettings, bool CastShadow);

	IDirect3DTexture9*		ShadowMapTexture[4];
	IDirect3DSurface9*		ShadowMapSurface[4];
	IDirect3DSurface9*		ShadowMapDepthSurface[4];
	ShaderRecord*			ShadowMapVertex;
	ShaderRecord*			ShadowMapPixel;
	IDirect3DVertexShader9* ShadowMapVertexShader;
	IDirect3DPixelShader9*  ShadowMapPixelShader;

	// Hardware instancing for repeated opaque statics in the exterior directional passes.
	ShaderRecord*			ShadowMapInstancedVertex;
	IDirect3DVertexShader9* ShadowMapInstancedVertexShader;
	IDirect3DVertexBuffer9* InstanceVB;			// dynamic per-instance world-matrix stream
	UINT					InstanceVBCapacity;	// capacity in instances
	std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> InstancedDeclCache;
	// Per-mesh instance groups, pooled so the inner vectors keep their capacity across passes:
	// only the index map and active count are reset each pass; InstancePool grows but is reused.
	// Decl is the instanced vertex declaration for GeoData, resolved once when the group is
	// created (NULL if the buffer can't be instanced) so the flush passes don't re-look-it-up.
	// ItemIdx indexes into ShadowGeoPool (stable across passes) so both the instanced draw and
	// the small-group fallback reuse the precomputed world matrices.
	struct InstanceGroup { NiGeometryBufferData* GeoData; IDirect3DVertexDeclaration9* Decl; std::vector<int> ItemIdx; };
	std::vector<InstanceGroup> InstancePool;
	std::unordered_map<NiGeometryBufferData*, int> InstanceGroupIndex;
	int						InstanceGroupCount;
	D3DVIEWPORT9			ShadowMapViewPort[4];
	D3DXPLANE				ShadowMapFrustum[4][6];
	NiVector4				BillboardRight;
	NiVector4				BillboardUp;

	// Cached directional regions (MapNear, MapFar). The static depth in each map is baked in
	// world space around a snapped anchor and reused across frames; only the per-frame sample
	// matrix (below) follows the camera. See Task 7/8/9.
	struct CachedRegion {
		D3DXMATRIX  BakedViewProj;   // world->light-clip used at last bake; apply samples via this
		D3DXVECTOR3 AnchorPos;       // snapped world center the map was baked around
		D3DXVECTOR4 BakedSunDir;     // sun direction at last bake
		bool        Valid;           // false => force rebake
	};
	CachedRegion			Regions[2];         // [0]=MapNear, [1]=MapFar

	//-------SHADOW CUBE MAP--------

	// Per-slot state for the point-light shadow cubes. A slot owns one cube map; its contents
	// are baked camera-relative and cached until the slot is dirtied (light reassigned/moved,
	// contributing refs moved, or cell change). Light pointers are only valid for the frame
	// they were collected — validate membership in SceneLights before dereferencing.
	struct PointLightSlot {
		NiPointLight*	Light;			// NULL = slot empty
		D3DXVECTOR3		BakedLightPos;	// world-space light position at last bake
		double			Checksum;		// quantized bound-center sum of in-radius refs at last bake
		bool			Valid;			// false => cube must be (re)baked
	};
	PointLightSlot			PointSlots[PointLightMax];

	//TEXTURES
	IDirect3DCubeTexture9*	ShadowCubeMapTexture[PointLightMax];

	//SURFACES
	IDirect3DSurface9*		ShadowCubeMapSurface[PointLightMax][6];
	IDirect3DSurface9*		ShadowCubeMapDepthSurface; // single shared depth-stencil: faces render sequentially

	//SHADERS
	ShaderRecord*			ShadowCubeMapVertex;
	ShaderRecord*			ShadowCubeMapPixel;
	IDirect3DVertexShader9* ShadowCubeMapVertexShader;
	IDirect3DPixelShader9*	ShadowCubeMapPixelShader;

	//MISC
	D3DVIEWPORT9			ShadowCubeMapViewPort;
	NiPointLight*			ShadowCubeMapLights[PointLightMax];
	// Per-face cube view-projection + frustum, precomputed once per light in the actor path so
	// each actor geo can be culled to the 1-2 faces it actually overlaps instead of being drawn
	// to all 6 unconditionally. CubeActorFaceVisible is the per-geo result reused across the
	// geo's skin partitions (all partitions share the geo's world bound).
	D3DXMATRIX				CubeFaceViewProj[6];
	D3DXPLANE				CubeFaceFrustum[6][6];
	bool					CubeActorFaceVisible[6];
	int                     ShadowCubeLightCount;
	int						ShadowCubeCullLightCount;
	double					ShadowCubeMapStaticValue[PointLightMax];
	bool					ShadowCubeMapStaticTracker[PointLightMax];
	bool					EnableStaticMaps;
	int						EnableStaticMapsFrameCount;
	int						EnableStaticMapsFrameThreshold = 30;

	//-------END SHADOW CUBE--------

	ShaderRecord*			CurrentVertex;
	ShaderRecord*			CurrentPixel;
	TESObjectCELL*			CurrentCell;
	ShadowCubeMapStateEnum	ShadowCubeMapState;
	bool					AlphaEnabled;
	bool					FakeExtShadowLightDirSet;
	int						FakeExtShadowLightDirCnt;
	D3DXVECTOR4				FakeExtShadowLightDir;
	D3DXVECTOR4				ShadowLightDirOld;
	D3DXVECTOR4				ShadowLightDirNew;
	bool					UpdateShadowLightDir;
	float					UpdateTargetTime;
	D3DXVECTOR3				LookAtPosition;
	int						GeneralPointLightCount;
	float					GameTime;
	SettingsShadowPointLightsStruct* ShadowLightPointSettings;

	// Reusable per-light node lists for cube map passes, cleared each frame
	// instead of allocating fresh std::map<int, std::vector> containers every frame.
	std::vector<NiNode*>	CubeMapRefMap[PointLightMax];
	std::vector<NiNode*>	CubeMapActorMap[PointLightMax];
	// Scene point lights gathered each frame for the cube-map passes; pooled (cleared + refilled
	// by CollectSceneLights) instead of allocating a fresh vector per frame.
	std::vector<std::pair<int, NiPointLight*>> SceneLights;
	// Flattened renderable geometry for the current light, reused across all 6 cube faces.
	std::vector<NiGeometry*> CubeMapGeoList;
	// Camera-relative world matrices for CubeMapGeoList, computed once per light and reused
	// across all 6 faces instead of rebuilding each geo's matrix per visible face.
	std::vector<D3DMATRIX>	CubeMapGeoWorld;
	// Exterior shadow-casting geometry for the single live ortho pass, flattened once per frame.
	// BuildExteriorGeoItems applies the Forms filter, the ref-root + per-leaf frustum cull, and the
	// MinRadius cut during collection, so this pool is already filtered to exactly what gets drawn;
	// RenderShadowMap then iterates it and draws with no further culling. World transforms, bounds,
	// and instancing eligibility are precomputed here.
	struct ShadowGeoItem {
		NiGeometry*            Geo;
		NiGeometryBufferData*  GeoData;          // geomData->BuffData (NULL => skinned, drawn via RenderSkinnedGeo)
		D3DMATRIX              World;            // camera-relative; valid only when GeoData != NULL
		D3DXVECTOR3            Center;           // camera-relative world-bound center
		float                  Radius;           // world-bound radius
		bool                   BaseInstanceable; // instanceable ignoring the per-pass AlphaEnabled
		bool                   HasAlphaMask;     // alpha blend/test present (blocks instancing when AlphaEnabled)
		bool                   PassesWater;      // skinInstance || !HasWater || center.z > 0
		bool                   IsActor;          // ref is an actor/creature => dynamic caster (Stage 2 split)
	};
	// Pooled across frames; only the *Count fields reset each frame so capacity is retained.
	std::vector<ShadowGeoItem>  ShadowGeoPool;
	int                         ShadowGeoCount;
	// When true, BuildExteriorGeoItems/CollectExteriorGeo collect ANCHOR-relative centers/matrices
	// (subtract CollectAnchor) for a cached static bake against an anchor-relative ShadowViewProj; set/cleared
	// around each bake (BakeStaticRegion, RenderActorOverlay). False (default) keeps the normal
	// camera-relative per-frame collection. Anchor-relative (not absolute-world) keeps coordinates small so
	// float32 shadow-map precision is preserved (absolute world coords ~1e5 make shadow edges flicker).
	bool                        CollectWorldSpace;
	D3DXVECTOR3                 CollectAnchor; // world origin the current cached bake draws relative to
	// When true (the per-frame actor overlay), CollectExteriorGeo skips rigid non-actor geometry during
	// collection so the walk doesn't build+store matrices for statics that the overlay would only discard.
	bool                        CollectSkinnedOnly;
};

void CreateShadowsHook();
void CreateEditorShadowsHook();