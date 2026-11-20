#include "OcclusionCullMath.h"
#include <cstdio>
#include <cmath>

using namespace OcclusionCullMath;

static int Failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double va_ = (a), vb_ = (b); if (!(std::fabs(va_ - vb_) <= (eps))) { std::printf("%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n", __FILE__, __LINE__, #a, va_, #b, vb_); ++Failures; } } while (0)

static CullCamera Cam() {
	CullCamera C = {};
	C.Pos[0] = 0; C.Pos[1] = 0; C.Pos[2] = 0;
	C.Forward[0] = 0; C.Forward[1] = 1; C.Forward[2] = 0;
	C.Up[0] = 0; C.Up[1] = 0; C.Up[2] = 1;
	C.Right[0] = 1; C.Right[1] = 0; C.Right[2] = 0;
	C.Left = -1.0f; C.RightEdge = 1.0f;
	C.Bottom = -0.5625f; C.Top = 0.5625f;
	C.Near = 15.0f; C.Far = 400000.0f;
	return C;
}

static RectResult Project(float X, float Y, float Z, float R, QueryRect* Out, float VX = 0, float VY = 0) {
	float Center[3] = { X, Y, Z };
	return ProjectBoundRect(Cam(), Center, R, VX, VY, 1920.0f, 1080.0f, Out);
}

static bool InView(float X, float Y, float Z, float R) {
	float Center[3] = { X, Y, Z };
	return SphereInView(Cam(), Center, R);
}

static void DepthEndpoints() {
	CHECK_NEAR(FarPassDepth(15.0f, 15.0f, 400000.0f), 0.0, 1e-7);
	CHECK_NEAR(FarPassDepth(400000.0f, 15.0f, 400000.0f), 1.0, 1e-6);
}

static void DepthMatchesDeviceRow() {
	CHECK_NEAR(FarPassDepth(990.0f, 15.0f, 400000.0f), 0.9848854, 1e-5);
}

static void CenteredBound() {
	QueryRect R = {};
	CHECK(Project(0, 1000, 0, 10, &R) == Rect_Ok);
	CHECK_NEAR(R.X0, 949.5, 1e-3);
	CHECK_NEAR(R.X1, 970.5, 1e-3);
	CHECK_NEAR(R.Y0, 529.5, 1e-3);
	CHECK_NEAR(R.Y1, 550.5, 1e-3);
	CHECK_NEAR(R.Z, FarPassDepth(990.0f, 15.0f, 400000.0f), 1e-5);
}

static void CrossesNearPlane() {
	QueryRect R = {};
	CHECK(Project(0, 20, 0, 10, &R) == Rect_CrossesNear);
}

static void BehindCamera() {
	QueryRect R = {};
	CHECK(Project(0, -1000, 0, 10, &R) == Rect_CrossesNear);
}

static void OffScreen() {
	QueryRect R = {};
	CHECK(Project(5000, 1000, 0, 10, &R) == Rect_OffScreen);
}

static void ClampsAtScreenEdge() {
	QueryRect R = {};
	CHECK(Project(1000, 1000, 0, 50, &R) == Rect_Ok);
	CHECK(R.X1 == 1919.5f);
	CHECK(R.X0 < 1919.5f);
}

static void SubPixelBoundStillCoversAPixel() {
	QueryRect R = {};
	CHECK(Project(0, 100000, 0, 1, &R) == Rect_Ok);
	CHECK(R.X1 - R.X0 >= 1.0f);
	CHECK(R.Y1 - R.Y0 >= 1.0f);
}

static void ViewportOffset() {
	QueryRect R = {};
	CHECK(Project(0, 1000, 0, 10, &R, 100, 50) == Rect_Ok);
	CHECK_NEAR(R.X0, 1049.5, 1e-3);
	CHECK_NEAR(R.Y0, 579.5, 1e-3);
}

static void SphereInViewCases() {
	CHECK(InView(0, 1000, 0, 10));
	CHECK(!InView(0, -1000, 0, 10));
	CHECK(InView(0, -5, 0, 10));
	CHECK(!InView(5000, 1000, 0, 10));
	CHECK(InView(1100, 1000, 0, 200));
}

static void ClassifyCases() {
	CHECK(ClassifyCandidate(2000, 1024, false, 0, 0) == Mode_Cullable);
	CHECK(ClassifyCandidate(500, 1024, false, 0, 0) == Mode_AlwaysDraw);
	CHECK(ClassifyCandidate(2000, 1024, true, 10, 20) == Mode_AlwaysDraw);
	CHECK(ClassifyCandidate(2000, 1024, true, 30, 20) == Mode_Cullable);
	CHECK(ClassifyCandidate(2000, 1024, false, 10, 20) == Mode_Cullable);
}

static int WorldA = 0;
static int WorldB = 0;

static SlotMeta Slot7() {
	SlotMeta S = {};
	S.Serial = 7;
	S.CameraPos[0] = 0; S.CameraPos[1] = 0; S.CameraPos[2] = 0;
	S.WorldKey = &WorldA;
	S.Valid = true;
	return S;
}

static void ResultsUsableWhenConsecutive() {
	float Cam0[3] = { 0, 0, 0 };
	CHECK(ResultsUsable(Slot7(), 8, Cam0, &WorldA, 128.0f));
}

static void ResultsUnusableWhenStale() {
	float Cam0[3] = { 0, 0, 0 };
	CHECK(!ResultsUsable(Slot7(), 9, Cam0, &WorldA, 128.0f));
	SlotMeta S = Slot7();
	S.Valid = false;
	CHECK(!ResultsUsable(S, 8, Cam0, &WorldA, 128.0f));
}

static void ResultsUnusableAfterCameraCut() {
	float Far[3] = { 0, 129, 0 };
	float Near[3] = { 0, 127, 0 };
	CHECK(!ResultsUsable(Slot7(), 8, Far, &WorldA, 128.0f));
	CHECK(ResultsUsable(Slot7(), 8, Near, &WorldA, 128.0f));
}

static void ResultsUnusableAcrossWorlds() {
	float Cam0[3] = { 0, 0, 0 };
	CHECK(!ResultsUsable(Slot7(), 8, Cam0, &WorldB, 128.0f));
}

int main() {
	DepthEndpoints();
	DepthMatchesDeviceRow();
	CenteredBound();
	CrossesNearPlane();
	BehindCamera();
	OffScreen();
	ClampsAtScreenEdge();
	SubPixelBoundStillCoversAPixel();
	ViewportOffset();
	SphereInViewCases();
	ClassifyCases();
	ResultsUsableWhenConsecutive();
	ResultsUnusableWhenStale();
	ResultsUnusableAfterCameraCut();
	ResultsUnusableAcrossWorlds();
	std::printf("%d failures\n", Failures);
	return Failures ? 1 : 0;
}
