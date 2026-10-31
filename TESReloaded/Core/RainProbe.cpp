#include "RainProbe.h"

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
static const int	kReadbackTimeoutFrames	= 30;

/// One in-flight probe: its render target and the event query issued after it was drawn.
struct RainProbeSlot {
	IDirect3DSurface9*	Target;
	IDirect3DQuery9*	Done;
	bool				Pending;
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
		if (FAILED(Device->CreateRenderTarget(kProbePointCount, 1, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, TRUE, &Ring[i].Target, NULL))) return false;
		if (FAILED(Device->CreateQuery(D3DQUERYTYPE_EVENT, &Ring[i].Done))) return false;
	}
	return true;

}

// The same gates RenderEffects uses to draw rain, so the probe runs exactly when rain is drawn.
static bool RainDrawn() {

	return TheSettingManager->SettingsMain.Effects.Precipitations && TheShaderManager->RainEffect
		&& Player && Player->IsExteriorLike() && TheShaderManager->ShaderConst.Precipitations.RainData.x > 0.0f;

}

static void DiscardProbes() {

	for (int i = 0; i < kProbeRingSize; i++) Ring[i].Pending = false;
	FramesSinceReadback = 0;
	RainHitting = 0;

}

void UpdateRainProbe() {

	if (!TheSettingManager->SettingsPrecipitations.Rain.IsRainHittingPlayer) {
		ReleaseProbeResources();
		ProbeFailed = false;
		ProbeArmed = false;
		LoggedD3DFailure = false;
		DiscardProbes();
		return;
	}
	ProbeArmed = RainDrawn();
	if (!ProbeArmed) {
		DiscardProbes();
		return;
	}
	if (!ProbeEffect && !ProbeFailed && !CreateProbeResources(TheRenderManager->device)) {
		Logger::Log("RainProbe: probe resources unavailable (is Precipitations\\RainProbe.fx compiled?); IsRainHittingPlayer stays 0.");
		ReleaseProbeResources();
		ProbeFailed = true;
	}

	// Watchdog: no readback for a while (a device error, or the rain pass skipped while the camera is
	// underwater) drops the answer to 0 instead of leaving it stale.
	if (++FramesSinceReadback > kReadbackTimeoutFrames) DiscardProbes();

}

// Each point is pushed up the fall line (against the rain) until it has left a body capsule, so it
// tests whether rain reaches that spot on the player's surface. Mirrors Rain.fx's slant.
static void BuildProbePoints(D3DXVECTOR4* Points) {

	const ShaderConstants& Const = TheShaderManager->ShaderConst;
	float Slant = Const.Precipitations.SlantData.x * Const.WindData.z;
	D3DXVECTOR3 Axis(-Slant * Const.WindData.x, -Slant * Const.WindData.y, 1.0f);
	D3DXVec3Normalize(&Axis, &Axis);

	bool Sneaking = Player->process && (Player->process->GetMovementFlags() & kMovementSneak);
	float Height = (Sneaking ? kSneakingHeight : kStandingHeight) * Player->scale;
	float Radius = kBodyRadius * Player->scale;
	float Horizontal = sqrtf(Axis.x * Axis.x + Axis.y * Axis.y);
	float SideX = 1.0f;
	float SideY = 0.0f;
	if (Horizontal > 1e-3f) {
		SideX = -Axis.y / Horizontal;
		SideY = Axis.x / Horizontal;
	}
	for (int h = 0; h < kProbeHeightCount; h++) {
		float Z = Height * kProbeHeights[h];
		float Exit = Horizontal > 1e-3f ? Radius / Horizontal : FLT_MAX;
		Exit = min(Exit, (Height - Z) / Axis.z) + kExitMargin;
		for (int s = 0; s < 2; s++) {
			float Side = s ? kLateralOffset : -kLateralOffset;
			D3DXVECTOR4& P = Points[h * 2 + s];
			P.x = Player->pos.x + SideX * Side + Axis.x * Exit;
			P.y = Player->pos.y + SideY * Side + Axis.y * Exit;
			P.z = Player->pos.z + Z + Axis.z * Exit;
			P.w = 1.0f;
		}
	}

}

// Publishes 0 and logs once on a probe D3D call failure.
static void ProbeFailure() {

	RainHitting = 0;
	if (!LoggedD3DFailure) {
		LoggedD3DFailure = true;
		Logger::Log("RainProbe: probe D3D call failed; IsRainHittingPlayer reports 0.");
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
		float Exposure[kProbePointCount];
		memcpy(Exposure, Locked.pBits, sizeof(Exposure));
		Slot.Target->UnlockRect();
		FramesSinceReadback = 0;
		LONG Hit = 0;
		for (int i = 0; i < kProbePointCount; i++)
			if (Exposure[i] > 0.5f) Hit = 1;
		RainHitting = Hit;
	}

}

void RenderRainProbe(IDirect3DDevice9* Device) {

	if (!ProbeArmed || !ProbeEffect) return;
	ReadFinishedProbes();
	RainProbeSlot& Slot = Ring[NextSlot];
	if (Slot.Pending) return;

	D3DXVECTOR4 Points[kProbePointCount];
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
	if (FAILED(ProbeEffect->Effect->SetVectorArray(ProbePointsHandle, Points, kProbePointCount))) {
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
	Slot.Pending = true;
	NextSlot = (NextSlot + 1) % kProbeRingSize;

}

bool IsRainHittingPlayer() {

	return TheSettingManager->SettingsPrecipitations.Rain.IsRainHittingPlayer && RainHitting != 0;

}
