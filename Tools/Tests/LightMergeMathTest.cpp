#include "LightMergeMath.h"
#include <cstdio>
#include <cmath>

using namespace LightMergeMath;

static int Failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double va_ = (a), vb_ = (b); if (!(std::fabs(va_ - vb_) <= (eps))) { std::printf("%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n", __FILE__, __LINE__, #a, va_, #b, vb_); ++Failures; } } while (0)

static const Params P = { 128.0f, 192.0f, 0.5f, 0.5f, 0.02f, 1.0f };

static int KeyA, KeyB, KeyC, KeyD;

static LightInput Torch(const void* Key, float X, float Y = 0.0f, float Z = 0.0f) {
	LightInput L = {};
	L.Key = Key;
	L.Pos[0] = X; L.Pos[1] = Y; L.Pos[2] = Z;
	L.Color[0] = 1.0f; L.Color[1] = 0.6f; L.Color[2] = 0.3f;
	L.Radius = 300.0f;
	L.Eligible = true;
	return L;
}

static const LightOutput& Out(const Clusterer& C, int Index) { return C.Outputs()[Index]; }

static void TwoMatchingSnap() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	C.Update(L, 2, P, 0.0f, true);
	CHECK(Out(C, 0).Role == Role_Leader);
	CHECK(Out(C, 1).Role == Role_Follower);
	CHECK(Out(C, 1).Hidden);
	CHECK(Out(C, 1).LeaderKey == &KeyA);
	CHECK_NEAR(Out(C, 1).Weight, 1.0, 1e-6);
	CHECK_NEAR(Out(C, 1).ColorScale, 0.0, 1e-6);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.41421, 1e-4);
	CHECK_NEAR(Out(C, 0).Pos[0], 50.0, 1e-3);
	CHECK_NEAR(Out(C, 0).Pos[1], 0.0, 1e-3);
	CHECK_NEAR(Out(C, 0).Radius, 350.0, 1e-3);
	CHECK(!Out(C, 0).Hidden);
}

static void ThreeMatchingLeaderNearestCentroid() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100), Torch(&KeyC, 50) };
	C.Update(L, 3, P, 0.0f, true);
	CHECK(Out(C, 2).Role == Role_Leader);
	CHECK(Out(C, 0).Role == Role_Follower);
	CHECK(Out(C, 1).Role == Role_Follower);
	CHECK_NEAR(Out(C, 2).ColorScale, 1.73205, 1e-4);
	CHECK_NEAR(Out(C, 2).Pos[0], 50.0, 1e-3);
	CHECK_NEAR(Out(C, 2).Radius, 350.0, 1e-3);
}

static void ColorMismatchStaysSolo() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	L[1].Color[0] = 0.5f; L[1].Color[1] = 0.5f; L[1].Color[2] = 1.0f;
	C.Update(L, 2, P, 0.0f, true);
	CHECK(Out(C, 0).Role == Role_Solo);
	CHECK(Out(C, 1).Role == Role_Solo);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.0, 1e-6);
	CHECK_NEAR(Out(C, 1).ColorScale, 1.0, 1e-6);
	CHECK_NEAR(Out(C, 1).Pos[0], 100.0, 1e-6);
	CHECK_NEAR(Out(C, 1).Radius, 300.0, 1e-6);
}

static void MatchesWithinTolerance() {
	LightInput A = Torch(&KeyA, 0), B = Torch(&KeyB, 0);
	B.Color[1] = 0.61f;
	B.Radius = 300.5f;
	CHECK(Matches(A, B, P));
	B.Radius = 302.0f;
	CHECK(!Matches(A, B, P));
	B.Radius = 300.0f;
	B.Color[2] = 0.35f;
	CHECK(!Matches(A, B, P));
}

static void IneligibleNeverJoins() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	L[1].Eligible = false;
	C.Update(L, 2, P, 0.0f, true);
	CHECK(Out(C, 0).Role == Role_Solo);
	CHECK(Out(C, 1).Role == Role_Solo);
	CHECK(!Out(C, 1).Hidden);
}

static void FadeIsContinuous() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	C.Update(L, 2, P, 0.25f, false);
	CHECK(Out(C, 1).Role == Role_Follower);
	CHECK_NEAR(Out(C, 1).Weight, 0.5, 1e-6);
	CHECK_NEAR(Out(C, 1).ColorScale, 0.5, 1e-6);
	CHECK(!Out(C, 1).Hidden);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.22474, 1e-4);
	CHECK_NEAR(Out(C, 0).Pos[0], 33.3333, 1e-3);
	C.Update(L, 2, P, 0.25f, false);
	CHECK_NEAR(Out(C, 1).Weight, 1.0, 1e-6);
	CHECK(Out(C, 1).Hidden);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.41421, 1e-4);
}

static void HysteresisHolds() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	C.Update(L, 2, P, 0.0f, true);
	L[1].Pos[0] = 230.0f;
	C.Update(L, 2, P, 0.25f, false);
	CHECK(Out(C, 1).Role == Role_Follower);
	CHECK_NEAR(Out(C, 1).Weight, 1.0, 1e-6);
}

static void FollowerLeavesPastLeaveDistance() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	C.Update(L, 2, P, 0.0f, true);
	L[1].Pos[0] = 260.0f;
	C.Update(L, 2, P, 0.25f, false);
	CHECK(Out(C, 1).Role == Role_Follower);
	CHECK_NEAR(Out(C, 1).Weight, 0.5, 1e-6);
	CHECK(!Out(C, 1).Hidden);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.22474, 1e-4);
	C.Update(L, 2, P, 0.25f, false);
	CHECK(Out(C, 0).Role == Role_Solo);
	CHECK(Out(C, 1).Role == Role_Solo);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.0, 1e-6);
	CHECK_NEAR(Out(C, 1).ColorScale, 1.0, 1e-6);
	CHECK_NEAR(Out(C, 0).Pos[0], 0.0, 1e-6);
}

static void LightVanishes() {
	{
		Clusterer C;
		LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
		C.Update(L, 2, P, 0.0f, true);
		LightInput Only[] = { Torch(&KeyA, 0) };
		C.Update(Only, 1, P, 0.016f, false);
		CHECK(Out(C, 0).Role == Role_Solo);
		CHECK_NEAR(Out(C, 0).ColorScale, 1.0, 1e-6);
		CHECK_NEAR(Out(C, 0).Pos[0], 0.0, 1e-6);
		CHECK_NEAR(Out(C, 0).Radius, 300.0, 1e-6);
	}
	{
		Clusterer C;
		LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
		C.Update(L, 2, P, 0.0f, true);
		LightInput Only[] = { Torch(&KeyB, 100) };
		C.Update(Only, 1, P, 0.016f, false);
		CHECK(Out(C, 0).Role == Role_Solo);
		CHECK_NEAR(Out(C, 0).ColorScale, 1.0, 1e-6);
		CHECK(!Out(C, 0).Hidden);
	}
}

static void SnapOnGap() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	C.Update(L, 2, P, 0.25f, false);
	CHECK_NEAR(Out(C, 1).Weight, 0.5, 1e-6);
	C.Update(L, 2, P, 0.0f, true);
	CHECK_NEAR(Out(C, 1).Weight, 1.0, 1e-6);
	CHECK(Out(C, 1).Hidden);
}

static void LeaderWalksAwayDissolves() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100), Torch(&KeyC, 50), Torch(&KeyD, 1000) };
	C.Update(L, 4, P, 0.0f, true);
	CHECK(Out(C, 2).Role == Role_Leader);
	CHECK(Out(C, 3).Role == Role_Solo);
	L[2].Pos[0] = 350.0f;
	L[3].Pos[0] = 250.0f;
	C.Update(L, 4, P, 0.25f, false);
	CHECK(Out(C, 2).Role == Role_Leader);
	CHECK_NEAR(Out(C, 0).Weight, 0.5, 1e-6);
	CHECK_NEAR(Out(C, 1).Weight, 0.5, 1e-6);
	CHECK_NEAR(Out(C, 2).ColorScale, 1.41421, 1e-4);
	CHECK(Out(C, 3).Role == Role_Solo);
	C.Update(L, 4, P, 0.25f, false);
	CHECK(Out(C, 0).Role == Role_Solo);
	CHECK(Out(C, 1).Role == Role_Solo);
	CHECK_NEAR(Out(C, 0).ColorScale, 1.0, 1e-6);
}

static void LeaderScaleExponent() {
	CHECK_NEAR(LeaderScale(0.0f, 0.5f), 1.0, 1e-6);
	CHECK_NEAR(LeaderScale(3.0f, 0.5f), 2.0, 1e-6);
	CHECK_NEAR(LeaderScale(3.0f, 1.0f), 4.0, 1e-6);
}

static void LeaderVanishesKeepsGroup() {
	Clusterer C;
	LightInput L[] = { Torch(&KeyA, 0), Torch(&KeyB, 100), Torch(&KeyC, 50), Torch(&KeyD, 60) };
	C.Update(L, 4, P, 0.0f, true);
	CHECK(Out(C, 2).Role == Role_Leader);
	LightInput Rest[] = { Torch(&KeyA, 0), Torch(&KeyB, 100), Torch(&KeyD, 60) };
	C.Update(Rest, 3, P, 0.016f, false);
	CHECK(Out(C, 2).Role == Role_Leader);
	CHECK(Out(C, 0).Role == Role_Follower);
	CHECK(Out(C, 1).Role == Role_Follower);
	CHECK_NEAR(Out(C, 0).Weight, 1.0, 1e-6);
	CHECK_NEAR(Out(C, 1).Weight, 1.0, 1e-6);
	CHECK(Out(C, 0).Hidden);
	CHECK_NEAR(Out(C, 2).ColorScale, 1.73205, 1e-4);
}

static void SceneBreakOnlyOnRealChange() {
	int WorldA, WorldB;
	CHECK(IsSceneBreak(nullptr, &WorldA, 16.0, 1000.0));
	CHECK(!IsSceneBreak(&WorldA, &WorldA, 16.0, 1000.0));
	CHECK(IsSceneBreak(&WorldA, &WorldB, 16.0, 1000.0));
	CHECK(IsSceneBreak(&WorldA, &WorldA, 1500.0, 1000.0));
}

static void TrueLightShareMatchesEngine() {
	TrueLight L = { &KeyA, &KeyB, { 0.0f, 0.0f, 0.0f }, 200.0f, 0.8f, false };
	float At[3] = { 100.0f, 0.0f, 0.0f };
	CHECK_NEAR(LightLevelShare(L, At, nullptr), 0.6, 1e-6);
	float Far[3] = { 300.0f, 0.0f, 0.0f };
	CHECK_NEAR(LightLevelShare(L, Far, nullptr), 0.0, 1e-6);
	CHECK_NEAR(LightLevelShare(L, At, &KeyB), 0.0, 1e-6);
}

static void TrueLightViewSurvivesNextPublish() {
	TrueLightTable T;
	TrueLight* W = T.BeginWrite();
	W[0] = { &KeyA, &KeyB, { 0.0f, 0.0f, 0.0f }, 200.0f, 0.8f, true };
	T.Publish(1);
	TrueLightTable::View V = T.Read();
	CHECK(V.Count == 1);
	CHECK(V.Find(&KeyA) != nullptr);
	CHECK(V.Find(&KeyC) == nullptr);
	W = T.BeginWrite();
	W[0] = { &KeyC, &KeyD, { 9.0f, 9.0f, 9.0f }, 50.0f, 0.1f, false };
	T.Publish(1);
	CHECK(V.Find(&KeyA) != nullptr);
	CHECK_NEAR(V.Find(&KeyA)->Radius, 200.0, 1e-6);
	CHECK(T.Read().Find(&KeyC) != nullptr);
	CHECK(T.Read().Find(&KeyA) == nullptr);
}

static void SanitizeKeepsLeaveAtLeastJoin() {
	Params Bad = { 200.0f, 150.0f, 0.5f, 0.5f, 0.02f, 1.0f };
	CHECK_NEAR(Sanitize(Bad).LeaveDistance, 200.0, 1e-6);
	CHECK_NEAR(Sanitize(Bad).JoinDistance, 200.0, 1e-6);
	CHECK_NEAR(Sanitize(P).LeaveDistance, 192.0, 1e-6);
}

static void SnapsForASecondAfterABreak() {
	CHECK(IsSnapWindow(0.0, 1000.0));
	CHECK(IsSnapWindow(300.0, 1000.0));
	CHECK(!IsSnapWindow(1000.0, 1000.0));
	CHECK(!IsSnapWindow(5000.0, 1000.0));
}

static void LateLightSnapsInsideWindow() {
	Clusterer C;
	LightInput One[] = { Torch(&KeyA, 0) };
	C.Update(One, 1, P, 0.0f, true);
	LightInput Two[] = { Torch(&KeyA, 0), Torch(&KeyB, 100) };
	C.Update(Two, 2, P, 0.016f, IsSnapWindow(300.0, 1000.0));
	CHECK(Out(C, 1).Hidden);
}

static void TrueLightReachMatchesEngine() {
	TrueLight L = { &KeyA, &KeyB, { 0.0f, 0.0f, 0.0f }, 200.0f, 0.8f, true };
	float Near[3] = { 250.0f, 0.0f, 0.0f };
	CHECK(ReachesSphere(L, Near, 60.0f));
	float Far[3] = { 300.0f, 0.0f, 0.0f };
	CHECK(!ReachesSphere(L, Far, 60.0f));
	L.PlanesActive = true;
	L.PlaneMask = 1;
	L.Planes[0][0] = 1.0f;
	float Behind[3] = { -100.0f, 0.0f, 0.0f };
	CHECK(!ReachesSphere(L, Behind, 50.0f));
	float Straddle[3] = { -20.0f, 0.0f, 0.0f };
	CHECK(ReachesSphere(L, Straddle, 50.0f));
	CHECK(ReachesSphere(L, Far, 60.0f));
	L.PlaneMask = 0;
	CHECK(ReachesSphere(L, Behind, 50.0f));
}

int main() {
	TwoMatchingSnap();
	ThreeMatchingLeaderNearestCentroid();
	ColorMismatchStaysSolo();
	MatchesWithinTolerance();
	IneligibleNeverJoins();
	FadeIsContinuous();
	HysteresisHolds();
	FollowerLeavesPastLeaveDistance();
	LightVanishes();
	SnapOnGap();
	LeaderWalksAwayDissolves();
	LeaderScaleExponent();
	LeaderVanishesKeepsGroup();
	SceneBreakOnlyOnRealChange();
	TrueLightShareMatchesEngine();
	TrueLightViewSurvivesNextPublish();
	SanitizeKeepsLeaveAtLeastJoin();
	SnapsForASecondAfterABreak();
	LateLightSnapsInsideWindow();
	TrueLightReachMatchesEngine();
	if (Failures) { std::printf("LightMergeMath: %d check(s) failed\n", Failures); return 1; }
	std::printf("LightMergeMath: all tests passed\n");
	return 0;
}
