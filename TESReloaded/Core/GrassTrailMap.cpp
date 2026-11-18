#include "GrassTrailMap.h"

namespace GrassTrailMap {

namespace {

const float kPi = 3.14159265358979f;
const float kStampMargin = 1.5f * kCellSize;
const float kBilinearPad = 0.5f * kCellSize;
const uint16_t kNeutral = 32768;
const int kDirtyChunk = 8;
const int kChunks = kSize / kDirtyChunk;
const uint64_t kAllChunks = ~0ull;

float MinF(float A, float B) { return A < B ? A : B; }
float MaxF(float A, float B) { return A > B ? A : B; }
int MinI(int A, int B) { return A < B ? A : B; }
int MaxI(int A, int B) { return A > B ? A : B; }
float Saturate(float V) { return MinF(MaxF(V, 0.0f), 1.0f); }
float Smooth01(float T) { return T * T * (3.0f - 2.0f * T); }

float SmoothStep(float A, float B, float X) {
	return Smooth01(Saturate((X - A) / (B - A)));
}

int FloorDiv(float World) {
	return (int)std::floor(World / kCellSize);
}

int PosMod(int V) {
	int R = V % kSize;
	return R < 0 ? R + kSize : R;
}

uint16_t Quantise01(float V) {
	return (uint16_t)(Saturate(V) * 65535.0f + 0.5f);
}

uint16_t EncodeSigned(float V) {
	return Quantise01(V * 0.5f + 0.5f);
}

float DecodeSigned(uint16_t V) {
	return V / 65535.0f * 2.0f - 1.0f;
}

void ClearTexel(uint16_t* Texel) {
	Texel[0] = kNeutral;
	Texel[1] = kNeutral;
	Texel[2] = 0;
	Texel[3] = 0;
}

}

float SpringOmega(float Springiness) {
	return kPi * (0.5f + 0.7f * Saturate(Springiness));
}

float SpringWeight(float Age, float Recovery, float Springiness) {
	if (Age < 0.0f) return 0.0f;
	float T = Age / Recovery;
	if (T >= 1.0f) return 0.0f;
	return (1.0f - T) * std::cos(SpringOmega(Springiness) * T);
}

Map::Map() {
	Reset();
}

void Map::ClearLayerAll(Layer& L) {
	L.Cells.resize((size_t)kSize * kSize * 4);
	for (int I = 0; I < kSize * kSize; ++I) ClearTexel(&L.Cells[(size_t)I * 4]);
	L.Live.assign(kSize, 0);
	L.LiveMin.assign(kSize, 0);
	L.LiveMax.assign(kSize, 0);
	L.DirtyMask.assign(kSize, kAllChunks);
}

void Map::Reset() {
	for (int L = 0; L < Layer_Count; ++L) ClearLayerAll(Layers[L]);
	HasWindow = false;
	OriginCellX = 0;
	OriginCellY = 0;
	SweepCursor = 0;
	NowNValue = 0.0f;
	RecoveryValue = 1.0f;
	SpringinessValue = 0.0f;
}

void Map::MarkDirty(Layer& L, int Row, int Col) {
	L.DirtyMask[Row] |= 1ull << (Col / kDirtyChunk);
}

void Map::ClearColumns(int CellStart, int Count) {
	for (int L = 0; L < Layer_Count; ++L) {
		Layer& Lay = Layers[L];
		for (int C = 0; C < Count; ++C) {
			int Col = PosMod(CellStart + C);
			for (int Row = 0; Row < kSize; ++Row) {
				ClearTexel(&Lay.Cells[((size_t)Row * kSize + Col) * 4]);
				MarkDirty(Lay, Row, Col);
			}
		}
	}
}

void Map::ClearRows(int CellStart, int Count) {
	for (int L = 0; L < Layer_Count; ++L) {
		Layer& Lay = Layers[L];
		for (int C = 0; C < Count; ++C) {
			int Row = PosMod(CellStart + C);
			for (int Col = 0; Col < kSize; ++Col) ClearTexel(&Lay.Cells[((size_t)Row * kSize + Col) * 4]);
			Lay.Live[Row] = 0;
			Lay.DirtyMask[Row] = kAllChunks;
		}
	}
}

void Map::ClipLiveSpans(int MinCell, int MaxCell) {
	for (int L = 0; L < Layer_Count; ++L) {
		Layer& Lay = Layers[L];
		for (int Row = 0; Row < kSize; ++Row) {
			if (!Lay.Live[Row]) continue;
			Lay.LiveMin[Row] = MaxI(Lay.LiveMin[Row], MinCell);
			Lay.LiveMax[Row] = MinI(Lay.LiveMax[Row], MaxCell);
			if (Lay.LiveMin[Row] > Lay.LiveMax[Row]) Lay.Live[Row] = 0;
		}
	}
}

float Map::AgeOf(uint16_t TimeValue) const {
	float D = NowNValue - TimeValue / 65535.0f;
	if (D < 0.0f) D += 1.0f;
	return D * kTimePeriod;
}

float Map::Weight(LayerId Id, uint16_t Peak, uint16_t TimeValue) const {
	if (Peak == 0) return 0.0f;
	float W = SpringWeight(AgeOf(TimeValue), RecoveryValue, Id == Layer_Ground ? 0.0f : SpringinessValue);
	return Peak / 65535.0f * W;
}

void Map::SweepLayer(Layer& L) {
	for (int N = 0; N < kSweepRowsPerFrame; ++N) {
		int Row = (SweepCursor + N) % kSize;
		if (!L.Live[Row]) continue;
		int NewMin = 0, NewMax = -1;
		bool Any = false;
		for (int Cell = L.LiveMin[Row]; Cell <= L.LiveMax[Row]; ++Cell) {
			int Col = PosMod(Cell);
			uint16_t* Texel = &L.Cells[((size_t)Row * kSize + Col) * 4];
			if (Texel[2] == 0) continue;
			if (AgeOf(Texel[3]) >= RecoveryValue) {
				Texel[0] = kNeutral;
				Texel[1] = kNeutral;
				Texel[2] = 0;
				MarkDirty(L, Row, Col);
			} else {
				if (!Any) NewMin = Cell;
				NewMax = Cell;
				Any = true;
			}
		}
		L.Live[Row] = Any ? 1 : 0;
		L.LiveMin[Row] = NewMin;
		L.LiveMax[Row] = NewMax;
	}
}

void Map::BeginFrame(float PlayerX, float PlayerY, double Now, float Recovery, float Springiness) {
	double Frac = std::fmod(Now, (double)kTimePeriod);
	if (Frac < 0.0) Frac += kTimePeriod;
	int Ticks = (int)(Frac / kTimePeriod * 65535.0 + 0.5);
	NowNValue = (float)MinI(Ticks, 65535) / 65535.0f;
	RecoveryValue = MinF(MaxF(Recovery, kMinRecovery), kMaxRecovery);
	SpringinessValue = Saturate(Springiness);

	int WantX = FloorDiv(PlayerX) - kSize / 2;
	int WantY = FloorDiv(PlayerY) - kSize / 2;
	if (!HasWindow) {
		OriginCellX = WantX;
		OriginCellY = WantY;
		HasWindow = true;
	} else {
		float CentreX = (OriginCellX + kSize / 2) * kCellSize;
		float CentreY = (OriginCellY + kSize / 2) * kCellSize;
		int ShiftX = std::fabs(PlayerX - CentreX) > kRecentreDistance ? WantX - OriginCellX : 0;
		int ShiftY = std::fabs(PlayerY - CentreY) > kRecentreDistance ? WantY - OriginCellY : 0;
		if (ShiftX >= kSize || ShiftX <= -kSize || ShiftY >= kSize || ShiftY <= -kSize) {
			for (int L = 0; L < Layer_Count; ++L) ClearLayerAll(Layers[L]);
			if (ShiftX != 0) OriginCellX = WantX;
			if (ShiftY != 0) OriginCellY = WantY;
		} else {
			if (ShiftX != 0) {
				if (ShiftX > 0) ClearColumns(OriginCellX + kSize, ShiftX);
				else ClearColumns(WantX, -ShiftX);
				OriginCellX = WantX;
				ClipLiveSpans(OriginCellX, OriginCellX + kSize - 1);
			}
			if (ShiftY != 0) {
				if (ShiftY > 0) ClearRows(OriginCellY + kSize, ShiftY);
				else ClearRows(WantY, -ShiftY);
				OriginCellY = WantY;
			}
		}
	}

	for (int L = 0; L < Layer_Count; ++L) SweepLayer(Layers[L]);
	SweepCursor = (SweepCursor + kSweepRowsPerFrame) % kSize;
}

bool Map::InWindow(int CellX, int CellY) const {
	return HasWindow && CellX >= OriginCellX && CellX < OriginCellX + kSize
		&& CellY >= OriginCellY && CellY < OriginCellY + kSize;
}

void Map::StampLayer(LayerId Id, const Source& S, float Radius) {
	if (Radius <= 0.0f) return;
	Layer& L = Layers[Id];
	float Reach = Radius + kStampMargin;
	int X0 = MaxI(FloorDiv(S.X - Reach), OriginCellX);
	int X1 = MinI(FloorDiv(S.X + Reach), OriginCellX + kSize - 1);
	int Y0 = MaxI(FloorDiv(S.Y - Reach), OriginCellY);
	int Y1 = MinI(FloorDiv(S.Y + Reach), OriginCellY + kSize - 1);
	for (int CY = Y0; CY <= Y1; ++CY) {
		int Row = PosMod(CY);
		for (int CX = X0; CX <= X1; ++CX) {
			float DX = (CX + 0.5f) * kCellSize - S.X;
			float DY = (CY + 0.5f) * kCellSize - S.Y;
			float D = std::sqrt(DX * DX + DY * DY);
			if (D > Reach) continue;

			float VecX, VecY, Peak;
			float DirX = D > 0.0f ? DX / D : 0.0f;
			float DirY = D > 0.0f ? DY / D : 0.0f;
			if (Id == Layer_Tall) {
				float T = Saturate(D / Radius);
				float Influence = 1.0f - Smooth01(T);
				float Fade = SmoothStep(0.0f, 0.3f, T);
				VecX = DirX * Influence * Fade;
				VecY = DirY * Influence * Fade;
				Peak = Influence;
			} else {
				float Spread = Saturate(1.0f - D / Radius);
				float Pull = D * Spread * Spread / kPullScale;
				VecX = DirX * Pull;
				VecY = DirY * Pull;
				Peak = SmoothStep(Radius, 0.35f * Radius, D);
			}

			int Col = PosMod(CX);
			uint16_t* Texel = &L.Cells[((size_t)Row * kSize + Col) * 4];
			uint16_t QPeak = Quantise01(Peak);
			uint16_t NowT = Quantise01(NowNValue);
			if (QPeak > 0) {
				float Cur = Weight(Id, Texel[2], Texel[3]);
				if (QPeak / 65535.0f >= Cur) {
					Texel[0] = EncodeSigned(VecX);
					Texel[1] = EncodeSigned(VecY);
					Texel[2] = QPeak;
					Texel[3] = NowT;
					if (!L.Live[Row]) {
						L.Live[Row] = 1;
						L.LiveMin[Row] = CX;
						L.LiveMax[Row] = CX;
					} else {
						L.LiveMin[Row] = MinI(L.LiveMin[Row], CX);
						L.LiveMax[Row] = MaxI(L.LiveMax[Row], CX);
					}
					MarkDirty(L, Row, Col);
				}
			} else if (Texel[2] == 0) {
				Texel[3] = NowT;
				MarkDirty(L, Row, Col);
			}
		}
	}
}

void Map::Stamp(const Source& S) {
	if (!HasWindow) return;
	StampLayer(Layer_Tall, S, S.TallRadius);
	StampLayer(Layer_Ground, S, S.GroundRadius);
}

void Map::CollectDirty(LayerId Id, std::vector<Rect>& Out) {
	Layer& L = Layers[Id];
	int Start = -1;
	uint64_t Band = 0;
	for (int Row = 0; Row <= kSize; ++Row) {
		uint64_t Mask = Row < kSize ? L.DirtyMask[Row] : 0;
		bool Joins = Start >= 0 && Mask != 0 && (Mask & (Band | (Band << 1) | (Band >> 1))) != 0;
		if (Start >= 0 && !Joins) {
			for (int C = 0; C < kChunks; ++C) {
				if (!(Band >> C & 1)) continue;
				int RunStart = C;
				while (C + 1 < kChunks && (Band >> (C + 1) & 1)) ++C;
				Rect R = {RunStart * kDirtyChunk, Start, (C + 1) * kDirtyChunk, Row};
				Out.push_back(R);
			}
			Start = -1;
			Band = 0;
		}
		if (Mask == 0) continue;
		if (Start < 0) Start = Row;
		Band |= Mask;
		L.DirtyMask[Row] = 0;
	}
}

const uint16_t* Map::Data(LayerId Id) const {
	return Layers[Id].Cells.data();
}

bool Map::LiveBounds(LayerId Id, WorldBounds& Out) const {
	const Layer& L = Layers[Id];
	bool Any = false;
	int MinX = 0, MaxX = 0, MinY = 0, MaxY = 0;
	for (int Row = 0; Row < kSize; ++Row) {
		if (!L.Live[Row]) continue;
		int CY = OriginCellY + PosMod(Row - OriginCellY);
		int X0 = MaxI(L.LiveMin[Row], OriginCellX);
		int X1 = MinI(L.LiveMax[Row], OriginCellX + kSize - 1);
		if (X0 > X1) continue;
		if (!Any) {
			MinX = X0; MaxX = X1; MinY = CY; MaxY = CY;
			Any = true;
		} else {
			MinX = MinI(MinX, X0); MaxX = MaxI(MaxX, X1);
			MinY = MinI(MinY, CY); MaxY = MaxI(MaxY, CY);
		}
	}
	if (!Any) return false;
	float WinMinX = OriginCellX * kCellSize, WinMinY = OriginCellY * kCellSize;
	Out.MinX = MaxF(MinX * kCellSize - kBilinearPad, WinMinX);
	Out.MinY = MaxF(MinY * kCellSize - kBilinearPad, WinMinY);
	Out.MaxX = MinF((MaxX + 1) * kCellSize + kBilinearPad, WinMinX + kSize * kCellSize);
	Out.MaxY = MinF((MaxY + 1) * kCellSize + kBilinearPad, WinMinY + kSize * kCellSize);
	return true;
}

float Map::OriginX() const { return OriginCellX * kCellSize; }
float Map::OriginY() const { return OriginCellY * kCellSize; }
float Map::NowN() const { return NowNValue; }
float Map::Recovery() const { return RecoveryValue; }
float Map::Springiness() const { return SpringinessValue; }

CellValue Map::Decode(LayerId Id, float WorldX, float WorldY) const {
	int CX = FloorDiv(WorldX), CY = FloorDiv(WorldY);
	CellValue V = {0.0f, 0.0f, 0.0f, 0.0f};
	if (!InWindow(CX, CY)) return V;
	const uint16_t* Texel = &Layers[Id].Cells[((size_t)PosMod(CY) * kSize + PosMod(CX)) * 4];
	V.VecX = DecodeSigned(Texel[0]);
	V.VecY = DecodeSigned(Texel[1]);
	V.Peak = Texel[2] / 65535.0f;
	V.TimeN = Texel[3] / 65535.0f;
	return V;
}

float Map::CurrentWeight(LayerId Id, float WorldX, float WorldY) const {
	int CX = FloorDiv(WorldX), CY = FloorDiv(WorldY);
	if (!InWindow(CX, CY)) return 0.0f;
	const uint16_t* Texel = &Layers[Id].Cells[((size_t)PosMod(CY) * kSize + PosMod(CX)) * 4];
	return Weight(Id, Texel[2], Texel[3]);
}

}
