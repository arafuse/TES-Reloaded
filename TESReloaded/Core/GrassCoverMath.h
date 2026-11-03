#pragma once

#include "TreeCoverMath.h"

/// One tall grass clump near the player, after the player's collision push, in world units (z up).
struct GrassClump {
	float X, Y;			///< Centre, moved outward by the player's collision push
	float BaseZ;		///< Root height (the instance z)
	float TopZ;			///< Top height, lowered by the player's collision flattening
	float HalfWidth;	///< Horizontal half-width of the clump's cylinder
	float Weight;		///< How much the clump blocks, 0-1 (opacity × tallness)
};

/// The settings the grass cover math needs, in world units.
struct GrassCoverParams {
	float MinHeight;		///< Clump top above the player's feet where cover begins
	float FullHeight;		///< Clump top above the player's feet where a clump counts fully
	float Opacity;			///< Weight of one full-height clump
	float CollisionRadius;	///< The player's live grass collision radius; 0 when grass collision is off
	float PushStrength;		///< Maximum outward push at the collision ring (CollisionStrength)
	float FlattenStrength;	///< Maximum downward push at the collision centre (CollisionFlattenStrength)
};

/// Angular sectors around the player that GrassCoverAt averages.
static const int	kGrassCoverSectors	= 16;
/// Pi, for the sector angles.
static const float	kGrassCoverPi		= 3.14159265f;

/// HLSL smoothstep(E0, E1, X); E0 > E1 gives the falling edge, and equal edges a step at E0.
inline float GrassCoverSmoothstep(float E0, float E1, float X) {
	if (E1 == E0) return X >= E0 ? 1.0f : 0.0f;
	return TreeCoverSmoothstep((X - E0) / (E1 - E0));
}

/// Builds the clump for one grass instance (IX, IY, IZ) seen from the player's feet (PX, PY, PZ). The
/// collision push and flattening mirror GrassCollisionDisplacement in GrassCollision.hlsl for the player
/// source at full tip weight. Returns false when the clump gives no cover.
inline bool GrassCoverClumpOf(float PX, float PY, float PZ, float IX, float IY, float IZ, float ModelHeight, float HeightFactor, float HalfWidth, const GrassCoverParams& Params, GrassClump& Out) {
	float DX = IX - PX;
	float DY = IY - PY;
	float D = sqrtf(DX * DX + DY * DY);
	float X = IX;
	float Y = IY;
	float Top = IZ + ModelHeight * HeightFactor;
	if (Params.CollisionRadius > 0.0f) {
		float T = TreeCoverSaturate(D / Params.CollisionRadius);
		float Influence = GrassCoverSmoothstep(1.0f, 0.0f, T);
		float Push = Influence * GrassCoverSmoothstep(0.0f, 0.3f, T) * Params.PushStrength;
		if (D > 0.001f) {
			X += DX / D * Push;
			Y += DY / D * Push;
		}
		Top -= Influence * Params.FlattenStrength;
	}
	float Weight = TreeCoverSaturate(Params.Opacity * GrassCoverSmoothstep(Params.MinHeight, Params.FullHeight, Top - PZ));
	if (!(Weight > 0.0f)) return false;
	Out.X = X;
	Out.Y = Y;
	Out.BaseZ = IZ;
	Out.TopZ = Top;
	Out.HalfWidth = HalfWidth;
	Out.Weight = Weight;
	return true;
}

/// Adds a clump at squared distance DistSq to a list of at most Max (≥ 1) clumps, keeping the nearest.
/// DistSqs runs parallel to Clumps; Count is the list's current length.
inline void GrassCoverKeepNearest(GrassClump* Clumps, float* DistSqs, int& Count, int Max, const GrassClump& Clump, float DistSq) {
	int Slot = Count;
	if (Count < Max) {
		Count++;
	}
	else {
		Slot = 0;
		for (int i = 1; i < Count; i++) {
			if (DistSqs[i] > DistSqs[Slot]) Slot = i;
		}
		if (DistSq >= DistSqs[Slot]) return;
	}
	Clumps[Slot] = Clump;
	DistSqs[Slot] = DistSq;
}

/// Returns how fully the clumps surround the point (PX, PY), 0-1: the mean over kGrassCoverSectors
/// sectors of 1 − Π(1 − Weight) over the clumps each sector sees. A clump covers the sectors within its
/// angular half-width atan2(HalfWidth, distance) of its bearing; a clump containing the point covers all.
inline float GrassCoverAt(const GrassClump* Clumps, int Count, float PX, float PY) {
	float Exposure[kGrassCoverSectors];
	for (int s = 0; s < kGrassCoverSectors; s++) Exposure[s] = 1.0f;
	float SectorHalf = kGrassCoverPi / kGrassCoverSectors;
	for (int i = 0; i < Count; i++) {
		const GrassClump& Clump = Clumps[i];
		float Keep = 1.0f - Clump.Weight;
		float DX = Clump.X - PX;
		float DY = Clump.Y - PY;
		float D = sqrtf(DX * DX + DY * DY);
		if (D <= Clump.HalfWidth) {
			for (int s = 0; s < kGrassCoverSectors; s++) Exposure[s] *= Keep;
			continue;
		}
		float Bearing = atan2f(DY, DX);
		float Reach = atan2f(Clump.HalfWidth, D) + SectorHalf;
		for (int s = 0; s < kGrassCoverSectors; s++) {
			float Centre = (s + 0.5f) * 2.0f * SectorHalf - kGrassCoverPi;
			if (fabsf(remainderf(Centre - Bearing, 2.0f * kGrassCoverPi)) < Reach) Exposure[s] *= Keep;
		}
	}
	float Sum = 0.0f;
	for (int s = 0; s < kGrassCoverSectors; s++) Sum += 1.0f - Exposure[s];
	return Sum / kGrassCoverSectors;
}

/// Returns the weighted length, world units, of the segment A → B inside one clump's vertical cylinder:
/// its half-width circle in XY, from BaseZ to TopZ.
inline float GrassClumpRayDepth(const GrassClump& Clump, float AX, float AY, float AZ, float BX, float BY, float BZ) {
	float DX = BX - AX;
	float DY = BY - AY;
	float DZ = BZ - AZ;
	float FX = AX - Clump.X;
	float FY = AY - Clump.Y;
	float RadiusSq = Clump.HalfWidth * Clump.HalfWidth;
	float A = DX * DX + DY * DY;
	float T0 = 0.0f;
	float T1 = 1.0f;
	if (A < 1e-6f) {
		if (FX * FX + FY * FY > RadiusSq) return 0.0f;
	}
	else {
		float B = 2.0f * (FX * DX + FY * DY);
		float Disc = B * B - 4.0f * A * (FX * FX + FY * FY - RadiusSq);
		if (!(Disc > 0.0f)) return 0.0f;
		float Root = sqrtf(Disc);
		float Enter = (-B - Root) / (2.0f * A);
		float Exit = (-B + Root) / (2.0f * A);
		if (Enter > T0) T0 = Enter;
		if (Exit < T1) T1 = Exit;
		if (!(T0 < T1)) return 0.0f;
	}
	if (!TreeCoverClipSlab(AZ, DZ, Clump.BaseZ, Clump.TopZ, T0, T1)) return 0.0f;
	return (T1 - T0) * sqrtf(DX * DX + DY * DY + DZ * DZ) * Clump.Weight;
}

/// Returns the summed GrassClumpRayDepth of the clumps on the segment A → B.
inline float GrassCoverRayDepth(const GrassClump* Clumps, int Count, float AX, float AY, float AZ, float BX, float BY, float BZ) {
	float Depth = 0.0f;
	for (int i = 0; i < Count; i++) Depth += GrassClumpRayDepth(Clumps[i], AX, AY, AZ, BX, BY, BZ);
	return Depth;
}

/// Returns the detection light multiplier for tree and grass cover, 0-1. Each cover's reduction is
/// clamped on its own, so an out-of-range reduction can't flip the product's sign.
inline float FoliageLightScale(float TreeCover, float TreeReduction, float GrassCover, float GrassReduction) {
	return TreeCoverSaturate(1.0f - TreeCover * TreeReduction) * TreeCoverSaturate(1.0f - GrassCover * GrassReduction);
}

/// Returns true when tree and grass foliage together block a sight line: each depth over its threshold,
/// summed, reaches 1. A threshold ≤ 0 leaves its depth out.
inline bool FoliageBlocksSight(float TreeDepth, float TreeThreshold, float GrassDepth, float GrassThreshold) {
	float Sum = 0.0f;
	if (TreeThreshold > 0.0f) Sum += TreeDepth / TreeThreshold;
	if (GrassThreshold > 0.0f) Sum += GrassDepth / GrassThreshold;
	return Sum >= 1.0f;
}
