#include <CommCtrl.h>
#include <intrin.h>
#include "RenderHook.h"

// --- Whole-frame plugin profiler (Develop.ProfileFrame) -----------------------
// Definitions for FrameProfiler.h; see that header for the bucket model and for
// why both clocks are measured. This TU is always compiled, so cross-file call
// sites only need the header (pulled in via Managers.h).
namespace FrameProfiler {

	bool         Active = false;
	unsigned int Counters[Cnt_COUNT] = {};

	namespace {
		const char* const BucketNames[Buck_COUNT] = {
			"FrameTotal",
			"UpdateShaderStates", "SetSceneGraph", "UpdateConstants", "ShadowMaps",
			"Effects:PreHdr", "Effects:PostHdr", "BeginScene", "ShaderHook",
			"Hook:Engine", "Hook:SetCT", "Hook:Resolve", "Hook:MidScene", "Hook:ShellWater"
		};
		const char* const CounterNames[Cnt_COUNT] = {
			"Draws", "SetCT", "SetPerGeomCT", "DepthResolves", "RenderedBlits", "Scenes"
		};
		const int ReportFrames = 600;

		double       gFrameMs[Buck_COUNT] = {};   // this frame only; reset by FrameBegin
		double       gAccumMs[Buck_COUNT] = {};   // summed over the report window
		double       gMaxMs[Buck_COUNT]   = {};   // worst single frame in the window
		unsigned int gCalls[Buck_COUNT]   = {};
		unsigned int gAccumCounters[Cnt_COUNT] = {};
		unsigned int gMaxCounters[Cnt_COUNT]   = {};
		LONGLONG     gQpcFreq = 0;
		int          gFrames  = 0;

		// --- GPU timing (D3D9 timestamp queries), double-buffered --------------
		// Read one frame late so GetData never stalls the CPU, the same shape as
		// the effect-chain profiler. A device reset invalidates these queries and
		// timing then simply stops yielding samples until the game is restarted -
		// acceptable for a dev-only knob, and the report says so explicitly rather
		// than printing a wrong number.
		struct GpuSlot {
			IDirect3DQuery9* Disjoint = NULL;
			IDirect3DQuery9* Freq     = NULL;
			IDirect3DQuery9* Begin    = NULL;
			IDirect3DQuery9* End      = NULL;
			bool             Pending  = false;
		};
		GpuSlot      gSlot[2];
		int          gActiveSlot     = -1;
		bool         gGpuAvailable   = false;
		bool         gGpuTried       = false;
		double       gGpuMs          = 0.0;
		double       gGpuMaxMs       = 0.0;
		unsigned int gGpuSamples     = 0;
		unsigned int gGpuRejNotReady = 0;
		unsigned int gGpuRejDisjoint = 0;

		void GpuEnsureQueries(IDirect3DDevice9* Device) {
			if (gGpuTried) return;
			gGpuTried = true;
			bool ok = true;
			for (int i = 0; i < 2 && ok; i++) {
				GpuSlot& s = gSlot[i];
				ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.Disjoint) == D3D_OK;
				ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,     &s.Freq)     == D3D_OK;
				ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,         &s.Begin)    == D3D_OK;
				ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,         &s.End)      == D3D_OK;
			}
			gGpuAvailable = ok;
			Logger::Log(ok ? "[FrameProfile] GPU timestamp queries created OK."
			               : "[FrameProfile] GPU timestamp queries unavailable (CreateQuery failed); reporting CPU only.");
		}

		// A slot that is not ready stays Pending and is retried next frame - it is
		// NEVER re-issued while pending, which would corrupt it so it never signals.
		void GpuTryCollect(GpuSlot& s) {
			if (!s.Pending) return;
			BOOL   disjoint = FALSE;
			UINT64 freq = 0, t0 = 0, t1 = 0;
			if (s.Disjoint->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) != S_OK) { gGpuRejNotReady++; return; }
			if (s.Freq->GetData(&freq, sizeof(freq), 0) != S_OK) { gGpuRejNotReady++; return; }
			if (s.Begin->GetData(&t0, sizeof(t0), 0)    != S_OK) { gGpuRejNotReady++; return; }
			if (s.End->GetData(&t1, sizeof(t1), 0)      != S_OK) { gGpuRejNotReady++; return; }
			s.Pending = false;
			if (disjoint || freq == 0 || t1 < t0) { gGpuRejDisjoint++; return; }
			double ms = (double)(t1 - t0) * 1000.0 / (double)freq;
			gGpuMs += ms;
			if (ms > gGpuMaxMs) gGpuMaxMs = ms;
			gGpuSamples++;
		}

		void Report() {
			double inv    = 1.0 / gFrames;
			double total  = gAccumMs[Buck_FrameTotal] * inv;
			double pctInv = total > 0.0 ? 100.0 / total : 0.0;

			Logger::Log("[FrameProfile] avg over %d frames (ms/frame; max = worst single frame):", gFrames);
			Logger::Log("[FrameProfile]   %-18s %8.4f ms  (max %8.4f)   CPU submission envelope", "FrameTotal", total, gMaxMs[Buck_FrameTotal]);
			if (gGpuSamples)
				Logger::Log("[FrameProfile]   %-18s %8.4f ms  (max %8.4f)   %u samples", "FrameTotal GPU", gGpuMs / gGpuSamples, gGpuMaxMs, gGpuSamples);
			else
				Logger::Log("[FrameProfile]   %-18s (no samples: available=%d notReady=%u disjoint=%u)", "FrameTotal GPU", (int)gGpuAvailable, gGpuRejNotReady, gGpuRejDisjoint);

			double sum = 0.0;
			Logger::Log("[FrameProfile]   -- plugin buckets (disjoint; subtracted from the envelope) --");
			for (int i = TopLevelBegin; i < TopLevelEnd; i++) {
				double ms = gAccumMs[i] * inv;
				sum += ms;
				Logger::Log("[FrameProfile]   %-18s %8.4f ms  (max %8.4f) %6.2f %%  %9.1f calls/frame",
					BucketNames[i], ms, gMaxMs[i], ms * pctInv, gCalls[i] * inv);
			}
			// A negative residual means two top-level buckets overlapped, i.e. the
			// disjointness assumption is wrong - not that the game render is free.
			Logger::Log("[FrameProfile]   %-18s %8.4f ms                       %6.2f %%  (game render + unhooked plugin work)",
				"Other", total - sum, (total - sum) * pctInv);

			Logger::Log("[FrameProfile]   -- ShaderHook detail (NESTED; not part of the sum above) --");
			for (int i = TopLevelEnd; i < Buck_COUNT; i++) {
				double ms = gAccumMs[i] * inv;
				Logger::Log("[FrameProfile]   %-18s %8.4f ms  (max %8.4f) %6.2f %%  %9.1f calls/frame",
					BucketNames[i], ms, gMaxMs[i], ms * pctInv, gCalls[i] * inv);
			}

			Logger::Log("[FrameProfile]   -- counters (per frame; ShaderHook carries ~0.15 us/draw of profiler overhead) --");
			for (int i = 0; i < Cnt_COUNT; i++)
				Logger::Log("[FrameProfile]   %-18s %9.1f  (max %u)", CounterNames[i], gAccumCounters[i] * inv, gMaxCounters[i]);
		}

		// Each report window stands alone, so two builds can be compared by reading
		// one window from each rather than by differencing running totals.
		void ResetWindow() {
			for (int i = 0; i < Buck_COUNT; i++) { gAccumMs[i] = 0.0; gMaxMs[i] = 0.0; gCalls[i] = 0; }
			for (int i = 0; i < Cnt_COUNT; i++)  { gAccumCounters[i] = 0; gMaxCounters[i] = 0; }
			gGpuMs = 0.0; gGpuMaxMs = 0.0; gGpuSamples = 0;
			gGpuRejNotReady = 0; gGpuRejDisjoint = 0;
			gFrames = 0;
		}
	}

	void Accumulate(int bucket, LONGLONG start) {
		gFrameMs[bucket] += (double)(QpcNow() - start) * 1000.0 / (double)gQpcFreq;
		gCalls[bucket]++;
	}

	void FrameBegin() {
		Active = TheSettingManager && TheSettingManager->SettingsMain.Develop.ProfileFrame != 0;
		if (!Active) return;
		if (gQpcFreq == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); gQpcFreq = f.QuadPart; }
		for (int i = 0; i < Buck_COUNT; i++) gFrameMs[i] = 0.0;
		for (int i = 0; i < Cnt_COUNT; i++)  Counters[i] = 0;

		gActiveSlot = -1;
		IDirect3DDevice9* Device = TheRenderManager ? TheRenderManager->device : NULL;
		if (!Device) return;
		GpuEnsureQueries(Device);
		if (!gGpuAvailable) return;
		// Only start a measurement on a slot whose previous result has been
		// collected; if both are still pending, skip GPU timing this frame.
		for (int i = 0; i < 2; i++) if (!gSlot[i].Pending) { gActiveSlot = i; break; }
		if (gActiveSlot >= 0) {
			GpuSlot& s = gSlot[gActiveSlot];
			s.Disjoint->Issue(D3DISSUE_BEGIN);
			s.Begin->Issue(D3DISSUE_END);
		}
	}

	void FrameEnd() {
		if (!Active) return;
		Active = false; // disarm first: nothing below should record into this frame

		if (gGpuAvailable) {
			if (gActiveSlot >= 0) {
				GpuSlot& s = gSlot[gActiveSlot];
				s.End->Issue(D3DISSUE_END);
				s.Freq->Issue(D3DISSUE_END);
				s.Disjoint->Issue(D3DISSUE_END);
				s.Pending = true;
			}
			// Collect any slot pending from a previous frame (not the one just issued).
			for (int i = 0; i < 2; i++) if (i != gActiveSlot) GpuTryCollect(gSlot[i]);
			gActiveSlot = -1;
		}

		for (int i = 0; i < Buck_COUNT; i++) {
			gAccumMs[i] += gFrameMs[i];
			if (gFrameMs[i] > gMaxMs[i]) gMaxMs[i] = gFrameMs[i];
		}
		for (int i = 0; i < Cnt_COUNT; i++) {
			gAccumCounters[i] += Counters[i];
			if (Counters[i] > gMaxCounters[i]) gMaxCounters[i] = Counters[i];
		}
		if (++gFrames < ReportFrames) return;
		Report();
		ResetWindow();
	}
}


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

// --- Savegame screenshot -----------------------------------------------------------------------
// The engine takes a save's thumbnail by re-entering kRender (0x0040C830) - the SAME function the
// visible frame goes through - a second time in the saving frame, with the thumbnail texture as the
// render-to-texture argument that arrives at TrackProcessImageSpaceShaders as RenderedTexture2.
// That render is NOT a private one: it rasterises the scene into the engine's shared
// full-resolution HDR scene buffer (measured: 3840x2160 A16B16G16R16F, the same surface every
// normal frame renders into), through a viewport the engine narrows to the top 9/16 of the target
// so the thumbnail comes out 16:9. The frame's display path then consumes that buffer, so its
// post-screenshot contents get presented for one frame: the scene squeezed into the top half of the
// screen with OR's post chain deliberately skipped - the "flash of sky" on every quicksave.
//
// SceneHdrSurface is latched from a NORMAL frame, where RenderedTexture1's buffer is that shared
// surface; the screenshot render is bracketed with a snapshot/restore of it so the visible frame
// keeps the image it already had. SceneHdrBackup is created on the first save and kept, and is
// rebuilt if the buffer's description ever changes (resolution change / device reset).
static IDirect3DSurface9*	SceneHdrSurface		= NULL;
static IDirect3DSurface9*	SceneHdrBackup		= NULL;
static D3DSURFACE_DESC		SceneHdrBackupDesc	= {};

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
	void	TrackWaterReflectionCull(NiAVObject*);
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
	FrameProfiler::FrameBegin();
	FrameProfiler::Scope FrameScope(FrameProfiler::Buck_FrameTotal);

	// The sampler's phase boundary is the same envelope FrameProfiler measures,
	// so "render" means the same thing in both reports.
	SampleProfiler::CheckKey();
	SampleProfiler::InRender = true;

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

	// Close the envelope before FrameEnd, so the residual FrameEnd computes covers
	// everything the engine's nested Render() did and the report is not timed into
	// the frame it is reporting on.
	FrameScope.Close();
	FrameProfiler::FrameEnd();
	SampleProfiler::InRender = false;
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
	
	// Not for the savegame thumbnail: it skips OR's effect chain, so EffectTexture
	// still holds the previous frame; let the engine tonemap its own render.
	if (TheSettingManager->SettingsMain.Main.RenderEffectsBeforeHdr && !TheRenderManager->IsSaveGameScreenShot) {
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

#if defined(OBLIVION)
// Vtables used to recognise the scene-graph node types the reflection cull may drop. Own copies:
// ShadowManager.cpp's set is file-static, and only the three container node types matter here.
static const void* VFTNiNodeRefl			= (void*)0x00A7E38C;
static const void* VFTBSFadeNodeRefl		= (void*)0x00A3F944;
static const void* VFTBSFaceGenNiNodeRefl	= (void*)0x00A64F5C;

// TESWaterCullingProcess_ReflectionCull (0x0049CBF0), the per-object entry point of the culling
// process that fills the water reflection map. The reflection pass re-renders the scene into a
// separate target after the main pass (see memory water-reflection-pass-detection), so anything
// dropped here is a draw saved outright.
//
// Two rejections, both cheap and both conservative:
//   * the bound is too small on screen to survive the reflection map's downscale, and
//   * the whole bound sits below the water plane, so it cannot appear in a reflection of it.
// Only container nodes are tested; geometry and anything else is passed straight through, which
// keeps the cull at the subtree granularity the engine already walks.
//
// The size test is taken against the main scene camera, not the mirrored reflection camera. That
// is deliberate and approximate: the two see the same world at nearly the same distance, so an
// object too small to matter in the player's view is too small in the reflection as well, and the
// scene camera is the one already published here.
//
// Off unless Main.WaterReflectionCullMinSize is set: the threshold is a share of screen area
// (see GetScreenSpaceBoundSize for the scale) and the useful value depends on reflection map size
// and view distance, so it wants tuning in game rather than a guessed default.
void (__thiscall RenderHook::* WaterReflectionCull)(NiAVObject*);
void (__thiscall RenderHook::* TrackWaterReflectionCull)(NiAVObject*);
void RenderHook::TrackWaterReflectionCull(NiAVObject* Object) {

	float MinScreenPercent = TheSettingManager->SettingsMain.Main.WaterReflectionCullMinSize;

	if (MinScreenPercent > 0.0f && Object && WorldSceneGraph) {
		const void* VFT = *(const void**)Object;
		if (VFT == VFTNiNodeRefl || VFT == VFTBSFadeNodeRefl || VFT == VFTBSFaceGenNiNodeRefl) {
			NiCamera* Camera = WorldSceneGraph->camera;
			NiBound* Bound = Object->GetWorldBound();
			if (Camera && Bound && Bound->Radius > 0.0f) {
				// Bound top, not the node pivot: the pivot can sit arbitrarily far from the mesh.
				if (Bound->Center.z + Bound->Radius < TheShaderManager->ShaderConst.Water.waterSettings.x) return;

				NiPoint2 BoundSize = { 0.0f, 0.0f };
				TheRenderManager->GetScreenSpaceBoundSize(&BoundSize, Bound, Camera);
				if (BoundSize.x * BoundSize.y * 100.0f < MinScreenPercent) return;
			}
		}
	}
	(this->*WaterReflectionCull)(Object);

}
#endif

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

// "The main WorldSceneGraph render is on the stack" lives on ShaderManager (InMainScenePass), not
// here, because ShaderRecord::SetCT needs it too. Written below in TrackRenderObject; see the
// declaration in ShaderManager.h for what it protects and why the per-scene latches cannot do it
// alone.

// [ShellDraw] / [ShellWater] retained diagnostics, gated behind Develop.NearShellDebug so they cost
// nothing in normal play. Kept deliberately: between them they root-caused four of this feature's
// defects, and they are the only way to see the shell's draw stream. Counters reset per frame at
// far-pass entry.
static int		ShellDrawLogCount	= 0;
static int		FarWaterLogCount	= 0;

// Sampler registers that shell-pass water draws had TESR_DepthBufferPreWater bound to in place of
// TESR_DepthBuffer, one bit per register, so every one of them is put back when the shell ends.
// A bitmask and not a single index: today all eight numbered WATER*.pso bind TESR_DepthBuffer to
// exactly one sampler (s6), but nothing enforces that, and a shader that bound it twice would leave
// the second sampler pointing at the pre-water texture for the rest of the frame - exactly the leak
// into the off-screen reflection render that the restore exists to prevent. 0 = nothing swapped.
static UInt32	ShellWaterDepthSamplers	= 0;

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

	// EBP-chain backtraces stop at the first FPO engine frame, so also dump every
	// stack value in Oblivion.exe's code range (a superset of the call chain).
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

// SKY* vertex shaders write no depth in the near shell: they pin z == w, which the shell's sub-1.0
// depth clear rejects, or sit far beyond its far plane at M.
static bool IsSkyShader(NiD3DVertexShaderEx* VertexShader) {

	return VertexShader && VertexShader->ShaderName && !memcmp(VertexShader->ShaderName, "SKY", 3);

}

// Logs one draw of the near-shell diagnostics, with the render state it inherits from the previous
// draw. pin=1 marks the SKY* draws that are excluded from ShellDraws.
static void LogShellPassDraw(const char* Tag, int Index, NiGeometry* Geometry, NiTransform* WorldTransform, NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader, NiDX9RenderState* RenderState) {

	float Dist = 0.0f;
	if (WorldTransform) {
		NiPoint3& CamPos = WorldSceneGraph->camera->m_worldTransform.pos;
		float dx = WorldTransform->pos.x - CamPos.x;
		float dy = WorldTransform->pos.y - CamPos.y;
		float dz = WorldTransform->pos.z - CamPos.z;
		Dist = sqrtf(dx * dx + dy * dy + dz * dz);
	}
	Logger::Log("%s %04d VS=%s PS=%s Geo=%s pin=%d cweIn=%d dist=%.1f ZEnable=%d ZWrite=%d ZFunc=%d",
		Tag, Index,
		VertexShader && VertexShader->ShaderName ? VertexShader->ShaderName : "-",
		PixelShader && PixelShader->ShaderName ? PixelShader->ShaderName : "-",
		Geometry && Geometry->m_pcName ? Geometry->m_pcName : "-",
		IsSkyShader(VertexShader) ? 1 : 0,
		RenderState->GetRenderState(D3DRS_COLORWRITEENABLE),
		Dist,
		RenderState->GetRenderState(D3DRS_ZENABLE),
		RenderState->GetRenderState(D3DRS_ZWRITEENABLE),
		RenderState->GetRenderState(D3DRS_ZFUNC));

}

// Runs at the shell's first near-water draw and gives its water what the far pass gave its own: a
// TESR_RenderedBuffer capture that contains the shell's geometry, masked to the shell's coverage,
// and a TESR_DepthBufferPreWater clamped to M under that coverage. Without them, submerged
// first-person arms vanish, or shade as if lying on the lake bottom.
static void PrepareShellNearWater() {

	FrameProfiler::Scope ShellWaterScope(FrameProfiler::Buck_HookShellWater);
	bool MaskResolved = TheShaderManager->CaptureShellRenderedBuffer();
	bool Captured = MaskResolved;
	if (!Captured && TheRenderManager->currentRTGroup && TheShaderManager->RenderedSurface) {
		TheRenderManager->device->StretchRect(TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, NULL, TheShaderManager->RenderedSurface, NULL, D3DTEXF_NONE);
		Captured = true;
	}
	// Close the SetCT colour latch only if something captured: a far pass with no
	// water draw leaves it open, and a later fire would capture over the water.
	if (Captured) TheShaderManager->RenderedBufferFilled = true;
	TheShaderManager->FlattenShellPreWaterDepth(MaskResolved);

}

// Shell water must never sample TESR_DepthBuffer: it holds the far pass's own water surface, cut open
// at M, and water's offset depth taps land on it along that edge. Rebinds those samplers to
// TESR_DepthBufferPreWater (same (M, F) encoding, no water in it) for this draw. Per draw because
// SetCT only runs when the shader handle changes. The WATER prefix test also matches WATERHMAP*,
// harmlessly: those shaders do not declare the depth buffer.
static void BindShellWaterPreWaterDepth(NiD3DPixelShaderEx* PixelShader) {

	if (!PixelShader->ShaderProg || !TheRenderManager->DepthTexturePreWater || !PixelShader->ShaderName || memcmp(PixelShader->ShaderName, "WATER", 5)) return;
	ShaderValue* Values = PixelShader->ShaderProg->TextureShaderValues;
	for (UInt32 c = 0; c < PixelShader->ShaderProg->TextureShaderValuesCount; c++) {
		if (Values[c].Texture && Values[c].Texture->Texture == TheRenderManager->DepthTexture && Values[c].RegisterIndex < 16) {
			TheRenderManager->device->SetTexture(Values[c].RegisterIndex, TheRenderManager->DepthTexturePreWater);
			ShellWaterDepthSamplers |= 1 << Values[c].RegisterIndex;
		}
	}

}

// Undoes BindShellWaterPreWaterDepth before anything else inherits the binding, notably the water
// reflection render, which re-binds water shaders later without necessarily re-running SetCT.
static void RestoreShellWaterDepthSamplers() {

	for (UInt32 s = 0; s < 16; s++) {
		if (ShellWaterDepthSamplers & (1 << s)) TheRenderManager->device->SetTexture(s, TheRenderManager->DepthTexture);
	}
	ShellWaterDepthSamplers = 0;

}

// Uploads the grass collision sources relative to this instance (c254/c255), the only grass constants
// that change per draw; SetCT already uploads c253 at every grass shader bind. The vertex constant
// space is full, so the two recovery weights ride in c255.zw, which is why there are three sources.
static void SetGrassCollisionConstants(NiTransform* WorldTransform) {

	float cx = WorldTransform->pos.x;
	float cy = WorldTransform->pos.y;
	D3DXVECTOR4 xy0(
		TheShaderManager->GrassCollisionSources[0].x - cx,
		TheShaderManager->GrassCollisionSources[0].y - cy,
		TheShaderManager->GrassCollisionSources[1].x - cx,
		TheShaderManager->GrassCollisionSources[1].y - cy);
	D3DXVECTOR4 xy1(
		TheShaderManager->GrassCollisionSources[2].x - cx,
		TheShaderManager->GrassCollisionSources[2].y - cy,
		TheShaderManager->GrassCollisionWeights[0],
		TheShaderManager->GrassCollisionWeights[1]);
	TheRenderManager->device->SetVertexShaderConstantF(254, (const float*)&xy0, 1);
	TheRenderManager->device->SetVertexShaderConstantF(255, (const float*)&xy1, 1);

}

UInt32 (__thiscall RenderHook::* SetupShaderPrograms)(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, UInt32);
UInt32 (__thiscall RenderHook::* TrackSetupShaderPrograms)(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, UInt32);
UInt32 RenderHook::TrackSetupShaderPrograms(NiGeometry* Geometry, NiSkinInstance* SkinInstance, NiSkinPartition::Partition* SkinPartition, NiGeometryBufferData* GeometryBufferData, NiPropertyState* PropertyState, NiDynamicEffectState* EffectState, NiTransform* WorldTransform, UInt32 WorldBound) {

	FrameProfiler::Scope HookScope(FrameProfiler::Buck_ShaderHook);
	FrameProfiler::Count(FrameProfiler::Cnt_Draws);

	D3DMATRIX* Proj = &TheRenderManager->projMatrix;

	if (!TheShaderManager->jitterSet) {
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

	// StampPassProjection is inert outside the two shell passes, where projMatrix
	// belongs to whoever set it up.
	RenderManager::StampPassProjection(Proj);
	if (RenderManager::CurrentPass == RenderManager::PassNear) {

		// Only draws that can write depth hold the post-shell flatten gate open. An
		// upper bound: a counted draw may still be depth-rejected.
		if (!IsSkyShader(VertexShader)) RenderManager::ShellDraws++;
		if (TheSettingManager->SettingsMain.Develop.NearShellDebug && ShellDrawLogCount < 64)
			LogShellPassDraw("[ShellDraw]", ++ShellDrawLogCount, Geometry, WorldTransform, VertexShader, PixelShader, RenderState);
	}
	else if (RenderManager::CurrentPass == RenderManager::PassFar) {
		if (TheSettingManager->SettingsMain.Develop.NearShellDebug && FarWaterLogCount < 12 &&
			PixelShader && PixelShader->ShaderName && !memcmp(PixelShader->ShaderName, "WATER", 5))
			LogShellPassDraw("[ShellWater]", ++FarWaterLogCount, Geometry, WorldTransform, VertexShader, PixelShader, RenderState);
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

#if defined(OBLIVION)
		// POM shadow side channel: RT1 only under a depth-writing, unblended PAR first
		// pass of the main scene. Must precede the mid-scene shadow apply below.
		TheShaderManager->BindPOMDepth(PixelShader->isPOMShadowWriter && TheShaderManager->InMainScenePass &&
			RenderState->GetRenderState(D3DRS_ZWRITEENABLE) && !RenderState->GetRenderState(D3DRS_ALPHABLENDENABLE));
#endif

		// Apply shadows at the main pass's first near-water draw: every receiver is
		// drawn by then and no near water yet. InMainScenePass keeps it out of the
		// water reflection render, which binds the same shaders later.
		if (TheShaderManager->InMainScenePass && !TheShaderManager->PreWaterDepthBufferFilled && PixelShader->isNearWater) {
			FrameProfiler::Scope MidSceneScope(FrameProfiler::Buck_HookMidScene);
			TheRenderManager->ResolvePreWaterDepthBuffer();
			TheShaderManager->RenderShadowsMidScene();
			TheShaderManager->PreWaterDepthBufferFilled = true;
		}

		if (RenderManager::CurrentPass == RenderManager::PassNear && !ShellNearWaterPrepDone && PixelShader->isNearWater) {
			ShellNearWaterPrepDone = true;
			PrepareShellNearWater();
		}

		if (PixelShader->ShaderProg && TheRenderManager->renderState->GetPixelShader() != PixelShader->ShaderHandle) PixelShader->ShaderProg->SetCT();

		if (RenderManager::CurrentPass == RenderManager::PassNear) BindShellWaterPreWaterDepth(PixelShader);

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

	UInt32 result;
	{
		FrameProfiler::Scope EngineScope(FrameProfiler::Buck_HookEngine);
		result = (this->*SetupShaderPrograms)(Geometry, SkinInstance, SkinPartition, GeometryBufferData, PropertyState, EffectState, WorldTransform, WorldBound);
	}

	if (VertexShader && VertexShader->ShaderProg && VertexShader->isGrass) SetGrassCollisionConstants(WorldTransform);

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

	// Hold the visible frame across the screenshot render, which rasterises into
	// this buffer. Best-effort: without the backup the save just flashes.
	bool Restore = false;
	if (SceneHdrSurface) {
		D3DSURFACE_DESC Desc;
		if (SceneHdrSurface->GetDesc(&Desc) == D3D_OK) {
			if (SceneHdrBackup && (Desc.Width != SceneHdrBackupDesc.Width || Desc.Height != SceneHdrBackupDesc.Height || Desc.Format != SceneHdrBackupDesc.Format)) {
				SceneHdrBackup->Release();
				SceneHdrBackup = NULL;
			}
			if (!SceneHdrBackup && SUCCEEDED(TheRenderManager->device->CreateRenderTarget(Desc.Width, Desc.Height, Desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &SceneHdrBackup, NULL)))
				SceneHdrBackupDesc = Desc;
			if (SceneHdrBackup)
				Restore = SUCCEEDED(TheRenderManager->device->StretchRect(SceneHdrSurface, NULL, SceneHdrBackup, NULL, D3DTEXF_NONE));
		}
	}

	TheRenderManager->IsSaveGameScreenShot = 1;
	r = SaveGameScreenshot(pWidth, pHeight);
	TheRenderManager->IsSaveGameScreenShot = 0;

	if (Restore) TheRenderManager->device->StretchRect(SceneHdrBackup, NULL, SceneHdrSurface, NULL, D3DTEXF_NONE);

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

	// The camera and reflection-flag checks reject off-screen renders of the same
	// root (the water function at 0x4D040B/0x4D04A8 passes its own camera).
	bool MainScenePass = (Object == WorldSceneGraph) && (Camera == WorldSceneGraph->camera) && !*kIsRenderingWaterReflections;
	if (MainScenePass) {
		TheShaderManager->InMainScenePass = true;
		TheShaderManager->PreWaterDepthBufferFilled = false;
		TheShaderManager->ClearPOMDepth();
		if (TheSettingManager->SettingsMain.Develop.LogShaders && TheKeyboardManager->OnKeyDown(TheSettingManager->SettingsMain.Develop.LogShaders)) { // [GrassOrderDbg]
			GrassOrderCapture = true;
			GrassOrderSeq = GrassOrderGrassTraces = GrassOrderWaterTraces = 0;
			Logger::Log("[GrassOrderDbg] ==== capture start (WorldSceneGraph render) ====");
		}
	}
	if (MainScenePass && RenderManager::ShellActive) {
		ShellDrawLogCount = FarWaterLogCount = 0;
		RenderManager::ApplyPass(RenderManager::PassFar);
	}
	RenderObject(Camera, Object, CullingProcess, VisibleArray);
	// Clear before the shell renders: this disarms the mid-scene shadow trigger and
	// SetCT's depth resolve for the rest of the frame (shell, first-person and
	// reflection renders). Unlike the latches, BeginScene does not reset it.
	if (MainScenePass) TheShaderManager->InMainScenePass = false;
	if (MainScenePass) TheShaderManager->BindPOMDepth(false);
	if (Object == WorldSceneGraph && GrassOrderCapture) { // [GrassOrderDbg]
		GrassOrderCapture = false;
		Logger::Log("[GrassOrderDbg] ==== capture end (%d passes) ====", GrassOrderSeq);
	}
	if (MainScenePass) {
		if (!TheShaderManager->PreWaterDepthBufferFilled) {
			// No near-water draw this frame (common in interiors): resolve the receiver
			// depth and apply shadows now, at the end of the main scene render.
			TheRenderManager->ResolvePreWaterDepthBuffer();
			TheShaderManager->RenderShadowsMidScene();
			TheShaderManager->PreWaterDepthBufferFilled = true;
		}
		// Resolve the far pass's depth before the shell clears it, so post-processing
		// sees only [M, F]. SetCT cannot resolve over it later: InMainScenePass is
		// already cleared.
		TheRenderManager->ResolveDepthBuffer();

		if (RenderManager::ShellActive) {
			UInt8 Debug = TheSettingManager->SettingsMain.Develop.NearShellDebug;

			// NearShellDebug 2 skips the shell, leaving a hole that shows what it holds.
			if (Debug != 2) {
				// Armed here, not at far-pass entry, so it only arms for a shell that runs.
				ShellNearWaterPrepDone = false;
				RenderManager::ApplyPass(RenderManager::PassNear);
				// Clear to one ULP below 1.0 so z == w sky shaders fail LESSEQUAL instead of
				// painting over the far pass; the shell loses only ~0.00001 units of depth.
				// The engine's Clear() cannot set the value, hence the device call.
				TheRenderManager->device->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, RenderManager::ShellClearDepth, 0);
				RenderObject(Camera, Object, CullingProcess, VisibleArray);
				RestoreShellWaterDepthSamplers();
				// Flatten only after the last shell draw, since shell water reads the untouched
				// far-pass depth, and before the image-space effects that sample it.
				TheShaderManager->FlattenShellDepth();
			}

			RenderManager::ApplyPass(RenderManager::PassFull);

			if (Debug) {
				Logger::Log("[NearShell] n=%.3f M=%.3f F=%.1f shellDraws=%d skipShell=%d",
					RenderManager::RealNear, RenderManager::ShellBoundary, RenderManager::RealFar,
					RenderManager::ShellDraws, Debug == 2 ? 1 : 0);
			}
		}
	}
	else if (Object == Player->firstPersonNiNode) {
		// With the shell active, depth was already resolved and then cleared, so a
		// resolve here would replace the far-pass depth with the near-blank shell.
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
	else if (TheRenderManager->IsSaveGameScreenShot) {
		// The engine's image-space chain writes the save thumbnail, so run it; OR's
		// effect chain is built for full-resolution buffers and stays out.
		ProcessImageSpaceShaders(Renderer, RenderedTexture1, RenderedTexture2);
	}

	if (TheRenderManager->IsSaveGameScreenShot) {
		if (MenuRenderedTexture)
			TheRenderManager->device->StretchRect(MenuRenderedTexture->RenderTargetGroup->RenderTargets[0]->data->Surface, NULL, TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, &TheRenderManager->SaveGameScreenShotRECT, D3DTEXF_NONE);
		else
			TheRenderManager->device->StretchRect(TheRenderManager->defaultRTGroup->RenderTargets[0]->data->Surface, NULL, TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, &TheRenderManager->SaveGameScreenShotRECT, D3DTEXF_NONE);
	}

	// Latch the engine's shared scene buffer from normal frames, so a save knows
	// what to preserve.
	if (!RenderedTexture2 && RenderedTexture1 && RenderedTexture1->RenderedTexture && RenderedTexture1->RenderedTexture->buffer)
		SceneHdrSurface = RenderedTexture1->RenderedTexture->buffer->data->Surface;

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
	*((int*)&WaterReflectionCull)			= 0x0049CBF0;
	TrackWaterReflectionCull				= &RenderHook::TrackWaterReflectionCull;
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
	DetourAttach(&(PVOID&)WaterReflectionCull,			*((PVOID*)&TrackWaterReflectionCull));
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
