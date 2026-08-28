#pragma once
#include <d3dx9mesh.h>
#include <filesystem>

enum EffectRecordType
{
	EffectRecordType_Underwater,
	EffectRecordType_WaterLens,
	EffectRecordType_GodRays,
	EffectRecordType_KhajiitRays,
	EffectRecordType_MasserRays,
	EffectRecordType_SecundaRays,
	EffectRecordType_DepthOfField,
	EffectRecordType_AmbientOcclusion,
	EffectRecordType_Coloring,
	EffectRecordType_Cinema,
	EffectRecordType_Bloom,
	EffectRecordType_SnowAccumulation,
	EffectRecordType_SMAA,
	EffectRecordType_TAA,
	EffectRecordType_MotionBlur,
	EffectRecordType_WetWorld,
	EffectRecordType_Sharpening,
	EffectRecordType_VolumetricFog,
	EffectRecordType_VolumetricLight,
	EffectRecordType_Precipitations,
	EffectRecordType_ShadowsExteriors,
	EffectRecordType_ShadowsPoint,
	EffectRecordType_Extra,
};

enum ShaderType
{
	ShaderType_Vertex,
	ShaderType_Pixel,
};

enum DayPhase
{
	Dawn,
	Sunrise,
	Day,
	Sunset,
	Dusk,
	Night
};

enum CellLocation
{
	Unk,
	Interior,
	Exterior,
	Fake_Exterior
};

struct ShaderConstants {
	
	struct ShadowMapStruct {
		D3DXMATRIX	    ShadowWorld;
		D3DXMATRIX		ShadowViewProj;
		D3DXMATRIX		ShadowCameraToLight[4];
		D3DXVECTOR4		ShadowCubeMapLightPosition;	// light being baked this pass (camera-relative)
		// Per point-light slot: xyz camera-relative, w = far plane (0 = empty slot).
		// Size must stay in step with ShadowManager::PointLightMax (this header is included first,
		// so it cannot reference it) and with the sampler count in ShadowsPoint.fx.
		D3DXVECTOR4		ShadowCastLightPosition[4];
		// One component per point-light slot (x = slot 0 .. w = slot 3): luma(Diff) * Dimmer, the
		// brightness this light contributes and therefore the amount its shadow removes. Brightness
		// only, never colour -- the shadow darkens all three channels equally. The component count
		// must stay in step with ShadowManager::PointLightMax (this header is included first, so it
		// cannot reference it).
		D3DXVECTOR4		ShadowCastLightLuminance;
		D3DXVECTOR4		ShadowLightDir;
		D3DXVECTOR4		ShadowBiasDeferred;
		// x = terminator width, y = max slope clamp, z = adaptive enable (0/1),
		// w = sun active (0/1) -- 1 while the sun shadow maps are being updated this frame. Published
		// every exterior frame by ShadowManager::RenderExteriorShadows (NOT by PublishShadowBiasConstants,
		// which only runs on sun frames); the apply shader disables its terminator ramp when it is 0.
		D3DXVECTOR4		ShadowBiasAdaptive;
		// Camera->light sample matrices for the PREVIOUS static bake, built exactly like
		// ShadowCameraToLight[MapNear]/[MapFar] but from the region's Prev* fields. [0] = Near,
		// [1] = Far. Only published, and only read by the apply shader, while ShadowFadeData.x < 1.
		D3DXMATRIX		ShadowCameraToLightPrev[2];
		// x = static-map crossfade progress, 0 (fully on the previous bake) .. 1 (fully on the
		// current one). 1 means no crossfade is in flight, which is the steady state and the value
		// the apply shader branches on to skip the previous map set entirely. y/z/w unused.
		D3DXVECTOR4		ShadowFadeData;
	};

	struct PointLightStruct {
		D3DXVECTOR4		LightPosition[2];
		D3DXVECTOR4		LightColor[2];
	};

	struct WaterStruct {
		D3DXVECTOR4		waterCoefficients;
		D3DXVECTOR4		waveParams;
		D3DXVECTOR4		waterVolume;
		D3DXVECTOR4		waterSettings;
		D3DXVECTOR4		shorelineParams;
	};
	struct GrassStruct {
		D3DXVECTOR4		Scale;
		D3DXVECTOR4		CollisionParams;
		D3DXVECTOR4		CollisionXY[2];
	};
	struct POMStruct {
		D3DXVECTOR4		ParallaxData;
	};
	struct TerrainStruct {
		D3DXVECTOR4		Data;
	};
	struct SkinStruct {
		D3DXVECTOR4		SkinData;
		D3DXVECTOR4		SkinColor;
	};
	struct ShadowStruct {
		D3DXVECTOR4		Data;
		D3DXVECTOR4		ShadowSkinData;
		D3DXVECTOR4		OrthoData;
	};
	// Point-light cube shadows. Bake and sample constants are deliberately separate vectors
	// (the old system overloaded one TESR_ShadowCubeData between phases — a known trap).
	struct ShadowPointStruct {
		D3DXVECTOR4		BakeData;	// bake phase: x = skinned flag, y = alpha flag, z = far plane
		D3DXVECTOR4		PointData;	// sample phase: z = 1 / cube map size, w = depth bias (x unused;
									// y is a neutral 1.0, kept only so a stale compiled ShadowsPoint.fx
									// reading it as a darkness preshader degrades to "no point shadows")
	};
	struct PrecipitationsStruct {
		D3DXVECTOR4		RainData;
		D3DXVECTOR4		SnowData;
	};
	struct WaterLensStruct {
		D3DXVECTOR4		Time;
		float			TimeAmount;
		float			Percent;
	};
	struct GodRaysStruct {
		D3DXVECTOR4		Ray;
		D3DXVECTOR4		RayColor;
		D3DXVECTOR4		Data;
	};
	struct DepthOfFieldStruct {
		bool			Enabled;
		D3DXVECTOR4		Blur;
		D3DXVECTOR4		Data;
	};
	struct AmbientOcclusionStruct {
		bool			Enabled;
		D3DXVECTOR4		AOData;
		D3DXVECTOR4		Data;
	};
	struct ColoringStruct {
		D3DXVECTOR4		ColorCurve;
		D3DXVECTOR4		EffectGamma;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Values;
	};
	struct CinemaStruct {
		D3DXVECTOR4		Data;
	};
	struct BloomStruct {
		D3DXVECTOR4		BloomData;
		D3DXVECTOR4		BloomValues;
	};

	struct SpecularStruct {
		D3DXVECTOR4		EyePosition;
		D3DXVECTOR4		SpecularData;
	};

	struct GeometryStruct {
		D3DXVECTOR4		Toggles;
	};

	struct SnowAccumulationStruct {
		D3DXVECTOR4		Params;
	};
	struct MotionBlurStruct {
		D3DXVECTOR4		BlurParams;
		D3DXVECTOR4		Data;
		float			oldAngleX;
		float			oldAngleZ;
		float			oldAmountX;
		float			oldAmountY;
		float			oldoldAmountX;
		float			oldoldAmountY;
	};
	struct WetWorldStruct {
		D3DXVECTOR4		Coeffs;
		D3DXVECTOR4		Data;
	};
	struct SharpeningStruct {
		D3DXVECTOR4		Data;
	};
	struct VolumetricFogStruct {
		D3DXVECTOR4		Data;
	};
	struct TAAStruct {
		D3DXVECTOR4		Data;
	};
	struct VolumetricLightStruct {

		D3DXVECTOR4 data1;
		//x = Accum R
		//y = Accum G
		//z = Accum B
		//w = Accum Distance

		D3DXVECTOR4 data2;
		//x = Base R
		//y = Base G
		//z = Base B
		//w = Base Distance

		D3DXVECTOR4 data3;
		//x = UNUSED
		//y = Accum cutoff
		//z = Blur
		//w = Accum height Cutoff

		D3DXVECTOR4 data4;
		//x = Ray-march resolution scale ([Main] VolumetricLightResolution)
		//y = Animated fog toggle
		//z = Screen Res X
		//w = Screen Res Y 

		D3DXVECTOR4 data5;
		//x = Fog Direction x
		//y = Fog Direction y
		//z = Fog Direction z
		//w = Fog Power

		D3DXVECTOR4 data6;
		//x = Sun Scatter R
		//y = Sun Scatter G
		//z = Sun Scatter B
		//w = UNUSED

	};

	struct SimpleLightingStruct {
		UInt8	r;
		UInt8	g;
		UInt8	b;
		UInt8	a;
	};

	typedef std::map<std::string, SimpleLightingStruct> InteriorLightingMap;

	D3DXVECTOR4				ReciprocalResolution;
	D3DXVECTOR4				ReciprocalResolutionWater;
	D3DXVECTOR4				DirectionalLight; //currently only used for moon lighting
	bool					OverrideVanillaDirectionalLight;
	DayPhase				DayPhase;
	D3DXVECTOR4				SunDir;
	D3DXVECTOR4				SunTiming;
	D3DXVECTOR4				SunAmount;
	D3DXVECTOR4				MasserDir;
	D3DXVECTOR4				MasserAmount;
	float					MasserFade;
	D3DXVECTOR4				SecundaDir;
	D3DXVECTOR4				SecundaAmount;
	float					SecundaFade;
	bool					MoonsExist;
	InteriorLightingMap		InteriorLighting;
	float					MoonPhaseCoeff;
	D3DXVECTOR4				RaysPhaseCoeff;
	D3DXVECTOR4				GameTime;
	D3DXVECTOR4				Tick;
	D3DXVECTOR4				InteriorDimmer;
	float					InteriorDimmerStart;
	float					InteriorDimmerEnd;
	D3DXVECTOR4				TextureData;
	TESWeather*				pWeather;
	float					currentsunGlare;
	float					currentwindSpeed;
	UInt8					oldsunGlare;
	UInt8					oldwindSpeed;
	D3DXVECTOR4				fogColor;
	D3DXVECTOR4				oldfogColor;
	D3DXVECTOR4				sunColor;
	D3DXVECTOR4				oldsunColor;
	D3DXVECTOR4				fogData;
	float					currentfogStart;
	float					oldfogStart;
	float					currentfogEnd;
	float					oldfogEnd;
	bool					HasWater;
	ShadowMapStruct			ShadowMap;
	PointLightStruct		PointLights;
	WaterStruct				Water;
	GrassStruct				Grass;
	POMStruct				POM;
	TerrainStruct			Terrain;
	SkinStruct				Skin;
	ShadowStruct			Shadow;
	ShadowStruct			ShadowCube;
	ShadowPointStruct		ShadowPoint;
	PrecipitationsStruct	Precipitations;
	WaterLensStruct			WaterLens;
	GodRaysStruct			GodRays;
	GodRaysStruct			KhajiitRaysMasser;
	GodRaysStruct			KhajiitRaysSecunda;
	DepthOfFieldStruct		DepthOfField;
	AmbientOcclusionStruct	AmbientOcclusion;
	ColoringStruct			Coloring;
	CinemaStruct			Cinema;
	BloomStruct				Bloom;
	SpecularStruct			Specular;
	GeometryStruct			Geometry;
	SnowAccumulationStruct	SnowAccumulation;
	MotionBlurStruct		MotionBlur;
	WetWorldStruct			WetWorld;
	SharpeningStruct		Sharpening;
	TAAStruct				TAA;
	VolumetricFogStruct		VolumetricFog;
	VolumetricLightStruct	VolumetricLight;
	bool					EveningTransLightDirSet;
	D3DXVECTOR4				EveningTransLightDir;
	bool					MorningTransLightDirSet;
	D3DXVECTOR4				MorningTransLightDir;
	D3DXVECTOR4			    Jitter;
};

struct ShaderValue {
	UInt32				RegisterIndex;
	UInt32				RegisterCount;
	union {
	D3DXVECTOR4*		Value;
	TextureRecord*		Texture;
	};
};

class ShaderProgram {
public:
	ShaderProgram();
	~ShaderProgram();

	bool					SetConstantTableValue1(LPCSTR Name, UInt32 Index);
	bool					SetConstantTableValue2(LPCSTR Name, UInt32 Index);
	bool					SetPerGeomConstantTableValue(LPCSTR Name, UInt32 Index);
	void					SetConstantTableCustom(LPCSTR Name, UInt32 Index);

	ShaderValue*			FloatShaderValues;
	UInt32					FloatShaderValuesCount;

	ShaderValue*			PerGeomFloatShaderValues;
	UInt32					PerGeomFloatShaderValuesCount;

	ShaderValue*			TextureShaderValues;
	UInt32					TextureShaderValuesCount;
};

class ShaderRecord : public ShaderProgram {
public:
	ShaderRecord();
	~ShaderRecord();

	void					CreateCT();
	void					SetCT();
	void					SetPerGeomCT();
	bool					LoadShader(const char* Name, const char* DirPostFix = "");
	
	ShaderType				Type;
	bool					Enabled;
	bool					HasCT;
	bool					HasRB; 
	bool					HasDB;
	void*					Function;
	char* 					Source;
	ID3DXBuffer*			Errors;
	ID3DXBuffer*			Shader;
	ID3DXConstantTable*		Table;
};

class EffectRecord : public ShaderProgram {
public:
	EffectRecord();
	~EffectRecord();

	bool						LoadEffect(const char* Name);
	void						CreateCT();
	void						SetCT();
	void						Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, bool ClearRenderTarget);
	// Chain-rotation variant of Render (Main.EffectChainPingPong). Reads the chain's current
	// buffer and writes into the two scratch buffers, leaving the result in one of them; the caller
	// then adopts it as the new current. Issues NO StretchRect at all.
	void						RenderChained(IDirect3DDevice9* Device, bool ClearRenderTarget);

	bool						Enabled;
	// True when the effect declares TESR_SourceBuffer, i.e. it actually reads the pre-chain image.
	// Latched in CreateCT the way ShaderRecord latches HasRB/HasDB, so the caller can skip the
	// full-screen scene->SourceSurface blit for effects that never sample it.
	bool						HasSB;
	// Sampler ORDINALS of TESR_SourceBuffer / TESR_RenderedBuffer, latched in CreateCT. Ordinals,
	// not declared registers: EffectRecord::CreateCT numbers TESR_ samplers in parameter order and
	// SetCT binds with SetTexture(ordinal), so this is the same index space it uses.
	//
	// RBRegister is why the chain rotation cannot simply assume sampler 0 the way the in-effect
	// ping-pong in Render() does. SMAA puts TESR_SourceBuffer at s0 and TESR_RenderedBuffer at s1.
	bool						HasRB;
	UInt32						SBRegister;
	UInt32						RBRegister;
	char*	 					Source;
	ID3DXBuffer*				Errors;
	ID3DXEffect*				Effect;
	std::string					ProfileName = "?"; // label for the ProfileEffects per-effect GPU breakdown
};

typedef std::map<std::string, EffectRecord*> ExtraEffectsList;
typedef std::map<std::string, D3DXVECTOR4> CustomConstants;

class ShaderManager { // Never disposed
public:
	ShaderManager();


	
	struct JitterPair {
		float x;
		float y;
	};

	struct JitterPattern {
		JitterPair pattern[2];
	};

	void					CreateEffects();
	void					CompileShader(char* FileName, char* FileNameBinary, char* Source, ShaderType Type, ID3DXBuffer* Errors, ID3DXBuffer* Shader, ID3DXConstantTable* Table);
	void					CompileEffect(char* FileName, char* FileNameBinary, char* Source, ID3DXBuffer* Errors);
	void					CompileShaders(const std::filesystem::path&);
	void					InitializeConstants();
	void					UpdateConstants();
	void					UpdateShaderStates();
	void					BeginScene();
	void					SetVolumetricLightModifiers(SettingsVolumetricLightStruct* currentSettings);
	void					CreateShader(const char *Name);
	void					LoadShader(NiD3DVertexShader* Shader, const char* DirPostFix = "");
	void					LoadShader(NiD3DPixelShader* Shader, const char* DirPostFix = "");
	void					DisposeShader(const char* Name);
	void					CreateEffect(EffectRecordType EffectType);
	bool					LoadEffect(EffectRecord* TheEffect, char* Filename, char* CustomEffectName);
	void					LoadEffectSettings();
	void					DisposeEffect(EffectRecord* TheEffect);
	void					RenderEffectsPreHdr(IDirect3DSurface9* RenderTarget);
	void					RenderEffectsPostHdr(IDirect3DSurface9* RenderTarget);
	void					RenderEffects(IDirect3DSurface9* RenderTarget);
	void					RenderShadowsMidScene(); // sun + point shadow apply, run mid-scene before the first near-water draw
	void					FlattenShellDepth();	 // near shell: rewrite TESR_DepthBuffer to "at M" over shell-covered pixels, after the shell
	void					FlattenShellPreWaterDepth(bool MaskResolved); // same over TESR_DepthBufferPreWater, DURING the shell, before its first near water
	void					FlattenShellDepthInto(IDirect3DTexture9* Target, IDirect3DSurface9** TargetSurface, bool ResolveMask); // shared body of the two above
	bool					CaptureShellRenderedBuffer(); // near shell: refresh TESR_RenderedBuffer over shell coverage only; true = ShellMaskTexture now holds it
	bool					CreateShellMask();		 // ShellMaskTexture, shared by the flatten and the capture
	bool					CreateShellQuadVertexShader(); // ShellFlatten.vso, likewise shared
	bool					CreateShellFlatten(IDirect3DTexture9* Target, IDirect3DSurface9** TargetSurface); // lazily builds what the flatten needs; false = feature stays off
	bool					CreateShellCopy();		 // lazily builds what the masked capture needs; false = caller falls back to a blind blit
	bool					CaptureDeviceState();	 // snapshot the full device state into CachedStateBlock; false = unavailable, caller must bail
	void					ProfileBlitToSource(IDirect3DSurface9* RenderTarget); // counted scene->SourceSurface copy
	// --- Post chain, opt-in rotation (Main.EffectChainPingPong) --------------------------------
	// The legacy chain hands the image between effects through the render target: every effect ends
	// by copying it back into RenderedSurface, and every effect that reads TESR_SourceBuffer takes a
	// second full-screen copy first. That is ~11 full-screen FP16 StretchRects a frame in a typical
	// exterior, measured at roughly 0.2 ms each.
	//
	// The rotation instead keeps the live image in one of three interchangeable full-screen textures
	// (Rendered / Ping / Effect - all created identically) and never copies between effects at all.
	// Each effect reads the current one, ping-pongs its passes across the other two, and the chain
	// simply relabels which buffer is current. TESR_SourceBuffer binds to the current buffer, which
	// is guaranteed untouched for the duration of the effect precisely because the passes write only
	// the other two - that is what makes the source copy unnecessary rather than merely skipped.
	// One copy remains, at the very end, to put the result in the caller's render target.
	bool					ChainActive;	// rotation in use for the chain currently running
	int						ChainCur;		// index into ChainTex/ChainSurf of the live image
	IDirect3DTexture9*		ChainTex[3];
	IDirect3DSurface9*		ChainSurf[3];
	void					ChainBegin();							// adopt RenderedTexture as current
	void					ChainEnd(IDirect3DSurface9* RenderTarget);	// materialise into the render target
	void					RunEffect(EffectRecord* Effect, IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool BlitSource, bool ClearRenderTarget);
	void					SwitchShaderStatus(const char* Name);
	void					SetCustomConstant(const char* Name, D3DXVECTOR4 Value);
	void					SetExtraEffectEnabled(const char* Name, bool Value);
	void					SetPhaseLumCoeff(int phaseLength, int phaseDay);

	int						jitterIndex = 0;
	float					jitterProjectionX;
	float					jitterProjectionY;
	const JitterPattern     jitterPattern[2] = { {-0.25f,0.25f,0.25f,-0.25f},{-0.5f,0.5f,0.5f,-0.5f} };
	bool					jitterSet;
	int						GameDay;
	int						InitFrameCount;
	int						InitFrameTarget;
	struct					EffectQuad { float x, y, z; float u, v; };
	struct					GrassActorPos { float x, y; };
	ShaderConstants			ShaderConst;
	/// World-space grass collision sources. Slot 0 is always the player's live position; slots 1-2
	/// hold either a fading footprint left behind by the player or a nearby actor's live position.
	GrassActorPos			GrassCollisionSources[3];
	/// Recovery weights for sources 1 and 2. Source 0 is implicitly 1.0 and is not stored.
	/// Negative values are the spring overshoot past upright and are expected.
	float					GrassCollisionWeights[2];
	int						GrassCollisionSourceCount;
	CustomConstants			CustomConst;
	CellLocation			LocationState;
	bool					DialogState;
	IDirect3DTexture9*		SourceTexture;
	IDirect3DSurface9*		SourceSurface;
	IDirect3DTexture9* 		RenderedTexture;
	IDirect3DSurface9*		RenderedSurface;
	IDirect3DTexture9*		RenderTextureSMAA;
	IDirect3DSurface9*		RenderSurfaceSMAA;
	IDirect3DTexture9*		EffectTexture;
	IDirect3DSurface9*		EffectSurface;
	IDirect3DTexture9*		TAATexture;
	IDirect3DSurface9*		TAASurface;
	IDirect3DTexture9*		PingTexture;   // multi-pass ping-pong scratch (avoids per-pass blits)
	IDirect3DSurface9*		PingSurface;
	D3DMATRIX				PrevWorldViewProjMatrix;
	bool					RenderedBufferFilled;
	bool					DepthBufferFilled;
	bool					PreWaterDepthBufferFilled;
	// True only while the MAIN WorldSceneGraph render is on the stack (set in
	// RenderHook::TrackRenderObject). The three latches above are per-SCENE: ShaderManager::BeginScene
	// clears them, and the game calls BeginScene again for the off-screen renders that follow the main
	// pass - the water REFLECTION map among them (confirmed by [ReflDbg] log, 2026-07-17: the
	// reflection renders AFTER the main pass, at 1024x1024, NOT through RenderObject(WorldSceneGraph)),
	// and numbered WATER* shaders DO bind during it. Anything that must hold for the rest of the FRAME
	// has to be gated on this as well as on its latch. Two things are: the mid-scene sun-shadow apply
	// (RenderHook, near-water trigger - without it the apply re-fired INTO the reflection map, leaving
	// camera-tracking caster silhouettes floating in the water) and ShaderRecord::SetCT's depth resolve
	// (see the comment there).
	bool					InMainScenePass;
	bool					isFullyInitialized;
	bool					UseIntervalUpdate;
	TESObjectCELL*			previousCell;
	float					previousBlend;
	//Begin Volume Light
	float					previousModifier = 1.0f;
	float					currentModifier = 1.0f;
	D3DXVECTOR3				currentWind = D3DXVECTOR3(0.0f, 1.0f, 0.0f);
	D3DXVECTOR3				previousWind = D3DXVECTOR3(0.0f, 1.0f, 0.0f);
	float					currentFogHeight;
	float					previousFogHeight;
	float					currentAccumDistance;
	float					previousAccumDistance;
	bool					modifiersSet = false;
	bool					modifiersInitialzed = false;
	//End Volume Light
	SettingsWaterStruct*	sws;
	SettingsAmbientOcclusionStruct* sas;
	SettingsBloomStruct* sbs;
	SettingsColoringStruct* scs;
	ShaderConstants::SimpleLightingStruct	InteriorLighting;
	IDirect3DVertexBuffer9*	EffectVertex;
	// Near-shell depth flatten. All of it is built on first use and left NULL when the shell never
	// runs, so a disabled shell costs neither the full-screen INTZ surface nor the shader loads.
	IDirect3DTexture9*		ShellMaskTexture;			// post-shell depth resolve = the shell's coverage mask
	IDirect3DSurface9*		ShellFlattenDepthSurface;	// level 0 of RenderManager::DepthTexture, bound as the target
	IDirect3DSurface9*		ShellFlattenPreWaterSurface;// level 0 of RenderManager::DepthTexturePreWater, ditto
	ShaderRecord*			ShellFlattenVertex;
	ShaderRecord*			ShellFlattenPixel;
	ShaderRecord*			ShellCopyPixel;
	IDirect3DVertexShader9*	ShellFlattenVertexShader;	// shared: the flatten quad and the masked colour copy use the same one
	IDirect3DPixelShader9*	ShellFlattenPixelShader;
	IDirect3DPixelShader9*	ShellCopyPixelShader;		// masked TESR_RenderedBuffer capture (CaptureShellRenderedBuffer)
	bool					ShellFlattenFailed;			// latched on the first failure so it is not retried per frame;
														// SHARED by both flatten targets - see CreateShellFlatten for why
	// Shared by every mid-scene pass that has to hand the engine back the exact device state its own
	// state caches believe is current. Built on first use and kept: CreateStateBlock(D3DSBT_ALL) both
	// allocates the block AND snapshots the whole device state vector, which is among the most
	// expensive calls in D3D9, and these sites fire up to three times a frame. Capture() re-snapshots
	// into the existing block, which is the only half of that they actually need. The captured state
	// SET is fixed at creation, so a block made by one site restores correctly for all of them.
	IDirect3DStateBlock9*	CachedStateBlock;
	IDirect3DDevice9*		CachedStateBlockDevice;		// the device it was built against; a change forces a rebuild
	EffectRecord*			UnderwaterEffect;
	EffectRecord*			WaterLensEffect;
	EffectRecord*			GodRaysEffect;
	EffectRecord*			MasserRaysEffect;
	EffectRecord*			SecundaRaysEffect;
	EffectRecord*			DepthOfFieldEffect;
	EffectRecord*			AmbientOcclusionEffect;
	EffectRecord*			ColoringEffect;
	EffectRecord*			CinemaEffect;
	EffectRecord*			BloomEffect;
	EffectRecord*			SnowAccumulationEffect;
	EffectRecord*			SMAAEffect;
	EffectRecord*			TAAEffect;
	EffectRecord*			MotionBlurEffect;
	EffectRecord*			WetWorldEffect;
	EffectRecord*			SharpeningEffect;
	EffectRecord*			VolumetricFogEffect;
	EffectRecord*			VolumetricLightEffect;
	EffectRecord*			RainEffect;
	EffectRecord*			SnowEffect;
	EffectRecord*			ShadowsExteriorsEffect;
	EffectRecord*			ShadowsPointEffect;
	ExtraEffectsList		ExtraEffects;
	NiD3DVertexShader*		WaterHeightMapVertexShader;
	NiD3DPixelShader*		WaterHeightMapPixelShader;
	NiD3DVertexShader*		WaterVertexShaders[51];
	NiD3DPixelShader*		WaterPixelShaders[51];

private:
	// UpdateConstants helpers — one function per rendering concern
	static void UpdateGameTime(ShaderConstants& ShaderConst);
	static void UpdateCelestialDirections(ShaderConstants& ShaderConst, NiNode* SunRoot, Moon* Masser, Moon* Secunda, TESClimate* climate, float lastGameTime);
	static void UpdateMoonPhaseCoeff(ShaderConstants& ShaderConst, TESClimate* climate, int& GameDay);
	static float UpdateExteriorLighting(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent);
	static void UpdateInteriorLighting(ShaderConstants& ShaderConst, TESObjectCELL* currentCell, ShaderConstants::SimpleLightingStruct& InteriorLighting, bool& isFullyInitialized, int& InitFrameCount);
	static void UpdateWater(ShaderConstants& ShaderConst, TESObjectCELL* currentCell, SettingsWaterStruct* sws);
	static void UpdateSnowAccumulation(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather);
	static void UpdateWetWorld(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent);
	static void UpdatePrecipitation(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent);
	static void UpdateGrass(ShaderConstants& ShaderConst, GrassActorPos GrassCollisionSources[3], float GrassCollisionWeights[2], int& GrassCollisionSourceCount);
	static void UpdatePOM(ShaderConstants& ShaderConst);
	static void UpdateTerrain(ShaderConstants& ShaderConst);
	static void UpdateSkin(ShaderConstants& ShaderConst);
	static void UpdateGodRays(ShaderConstants& ShaderConst);
	static void UpdateKhajiitRays(ShaderConstants& ShaderConst);
	static void UpdateAmbientOcclusion(ShaderConstants& ShaderConst, SettingsAmbientOcclusionStruct* sas);
	static void UpdateBloom(ShaderConstants& ShaderConst, SettingsBloomStruct* sbs);
	static void UpdateColoring(ShaderConstants& ShaderConst, SettingsColoringStruct* scs);
	static void UpdateDepthOfField(ShaderConstants& ShaderConst, bool IsThirdPersonView);
	static void UpdateCinema(ShaderConstants& ShaderConst);
	static void UpdateMotionBlur(ShaderConstants& ShaderConst, bool IsThirdPersonView);
	static void UpdateSharpening(ShaderConstants& ShaderConst);
	static void UpdateVolumetricFog(ShaderConstants& ShaderConst, float weatherPercent);
	static void UpdateTAA(ShaderConstants& ShaderConst, int& jitterIndex, const JitterPattern jitterPattern[2]);
	static void UpdateVolumetricLight(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent, float dayPercent);
	static void UpdateSpecular(ShaderConstants& ShaderConst);
};