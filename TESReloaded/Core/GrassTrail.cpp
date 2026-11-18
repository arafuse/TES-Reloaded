#include <unordered_map>
#include <vector>
#include <algorithm>
#include "GrassTrail.h"
#include "GrassTrailMap.h"

namespace GrassTrail {

namespace {

using GrassTrailMap::Layer_Count;
using GrassTrailMap::kSize;

const double kMaxDeltaSeconds = 0.1;
const float  kTeleportDist = 1024.0f;
const float  kStampMoveDist = 2.0f;
const float  kStampMinInterval = 0.033f;
const size_t kMaxCachedStampers = 256;

struct LayerTextures {
	IDirect3DTexture9* Default = NULL;
	IDirect3DTexture9* Staging = NULL;
	IDirect3DSurface9* DefaultSurface = NULL;
	IDirect3DSurface9* StagingSurface = NULL;
	bool Enabled = false;
};

struct LastStamp { float X, Y, T; };

GrassTrailMap::Map* TrailMap = NULL;
LayerTextures Layers[Layer_Count];
std::unordered_map<UInt32, LastStamp> LastStamps;
std::vector<GrassTrailMap::Rect> DirtyRects;
const void* LastSpace = NULL;
bool HavePlayerPos = false;
float LastPlayerX = 0.0f, LastPlayerY = 0.0f;
double LastMs = -1.0;
double TrailClock = 0.0;

bool CreateLayer(IDirect3DDevice9* Device, LayerTextures& L, const char* Name) {
	if (FAILED(Device->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_DEFAULT, &L.Default, NULL)) ||
		FAILED(Device->CreateTexture(kSize, kSize, 1, 0, D3DFMT_A16B16G16R16, D3DPOOL_SYSTEMMEM, &L.Staging, NULL)) ||
		FAILED(L.Default->GetSurfaceLevel(0, &L.DefaultSurface)) ||
		FAILED(L.Staging->GetSurfaceLevel(0, &L.StagingSurface))) {
		Logger::Log("Grass trail map: could not create the %s texture", Name);
		if (L.StagingSurface) L.StagingSurface->Release();
		if (L.DefaultSurface) L.DefaultSurface->Release();
		if (L.Staging) L.Staging->Release();
		if (L.Default) L.Default->Release();
		L = LayerTextures();
		return false;
	}
	return true;
}

bool SupportsVertexTextures(IDirect3DDevice9* Device) {
	IDirect3D9* D3D = NULL;
	D3DDEVICE_CREATION_PARAMETERS Params;
	D3DDISPLAYMODE Mode;
	bool Supported = false;
	if (SUCCEEDED(Device->GetDirect3D(&D3D)) && SUCCEEDED(Device->GetCreationParameters(&Params)) &&
		SUCCEEDED(D3D->GetAdapterDisplayMode(Params.AdapterOrdinal, &Mode))) {
		Supported = SUCCEEDED(D3D->CheckDeviceFormat(Params.AdapterOrdinal, Params.DeviceType, Mode.Format,
			D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16));
	}
	if (D3D) D3D->Release();
	return Supported;
}

void ResetAll() {
	TrailMap->Reset();
	LastStamps.clear();
}

void Upload(GrassTrailMap::LayerId Id) {
	LayerTextures& L = Layers[Id];
	if (!L.Enabled) return;
	DirtyRects.clear();
	TrailMap->CollectDirty(Id, DirtyRects);
	const uint16_t* Data = TrailMap->Data(Id);
	for (const GrassTrailMap::Rect& R : DirtyRects) {
		RECT Region = { R.X0, R.Y0, R.X1, R.Y1 };
		D3DLOCKED_RECT Locked;
		if (FAILED(L.StagingSurface->LockRect(&Locked, &Region, 0))) continue;
		const size_t RowBytes = (size_t)(R.X1 - R.X0) * 4 * sizeof(uint16_t);
		for (int Row = R.Y0; Row < R.Y1; Row++) {
			memcpy((BYTE*)Locked.pBits + (size_t)(Row - R.Y0) * Locked.Pitch,
				Data + ((size_t)Row * kSize + R.X0) * 4, RowBytes);
		}
		L.StagingSurface->UnlockRect();
		POINT Dest = { R.X0, R.Y0 };
		TheRenderManager->device->UpdateSurface(L.StagingSurface, &Region, L.DefaultSurface, &Dest);
	}
}

bool Reaches(GrassTrailMap::LayerId Id, float MinX, float MinY, float MaxX, float MaxY) {
	if (!Layers[Id].Enabled || !TrailMap) return false;
	GrassTrailMap::WorldBounds B;
	if (!TrailMap->LiveBounds(Id, B)) return false;
	return MaxX >= B.MinX && MinX <= B.MaxX && MaxY >= B.MinY && MinY <= B.MaxY;
}

}

void Create(IDirect3DDevice9* Device) {
	if (!TrailMap) TrailMap = new GrassTrailMap::Map();
	Layers[GrassTrailMap::Layer_Ground].Enabled = CreateLayer(Device, Layers[GrassTrailMap::Layer_Ground], "ground");
	bool Created = CreateLayer(Device, Layers[GrassTrailMap::Layer_Tall], "tall");
	if (Created && !SupportsVertexTextures(Device)) {
		Logger::Log("Grass trail map: vertex texture fetch unsupported, tall-grass bending off");
		Created = false;
	}
	Layers[GrassTrailMap::Layer_Tall].Enabled = Created;
}

void Update(const Stamper* Stampers, int Count, float PlayerX, float PlayerY, const void* Space, Constants& Out) {
	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_GrassTrail);
	if (!TrailMap) return;

	double NowMs = TheFrameRateManager->GetPerformance();
	double Dt = LastMs < 0.0 ? 0.0 : (NowMs - LastMs) * 0.001;
	LastMs = NowMs;
	TrailClock += std::clamp(Dt, 0.0, kMaxDeltaSeconds);

	if (Space != LastSpace) {
		LastSpace = Space;
		ResetAll();
	}
	if (HavePlayerPos) {
		float Jx = PlayerX - LastPlayerX, Jy = PlayerY - LastPlayerY;
		if (Jx * Jx + Jy * Jy > kTeleportDist * kTeleportDist) ResetAll();
	}
	LastPlayerX = PlayerX;
	LastPlayerY = PlayerY;
	HavePlayerPos = true;

	const SettingsGrassStruct& Settings = TheSettingManager->SettingsGrass;
	TrailMap->BeginFrame(PlayerX, PlayerY, TrailClock, Settings.CollisionRecoveryTime, Settings.CollisionSpringiness);

	if (LastStamps.size() > kMaxCachedStampers) LastStamps.clear();
	for (int i = 0; i < Count; i++) {
		const Stamper& S = Stampers[i];
		auto It = LastStamps.find(S.RefId);
		if (It != LastStamps.end()) {
			float Dx = S.X - It->second.X, Dy = S.Y - It->second.Y;
			if (Dx * Dx + Dy * Dy < kStampMoveDist * kStampMoveDist && TrailClock - It->second.T < kStampMinInterval) continue;
		}
		LastStamps[S.RefId] = { S.X, S.Y, (float)TrailClock };
		TrailMap->Stamp({ S.X, S.Y, Settings.CollisionRadius * S.RadiusScale, Settings.GroundcoverPressRadius * S.RadiusScale });
	}

	Upload(GrassTrailMap::Layer_Tall);
	Upload(GrassTrailMap::Layer_Ground);

	Out.Window = D3DXVECTOR4(TrailMap->OriginX(), TrailMap->OriginY(), GrassTrailMap::kSize * GrassTrailMap::kCellSize, 1.0f / (GrassTrailMap::kSize * GrassTrailMap::kCellSize));
	Out.Time = D3DXVECTOR4(TrailMap->NowN(), GrassTrailMap::kTimePeriod, 1.0f / TrailMap->Recovery(), GrassTrailMap::SpringOmega(TrailMap->Springiness()));
}

bool TallEnabled() { return Layers[GrassTrailMap::Layer_Tall].Enabled; }

bool GroundEnabled() { return Layers[GrassTrailMap::Layer_Ground].Enabled; }

bool TallReaches(float MinX, float MinY, float MaxX, float MaxY) { return Reaches(GrassTrailMap::Layer_Tall, MinX, MinY, MaxX, MaxY); }

bool GroundReaches(float MinX, float MinY, float MaxX, float MaxY) { return Reaches(GrassTrailMap::Layer_Ground, MinX, MinY, MaxX, MaxY); }

IDirect3DTexture9* TallTexture() { return TallEnabled() ? Layers[GrassTrailMap::Layer_Tall].Default : NULL; }

IDirect3DTexture9* GroundTexture() { return GroundEnabled() ? Layers[GrassTrailMap::Layer_Ground].Default : NULL; }

}
