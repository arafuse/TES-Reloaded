#include <CommCtrl.h>
#include <intrin.h>
#include "RenderHook.h"

#if defined(NEWVEGAS)
#define kRender 0x008706B0
#define kProcessImageSpaceShaders 0x00B55AC0
#define kBeginScene 0x00E74F40
#define kMenuRenderedTexture 0x011DED3C
#define kShowDetectorWindow 0x004D61B0
#define kDetectorWindowScale 0x004D67A5
#define kDetectorWindowScaleReturn 0x004D67E9
static const UInt32 kRenderInterface = 0x007144D3;
static const UInt32 kRenderInterfaceReturn = 0x007144D8;
static const UInt32 kRenderInterfaceMethod = 0x007134D0;
static const UInt32 kSetTileShaderConstants = 0x00BCAAD7;
static const UInt32 kSetTileShaderConstantsReturn = 0x00BCAADE;
static const UInt32 kMultiBoundWaterHeightFix1 = 0x006FAE04;
static const UInt32 kMultiBoundWaterHeightFix2 = 0x006FAF50;
static const UInt32 kDetectorWindowSetNodeName = 0x004D678A;
static const UInt32 kDetectorWindowCreateTreeView = 0x004D6476;
static const UInt32 kDetectorWindowCreateTreeViewReturn = 0x004D647F;
static const UInt32 kDetectorWindowDumpAttributes = 0x004D6A2E;
static const UInt32 kDetectorWindowDumpAttributesReturn = 0x004D69C5;
#elif defined(OBLIVION)
#define kRender 0x0040C830
#define kProcessImageSpaceShaders 0x007B48E0
#define kBeginScene 0x0076BE00
#define kMenuRenderedTexture 0x00B333E8
#define kShowDetectorWindow 0x00496CB0
#define kDetectorWindowScale 0x004965A8
#define kDetectorWindowScaleReturn 0x0049660F
static const UInt32 kRenderInterface = 0x0057F3F3;
static const UInt32 kRenderInterfaceReturn = 0x0057F3F8;
static const UInt32 kRenderInterfaceMethod = 0x0070E0A0;
static const UInt32 kDetectorWindowSetNodeName = 0x0049658E;
static const UInt32 kDetectorWindowCreateTreeView = 0x00495E1F;
static const UInt32 kDetectorWindowCreateTreeViewReturn = 0x00495E27;
static const UInt32 kDetectorWindowDumpAttributes = 0x004967C7;
static const UInt32 kDetectorWindowDumpAttributesReturn = 0x004967CD;
// Engine global set to 1 around the water-reflection WorldSceneGraph render (0x004D0404/0x004D0413)
// and read by the game's shader setup to alter behavior during that pass.
static UInt8* const kIsRenderingWaterReflections = (UInt8*)0x00B42E86;
#elif defined(SKYRIM)
#define kRender 0x0069BDF0
#define kProcessImageSpaceShaders 0x00C70DA0
#define kBeginScene 0x00CDA620
#define kMenuRenderedTexture 0x01B2E8D8
static const UInt32 kRenderInterface = 0x00A5CB57;
static const UInt32 kRenderInterfaceReturn = 0x00A5CB5C;
static const UInt32 kRenderInterfaceMethod = 0x00A623F0;
static const UInt32 kRenderFirstPersonNode = 0x0069BD27;
static const UInt32 kRenderFirstPersonNodeRepeat = 0x0069BCC4;
static const UInt32 kRenderFirstPersonNodeReturn = 0x0069BD32;
static const UInt32 kClearDepth = 0x00698BBA;
static const UInt32 kClearDepthReturn = 0x00698BC3;
static const UInt32 kSetShadowDistance = 0x00CBB140;
static const UInt32 kSetShadowDistanceReturn = 0x00CBB146;
static const UInt32 kSetShadowDistanceShader = 0x00CB7365;
static const UInt32 kSetShadowDistanceShaderReturn = 0x00CB736D;
static const UInt32 kFixSunFlags = 0x0069A92F;
static const UInt32 kFixSunFlagsReturn = 0x0069A938;
static UInt32 ClearMode = 0;
#endif

class RenderHook {
public:
	void*	TrackShowDetectorWindow(HWND, HINSTANCE, NiNode*, char*, int, int, int, int);
	bool	TrackBeginScene();
#if defined(NEWVEGAS)
	void	TrackRender(BSRenderedTexture* RenderedTexture, int Arg2, int Arg3);
	void	TrackRenderWorldSceneGraph(Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame, UInt8 Arg4);
	void	TrackRenderFirstPerson(NiDX9Renderer* Renderer, NiGeometry* Geo, Sun* SkySun, BSRenderedTexture* RenderedTexture);
	float	TrackGetWaterHeightLOD();
#elif defined (OBLIVION)
	void	TrackRender(BSRenderedTexture*);
	bool	TrackEndTargetGroup(NiCamera*, NiRenderTargetGroup*);
	void	TrackHDRRender(NiScreenElements*, BSRenderedTexture**, BSRenderedTexture**, UInt8);
	UInt32	TrackSetupShaderPrograms(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, UInt32);
	void	TrackCullingBSFadeNode(NiCullingProcess*);
	float	TrackFarPlane();
	HRESULT TrackSetSamplerState(UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8);
#elif defined(SKYRIM)
	void	TrackRender(BSRenderedTexture* RenderedTexture, int Arg2, int Arg3);
	bool	TrackSetupRenderingPass(UInt32 Arg1, UInt32 Arg2);
	void	TrackRenderWorldSceneGraph(Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame);
#endif

};

void* (__thiscall RenderHook::* ShowDetectorWindow)(HWND, HINSTANCE, NiNode*, char*, int, int, int, int);
void* (__thiscall RenderHook::* TrackShowDetectorWindow)(HWND, HINSTANCE, NiNode*, char*, int, int, int, int);
void* RenderHook::TrackShowDetectorWindow(HWND Handle, HINSTANCE Instance, NiNode* RootNode, char* FormCaption, int X, int Y, int Width, int Height) {

	NiAVObject* Object = NULL;
	void* r = NULL;
	
	r = (this->*ShowDetectorWindow)(Handle, Instance, RootNode, "Pipeline detector by Alenet", X, Y, 1024, 1024);
	for (int i = 0; i < RootNode->m_children.end; i++) {
		NiNode* Node = (NiNode*)RootNode->m_children.data[i];
		Node->m_children.data[0] = NULL;
		Node->m_children.data[1] = NULL;
		Node->m_children.end = 0;
		Node->m_children.numObjs = 0;
	}
	return r;

}

bool (__thiscall RenderHook::* BeginScene)();
bool (__thiscall RenderHook::* TrackBeginScene)();
bool RenderHook::TrackBeginScene() {

	TheShaderManager->BeginScene();
	return (this->*BeginScene)();

}
#if defined(NEWVEGAS)
void (__thiscall RenderHook::* Render)(BSRenderedTexture*, int, int);
void (__thiscall RenderHook::* TrackRender)(BSRenderedTexture*, int, int);
void RenderHook::TrackRender(BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	
	TheRenderManager->SetSceneGraph();
	TheShaderManager->UpdateConstants();
	if (TheSettingManager->SettingsMain.Develop.TraceShaders && *RenderWindowNode == NULL && TheKeyboardManager->OnKeyDown(TheSettingManager->SettingsMain.Develop.TraceShaders) && MenuManager->IsActive(Menu::MenuType::kMenuType_None)) {
		*RenderWindowNode = (NiNode*)MemoryAlloc(sizeof(NiNode));
		NiNode* Node = *RenderWindowNode;
		Node->New(4096);
		Node->SetName("Passes...");
	}
	(this->*Render)(RenderedTexture, Arg2, Arg3);

}

void (__cdecl * SetupRenderingPass)(UInt32, NiD3DShader*) = (void (__cdecl *)(UInt32, NiD3DShader*))0x00B99390;
void __cdecl TrackSetupRenderingPass(UInt32 PassIndex, NiD3DShader* Shader) {
	
	SetupRenderingPass(PassIndex, Shader);

	NiGeometry* Geometry = *(NiGeometry**)(*(void**)0x011F91E0);
	NiD3DPass* Pass = *(NiD3DPass**)0x0126F74C;
	NiD3DVertexShaderEx* VertexShader = (NiD3DVertexShaderEx*)Pass->VertexShader;
	NiD3DPixelShaderEx* PixelShader = (NiD3DPixelShaderEx*)Pass->PixelShader;
	NiNode* RenderWindowRootNode = *RenderWindowNode;

	if (VertexShader && PixelShader) {
		if (VertexShader->ShaderProg) VertexShader->ShaderProg->SetCT();
		if (PixelShader->ShaderProg) PixelShader->ShaderProg->SetCT();
		if (RenderWindowRootNode) {
			char Name[256];
			NiNode* Node = (NiNode*)MemoryAlloc(sizeof(NiNode)); Node->New(2);
			sprintf(Name, "Pass %s (%s %s)", Geometry->m_pcName, VertexShader->ShaderName, PixelShader->ShaderName);
			if (!VertexShader->ShaderProg) strcat(Name, " - Vertex: vanilla");
			if (!PixelShader->ShaderProg) strcat(Name, " - Pixel: vanilla");
			Node->SetName(Name);
			Node->m_children.Add((NiAVObject**)&Geometry->m_parent); // We do not use the AddObject to avoid to alter the original object
			Node->m_children.Add((NiAVObject**)&Geometry); // Same as above
			RenderWindowRootNode->AddObject(Node, 1);
		}
	}

}

void (__thiscall RenderHook::* RenderWorldSceneGraph)(Sun*, UInt8, UInt8, UInt8);
void (__thiscall RenderHook::* TrackRenderWorldSceneGraph)(Sun*, UInt8, UInt8, UInt8);
void RenderHook::TrackRenderWorldSceneGraph(Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame, UInt8 Arg4) {
	
	bool CameraMode = TheSettingManager->SettingsMain.CameraMode.Enabled;

	(this->*RenderWorldSceneGraph)(SkySun, IsFirstPerson, WireFrame, Arg4);
	if (CameraMode || Player->IsThirdPersonView(CameraMode, TheRenderManager->FirstPersonView)) TheRenderManager->ResolveDepthBuffer();

}

void (__thiscall RenderHook::* RenderFirstPerson)(NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*);
void (__thiscall RenderHook::* TrackRenderFirstPerson)(NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*);
void RenderHook::TrackRenderFirstPerson(NiDX9Renderer* Renderer, NiGeometry* Geo, Sun* SkySun, BSRenderedTexture* RenderedTexture) {
	
	(this->*RenderFirstPerson)(Renderer, Geo, SkySun, RenderedTexture);
	TheRenderManager->ResolveDepthBuffer();
	TheRenderManager->Clear(NULL, NiRenderer::kClear_ZBUFFER);
	ThisCall(0x00874C10, Global);
	(this->*RenderFirstPerson)(Renderer, Geo, SkySun, RenderedTexture);

}

float (__thiscall RenderHook::* GetWaterHeightLOD)();
float (__thiscall RenderHook::* TrackGetWaterHeightLOD)();
float RenderHook::TrackGetWaterHeightLOD() {
	
	TESWorldSpace* Worldspace = (TESWorldSpace*)this;
	float r = Worldspace->waterHeight;

	// Due to compiler optimization, the function GetWaterHeightLOD is "shared" as general method that returns ECX + 0x07C
	if (*(void**)Worldspace == (void*)0x0103195C) r = Tes->GetWaterHeight(Player);
	return r;

}

void (__cdecl * SetShaderPackage)(int, int, UInt8, int, char*, int) = (void (__cdecl *)(int, int, UInt8, int, char*, int))0x00B4F710;
void __cdecl TrackSetShaderPackage(int Arg1, int Arg2, UInt8 Force1XShaders, int Arg4, char* GraphicsName, int Arg6) {

	SetShaderPackage(Arg1, Arg2, Force1XShaders, Arg4, GraphicsName, Arg6);

}

void RenderMainMenuMovie() {

	if (TheSettingManager->SettingsMain.Main.ReplaceIntro && MenuManager->IsActive(Menu::MenuType::kMenuType_Main))
		Binker::Render(TheRenderManager->device, TheSettingManager->CurrentPath, MainMenuMovie, TheRenderManager->width, TheRenderManager->height);
	else
		Binker::Close(TheRenderManager->device);

}

void SetTileShaderAlpha() {

	if (MenuManager->IsActive(Menu::MenuType::kMenuType_Main)) {
		NiVector4 TintColor = { 1.0f, 1.0f, 1.0f, 0.0f };
		float ViewProj[16];
		TheRenderManager->device->GetVertexShaderConstantF(0, ViewProj, 4);
		if ((int)ViewProj[3] == -1 && (int)ViewProj[7] == 1 && (int)ViewProj[15] == 1) TheRenderManager->device->SetPixelShaderConstantF(0, (const float*)&TintColor, 1);
	}

}

static __declspec(naked) void SetTileShaderConstants() {

	__asm {
		pushad
		call	SetTileShaderAlpha
		popad
		cmp		byte ptr [esi + 0xAC], 0
		jmp		kSetTileShaderConstantsReturn
	}

}

float MultiBoundWaterHeightFix() {

	return Player->pos.z;

}

#elif defined(OBLIVION)
void (__thiscall RenderHook::* Render)(BSRenderedTexture*);
void (__thiscall RenderHook::* TrackRender)(BSRenderedTexture*);
void RenderHook::TrackRender(BSRenderedTexture* RenderedTexture) {
	TheShaderManager->UpdateShaderStates();
	TheRenderManager->SetSceneGraph();
	TheShaderManager->UpdateConstants();
	if (TheRenderManager->BackBuffer) TheRenderManager->defaultRTGroup->RenderTargets[0]->data->Surface = TheRenderManager->defaultRTGroup->RenderTargets[1]->data->Surface;
	if (TheSettingManager->SettingsMain.Develop.TraceShaders && *RenderWindowNode == NULL && TheKeyboardManager->OnKeyPressed(TheSettingManager->SettingsMain.Develop.TraceShaders)) {
		*RenderWindowNode = (NiNode*)MemoryAlloc(sizeof(NiNode));
		NiNode* Node = *RenderWindowNode;
		Node->New(4096);
		Node->SetName("Passes...");
	}
	if (TheSettingManager->SettingsMain.Develop.LogShaders && TheKeyboardManager->OnKeyPressed(TheSettingManager->SettingsMain.Develop.LogShaders)) Logger::Log("START FRAME LOG");
	(this->*Render)(RenderedTexture);

}

bool (__thiscall RenderHook::* EndTargetGroup)(NiCamera*, NiRenderTargetGroup*);
bool (__thiscall RenderHook::* TrackEndTargetGroup)(NiCamera*, NiRenderTargetGroup*);
bool RenderHook::TrackEndTargetGroup(NiCamera* Camera, NiRenderTargetGroup* TargetGroup) {

	if (TheRenderManager->BackBuffer) TargetGroup = TheRenderManager->defaultRTGroup;
	return (this->*EndTargetGroup)(Camera, TargetGroup);

}

void (__thiscall RenderHook::* HDRRender)(NiScreenElements*, BSRenderedTexture**, BSRenderedTexture**, UInt8);
void (__thiscall RenderHook::* TrackHDRRender)(NiScreenElements*, BSRenderedTexture**, BSRenderedTexture**, UInt8);
void RenderHook::TrackHDRRender(NiScreenElements* ScreenElements, BSRenderedTexture** RenderedTexture1, BSRenderedTexture** RenderedTexture2, UInt8 Arg4) {
	
	//NOTE: textures are set here because applying surface changes directly to the RT interferes with the Refraction shader
	
	if (TheSettingManager->SettingsMain.Main.RenderEffectsBeforeHdr) {
		BSRenderedTexture* rt1 = *RenderedTexture1;
		rt1->RenderedTexture->rendererData->dTexture = TheShaderManager->EffectTexture;
		(this->*HDRRender)(ScreenElements, RenderedTexture1, RenderedTexture2, Arg4);
		//Copy back to SourceBuffer for vanilla image space effects that needed fixes
		TheRenderManager->device->StretchRect(TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, NULL, TheShaderManager->SourceSurface, NULL, D3DTEXF_NONE);
	}
	else {
		(this->*HDRRender)(ScreenElements, RenderedTexture1, RenderedTexture2, Arg4);
	}

}

float (__thiscall RenderHook::* FarPlane)();
float (__thiscall RenderHook::* TrackFarPlane)();
float RenderHook::TrackFarPlane() {
	
	float r = (this->*FarPlane)();

	if (TheSettingManager->SettingsMain.Main.FarPlaneDistance && r == 283840.0f) r = TheSettingManager->SettingsMain.Main.FarPlaneDistance;
	return r;

}

// [GrassOrderDbg] TEMPORARY spike instrumentation (grass/water draw-order research).
// Armed for one WorldSceneGraph render by the Develop.LogShaders key: logs every shader pass in
// draw order, plus engine call-site addresses for the first few GRASS and WATER passes so the
// engine function that schedules grass after water can be located. Remove when the spike concludes.
static bool	GrassOrderCapture = false;
// True only while the MAIN WorldSceneGraph render is on the stack (set in TrackRenderObject). The
// game calls BeginScene again for off-screen renders after the main pass — the water REFLECTION
// map among them (confirmed by [ReflDbg] log: reflection renders AFTER the main pass, at 1024x1024,
// NOT through RenderObject(WorldSceneGraph)) — and ShaderManager::BeginScene resets
// PreWaterDepthBufferFilled each time. Without this guard, a water shader binding during the
// reflection render re-fired the mid-scene sun-shadow apply INTO the reflection map, leaving
// camera-tracking caster silhouettes floating in the water.
static bool	InMainScenePass = false;

// Diagnostic for the one open question in the spec: does the engine rebuild projMatrix from the
// frustum at pass start? These capture projMatrix as the ENGINE left it at the first draw of each
// pass, before StampPassProjection touches it.
static bool		FarProjSeen				= false;
static bool		ShellProjSeen			= false;
static float	FarProj33Seen			= 0.0f;
static float	ShellProj33Seen			= 0.0f;

// [ShellDraw] / [ShellWater] evidence capture. Reset per frame at far-pass entry; both gated behind
// Develop.NearShellDebug so they cost nothing in normal play.
static int		ShellDrawLogCount	= 0;
static int		FarWaterLogCount	= 0;

// Sampler register that a shell-pass water draw had TESR_DepthBufferPreWater bound to in place of
// TESR_DepthBuffer, so it can be put back when the shell ends. 0xFFFFFFFF = nothing swapped.
#define ShellWaterDepthSamplerNone 0xFFFFFFFF
static UInt32	ShellWaterDepthSampler	= ShellWaterDepthSamplerNone;

// The shell's once-per-frame preparation for its own water draws has run: its TESR_RenderedBuffer
// capture and its TESR_DepthBufferPreWater clamp, both of which have to land at the same instant -
// after the shell's opaque, detail and grass draws and before any of its water. Armed at the start
// of the shell, latched at the shell's first NEAR-water draw. Read and written only at PassNear.
static bool		ShellNearWaterPrepDone = false;

static int	GrassOrderSeq = 0;
static int	GrassOrderGrassTraces = 0;
static int	GrassOrderWaterTraces = 0;

static void GrassOrderLogTrace(const char* Tag, void* Ra, NiPropertyState* PropertyState) {

	NiAlphaProperty* Alpha = PropertyState ? (NiAlphaProperty*)PropertyState->prop[0] : NULL;
	NiProperty* ZBuf = PropertyState ? PropertyState->prop[9] : NULL;
	Logger::Log("[GrassOrderDbg] TRACE %s ra=%08X alphaFlags=%04X alphaRef=%d zFlags=%04X", Tag, (UInt32)Ra,
		Alpha ? Alpha->flags : 0xFFFF, Alpha ? (int)Alpha->alphaTestRef : -1, ZBuf ? *(UInt16*)((UInt8*)ZBuf + 0x18) : 0xFFFF);

	void* Frames[24];
	USHORT Count = CaptureStackBackTrace(0, 24, Frames, NULL);
	char Line[512];
	strcpy(Line, "[GrassOrderDbg]   bt:");
	for (USHORT i = 0; i < Count; i++) {
		char T[16];
		sprintf(T, " %08X", (UInt32)Frames[i]);
		strcat(Line, T);
	}
	Logger::Log(Line);

	// EBP-chain backtraces stop at the first FPO-compiled engine frame, so also dump every stack
	// value that lands in Oblivion.exe's code range - a superset of the true call chain.
	UInt32* Stack = (UInt32*)&Tag;
	int Hits = 0;
	strcpy(Line, "[GrassOrderDbg]   scan:");
	for (int i = 0; i < 768 && Hits < 64; i++) {
		UInt32 V = Stack[i];
		if (V >= 0x00401000 && V < 0x00A00000) {
			char T[16];
			sprintf(T, " %08X", V);
			strcat(Line, T);
			Hits++;
			if (strlen(Line) > 460) {
				Logger::Log(Line);
				strcpy(Line, "[GrassOrderDbg]   scan:");
			}
		}
	}
	Logger::Log(Line);

}

UInt32 (__thiscall RenderHook::* SetupShaderPrograms)(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, UInt32);
UInt32 (__thiscall RenderHook::* TrackSetupShaderPrograms)(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, UInt32);
UInt32 RenderHook::TrackSetupShaderPrograms(NiGeometry* Geometry, NiSkinInstance* SkinInstance, NiSkinPartition::Partition* SkinPartition, NiGeometryBufferData* GeometryBufferData, NiPropertyState* PropertyState, NiDynamicEffectState* EffectState, NiTransform* WorldTransform, UInt32 WorldBound) {

	D3DMATRIX* Proj = &TheRenderManager->projMatrix;

	if (!TheShaderManager->jitterSet) {
		D3DMATRIX ProjV = TheRenderManager->projMatrix;		
		TheShaderManager->jitterProjectionX = Proj->_31 + TheShaderManager->ShaderConst.Jitter.x;
		TheShaderManager->jitterProjectionY = Proj->_32 + TheShaderManager->ShaderConst.Jitter.y;
		TheShaderManager->jitterSet = true;
	}

	Proj->_31 = TheShaderManager->jitterProjectionX;
	Proj->_32 = TheShaderManager->jitterProjectionY;

	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	NiD3DPass* Pass = ((NiD3DShader*)this)->CurrentPass;
	NiD3DVertexShaderEx* VertexShader = (NiD3DVertexShaderEx*)Pass->VertexShader;
	NiD3DPixelShaderEx* PixelShader = (NiD3DPixelShaderEx*)Pass->PixelShader;
	NiNode* RenderWindowRootNode = *RenderWindowNode;

	// Sample the engine's own projMatrix at the first draw of each pass, before we stamp it. This is
	// the spec's open question: if these already match the pass frustum, the engine rebuilds
	// projMatrix per pass and StampPassProjection is redundant.
	if (RenderManager::ShellActive && TheSettingManager->SettingsMain.Develop.NearShellDebug) {
		if (RenderManager::CurrentPass == RenderManager::PassFar && !FarProjSeen) {
			FarProj33Seen = Proj->_33;
			FarProjSeen = true;
		}
		else if (RenderManager::CurrentPass == RenderManager::PassNear && !ShellProjSeen) {
			ShellProj33Seen = Proj->_33;
			ShellProjSeen = true;
		}
	}

	// Between passes projMatrix belongs to whoever set it up; StampPassProjection is inert unless we
	// are inside one of the two shell passes.
	RenderManager::StampPassProjection(Proj);
	if (RenderManager::CurrentPass == RenderManager::PassNear) {

		RenderManager::ShellDraws++;

		// [ShellDraw] Evidence capture for the shell-pass defects: what is drawn in the shell and what
		// colour-write/Z state does it see on entry - i.e. the state left behind by the PREVIOUS draw.
		// Cap raised from 40 to 64: the previous capture logged 40 of 44 shell draws, which left open
		// whether the WATERHMAP* height-map pre-pass is re-issued inside the shell. 64 covers the
		// whole stream with headroom.
		if (TheSettingManager->SettingsMain.Develop.NearShellDebug && ShellDrawLogCount < 64) {
			ShellDrawLogCount++;
			float Dist = 0.0f;
			if (WorldTransform) {
				NiPoint3& CamPos = WorldSceneGraph->camera->m_worldTransform.pos;
				float dx = WorldTransform->pos.x - CamPos.x;
				float dy = WorldTransform->pos.y - CamPos.y;
				float dz = WorldTransform->pos.z - CamPos.z;
				Dist = sqrtf(dx * dx + dy * dy + dz * dz);
			}
			// pin=1 when the vertex shader is one of the z==w sky/cloud/sun shaders (SKY*.vso) that the
			// depth-clear fix (ShellClearDepth) now rejects; label only, not used for any decision.
			bool Pin = VertexShader && VertexShader->ShaderName && !memcmp(VertexShader->ShaderName, "SKY", 3);
			Logger::Log("[ShellDraw] %04d VS=%s PS=%s Geo=%s pin=%d cweIn=%d dist=%.1f ZEnable=%d ZWrite=%d ZFunc=%d",
				ShellDrawLogCount,
				VertexShader && VertexShader->ShaderName ? VertexShader->ShaderName : "-",
				PixelShader && PixelShader->ShaderName ? PixelShader->ShaderName : "-",
				Geometry && Geometry->m_pcName ? Geometry->m_pcName : "-",
				Pin ? 1 : 0,
				RenderState->GetRenderState(D3DRS_COLORWRITEENABLE),
				Dist,
				RenderState->GetRenderState(D3DRS_ZENABLE),
				RenderState->GetRenderState(D3DRS_ZWRITEENABLE),
				RenderState->GetRenderState(D3DRS_ZFUNC));
		}
	}
	else if (RenderManager::CurrentPass == RenderManager::PassFar) {
		// [ShellWater] Same shape as [ShellDraw], for the far pass, gated to water draws only. Paired
		// with [ShellDraw] this answers whether a given near-water draw appears in the far pass, the
		// shell pass, both, or neither.
		if (TheSettingManager->SettingsMain.Develop.NearShellDebug && FarWaterLogCount < 12 &&
			PixelShader && PixelShader->ShaderName && !memcmp(PixelShader->ShaderName, "WATER", 5)) {
			FarWaterLogCount++;
			float Dist = 0.0f;
			if (WorldTransform) {
				NiPoint3& CamPos = WorldSceneGraph->camera->m_worldTransform.pos;
				float dx = WorldTransform->pos.x - CamPos.x;
				float dy = WorldTransform->pos.y - CamPos.y;
				float dz = WorldTransform->pos.z - CamPos.z;
				Dist = sqrtf(dx * dx + dy * dy + dz * dz);
			}
			bool Pin = VertexShader && VertexShader->ShaderName && !memcmp(VertexShader->ShaderName, "SKY", 3);
			Logger::Log("[ShellWater] %04d VS=%s PS=%s Geo=%s pin=%d cweIn=%d dist=%.1f ZEnable=%d ZWrite=%d ZFunc=%d",
				FarWaterLogCount,
				VertexShader && VertexShader->ShaderName ? VertexShader->ShaderName : "-",
				PixelShader && PixelShader->ShaderName ? PixelShader->ShaderName : "-",
				Geometry && Geometry->m_pcName ? Geometry->m_pcName : "-",
				Pin ? 1 : 0,
				RenderState->GetRenderState(D3DRS_COLORWRITEENABLE),
				Dist,
				RenderState->GetRenderState(D3DRS_ZENABLE),
				RenderState->GetRenderState(D3DRS_ZWRITEENABLE),
				RenderState->GetRenderState(D3DRS_ZFUNC));
		}
	}

	if (VertexShader && PixelShader) {
		if (GrassOrderCapture) { // [GrassOrderDbg]
			void* Ra = _ReturnAddress();
			GrassOrderSeq++;
			Logger::Log("[GrassOrderDbg] %04d ra=%08X pw=%d VS=%s PS=%s Geo=%s pos=(%.0f %.0f %.0f) s=%.1f", GrassOrderSeq, (UInt32)Ra,
				TheShaderManager->PreWaterDepthBufferFilled ? 1 : 0,
				VertexShader->ShaderName ? VertexShader->ShaderName : "-",
				PixelShader->ShaderName ? PixelShader->ShaderName : "-",
				Geometry && Geometry->m_pcName ? Geometry->m_pcName : "-",
				WorldTransform ? WorldTransform->pos.x : 0.0f,
				WorldTransform ? WorldTransform->pos.y : 0.0f,
				WorldTransform ? WorldTransform->pos.z : 0.0f,
				WorldTransform ? WorldTransform->scale : 0.0f);
			if (VertexShader->isGrass && GrassOrderGrassTraces < 4) {
				GrassOrderGrassTraces++;
				GrassOrderLogTrace("GRASS", Ra, PropertyState);
			}
			if (PixelShader->ShaderName && !memcmp(PixelShader->ShaderName, "WATER", 5) && GrassOrderWaterTraces < 16) {
				GrassOrderWaterTraces++;
				GrassOrderLogTrace("WATER", Ra, PropertyState);
			}
		}
		if (PixelShader->ShaderProg && Pass->Stages.numObjs && Pass->Stages.data[0]->Texture) {
			TheShaderManager->ShaderConst.TextureData.x = Pass->Stages.data[0]->Texture->GetWidth();
			TheShaderManager->ShaderConst.TextureData.y = Pass->Stages.data[0]->Texture->GetHeight();
		}

		if (VertexShader->ShaderProg && TheRenderManager->renderState->GetVertexShader() != VertexShader->ShaderHandle) {
			VertexShader->ShaderProg->SetCT();
		}
		if (VertexShader->ShaderProg && VertexShader->isSun) {
			if (!memcmp(Geometry->m_pcName, "Sun Ge", 6)) {
				TheShaderManager->ShaderConst.Geometry.Toggles.x = TheSettingManager->SettingsSpecular.SunPower;
			}
			else {
				TheShaderManager->ShaderConst.Geometry.Toggles.x = 1.0f;
			}
			VertexShader->ShaderProg->SetPerGeomCT();
		}

		if (VertexShader->ShaderProg && VertexShader->isEyePosition) {
			NiPoint3 eye = Geometry->GetEye(&WorldSceneGraph->camera->m_worldTransform.pos);
			eye = Geometry->m_worldTransform.rot < eye;
			D3DXVECTOR4 e = D3DXVECTOR4(eye.x, eye.y, eye.z, 1.0f);
			TheShaderManager->ShaderConst.Specular.EyePosition = e;
			VertexShader->ShaderProg->SetPerGeomCT();
		}

		if (PixelShader->ShaderProg && PixelShader->isSkin) { //TODO this condition isnt quite right for all tree situations but it just so happens to work
			TheShaderManager->ShaderConst.Geometry.Toggles.x = VertexShader->isSkin ? 1.0f : 0.0f;
			TheShaderManager->ShaderConst.Geometry.Toggles.y = VertexShader->isTree ? 1.0f : 0.0f;
			PixelShader->ShaderProg->SetPerGeomCT();
		}

		if (PixelShader->isRefraction) {
			RenderState->SetRenderState(D3DRS_ZWRITEENABLE, FALSE, 0);
		}

		// Sun-shadow apply + pre-water depth, fired at the first NEAR-water surface draw of the main
		// pass. Engine pass order is: opaque -> LOD water -> sky -> LOD terrain -> grass -> NEAR water
		// ([GrassOrderDbg] captures, 2026-07-15), so at this moment everything that should receive
		// shadows (land, grass, submerged floor) is in the color and depth buffers, and the near water
		// surface then composites OVER the shadowed scene. Resolve the depth-stencil into
		// DepthTexturePreWater (the apply's receiver depth) and render the darkening quad.
		// Near water is distinguished from the earlier LOD water planes by the pixel-shader NUMBER:
		// the pre-sky LOD group always binds WATER012+, the post-grass near group always WATER000-011
		// (confirmed by [GrassOrderDbg] captures incl. the close-camera water mode, where the near
		// surface switches to WATER007 and its NiAlphaProperty flips to opaque — so alpha flags are
		// NOT a usable discriminator). Match only the numbered water SURFACE shaders; exclude the
		// height-map pre-pass shaders (WATERHMAP*, 'H' at index 5).
		// InMainScenePass is REQUIRED, not just the PreWaterDepthBufferFilled latch: the water
		// REFLECTION map renders AFTER the main pass, outside RenderObject(WorldSceneGraph), and the
		// game's BeginScene for that off-screen render resets PreWaterDepthBufferFilled
		// (ShaderManager::BeginScene). Numbered water shaders DO bind during the reflection render
		// ([ReflDbg] log, 2026-07-17), so without this guard the apply fired again INTO the
		// reflection map — camera-tracking caster silhouettes floating in the water.
		if (InMainScenePass && !TheShaderManager->PreWaterDepthBufferFilled && !memcmp(PixelShader->ShaderName, "WATER", 5) && PixelShader->ShaderName[5] >= '0' && PixelShader->ShaderName[5] <= '9') {
			if (atoi(PixelShader->ShaderName + 5) < 12) {
				TheRenderManager->ResolvePreWaterDepthBuffer();
				TheShaderManager->RenderShadowsMidScene();
				TheShaderManager->PreWaterDepthBufferFilled = true;
			}
		}

		// Shell counterpart of the capture above, and the one point in the frame where the shell's own
		// water draws can be prepared. Two things are fixed here; both need the same instant.
		//
		// (1) TESR_RenderedBuffer. Water refracts the scene BEHIND it by sampling
		// TESR_RenderedBuffer (WATER*.pso: the centre tap plus the +0.01 UV refraction tap), so that
		// texture has to hold the frame as it stands immediately before the water surface goes down.
		// In the far pass it does: the ShaderRecord::SetCT latch fills it at the first water bind and
		// RenderShadowsMidScene's blit (just above) refreshes it right before the first NEAR water.
		// Neither happens again in the shell - the latch is closed for the rest of the frame
		// (RenderedBufferFilled, forced true at the end of the far pass) and the mid-scene block is
		// disarmed by InMainScenePass - so shell water refracts a buffer captured during the FAR pass,
		// which by construction cannot contain anything nearer than M: the far pass's near plane
		// clipped it away. Treading water, the player's own arms are inside M and are therefore drawn
		// ONLY in the shell ([ShellDraw] 0004-0013 and 0033-0037), while the water surface covering
		// their submerged half is also inside M and drawn only in the shell (0040, WATER007) - after
		// them. That water then overwrote the arms with a refraction of a buffer they were not in, so
		// the submerged part of the arms vanished while the fists above the waterline, which no water
		// covers, stayed correct.
		//
		// Take the shell's own capture here, at its first NEAR water draw. That is the same draw the
		// far pass's mid-scene blit fires on, so the capture lands at exactly the same point in the
		// engine's draw order: after the shell's opaque geometry, its EQUAL-depth detail passes and
		// its grass, and before any near water. Same WATER + digit + number < 12 discriminator as the
		// block above (LOD water binds WATER012+, near water WATER000-011); LOD water is deliberately
		// not a trigger, as it draws before the detail and grass passes and every one of its quads is
		// tens of thousands of units away, hence clipped by the shell's far plane at M.
		//
		// (2) TESR_DepthBufferPreWater, which the swap block below binds as those same water draws'
		// DEPTH term. It is resolved during the far pass too, so at shell-covered pixels it holds what
		// the far pass drew behind them - the lake bottom, treading water - and the arms that (1)
		// restores then shade as if they were lying on it. Clamp the shell's coverage to depth 0,
		// which decodes to exactly z = M: the arms shade as ~M units under water instead of ~200.
		// See ShaderManager::FlattenShellPreWaterDepth for why the exact value is unreachable without
		// moving the decode range, and why the clamp has to land HERE rather than after the shell like
		// the TESR_DepthBuffer flatten: the shell's water draws read the texture while the shell is
		// still running, and this is the last moment at which the shell's depth-stencil holds all of
		// its geometry and none of its water.
		//
		// Once per shell and only when the shell actually contains near water, so the added cost is
		// one full-resolution StretchRect, one depth resolve and one full-screen quad in exactly the
		// frames that exhibit the defects and nothing at all otherwise. PassNear implies ShellActive,
		// so this is inert with the near shell off and the far pass is left byte-identical.
		if (RenderManager::CurrentPass == RenderManager::PassNear && !ShellNearWaterPrepDone &&
			PixelShader->ShaderName && !memcmp(PixelShader->ShaderName, "WATER", 5) &&
			PixelShader->ShaderName[5] >= '0' && PixelShader->ShaderName[5] <= '9' && atoi(PixelShader->ShaderName + 5) < 12) {
			ShellNearWaterPrepDone = true;
			if (TheRenderManager->currentRTGroup && TheShaderManager->RenderedSurface) {
				TheRenderManager->device->StretchRect(TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, NULL, TheShaderManager->RenderedSurface, NULL, D3DTEXF_NONE);
				// Close the SetCT colour latch too: this capture is the valid one from here on. It is
				// normally already closed by the far pass, but the far pass need not have contained a
				// single HasRB draw (an interior water feature wholly within M), and a later latch fire
				// would re-capture at some arbitrary point after the water is already down.
				TheShaderManager->RenderedBufferFilled = true;
			}
			// After the blit, so the blit reads the scene render target while it is still bound. The
			// clamp restores every target and state it touches before it returns, so the water draw
			// this hook is setting up is unaffected.
			TheShaderManager->FlattenShellPreWaterDepth();
		}

		if (PixelShader->ShaderProg && TheRenderManager->renderState->GetPixelShader() != PixelShader->ShaderHandle) PixelShader->ShaderProg->SetCT();

		// Water must never sample a depth buffer that contains water. TESR_DepthBuffer is the far
		// pass's END-of-pass resolve, so during the shell it carries the far pass's own near-water
		// SURFACES - with a hole punched in them below the band boundary, where that surface was
		// clipped away by the far pass's near plane at M. WATER*.pso reconstructs world positions
		// from readDepth() at OFFSET texture coordinates (refract at +0.01 UV, reflect at +0.05), so
		// for pixels within that offset of the boundary the sample lands on the far pass's water
		// surface instead of the lake bottom: the reconstructed point sits at water level, the
		// extinction and volume-colour terms (both keyed on refract_uw_pos) collapse to nothing, and
		// the water renders as if it were not there. The offset is a SCREEN-space constant, which is
		// why the strip measured the same pixel height at M = 8, 15 and 30 - the observation that
		// ruled out every geometric explanation.
		//
		// TESR_DepthBufferPreWater is resolved during the far pass BEFORE any near water is drawn
		// (the first near-water bind above, or the end-of-main-pass fallback in TrackRenderObject),
		// so it holds the scene behind the water with no surface in it and no cliff at M. It is
		// resolved from the same far-pass depth-stencil, so it carries the same (M, F) encoding as
		// TESR_DepthBuffer and the fa7f347 decode through TESR_DepthProjectionTransform stays
		// correct unchanged. It is always this frame's content by the time the shell runs, because
		// the fallback fills it before the shell starts.
		//
		// Per-draw rather than inside SetCT: SetCT only runs when the pixel shader handle changes,
		// and the shell can re-issue the same water shader the far pass ended with.
		// PassNear implies ShellActive, so this is inert with the near shell off, and the far pass
		// is left byte-identical.
		if (RenderManager::CurrentPass == RenderManager::PassNear && PixelShader->ShaderProg &&
			TheRenderManager->DepthTexturePreWater && !memcmp(PixelShader->ShaderName, "WATER", 5)) {
			ShaderValue* Values = PixelShader->ShaderProg->TextureShaderValues;
			for (UInt32 c = 0; c < PixelShader->ShaderProg->TextureShaderValuesCount; c++) {
				if (Values[c].Texture && Values[c].Texture->Texture == TheRenderManager->DepthTexture) {
					TheRenderManager->device->SetTexture(Values[c].RegisterIndex, TheRenderManager->DepthTexturePreWater);
					ShellWaterDepthSampler = Values[c].RegisterIndex;
				}
			}
		}

		if (RenderWindowRootNode) {
			char Name[256];
			NiNode* Node = (NiNode*)MemoryAlloc(sizeof(NiNode)); Node->New(2);			
			sprintf(Name, "Pass %s (%s %s)", Geometry->m_pcName, VertexShader->ShaderName, PixelShader->ShaderName);
			if (!VertexShader->ShaderProg) strcat(Name, " - Vertex: vanilla");
			if (!PixelShader->ShaderProg) strcat(Name, " - Pixel: vanilla");
			if ((!VertexShader->ShaderProg && PixelShader->ShaderProg) || (VertexShader->ShaderProg && !PixelShader->ShaderProg)) {
				strcat(Name, "----------------------------SHADER MISMATCH HERE");
			}
			Node->SetName(Name);
			Node->m_children.Add((NiAVObject**)&Geometry->m_parent); // We do not use the AddObject to avoid to alter the original object
			Node->m_children.Add((NiAVObject**)&Geometry); // Same as above
			RenderWindowRootNode->AddObject(Node, 1);
		}
		if (TheSettingManager->SettingsMain.Develop.LogShaders && TheKeyboardManager->OnKeyPressed(TheSettingManager->SettingsMain.Develop.LogShaders)) {
			Logger::Log("Pass %s (%s %s)", Geometry->m_pcName, VertexShader->ShaderName, PixelShader->ShaderName);
		}
	}
	else {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_BigFour) && TheShaderManager->LocationState == CellLocation::Exterior) {
			RenderState->SetRenderState(D3DRS_ZWRITEENABLE, FALSE, 0);
		}
	}

	UInt32 result = (this->*SetupShaderPrograms)(Geometry, SkinInstance, SkinPartition, GeometryBufferData, PropertyState, EffectState, WorldTransform, WorldBound);

	if (VertexShader && VertexShader->ShaderProg && VertexShader->isGrass) {
		float cx = WorldTransform->pos.x;
		float cy = WorldTransform->pos.y;
		TheRenderManager->device->SetVertexShaderConstantF(253, (const float*)&TheShaderManager->ShaderConst.Grass.CollisionParams, 1);
		D3DXVECTOR4 xy0(
			TheShaderManager->GrassCollisionActors[0].x - cx,
			TheShaderManager->GrassCollisionActors[0].y - cy,
			TheShaderManager->GrassCollisionActors[1].x - cx,
			TheShaderManager->GrassCollisionActors[1].y - cy);
		D3DXVECTOR4 xy1(
			TheShaderManager->GrassCollisionActors[2].x - cx,
			TheShaderManager->GrassCollisionActors[2].y - cy,
			TheShaderManager->GrassCollisionActors[3].x - cx,
			TheShaderManager->GrassCollisionActors[3].y - cy);
		TheRenderManager->device->SetVertexShaderConstantF(254, (const float*)&xy0, 1);
		TheRenderManager->device->SetVertexShaderConstantF(255, (const float*)&xy1, 1);

	}

	return result;
}

HRESULT (__thiscall RenderHook::* SetSamplerState)(UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8);
HRESULT (__thiscall RenderHook::* TrackSetSamplerState)(UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8);
HRESULT RenderHook::TrackSetSamplerState(UInt32 Sampler, D3DSAMPLERSTATETYPE Type, UInt32 Value, UInt8 Save) {

	if (TheSettingManager->SettingsMain.Main.AnisotropicFilter >= 2) {
		if (Type == D3DSAMP_MAGFILTER) {
			if (Value != D3DTEXF_NONE && Value != D3DTEXF_POINT) Value = D3DTEXF_LINEAR;
		}
		if (Type == D3DSAMP_MINFILTER) {
			if (Value != D3DTEXF_NONE && Value != D3DTEXF_POINT) Value = D3DTEXF_ANISOTROPIC;
		}
		if ((Type == D3DSAMP_MIPFILTER) && ((Value == D3DTEXF_POINT) || (Value == D3DTEXF_LINEAR))) {
			Value = D3DTEXF_LINEAR;
		}
	}
	return (this->*SetSamplerState)(Sampler, Type, Value, Save);

}

NiPixelData* (__cdecl * SaveGameScreenshot)(int*, int*) = (NiPixelData* (__cdecl *)(int*, int*))0x00411B70;
NiPixelData* __cdecl TrackSaveGameScreenshot(int* pWidth, int* pHeight) {
	
	NiPixelData* r = NULL;
	
	TheRenderManager->IsSaveGameScreenShot = 1;
	r = SaveGameScreenshot(pWidth, pHeight);
	TheRenderManager->IsSaveGameScreenShot = 0;
	return r;

}

void (__cdecl * SetShaderPackage)(int, int, UInt8, int, char*, int) = (void (__cdecl *)(int, int, UInt8, int, char*, int))0x007B45F0;
void __cdecl TrackSetShaderPackage(int Arg1, int Arg2, UInt8 Force1XShaders, int Arg4, char* GraphicsName, int Arg6) {
	
	UInt32* ShaderPackage = (UInt32*)0x00B42F48;
	UInt32* ShaderPackageMax = (UInt32*)0x00B42D74;

	SetShaderPackage(Arg1, Arg2, Force1XShaders, Arg4, GraphicsName, Arg6);
	
	*ShaderPackage = 7;
	*ShaderPackageMax = 7;

}

void (__cdecl * RenderObject)(NiCamera*, NiNode*, NiCullingProcess*, NiVisibleArray*) = (void (__cdecl *)(NiCamera*, NiNode*, NiCullingProcess*, NiVisibleArray*))0x0070C0B0;
void __cdecl TrackRenderObject(NiCamera* Camera, NiNode* Object, NiCullingProcess* CullingProcess, NiVisibleArray* VisibleArray) {

	// Main-pass detection, hardened: the observed main-scene render ([ReflDbg] capture, 2026-07-17)
	// always arrives with Camera == WorldSceneGraph->camera and 0xB42E86 clear. The camera check and
	// engine reflection flag guard against other renders of the same root (the game CAN render
	// WorldSceneGraph off-screen — e.g. the water function at 0x4D040B/0x4D04A8 passes its own
	// camera, and only its first call sets 0xB42E86). Note the 1024x1024 water-reflection map seen
	// in captures renders AFTER the main pass without hitting this hook at all — its protection is
	// the InMainScenePass gate on the water-bind trigger, not these checks.
	bool MainScenePass = (Object == WorldSceneGraph) && (Camera == WorldSceneGraph->camera) && !*kIsRenderingWaterReflections;
	if (MainScenePass) {
		InMainScenePass = true;
		TheShaderManager->PreWaterDepthBufferFilled = false; // reset before the main pass so only main-pass water binds populate the pre-water depth
		if (TheSettingManager->SettingsMain.Develop.LogShaders && TheKeyboardManager->OnKeyDown(TheSettingManager->SettingsMain.Develop.LogShaders)) { // [GrassOrderDbg]
			GrassOrderCapture = true;
			GrassOrderSeq = GrassOrderGrassTraces = GrassOrderWaterTraces = 0;
			Logger::Log("[GrassOrderDbg] ==== capture start (WorldSceneGraph render) ====");
		}
	}
	if (MainScenePass && RenderManager::ShellActive) {
		FarProjSeen = ShellProjSeen = false;
		ShellDrawLogCount = FarWaterLogCount = 0;
		RenderManager::ApplyPass(RenderManager::PassFar);
	}
	RenderObject(Camera, Object, CullingProcess, VisibleArray);
	// Load-bearing HERE, before the shell render below - not just bookkeeping. It disarms the
	// near-water mid-scene trigger (line ~510), which would otherwise re-fire on the shell's copy of
	// the water surface and run a second shadow apply over an already-applied frame. Do not move
	// this after the shell block.
	if (MainScenePass) InMainScenePass = false;
	if (Object == WorldSceneGraph && GrassOrderCapture) { // [GrassOrderDbg]
		GrassOrderCapture = false;
		Logger::Log("[GrassOrderDbg] ==== capture end (%d passes) ====", GrassOrderSeq);
	}
	if (MainScenePass) {
		if (!TheShaderManager->PreWaterDepthBufferFilled) {
			// No near-water draw this frame (the common case in interiors): resolve the receiver depth
			// (== full scene depth) and run the shadow apply now, at the end of the main scene render.
			TheRenderManager->ResolvePreWaterDepthBuffer();
			TheShaderManager->RenderShadowsMidScene();
			TheShaderManager->PreWaterDepthBufferFilled = true;
		}
		// Resolves the FAR pass depth - deliberately before the clear, because the clear destroys it.
		// What post-processing therefore gets is the [M, F] band only: shell pixels carry the depth
		// of whatever is behind them, and anything the far pass never covered stays at 1.0. That is
		// the accepted ~20 cm limitation. The buffer is encoded with near = M, which is why
		// SetupSceneCamera republishes the (M, F) depth row at PassFull.
		TheRenderManager->ResolveDepthBuffer();
		// Close the SetCT depth latch (ShaderRecord::SetCT) for the rest of the frame. It resets once
		// per BeginScene and fires on the first HasDB shader bind; if no such shader bound during the
		// far pass but one binds inside the shell - a water body wholly within M, an interior water
		// feature - it would resolve AFTER the clear and hand post-processing a blank depth buffer.
		// This resolve is the only valid one for this frame.
		TheShaderManager->DepthBufferFilled = true;

		if (RenderManager::ShellActive) {
			UInt8 Debug = TheSettingManager->SettingsMain.Develop.NearShellDebug;

			// Debug 2 skips the shell, rendering it as a hole - the quickest way to see what it holds.
			if (Debug != 2) {
				// Arm the shell's own near-water preparation - its TESR_RenderedBuffer capture and its
				// TESR_DepthBufferPreWater clamp (TrackSetupShaderPrograms). Here rather than at
				// far-pass entry, so it can only ever be armed for a shell that runs.
				ShellNearWaterPrepDone = false;
				RenderManager::ApplyPass(RenderManager::PassNear);
				// Clear to one ULP below 1.0, not 1.0. Sky, cloud and sun shaders pin z == w
				// (SKYCLOUDS.vso, SKYT.vso), landing at depth exactly 1.0 (D24 0xFFFFFF); against a
				// 1.0 clear their LESSEQUAL test passes and they paint over the entire far pass.
				// 0xFFFFFE rejects them with no name matching, and catches ANY z == w shader rather
				// than a hardcoded list. Cost to real geometry: depth in the shell is
				// d(z) = M/(M-n) * (1 - n/z), so d = 0.99999994 is z ~ 14.99999 at n=1, M=15 - the
				// shell loses its final 0.00001 units, far below the depth buffer's resolution there.
				// The engine's Clear() gives no control of the clear value, hence the device call.
				TheRenderManager->device->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, RenderManager::ShellClearDepth, 0);
				RenderObject(Camera, Object, CullingProcess, VisibleArray);
				// Shell water draws leave TESR_DepthBufferPreWater bound to the sampler that
				// TESR_DepthBuffer normally occupies (TrackSetupShaderPrograms). Put it back before
				// anything else can inherit that binding - notably the water REFLECTION render, which
				// runs after the main pass with its own camera and re-binds water shaders, and where
				// SetCT does not necessarily re-run.
				if (ShellWaterDepthSampler != ShellWaterDepthSamplerNone) {
					TheRenderManager->device->SetTexture(ShellWaterDepthSampler, TheRenderManager->DepthTexture);
					ShellWaterDepthSampler = ShellWaterDepthSamplerNone;
				}
				// Flatten TESR_DepthBuffer to "exactly at M" wherever the shell drew. HERE and nowhere
				// earlier: water pixel shaders sample TESR_DepthBuffer DURING the shell and need the
				// untouched far-pass resolve to compute water depth (commit fa7f347) - flattening before
				// the last shell draw would undo that fix and make near water shallow again. And it must
				// happen before TrackProcessImageSpaceShaders, which is where the ~16 image-space effects
				// that sample the buffer actually run. Nothing between the two touches the texture: the
				// SetCT resolve latch is closed (DepthBufferFilled, set above) and the first-person node
				// branch below skips its own resolve while the shell is active.
				TheShaderManager->FlattenShellDepth();
			}

			RenderManager::ApplyPass(RenderManager::PassFull);

			if (Debug) {
				// engineFar/engineShell are projMatrix._33 as the engine left it at each pass's first
				// draw. wantFar/wantShell are what that pass needs; full is the un-rebuilt (n, F) row.
				// engine == want  => the engine rebuilds per pass, StampPassProjection is redundant.
				// engine == full  => it does not, and the stamp is load-bearing.
				Logger::Log("[NearShell] n=%.3f M=%.3f F=%.1f shellDraws=%d skipShell=%d | engineFar=%.7f wantFar=%.7f full=%.7f | engineShell=%.7f wantShell=%.7f",
					RenderManager::RealNear, RenderManager::ShellBoundary, RenderManager::RealFar,
					RenderManager::ShellDraws, Debug == 2 ? 1 : 0,
					FarProj33Seen,
					RenderManager::RealFar / (RenderManager::RealFar - RenderManager::ShellBoundary),
					RenderManager::RealFar / (RenderManager::RealFar - RenderManager::RealNear),
					ShellProj33Seen,
					RenderManager::ShellBoundary / (RenderManager::ShellBoundary - RenderManager::RealNear));
			}
		}
	}
	else if (Object == Player->firstPersonNiNode) {
		// This is a separate, LATER top-level call: the main pass has already resolved and then
		// cleared depth, and the depth buffer now holds only the shell (1.0 almost everywhere at
		// M = 15). Re-resolving it would overwrite the good far-pass depth with a blank one and blind
		// every image-space effect in first person. Before the shell existed this was a harmless
		// duplicate of the main-pass resolve; the clear is what made it destructive.
		if (!RenderManager::ShellActive) TheRenderManager->ResolveDepthBuffer();
		TheRenderManager->Clear(NULL, NiRenderer::kClear_ZBUFFER);
		RenderObject(Camera, Object, CullingProcess, VisibleArray);
	}

}

#elif defined(SKYRIM)
void (__thiscall RenderHook::* Render)(BSRenderedTexture*, int, int);
void (__thiscall RenderHook::* TrackRender)(BSRenderedTexture*, int, int);
void RenderHook::TrackRender(BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	
	TheRenderManager->SetSceneGraph();
	TheShaderManager->UpdateConstants();
	if (TheSettingManager->SettingsMain.Develop.TraceShaders) Logger::Log("RENDERING...");
	(this->*Render)(RenderedTexture, Arg2, Arg3);

}

bool (__thiscall RenderHook::* SetupRenderingPass)(UInt32, UInt32);
bool (__thiscall RenderHook::* TrackSetupRenderingPass)(UInt32, UInt32);
bool RenderHook::TrackSetupRenderingPass(UInt32 Arg1, UInt32 Arg2) {

	bool r = (this->*SetupRenderingPass)(Arg1, Arg2);
	NiD3DVertexShaderEx* VertexShader = *(NiD3DVertexShaderEx**)0x01BABFB4;
	NiD3DPixelShaderEx* PixelShader = *(NiD3DPixelShaderEx**)0x01BABFB0;
	
	if (VertexShader) {
		if (VertexShader->ShaderProg) VertexShader->ShaderProg->SetCT();
		if (TheSettingManager->SettingsMain.Develop.TraceShaders) Logger::Log("SetVertexShader: %s", VertexShader->ShaderName);
	}
	if (PixelShader) {
		if (PixelShader->ShaderProg) PixelShader->ShaderProg->SetCT();
		if (TheSettingManager->SettingsMain.Develop.TraceShaders) Logger::Log("SetPixelShader: %s", PixelShader->ShaderName);
	}
	return r;

}

void (__thiscall RenderHook::* RenderWorldSceneGraph)(Sun*, UInt8, UInt8);
void (__thiscall RenderHook::* TrackRenderWorldSceneGraph)(Sun*, UInt8, UInt8);
void RenderHook::TrackRenderWorldSceneGraph(Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame) {

	(this->*RenderWorldSceneGraph)(SkySun, IsFirstPerson, WireFrame);
	if (!IsFirstPerson) TheRenderManager->ResolveDepthBuffer();

}

static const UInt32 NiDX9RendererClear = 0x00CD5D00;
static __declspec(naked) void ClearDepth()
{
	__asm
	{
		push	ClearMode
		push	0
		call	NiDX9RendererClear
		jmp		kClearDepthReturn
	}
}

static const UInt32 RenderFirstPersonShadow = 0x00695740;
static __declspec(naked) void RenderFirstPersonNode()
{
	__asm
	{
		cmp		ClearMode, 0
		jnz		loc_jumpout
		mov		ClearMode, 4
		pushad
		mov		ecx, TheRenderManager
		call	RenderManager::ResolveDepthBuffer
		popad
		jmp		kRenderFirstPersonNodeRepeat

	loc_jumpout:
		call	RenderFirstPersonShadow
		mov		ClearMode, 0
		jmp		kRenderFirstPersonNodeReturn
	}
}

void SetShadowDistanceValue(float* Distance, UInt32 Pass) {

	if (Pass == 0) *Distance /= TheSettingManager->SettingsMain.ShadowMode.NearQuality;

}

static __declspec(naked) void SetShadowDistance()
{
	__asm
	{
		faddp   st(1), st
		fstp	dword ptr [esp + 0x4C]
		lea		ecx, [esp + 0x4C]
		mov		edx, [esp + 0x74]
		pushad
		pushfd
		push	edx
		push	ecx
		call	SetShadowDistanceValue
		add		esp, 8
		popfd
		popad
		jmp		kSetShadowDistanceReturn
	}
}

static __declspec(naked) void SetShadowDistanceShader()
{
	__asm
	{
		lea		ecx, [esp + 0xE0 - 0xC4 + 4]
		pushad
		pushfd
		push	0
		push	ecx
		call	SetShadowDistanceValue
		add		esp, 8
		popfd
		popad
		mov		ecx, [esp + 0xE0 - 0xC4 + 4]
		mov		[esp + esi * 4 + 0xE0 - 0x98], ecx
		jmp		kSetShadowDistanceShaderReturn
	}
}

static __declspec(naked) void FixSunFlags()
{
	static int max = 0;

	__asm
	{
		or		eax, 0x4002
		add		eax, 0x2B
		push	eax
		mov		eax, [esp + 0x90 - 0x74]
		mov		max, eax
		mov		eax, 0x1BA7680
		mov		eax, [eax]
		mov		eax, [eax + 0x138]
		cmp		max, eax
		je		loc_fix
		jmp		kFixSunFlagsReturn

	loc_fix:
		pop		eax
		mov		eax, 0x0040402D
		push	eax
		jmp		kFixSunFlagsReturn
	}
}

#endif

void (__cdecl * ProcessImageSpaceShaders)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*) = (void (__cdecl *)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*))kProcessImageSpaceShaders;
void __cdecl TrackProcessImageSpaceShaders(NiDX9Renderer* Renderer, BSRenderedTexture* RenderedTexture1, BSRenderedTexture* RenderedTexture2) {
	
	BSRenderedTexture* MenuRenderedTexture = *(BSRenderedTexture**)kMenuRenderedTexture;

	if (TheRenderManager->BackBuffer) TheRenderManager->defaultRTGroup->RenderTargets[0]->data->Surface = TheRenderManager->BackBuffer;
	if ((!RenderedTexture2 || MenuRenderedTexture) && TheRenderManager->currentRTGroup) {
		IDirect3DDevice9* Device = TheRenderManager->device;
		if (TheSettingManager->SettingsMain.Main.RenderEffectsBeforeHdr) {
			TheShaderManager->RenderEffectsPreHdr(RenderedTexture1->RenderedTexture->buffer->data->Surface);
			ProcessImageSpaceShaders(Renderer, RenderedTexture1, RenderedTexture2);
		}
		else {
			ProcessImageSpaceShaders(Renderer, RenderedTexture1, RenderedTexture2);
			TheShaderManager->RenderEffectsPostHdr(TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface);
		}
	}

	if (TheRenderManager->IsSaveGameScreenShot) {
		if (MenuRenderedTexture)
			TheRenderManager->device->StretchRect(MenuRenderedTexture->RenderTargetGroup->RenderTargets[0]->data->Surface, NULL, TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, &TheRenderManager->SaveGameScreenShotRECT, D3DTEXF_NONE);
		else
			TheRenderManager->device->StretchRect(TheRenderManager->defaultRTGroup->RenderTargets[0]->data->Surface, NULL, TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, &TheRenderManager->SaveGameScreenShotRECT, D3DTEXF_NONE);
	}

}

static __declspec(naked) void RenderInterface() {
	
	__asm {
#if defined(NEWVEGAS)
		pushad
		call	RenderMainMenuMovie
		popad
#endif
		call	kRenderInterfaceMethod
		pushad
		mov		ecx, TheGameMenuManager
		call	GameMenuManager::Render
		popad
		jmp		kRenderInterfaceReturn
	}

}

#if defined(OBLIVION)
void DetectorWindowSetNodeName(char* Buffer, char* Format, char* ClassName, char* Name, float LPosX, float LPosY, float LPosZ) {
	
	sprintf(Buffer, "%s", Name);

}
#elif defined(NEWVEGAS)
void DetectorWindowSetNodeName(char* Buffer, int Size, char* Format, char* ClassName, char* Name, float LPosX, float LPosY, float LPosZ) {

	sprintf(Buffer, "%s", Name);

}
#endif

void DetectorWindowCreateTreeView(HWND TreeView) {
	
	HFONT Font = CreateFontA(14, 0, 0, 0, FW_DONTCARE, NULL, NULL, NULL, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
	SendMessageA(TreeView, WM_SETFONT, (WPARAM)Font, TRUE);
	SendMessageA(TreeView, TVM_SETBKCOLOR, NULL, 0x001E1E1E);
	SendMessageA(TreeView, TVM_SETTEXTCOLOR, NULL, 0x00DCDCDC);

}

static __declspec(naked) void DetectorWindowCreateTreeViewHook() {

	__asm {
		pushad
		push	eax
		call	DetectorWindowCreateTreeView
		pop		eax
		popad
#if defined(OBLIVION)
		pop     edi
		mov		[esi + 0x0C], eax
		pop     esi
		add     esp, 0x40
#elif defined(NEWVEGAS)
		mov     ecx, [ebp - 0x48]
		mov		[ecx + 0x0C], eax
		mov     esp, ebp
		pop     ebp
#endif
		jmp		kDetectorWindowCreateTreeViewReturn
	}

}

void DetectorWindowDumpAttributes(HWND TreeView, UInt32 Msg, WPARAM wParam, LPTVINSERTSTRUCTA lParam) {

	TVITEMEXA Item;
	char T[260] = { '\0' };

	Item.pszText = T;
	Item.mask = TVIF_TEXT;
	Item.hItem = (HTREEITEM)SendMessageA(TreeView, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)lParam->hParent);
	Item.cchTextMax = 260;
	SendMessageA(TreeView, TVM_GETITEMA, 0, (LPARAM)&Item);
	if (!memcmp(Item.pszText, "Pass", 4))
		SendMessageA(TreeView, TVM_DELETEITEM, 0, (LPARAM)lParam->hParent);
	else
		if (strlen(Item.pszText)) SendMessageA(TreeView, Msg, wParam, (LPARAM)lParam);

}

static __declspec(naked) void DetectorWindowDumpAttributesHook() {

	__asm {
		call	DetectorWindowDumpAttributes
		add		esp, 16
#if defined(OBLIVION)
		movzx   ecx, word ptr [esi + 0x0A]
#endif
		jmp		kDetectorWindowDumpAttributesReturn
	}

}

void CreateRenderHook() {

	*((int*)&ShowDetectorWindow)			= kShowDetectorWindow;
	TrackShowDetectorWindow					= &RenderHook::TrackShowDetectorWindow;
	*((int*)&Render)						= kRender;
	TrackRender								= &RenderHook::TrackRender;
	*((int*)&BeginScene)					= kBeginScene;
	TrackBeginScene							= &RenderHook::TrackBeginScene;
#if defined(NEWVEGAS)
	*((int*)&RenderWorldSceneGraph)			= 0x00873200;
	TrackRenderWorldSceneGraph				= &RenderHook::TrackRenderWorldSceneGraph;
	*((int*)&RenderFirstPerson)				= 0x00875110;
	TrackRenderFirstPerson					= &RenderHook::TrackRenderFirstPerson;
	*((int*)&GetWaterHeightLOD)				= 0x0045CD80;
	TrackGetWaterHeightLOD					= &RenderHook::TrackGetWaterHeightLOD;
#elif defined(OBLIVION)
	*((int*)&SetupShaderPrograms)			= 0x0077A1F0;
	TrackSetupShaderPrograms				= &RenderHook::TrackSetupShaderPrograms;
	*((int*)&EndTargetGroup)				= 0x007AAA30;
	TrackEndTargetGroup						= &RenderHook::TrackEndTargetGroup;
	*((int*)&HDRRender)						= 0x007BDFC0;
	TrackHDRRender							= &RenderHook::TrackHDRRender;
	*((int*)&FarPlane)						= 0x00410EE0;
	TrackFarPlane							= &RenderHook::TrackFarPlane;
	*((int*)&SetSamplerState)				= 0x0077B610;
	TrackSetSamplerState					= &RenderHook::TrackSetSamplerState;
#elif defined(SKYRIM)
	*((int*)&SetupRenderingPass)			= 0x00CC4E80;
	TrackSetupRenderingPass					= &RenderHook::TrackSetupRenderingPass;
	*((int*)&RenderWorldSceneGraph)			= 0x00692290;
	TrackRenderWorldSceneGraph				= &RenderHook::TrackRenderWorldSceneGraph;
#endif

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)ShowDetectorWindow,			*((PVOID*)&TrackShowDetectorWindow));
	DetourAttach(&(PVOID&)Render,						*((PVOID*)&TrackRender));
	DetourAttach(&(PVOID&)BeginScene,					*((PVOID*)&TrackBeginScene));
	DetourAttach(&(PVOID&)ProcessImageSpaceShaders,				  &TrackProcessImageSpaceShaders);
#if defined(NEWVEGAS)
	DetourAttach(&(PVOID&)RenderWorldSceneGraph,		*((PVOID*)&TrackRenderWorldSceneGraph));
	DetourAttach(&(PVOID&)RenderFirstPerson,			*((PVOID*)&TrackRenderFirstPerson));
	DetourAttach(&(PVOID&)GetWaterHeightLOD,			*((PVOID*)&TrackGetWaterHeightLOD));
	DetourAttach(&(PVOID&)SetupRenderingPass,					  &TrackSetupRenderingPass);
	//DetourAttach(&(PVOID&)SetShaderPackage,						  &TrackSetShaderPackage);
#elif defined(OBLIVION)
	DetourAttach(&(PVOID&)SetupShaderPrograms,			*((PVOID*)&TrackSetupShaderPrograms));
	DetourAttach(&(PVOID&)EndTargetGroup,				*((PVOID*)&TrackEndTargetGroup));
	DetourAttach(&(PVOID&)HDRRender,					*((PVOID*)&TrackHDRRender));
	DetourAttach(&(PVOID&)FarPlane,						*((PVOID*)&TrackFarPlane));
	DetourAttach(&(PVOID&)SetSamplerState,				*((PVOID*)&TrackSetSamplerState));
	DetourAttach(&(PVOID&)SaveGameScreenshot,					  &TrackSaveGameScreenshot);
	//DetourAttach(&(PVOID&)SetShaderPackage,						  &TrackSetShaderPackage);
	DetourAttach(&(PVOID&)RenderObject,							  &TrackRenderObject);
#elif defined(SKYRIM)
	DetourAttach(&(PVOID&)SetupRenderingPass,			*((PVOID*)&TrackSetupRenderingPass));
	DetourAttach(&(PVOID&)RenderWorldSceneGraph,		*((PVOID*)&TrackRenderWorldSceneGraph));
#endif
    DetourTransactionCommit();

	WriteRelJump(kRenderInterface,		        (UInt32)RenderInterface);
	WriteRelCall(kDetectorWindowSetNodeName,    (UInt32)DetectorWindowSetNodeName);
	WriteRelJump(kDetectorWindowCreateTreeView, (UInt32)DetectorWindowCreateTreeViewHook);
	WriteRelJump(kDetectorWindowDumpAttributes, (UInt32)DetectorWindowDumpAttributesHook);
	WriteRelJump(kDetectorWindowScale,			kDetectorWindowScaleReturn); // Avoids to add the scale to the node description in the detector window
#if defined(NEWVEGAS)
	WriteRelJump(0x004E4C3B, 0x004E4C42); // Fixes reflections when cell water height is not like worldspace water height
	WriteRelJump(0x004E4DA4, 0x004E4DAC); // Fixes reflections on distant water
	WriteRelCall(kMultiBoundWaterHeightFix1,	(UInt32)MultiBoundWaterHeightFix);
	WriteRelCall(kMultiBoundWaterHeightFix2,	(UInt32)MultiBoundWaterHeightFix);
	if (TheSettingManager->SettingsMain.Main.ReplaceIntro) WriteRelJump(kSetTileShaderConstants, (UInt32)SetTileShaderConstants);
	SafeWrite8(0x008751C0, 0); // Stops to clear the depth buffer when rendering the 1st person node
#elif defined(OBLIVION)
	SafeWrite32(0x0049BFAF, TheSettingManager->SettingsMain.Main.WaterReflectionMapSize); // Constructor
	SafeWrite32(0x007C1045, TheSettingManager->SettingsMain.Main.WaterReflectionMapSize); // RenderedSurface
	SafeWrite32(0x007C104F, TheSettingManager->SettingsMain.Main.WaterReflectionMapSize); // RenderedSurface
	SafeWrite32(0x007C10F9, TheSettingManager->SettingsMain.Main.WaterReflectionMapSize); // RenderedSurface
	SafeWrite32(0x007C1103, TheSettingManager->SettingsMain.Main.WaterReflectionMapSize); // RenderedSurface
	SafeWrite8(0x00A38280, 0x5A); // Fixes the "purple water bug"
	SafeWrite8(0x0040CE11, 0); // Stops to clear the depth buffer when rendering the 1st person node
	WriteRelJump(0x00553EAC, 0x00553EB2); // Patches the use of Lighting30Shader only for the hair
	//WriteRelJump(0x007D1BC4, 0x007D1BFD); // Patches the use of Lighting30Shader only for the hair
	WriteRelJump(0x007D1BCD, 0x007D1BFD); // Patches the use of Lighting30Shader only for the hair
	if (TheSettingManager->SettingsMain.Main.AnisotropicFilter >= 2) {
		SafeWrite8(0x007BE1D3, TheSettingManager->SettingsMain.Main.AnisotropicFilter);
		SafeWrite8(0x007BE32B, TheSettingManager->SettingsMain.Main.AnisotropicFilter);
	}
	if (TheSettingManager->SettingsMain.Main.RemovePrecipitations) WriteRelJump(0x00543167, 0x00543176);
	*LocalWaterHiRes = 1; // Fixes a bug on the WaterHeightMapConstructor
#elif defined(SKYRIM)
	WriteRelJump(kClearDepth,				(UInt32)ClearDepth);
	WriteRelJump(kRenderFirstPersonNode,	(UInt32)RenderFirstPersonNode);
	if (TheSettingManager->SettingsMain.ShadowMode.NearQuality) {
		WriteRelJump(kSetShadowDistance,		(UInt32)SetShadowDistance);
		WriteRelJump(kSetShadowDistanceShader,	(UInt32)SetShadowDistanceShader);
		if (TheSettingManager->SettingsMain.ShadowMode.MultiPointLighting) {
			WriteRelJump(0x0069A7FF,	0x0069A8A7);
			WriteRelJump(kFixSunFlags,	(UInt32)FixSunFlags);
		}
	}
#endif

}
