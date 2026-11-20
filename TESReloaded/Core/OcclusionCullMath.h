#pragma once

#include <cmath>

/// Engine-free math for the occlusion culler (OcclusionCulling.cpp): the far pass's camera, the
/// conservative bound-to-pixel-rect projection its queries draw, and the rules for which results
/// may be used. Proven by Tools/Tests/OcclusionCullMathTest.cpp.
namespace OcclusionCullMath {

	/// The camera as the far pass rasterised it. Axes are world-space unit vectors; Left, RightEdge,
	/// Top and Bottom are the NiFrustum's view-space slopes at unit distance; Near and Far are the
	/// far pass's depth range (ShellBoundary and RealFar while the near shell is active).
	struct CullCamera {
		float Pos[3];
		float Forward[3];
		float Up[3];
		float Right[3];
		float Left, RightEdge, Top, Bottom;
		float Near, Far;
	};

	/// A pre-transformed screen rect in pixels, and the depth it is tested at.
	struct QueryRect { float X0, Y0, X1, Y1, Z; };

	/// Outcome of projecting a bound: a usable rect, a bound reaching in front of the near plane
	/// (never queried, so drawn), or a bound with no pixel on screen.
	enum RectResult { Rect_Ok, Rect_CrossesNear, Rect_OffScreen };

	/// Whether a candidate may be culled, or is always drawn (and so is not queried at all).
	enum CandidateMode { Mode_AlwaysDraw, Mode_Cullable };

	/// What a query slot was issued for, to decide whether its results still apply.
	struct SlotMeta {
		unsigned	Serial;
		float		CameraPos[3];
		const void*	WorldKey;
		bool		Valid;
	};

	inline float Dot3(const float A[3], const float B[3]) { return A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; }

	/// Device depth of a point ViewZ units in front of the eye, as RenderManager::DepthRow encodes it.
	inline float FarPassDepth(float ViewZ, float Near, float Far) {
		return Far / (Far - Near) * (1.0f - Near / ViewZ);
	}

	/// True when a sphere touches the view: in front of the eye plane and inside all four side planes.
	/// There is no far plane, as in ShadowManager's BuildViewCullPlanes.
	inline bool SphereInView(const CullCamera& C, const float Center[3], float Radius) {
		float D[3] = { Center[0] - C.Pos[0], Center[1] - C.Pos[1], Center[2] - C.Pos[2] };
		float Normals[5][3];
		for (int i = 0; i < 3; i++) {
			Normals[0][i] = C.Forward[i];
			Normals[1][i] = C.Right[i] - C.Left * C.Forward[i];
			Normals[2][i] = C.RightEdge * C.Forward[i] - C.Right[i];
			Normals[3][i] = C.Up[i] - C.Bottom * C.Forward[i];
			Normals[4][i] = C.Top * C.Forward[i] - C.Up[i];
		}
		for (int p = 0; p < 5; p++) {
			float Length = std::sqrt(Dot3(Normals[p], Normals[p]));
			if (Dot3(Normals[p], D) < -Radius * Length) return false;
		}
		return true;
	}

	/// Projects the bound's world AABB (Center ± Radius) to one conservative pixel rect at its nearest
	/// depth. The rect is widened to whole pixels, so even a sub-pixel bound covers a pixel centre,
	/// then clamped to the viewport.
	inline RectResult ProjectBoundRect(const CullCamera& C, const float Center[3], float Radius,
	                                   float ViewportX, float ViewportY, float ViewportW, float ViewportH, QueryRect* Out) {
		float MinX = 0, MaxX = 0, MinY = 0, MaxY = 0, MinZ = 0;
		for (int i = 0; i < 8; i++) {
			float D[3] = {
				Center[0] + ((i & 1) ? Radius : -Radius) - C.Pos[0],
				Center[1] + ((i & 2) ? Radius : -Radius) - C.Pos[1],
				Center[2] + ((i & 4) ? Radius : -Radius) - C.Pos[2],
			};
			float Z = Dot3(C.Forward, D);
			if (Z < C.Near) return Rect_CrossesNear;
			float NdcX = (2.0f * Dot3(C.Right, D) / Z - (C.RightEdge + C.Left)) / (C.RightEdge - C.Left);
			float NdcY = (2.0f * Dot3(C.Up, D) / Z - (C.Top + C.Bottom)) / (C.Top - C.Bottom);
			float SX = ViewportX + (NdcX + 1.0f) * 0.5f * ViewportW;
			float SY = ViewportY + (1.0f - NdcY) * 0.5f * ViewportH;
			if (i == 0) { MinX = MaxX = SX; MinY = MaxY = SY; MinZ = Z; continue; }
			if (SX < MinX) MinX = SX;
			if (SX > MaxX) MaxX = SX;
			if (SY < MinY) MinY = SY;
			if (SY > MaxY) MaxY = SY;
			if (Z < MinZ) MinZ = Z;
		}
		float X0 = std::floor(MinX) - 0.5f;
		float X1 = std::ceil(MaxX) + 0.5f;
		float Y0 = std::floor(MinY) - 0.5f;
		float Y1 = std::ceil(MaxY) + 0.5f;
		float LoX = ViewportX - 0.5f, HiX = ViewportX + ViewportW - 0.5f;
		float LoY = ViewportY - 0.5f, HiY = ViewportY + ViewportH - 0.5f;
		if (X0 < LoX) X0 = LoX;
		if (X1 > HiX) X1 = HiX;
		if (Y0 < LoY) Y0 = LoY;
		if (Y1 > HiY) Y1 = HiY;
		if (X1 <= X0 || Y1 <= Y0) return Rect_OffScreen;
		Out->X0 = X0; Out->X1 = X1; Out->Y0 = Y0; Out->Y1 = Y1;
		Out->Z = FarPassDepth(MinZ, C.Near, C.Far);
		return Rect_Ok;
	}

	/// Near bounds, and bounds reaching below the water plane while there is water (they show through
	/// refraction), are always drawn; everything else may be culled.
	inline CandidateMode ClassifyCandidate(float DistToSurface, float AlwaysDrawDistance, bool HasWater, float BoundBottomZ, float WaterHeight) {
		if (DistToSurface < AlwaysDrawDistance) return Mode_AlwaysDraw;
		if (HasWater && BoundBottomZ < WaterHeight) return Mode_AlwaysDraw;
		return Mode_Cullable;
	}

	/// True only for the immediately previous far pass, in the same world, with the camera moved no
	/// more than CutDistance since then. Anything else draws everything for a frame.
	inline bool ResultsUsable(const SlotMeta& Slot, unsigned CurrentSerial, const float CameraPos[3], const void* WorldKey, float CutDistance) {
		if (!Slot.Valid || Slot.Serial + 1 != CurrentSerial || Slot.WorldKey != WorldKey) return false;
		float D[3] = { CameraPos[0] - Slot.CameraPos[0], CameraPos[1] - Slot.CameraPos[1], CameraPos[2] - Slot.CameraPos[2] };
		return Dot3(D, D) <= CutDistance * CutDistance;
	}

}
