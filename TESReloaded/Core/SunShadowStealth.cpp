#include "SunShadowStealth.h"

static const UInt32	kSunTermCall					= 0x006561FD; // HighProcess_GetLightLevel's exterior sun term
static const UInt32	kShadowSceneLightContribution	= 0x007D31B0;

static volatile float PlayerSunLightScale = 1.0f;
static volatile float LastPlayerSunTerm = 0.0f;

static const UInt32	kShadowSceneNode	= 0x00B42F54;
static const int	kProbePointCount	= 8;
static const int	kProbeRingSize		= 3;
static const int	kProbeHeightCount	= 4;
static const float	kProbeHeights[kProbeHeightCount] = { 0.2f, 0.45f, 0.7f, 0.95f }; // fractions of body height
static const float	kBodyRadius			= 25.0f;
static const float	kStandingHeight		= 120.0f;
static const float	kSneakingHeight		= 80.0f;
static const float	kLateralOffset		= 15.0f;
static const float	kExitMargin			= 10.0f;
static const UInt32	kMovementSneak		= 0x400;
static const DWORD	kLogIntervalMs		= 1000;

/// One in-flight probe: its render target and the event query issued after it was drawn.
struct ProbeSlot {
	IDirect3DSurface9*	Target;
	IDirect3DQuery9*	Done;
	bool				Pending;
};

static EffectRecord*		ProbeEffect = NULL;
static D3DXHANDLE			ProbePointsHandle = NULL;
static ProbeSlot			Ring[kProbeRingSize] = {};
static int					NextSlot = 0;
static IDirect3DSurface9*	ReadbackSurface = NULL;
static bool					ProbeFailed = false;
static bool					ProbeArmed = false;
static float				Visibility[kProbePointCount] = {};
static float				LastShadowed = 0.0f;
static DWORD				LastLogTick = 0;
static bool					LoggedLightListCheck = false;

static void ReleaseProbeResources() {

	for (int i = 0; i < kProbeRingSize; i++) {
		if (Ring[i].Target) Ring[i].Target->Release();
		if (Ring[i].Done) Ring[i].Done->Release();
		Ring[i] = {};
	}
	if (ReadbackSurface) ReadbackSurface->Release();
	ReadbackSurface = NULL;
	if (ProbeEffect) TheShaderManager->DisposeEffect(ProbeEffect);
	ProbeEffect = NULL;
	ProbePointsHandle = NULL;
	NextSlot = 0;

}

static bool CreateProbeResources(IDirect3DDevice9* Device) {

	char Filename[MAX_PATH];
	strcpy(Filename, EffectsPath);
	strcat(Filename, "Shadows\\ShadowProbe.fx");
	ProbeEffect = new EffectRecord();
	if (!TheShaderManager->LoadEffect(ProbeEffect, Filename, NULL)) {
		ProbeEffect = NULL; // LoadEffect already disposed it
		return false;
	}
	ProbePointsHandle = ProbeEffect->Effect->GetParameterByName(NULL, "SunShadowProbePoints");
	if (!ProbePointsHandle) return false;
	for (int i = 0; i < kProbeRingSize; i++) {
		if (FAILED(Device->CreateRenderTarget(kProbePointCount, 1, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &Ring[i].Target, NULL))) return false;
		if (FAILED(Device->CreateQuery(D3DQUERYTYPE_EVENT, &Ring[i].Done))) return false;
	}
	return SUCCEEDED(Device->CreateOffscreenPlainSurface(kProbePointCount, 1, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &ReadbackSurface, NULL));

}

static bool SunMapsProbeable() {

	SettingsShadowStruct::ExteriorsStruct* Exteriors = &TheSettingManager->SettingsShadows.Exteriors;
	ShaderConstants::ShadowMapStruct* ShadowMap = &TheShaderManager->ShaderConst.ShadowMap;
	return Exteriors->UsePostProcessing && TheShaderManager->ShadowsExteriorsEffect && Player && Player->GetWorldSpace()
		&& ShadowMap->ShadowBiasAdaptive.w >= 0.5f && ShadowMap->ShadowLightDir.z > 0.0f;

}

static void DiscardProbes() {

	for (int i = 0; i < kProbeRingSize; i++) Ring[i].Pending = false;
	PlayerSunLightScale = 1.0f;

}

void UpdateSunShadowStealth() {

	if (!TheSettingManager->SettingsShadows.Exteriors.SunShadowStealth) {
		ReleaseProbeResources();
		ProbeFailed = false;
		ProbeArmed = false;
		PlayerSunLightScale = 1.0f;
		return;
	}
	ProbeArmed = SunMapsProbeable();
	if (!ProbeArmed) {
		DiscardProbes();
		return;
	}
	if (!ProbeEffect && !ProbeFailed && !CreateProbeResources(TheRenderManager->device)) {
		Logger::Log("SunShadowStealth: probe resources unavailable (is Shadows\\ShadowProbe.fx compiled?); the player's light level stays vanilla.");
		ReleaseProbeResources();
		ProbeFailed = true;
	}

}

// Each point is pushed along the sun direction until the sun ray has left a body capsule, so the
// player's own geometry in the actor shadow map never shadows it.
static void BuildProbePoints(D3DXVECTOR4* Points) {

	const D3DXVECTOR4& Sun = TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
	bool Sneaking = Player->process && (Player->process->GetMovementFlags() & kMovementSneak);
	float Height = (Sneaking ? kSneakingHeight : kStandingHeight) * Player->scale;
	float Radius = kBodyRadius * Player->scale;
	float Horizontal = sqrtf(Sun.x * Sun.x + Sun.y * Sun.y);
	float SideX = 1.0f;
	float SideY = 0.0f;
	if (Horizontal > 1e-3f) {
		SideX = -Sun.y / Horizontal;
		SideY = Sun.x / Horizontal;
	}
	for (int h = 0; h < kProbeHeightCount; h++) {
		float Z = Height * kProbeHeights[h];
		float Exit = Horizontal > 1e-3f ? Radius / Horizontal : FLT_MAX;
		Exit = min(Exit, (Height - Z) / Sun.z) + kExitMargin;
		for (int s = 0; s < 2; s++) {
			float Side = s ? kLateralOffset : -kLateralOffset;
			D3DXVECTOR4& P = Points[h * 2 + s];
			P.x = Player->pos.x + SideX * Side + Sun.x * Exit;
			P.y = Player->pos.y + SideY * Side + Sun.y * Exit;
			P.z = Player->pos.z + Z + Sun.z * Exit;
			P.w = 1.0f;
		}
	}

}

static void PublishScale() {

	float Sum = 0.0f;
	for (int i = 0; i < kProbePointCount; i++) {
		float V = Visibility[i];
		Sum += (V >= 0.0f && V <= 1.0f) ? V : 1.0f;
	}
	LastShadowed = 1.0f - Sum / kProbePointCount;
	float Darkness = TheShaderManager->ShaderConst.Shadow.Data.y;
	float Lit = std::clamp(TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir.w, Darkness, 1.0f);
	float ShadowRatio = Lit > 0.0f ? Darkness / Lit : 1.0f;
	float Scale = std::lerp(1.0f, ShadowRatio, LastShadowed);
	PlayerSunLightScale = (Scale >= 0.0f && Scale <= 1.0f) ? Scale : 1.0f;

}

// Oldest first; a slot is copied only once its event query reports the GPU is done with it.
static void ReadFinishedProbes(IDirect3DDevice9* Device) {

	for (int k = 0; k < kProbeRingSize; k++) {
		ProbeSlot& Slot = Ring[(NextSlot + k) % kProbeRingSize];
		if (!Slot.Pending) continue;
		if (Slot.Done->GetData(NULL, 0, 0) != S_OK) return;
		Slot.Pending = false;
		D3DLOCKED_RECT Locked;
		if (FAILED(Device->GetRenderTargetData(Slot.Target, ReadbackSurface))) continue;
		if (FAILED(ReadbackSurface->LockRect(&Locked, NULL, D3DLOCK_READONLY))) continue;
		memcpy(Visibility, Locked.pBits, sizeof(Visibility));
		ReadbackSurface->UnlockRect();
		PublishScale();
	}

}

static void LogProbe() {

	if (!TheSettingManager->SettingsMain.Develop.LogSunShadowStealth) return;
	DWORD Now = GetTickCount();
	if (Now - LastLogTick < kLogIntervalMs) return;
	LastLogTick = Now;
	if (!LoggedLightListCheck) {
		LoggedLightListCheck = true;
		ShadowSceneNode* SceneNode = *(ShadowSceneNode**)kShadowSceneNode;
		bool Listed = false;
		if (SceneNode) {
			for (NiTList<ShadowSceneLight>::Entry* Entry = SceneNode->lights.start; Entry; Entry = Entry->next)
				if (Entry->data == SceneNode->directionalLight) Listed = true;
		}
		Logger::Log("SunShadowStealth: directional light %s in ShadowSceneNode::lights", Listed ? "IS (sun double-counted)" : "is not");
	}
	float Darkness = TheShaderManager->ShaderConst.Shadow.Data.y;
	float Lit = std::clamp(TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir.w, Darkness, 1.0f);
	float Scale = PlayerSunLightScale;
	float Term = LastPlayerSunTerm;
	Logger::Log("SunShadowStealth: vis %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f shadowed %.2f D %.2f L %.2f scale %.2f sun term %.3f -> %.3f",
		Visibility[0], Visibility[1], Visibility[2], Visibility[3], Visibility[4], Visibility[5], Visibility[6], Visibility[7],
		LastShadowed, Darkness, Lit, Scale, Term, Term * Scale);

}

void RenderSunShadowProbe(IDirect3DDevice9* Device) {

	if (!ProbeArmed || !ProbeEffect || TheShaderManager->ShaderConst.ShadowMap.ShadowBiasAdaptive.w < 0.5f) return;
	ReadFinishedProbes(Device);
	LogProbe();
	ProbeSlot& Slot = Ring[NextSlot];
	if (Slot.Pending) return;

	D3DXVECTOR4 Points[kProbePointCount];
	BuildProbePoints(Points);
	if (FAILED(Device->SetRenderTarget(0, Slot.Target))) return;
	ProbeEffect->SetCT();
	ProbeEffect->Effect->SetVectorArray(ProbePointsHandle, Points, kProbePointCount);
	UINT Passes;
	ProbeEffect->Effect->Begin(&Passes, NULL);
	ProbeEffect->Effect->BeginPass(0);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	ProbeEffect->Effect->EndPass();
	ProbeEffect->Effect->End();
	Slot.Done->Issue(D3DISSUE_END);
	Slot.Pending = true;
	NextSlot = (NextSlot + 1) % kProbeRingSize;

}

static float CallShadowSceneLightContribution(void* Light, float X, float Y, float Z, UInt32 Exclude) {

	class T {}; union { UInt32 x; float(T::* m)(float, float, float, UInt32); } u = { kShadowSceneLightContribution };
	return ((T*)Light->*u.m)(X, Y, Z, Exclude);

}

// Replaces the sun-term call; thiscall with callee cleanup, so __fastcall fits once the stub puts the actor in edx.
static float __fastcall SunTermHook(void* SunLight, Actor* Owner, float X, float Y, float Z, UInt32 Exclude) {

	float Term = CallShadowSceneLightContribution(SunLight, X, Y, Z, Exclude);
	if (Owner != (Actor*)Player) return Term;
	LastPlayerSunTerm = Term;
	return Term * PlayerSunLightScale;

}

// edi holds the actor for the whole of HighProcess_GetLightLevel.
static __declspec(naked) void SunTermStub() {

	__asm {
		mov		edx, edi
		jmp		SunTermHook
	}

}

void CreateSunShadowStealthHook() {

	WriteRelCall(kSunTermCall, (UInt32)SunTermStub);

}
