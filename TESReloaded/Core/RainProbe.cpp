#include "RainProbe.h"
#include <unordered_map>

static const int	kPlayerPointCount	= 8;
static const int	kActorPointCount	= 2;
static const int	kMaxPointCount		= kPlayerPointCount + kActorPointCount * RainProbeMaxActors;
static const int	kProbeRingSize		= 3;
static const int	kPlayerHeightCount	= 4;
static const float	kPlayerHeights[kPlayerHeightCount] = { 0.2f, 0.45f, 0.7f, 0.95f }; // fractions of body height
static const float	kActorHeights[kActorPointCount] = { 0.55f, 0.9f };	// chest and head
static const float	kBodyRadius			= 25.0f;
static const float	kStandingHeight		= 120.0f;
static const float	kSneakingHeight		= 80.0f;
static const float	kLateralOffset		= 15.0f;
static const float	kExitMargin			= 10.0f;
static const UInt32	kMovementSneak		= 0x400;
static const int	kReadbackTimeoutFrames	= 30;

/// One in-flight probe: its render target, the event query issued after it was drawn, and the NPCs it tested.
struct RainProbeSlot {
	IDirect3DSurface9*	Target;
	IDirect3DQuery9*	Done;
	bool				Pending;
	int					ActorCount;
	UInt32				ActorRefIDs[RainProbeMaxActors];
};

static volatile LONG		RainHitting = 0;
static EffectRecord*		ProbeEffect = NULL;
static D3DXHANDLE			ProbePointsHandle = NULL;
static RainProbeSlot		Ring[kProbeRingSize] = {};
static int					NextSlot = 0;
static bool					ProbeFailed = false;
static bool					ProbeArmed = false;
static bool					LoggedD3DFailure = false;
static int					FramesSinceReadback = 0;
static RainProbeActor		Actors[RainProbeMaxActors];
static int					ActorCount = 0;
static std::unordered_map<UInt32, bool>	ActorExposure;

static void ReleaseProbeResources() {

	for (int i = 0; i < kProbeRingSize; i++) {
		if (Ring[i].Target) Ring[i].Target->Release();
		if (Ring[i].Done) Ring[i].Done->Release();
		Ring[i] = {};
	}
	if (ProbeEffect) TheShaderManager->DisposeEffect(ProbeEffect);
	ProbeEffect = NULL;
	ProbePointsHandle = NULL;
	NextSlot = 0;

}

static bool CreateProbeResources(IDirect3DDevice9* Device) {

	char Filename[MAX_PATH];
	strcpy(Filename, EffectsPath);
	strcat(Filename, "Precipitations\\RainProbe.fx");
	ProbeEffect = new EffectRecord();
	if (!TheShaderManager->LoadEffect(ProbeEffect, Filename, NULL)) {
		ProbeEffect = NULL; // LoadEffect already disposed it
		return false;
	}
	ProbePointsHandle = ProbeEffect->Effect->GetParameterByName(NULL, "RainProbePoints");
	if (!ProbePointsHandle) return false;
	for (int i = 0; i < kProbeRingSize; i++) {
		if (FAILED(Device->CreateRenderTarget(kMaxPointCount, 1, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, TRUE, &Ring[i].Target, NULL))) return false;
		if (FAILED(Device->CreateQuery(D3DQUERYTYPE_EVENT, &Ring[i].Done))) return false;
	}
	return true;

}

bool IsRainFalling() {

	return TheSettingManager->SettingsMain.Effects.Precipitations && TheShaderManager->RainEffect
		&& Player && Player->IsExteriorLike() && TheShaderManager->ShaderConst.Precipitations.RainData.x > 0.0f;

}

static void DiscardProbes() {

	for (int i = 0; i < kProbeRingSize; i++) Ring[i].Pending = false;
	FramesSinceReadback = 0;
	RainHitting = 0;
	ActorExposure.clear();

}

void UpdateRainProbe() {

	const SettingsPrecipitationsStruct& Settings = TheSettingManager->SettingsPrecipitations;
	if (!Settings.Rain.IsRainHittingPlayer && !Settings.WetActors.Enabled) {
		ReleaseProbeResources();
		ProbeFailed = false;
		ProbeArmed = false;
		LoggedD3DFailure = false;
		DiscardProbes();
		return;
	}
	ProbeArmed = IsRainFalling();
	if (!ProbeArmed) {
		DiscardProbes();
		return;
	}
	if (!ProbeEffect && !ProbeFailed && !CreateProbeResources(TheRenderManager->device)) {
		Logger::Log("RainProbe: probe resources unavailable (is Precipitations\\RainProbe.fx compiled?); IsRainHittingPlayer stays 0 and wet actors use the outdoors test.");
		ReleaseProbeResources();
		ProbeFailed = true;
	}

	// Watchdog: no readback for a while (a device error, or the rain pass skipped while the camera is
	// underwater) drops the answers instead of leaving them stale.
	if (++FramesSinceReadback > kReadbackTimeoutFrames) DiscardProbes();

}

// Mirrors Rain.fx's slant: the direction rain comes from, normalized, z up.
static D3DXVECTOR3 RainFallAxis() {

	const ShaderConstants& Const = TheShaderManager->ShaderConst;
	float Slant = Const.Precipitations.SlantData.x * Const.WindData.z;
	D3DXVECTOR3 Axis(-Slant * Const.WindData.x, -Slant * Const.WindData.y, 1.0f);
	D3DXVec3Normalize(&Axis, &Axis);
	return Axis;

}

// Each point is pushed up the fall line (against the rain) until it has left a body capsule, so it
// tests whether rain reaches that spot on the body's surface. Lateral adds a second point per height,
// offset sideways across the fall line.
static void BuildBodyPoints(D3DXVECTOR4* Points, const D3DXVECTOR3& Axis, const NiPoint3& Pos, float Height, float Radius, const float* Fractions, int FractionCount, bool Lateral) {

	float Horizontal = sqrtf(Axis.x * Axis.x + Axis.y * Axis.y);
	float SideX = 1.0f;
	float SideY = 0.0f;
	if (Horizontal > 1e-3f) {
		SideX = -Axis.y / Horizontal;
		SideY = Axis.x / Horizontal;
	}
	int Sides = Lateral ? 2 : 1;
	for (int h = 0; h < FractionCount; h++) {
		float Z = Height * Fractions[h];
		float Exit = Horizontal > 1e-3f ? Radius / Horizontal : FLT_MAX;
		Exit = min(Exit, (Height - Z) / Axis.z) + kExitMargin;
		for (int s = 0; s < Sides; s++) {
			float Side = Lateral ? (s ? kLateralOffset : -kLateralOffset) : 0.0f;
			D3DXVECTOR4& P = Points[h * Sides + s];
			P.x = Pos.x + SideX * Side + Axis.x * Exit;
			P.y = Pos.y + SideY * Side + Axis.y * Exit;
			P.z = Pos.z + Z + Axis.z * Exit;
			P.w = 1.0f;
		}
	}

}

static void BuildProbePoints(D3DXVECTOR4* Points) {

	D3DXVECTOR3 Axis = RainFallAxis();
	bool Sneaking = Player->process && (Player->process->GetMovementFlags() & kMovementSneak);
	BuildBodyPoints(Points, Axis, Player->pos, (Sneaking ? kSneakingHeight : kStandingHeight) * Player->scale,
		kBodyRadius * Player->scale, kPlayerHeights, kPlayerHeightCount, true);
	for (int i = 0; i < ActorCount; i++)
		BuildBodyPoints(Points + kPlayerPointCount + i * kActorPointCount, Axis, Actors[i].Position,
			kStandingHeight * Actors[i].Scale, kBodyRadius * Actors[i].Scale, kActorHeights, kActorPointCount, false);
	for (int i = kPlayerPointCount + ActorCount * kActorPointCount; i < kMaxPointCount; i++)
		Points[i] = D3DXVECTOR4(Player->pos.x, Player->pos.y, Player->pos.z, 1.0f); // unused pixels

}

// Publishes 0 and logs once on a probe D3D call failure.
static void ProbeFailure() {

	RainHitting = 0;
	ActorExposure.clear();
	if (!LoggedD3DFailure) {
		LoggedD3DFailure = true;
		Logger::Log("RainProbe: probe D3D call failed; IsRainHittingPlayer reports 0 and wet actors use the outdoors test.");
	}

}

// Oldest first; a slot is read only once its event query reports the GPU is done with it. Each target
// is Lockable, so this reads it directly with no GetRenderTargetData stall.
static void ReadFinishedProbes() {

	for (int k = 0; k < kProbeRingSize; k++) {
		RainProbeSlot& Slot = Ring[(NextSlot + k) % kProbeRingSize];
		if (!Slot.Pending) continue;
		if (Slot.Done->GetData(NULL, 0, 0) != S_OK) return;
		D3DLOCKED_RECT Locked;
		HRESULT hr = Slot.Target->LockRect(&Locked, NULL, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
		if (hr == D3DERR_WASSTILLDRAWING) return;
		Slot.Pending = false;
		if (FAILED(hr)) { ProbeFailure(); continue; }
		float Exposure[kMaxPointCount];
		memcpy(Exposure, Locked.pBits, sizeof(Exposure));
		Slot.Target->UnlockRect();
		FramesSinceReadback = 0;
		LONG Hit = 0;
		for (int i = 0; i < kPlayerPointCount; i++)
			if (Exposure[i] > 0.5f) Hit = 1;
		RainHitting = Hit;
		ActorExposure.clear();
		for (int a = 0; a < Slot.ActorCount; a++) {
			const float* Points = Exposure + kPlayerPointCount + a * kActorPointCount;
			ActorExposure[Slot.ActorRefIDs[a]] = Points[0] > 0.5f || Points[1] > 0.5f;
		}
	}

}

void RenderRainProbe(IDirect3DDevice9* Device) {

	if (!ProbeArmed || !ProbeEffect) return;
	ReadFinishedProbes();
	RainProbeSlot& Slot = Ring[NextSlot];
	if (Slot.Pending) return;

	D3DXVECTOR4 Points[kMaxPointCount];
	BuildProbePoints(Points);
	IDirect3DSurface9* Previous = NULL;
	if (FAILED(Device->GetRenderTarget(0, &Previous))) {
		ProbeFailure();
		return;
	}
	if (FAILED(Device->SetRenderTarget(0, Slot.Target))) {
		ProbeFailure();
		Previous->Release();
		return;
	}

	// Unbind depth: the scene's may be multisampled, and D3D9 will not pair it with the non-MS probe target.
	IDirect3DSurface9* DepthStencil = NULL;
	Device->GetDepthStencilSurface(&DepthStencil);
	Device->SetDepthStencilSurface(NULL);

	bool Ok = true;
	ProbeEffect->SetCT();
	if (FAILED(ProbeEffect->Effect->SetVectorArray(ProbePointsHandle, Points, kMaxPointCount))) {
		Ok = false;
	} else {
		UINT Passes;
		if (FAILED(ProbeEffect->Effect->Begin(&Passes, NULL))) {
			Ok = false;
		} else {
			if (SUCCEEDED(ProbeEffect->Effect->BeginPass(0))) {
				if (FAILED(Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2))) Ok = false;
				ProbeEffect->Effect->EndPass();
			} else {
				Ok = false;
			}
			ProbeEffect->Effect->End();
		}
	}

	Device->SetDepthStencilSurface(DepthStencil);
	if (DepthStencil) DepthStencil->Release();
	Device->SetRenderTarget(0, Previous);
	Previous->Release();

	if (!Ok) {
		ProbeFailure();
		return;
	}
	if (FAILED(Slot.Done->Issue(D3DISSUE_END))) {
		ProbeFailure();
		return;
	}
	Slot.ActorCount = ActorCount;
	for (int i = 0; i < ActorCount; i++) Slot.ActorRefIDs[i] = Actors[i].RefID;
	Slot.Pending = true;
	NextSlot = (NextSlot + 1) % kProbeRingSize;

}

bool IsRainHittingPlayer() {

	return TheSettingManager->SettingsPrecipitations.Rain.IsRainHittingPlayer && RainHitting != 0;

}

bool IsRainProbeWorking() {

	return ProbeArmed && ProbeEffect != NULL;

}

bool IsRainReachingPlayer() {

	return RainHitting != 0;

}

void SetRainProbeActors(const RainProbeActor* List, int Count) {

	ActorCount = min(max(Count, 0), RainProbeMaxActors);
	for (int i = 0; i < ActorCount; i++) Actors[i] = List[i];

}

bool GetRainProbeActorExposure(UInt32 RefID, bool* Exposed) {

	std::unordered_map<UInt32, bool>::const_iterator It = ActorExposure.find(RefID);
	if (It == ActorExposure.end()) return false;
	*Exposed = It->second;
	return true;

}
