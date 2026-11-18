#include "GrassTrailMap.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace GrassTrailMap;

static int Failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double va_ = (a), vb_ = (b); if (!(std::fabs(va_ - vb_) <= (eps))) { std::printf("%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n", __FILE__, __LINE__, #a, va_, #b, vb_); ++Failures; } } while (0)

static const float RT = 80.0f;
static const float RG = 35.0f;

static void Begin(Map& M) {
	M.Reset();
	M.BeginFrame(0, 0, 10.0, 3.5f, 0.8f);
}

static float Smooth01(float T) { return T * T * (3.0f - 2.0f * T); }

static void SpringValues() {
	CHECK(SpringWeight(0, 3.5f, 0.8f) == 1.0f);
	CHECK_NEAR(SpringWeight(1.75f, 3.5f, 0), 0.353553, 1e-4);
	CHECK_NEAR(SpringWeight(2.625f, 3.5f, 0.8f), -0.199921, 1e-4);
	CHECK(SpringWeight(3.5f, 3.5f, 0.8f) == 0.0f);
	CHECK(SpringWeight(-1, 3.5f, 0.8f) == 0.0f);
}

static void StampCentreFull() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	CHECK_NEAR(M.CurrentWeight(Layer_Tall, 4, 4), 1.0, 1e-3);
	CHECK_NEAR(M.CurrentWeight(Layer_Ground, 4, 4), 1.0, 1e-3);
}

static void StampFalloffMatchesShader() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	CellValue T = M.Decode(Layer_Tall, 44, 4);
	CHECK_NEAR(T.Peak, 1.0 - Smooth01(0.5f), 2e-3);
	CHECK_NEAR(T.VecX, 0.5, 2e-3);
	CHECK_NEAR(T.VecY, 0.0, 2e-3);
	CellValue G = M.Decode(Layer_Ground, 12, 4);
	float D = 8.0f;
	float S = 1.0f - D / RG;
	CHECK_NEAR(G.VecX * kPullScale, D * S * S, 0.05);
}

static void RingWritesTimeOnly() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	CellValue Ring = M.Decode(Layer_Tall, 4 + RT + 8, 4);
	CHECK(Ring.Peak == 0.0f);
	CHECK_NEAR(Ring.TimeN, M.NowN(), 1e-4);
	CellValue Far = M.Decode(Layer_Tall, 4 + RT + 40, 4);
	CHECK(Far.TimeN == 0.0f);
}

static void RecoversOverTime() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	M.BeginFrame(0, 0, 10.0 + 1.75, 3.5f, 0.8f);
	CHECK_NEAR(M.CurrentWeight(Layer_Tall, 4, 4), SpringWeight(1.75f, 3.5f, 0.8f), 5e-3);
	M.BeginFrame(0, 0, 10.0 + 3.6, 3.5f, 0.8f);
	CHECK(M.CurrentWeight(Layer_Tall, 4, 4) == 0.0f);
}

static void GroundNeverOvershoots() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	for (int I = 0; I <= 70; ++I) {
		M.BeginFrame(0, 0, 10.0 + I * 0.05, 3.5f, 0.8f);
		CHECK(M.CurrentWeight(Layer_Ground, 4, 4) >= 0.0f);
	}
}

static void ContinuousWake() {
	Map M;
	M.Reset();
	double Now = 10.0;
	float X = 0.0f;
	for (int I = 0; I < 120; ++I) {
		Now += 1.0 / 60.0;
		X += 5.0f;
		M.BeginFrame(X, 4, Now, 8.0f, 0.0f);
		M.Stamp({X, 4, RT, RG});
	}
	for (float W = 100; W <= 500; W += 8)
		CHECK(M.CurrentWeight(Layer_Tall, W, 4) > 0.2f);
}

static void WeakerStampKeepsFreshPeak() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	CellValue Before = M.Decode(Layer_Tall, 4, 4);
	M.BeginFrame(0, 0, 10.01, 3.5f, 0.8f);
	M.Stamp({64, 4, RT, RG});
	CellValue After = M.Decode(Layer_Tall, 4, 4);
	CHECK_NEAR(After.Peak, 1.0, 1e-3);
	CHECK(After.TimeN == Before.TimeN);
}

static void SweepClearsExpired() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	CellValue Before = M.Decode(Layer_Tall, 4, 4);
	double Now = 10.0;
	for (int I = 0; I < 36 + 64; ++I) {
		Now += 0.1;
		M.BeginFrame(0, 0, Now, 3.5f, 0.8f);
	}
	CellValue After = M.Decode(Layer_Tall, 4, 4);
	CHECK(After.Peak == 0.0f);
	CHECK(After.TimeN == Before.TimeN);
	WorldBounds B;
	CHECK(!M.LiveBounds(Layer_Tall, B));
	CHECK(!M.LiveBounds(Layer_Ground, B));
}

static void NoResurrectionAfterWrap() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	double Now = 10.0;
	for (int I = 0; I < 3000; ++I) {
		Now += 0.1;
		M.BeginFrame(0, 0, Now, 3.5f, 0.8f);
	}
	CHECK(M.CurrentWeight(Layer_Tall, 4, 4) == 0.0f);
	CHECK(M.Decode(Layer_Tall, 4, 4).Peak == 0.0f);
}

static void NoResurrectionInBusyRow() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	double Now = 10.0;
	for (int I = 0; I < 3000; ++I) {
		Now += 0.1;
		M.BeginFrame(0, 0, Now, 3.5f, 0.8f);
		M.Stamp({1000, 4, RT, RG});
	}
	CHECK(M.Decode(Layer_Tall, 4, 4).Peak == 0.0f);
	CHECK(M.Decode(Layer_Ground, 4, 4).Peak == 0.0f);
}

static void RecentreKeepsWorldData() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	float Old = M.OriginX();
	M.BeginFrame(600, 0, 10.0, 3.5f, 0.8f);
	CHECK(M.OriginX() != Old);
	CHECK_NEAR(M.Decode(Layer_Tall, 4, 4).Peak, 1.0, 1e-3);
}

static void RecentreClearsEnteringCells() {
	Map M; Begin(M);
	M.Stamp({1500, 4, RT, RG});
	CHECK_NEAR(M.Decode(Layer_Tall, 1500, 4).Peak, 1.0, 1e-3);
	M.BeginFrame(4000, 0, 10.0, 3.5f, 0.8f);
	CellValue V = M.Decode(Layer_Tall, 1500 + 4096, 4);
	CHECK(V.Peak == 0.0f);
	CHECK(V.TimeN == 0.0f);
}

static void ResetClearsEverything() {
	Map M; Begin(M);
	M.Stamp({4, 4, RT, RG});
	M.Reset();
	M.BeginFrame(0, 0, 10.0, 3.5f, 0.8f);
	CHECK(M.Decode(Layer_Tall, 4, 4).Peak == 0.0f);
	std::vector<Rect> R;
	M.CollectDirty(Layer_Tall, R);
	CHECK(R.size() == 1);
	if (R.size() == 1) CHECK(R[0].X0 == 0 && R[0].Y0 == 0 && R[0].X1 == 512 && R[0].Y1 == 512);
}

static void DirtyBandsCoverStampOnce() {
	Map M; Begin(M);
	std::vector<Rect> R;
	M.CollectDirty(Layer_Tall, R);
	std::vector<uint16_t> Snap(M.Data(Layer_Tall), M.Data(Layer_Tall) + kSize * kSize * 4);
	M.Stamp({1000, 500, RT, RG});
	R.clear();
	M.CollectDirty(Layer_Tall, R);
	long Area = 0;
	for (size_t I = 0; I < R.size(); ++I) Area += (long)(R[I].X1 - R[I].X0) * (R[I].Y1 - R[I].Y0);
	CHECK(Area <= 64 * 64);
	int Changed = 0;
	for (int Y = 0; Y < kSize; ++Y)
		for (int X = 0; X < kSize; ++X) {
			bool Diff = false;
			for (int C = 0; C < 4; ++C)
				if (M.Data(Layer_Tall)[(Y * kSize + X) * 4 + C] != Snap[(Y * kSize + X) * 4 + C]) Diff = true;
			if (!Diff) continue;
			++Changed;
			bool Covered = false;
			for (size_t I = 0; I < R.size(); ++I)
				if (X >= R[I].X0 && X < R[I].X1 && Y >= R[I].Y0 && Y < R[I].Y1) Covered = true;
			CHECK(Covered);
		}
	CHECK(Changed > 0);
	std::vector<Rect> Again;
	M.CollectDirty(Layer_Tall, Again);
	CHECK(Again.empty());
}

static void LiveBoundsContainStamp() {
	Map M; Begin(M);
	M.Stamp({1000, 500, RT, RG});
	WorldBounds B;
	CHECK(M.LiveBounds(Layer_Tall, B));
	CHECK(B.MinX <= 920 && B.MaxX >= 1080 && B.MinY <= 420 && B.MaxY >= 580);
}

static const uint16_t* Texel(const Map& M, LayerId L, int Col, int Row) {
	return M.Data(L) + (Row * kSize + Col) * 4;
}

static bool IsNeutral(const uint16_t* T) {
	return T[0] == 32768 && T[1] == 32768 && T[2] == 0;
}

static void ClearedCellsAreNeutral() {
	Map M; Begin(M);
	for (int L = 0; L < Layer_Count; ++L) {
		const uint16_t* T = Texel(M, (LayerId)L, 200, 100);
		CHECK(IsNeutral(T) && T[3] == 0);
	}

	M.Stamp({1004, 4, RT, RG});
	for (int L = 0; L < Layer_Count; ++L) {
		const uint16_t* T = Texel(M, (LayerId)L, 128, 0);
		CHECK(T[0] != 32768 && T[2] != 0);
	}
	uint16_t TimeBefore[Layer_Count];
	for (int L = 0; L < Layer_Count; ++L) TimeBefore[L] = Texel(M, (LayerId)L, 128, 0)[3];
	double Now = 10.0;
	for (int I = 0; I < 36 + 64; ++I) {
		Now += 0.1;
		M.BeginFrame(0, 0, Now, 3.5f, 0.8f);
	}
	for (int L = 0; L < Layer_Count; ++L) {
		const uint16_t* T = Texel(M, (LayerId)L, 128, 0);
		CHECK(IsNeutral(T) && T[3] == TimeBefore[L]);
	}

	Map C; Begin(C);
	C.Stamp({1500, 4, RT, RG});
	for (int L = 0; L < Layer_Count; ++L) CHECK(Texel(C, (LayerId)L, 190, 0)[0] != 32768);
	C.BeginFrame(4000, 0, 10.0, 3.5f, 0.8f);
	for (int L = 0; L < Layer_Count; ++L) {
		const uint16_t* T = Texel(C, (LayerId)L, 190, 0);
		CHECK(IsNeutral(T) && T[3] == 0);
	}

	Map R; Begin(R);
	R.Stamp({4, 1500, RT, RG});
	for (int L = 0; L < Layer_Count; ++L) CHECK(Texel(R, (LayerId)L, 3, 187)[1] == 32768 && Texel(R, (LayerId)L, 3, 187)[0] != 32768);
	R.BeginFrame(0, 4000, 10.0, 3.5f, 0.8f);
	for (int L = 0; L < Layer_Count; ++L) {
		const uint16_t* T = Texel(R, (LayerId)L, 3, 187);
		CHECK(IsNeutral(T) && T[3] == 0);
	}
}

int main() {
	SpringValues();
	StampCentreFull();
	StampFalloffMatchesShader();
	RingWritesTimeOnly();
	RecoversOverTime();
	GroundNeverOvershoots();
	ContinuousWake();
	WeakerStampKeepsFreshPeak();
	SweepClearsExpired();
	NoResurrectionAfterWrap();
	NoResurrectionInBusyRow();
	RecentreKeepsWorldData();
	RecentreClearsEnteringCells();
	ResetClearsEverything();
	DirtyBandsCoverStampOnce();
	LiveBoundsContainStamp();
	ClearedCellsAreNeutral();
	if (Failures == 0) std::printf("GrassTrailMap: all tests passed\n");
	else std::printf("GrassTrailMap: %d failure(s)\n", Failures);
	return Failures;
}
