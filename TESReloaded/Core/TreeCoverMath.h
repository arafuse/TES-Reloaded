#pragma once

#include <cmath>

/// One tree's geometry and the player's torso point, in world units (z up), for TreeCoverAt.
struct TreeCoverInput {
	float QX, QY, QZ;		///< Player torso point
	float BaseZ;			///< Tree ref position z, taken as the ground
	float CX, CY, CZ;		///< BSTreeNode world bound centre
	float R;				///< BSTreeNode world bound radius
	float PushStrength;		///< Live sideways push (TreeCollisionStrength); 0 when the tree isn't bending
};

/// Peak of smoothstep(1,0,t) * smoothstep(0,0.3,t) in TreeCollision.hlsl.
static const float kTreeCoverPushPeak	= 0.785f;
/// Maximum ring radius when tree bends; keeps a cover band inside the ellipsoid and avoids dividing by 1 - Ring.
static const float kTreeCoverMaxRing	= 0.85f;
/// Minimum ring radius; below this the coverage uses outer half-circle only.
static const float kTreeCoverMinRing	= 0.001f;

/// Clamps X to [0, 1].
inline float TreeCoverSaturate(float X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); }

/// Hermite smoothstep: 3t² - 2t³ applied to saturated X.
inline float TreeCoverSmoothstep(float X) { X = TreeCoverSaturate(X); return X * X * (3.0f - 2.0f * X); }

/// Samples TreeCoverRayDepth takes along the part of a segment inside a tree's ellipsoid bounds.
static const int kTreeCoverRaySamples = 16;

/// One tree's concealment ellipsoid and ring, derived from a TreeCoverInput.
struct TreeCoverShape {
	float CX, CY;			///< Horizontal centre (bound centre)
	float BaseZ;			///< Ground z (tree ref position)
	float TopZ;				///< Bound top z
	float CentreZ;			///< Ellipsoid centre z, midway from ground to top
	float VerticalAxis;		///< Vertical semi-axis
	float HorizontalAxis;	///< Horizontal semi-axis
	float Ring;				///< Normalised ring radius of best cover; 0 when the tree isn't bending
};

/// Builds the ellipsoid running from the ground to the bound top, and the ring the live push opens
/// (the shader's full-bend push over the horizontal semi-axis). Returns false when the tree has no volume.
inline bool TreeCoverShapeOf(const TreeCoverInput& In, TreeCoverShape& Out) {
	float Top = In.CZ + In.R;
	float Height = Top - In.BaseZ;
	if (In.R <= 0.0f || Height <= 0.0f) return false;

	float Rise = In.CZ - In.BaseZ;
	float MinAxis = 0.25f * In.R;
	float HorizontalSq = In.R * In.R - Rise * Rise;
	Out.CX = In.CX;
	Out.CY = In.CY;
	Out.BaseZ = In.BaseZ;
	Out.TopZ = Top;
	Out.CentreZ = (In.BaseZ + Top) * 0.5f;
	Out.VerticalAxis = Height * 0.5f;
	Out.HorizontalAxis = sqrtf(HorizontalSq > MinAxis * MinAxis ? HorizontalSq : MinAxis * MinAxis);
	Out.Ring = TreeCoverSaturate(In.PushStrength * kTreeCoverPushPeak / Out.HorizontalAxis);
	if (Out.Ring > kTreeCoverMaxRing) Out.Ring = kTreeCoverMaxRing;
	return true;
}

/// Returns how concealed the point (X, Y, Z) is by one tree's shape, in [0, 1]. Cover peaks on the ring
/// (the centre when Ring is 0) and falls to 0 at the ellipsoid surface; the lower half counts as fully
/// inside vertically. Callers keep Z at or above the ground.
inline float TreeCoverShapeAt(const TreeCoverShape& Shape, float X, float Y, float Z) {
	float DX = X - Shape.CX;
	float DY = Y - Shape.CY;
	float U = sqrtf(DX * DX + DY * DY) / Shape.HorizontalAxis;
	float Above = Z - Shape.CentreZ;
	float V = (Above > 0.0f ? Above : 0.0f) / Shape.VerticalAxis;
	float Ring = Shape.Ring;
	float Radial = (Ring < kTreeCoverMinRing || U >= Ring) ? (U - Ring) / (1.0f - Ring) : (Ring - U) / Ring;
	return 1.0f - TreeCoverSmoothstep(sqrtf(Radial * Radial + V * V));
}

/// Returns how concealed the player point is by one tree, in [0, 1].
/// Cover peaks at the centre of an ellipsoid running from the ground to the bound top. As the tree bends,
/// the peak moves out to a ring of normalised radius Ring (the shader's full-bend push over the horizontal
/// semi-axis) and the centre is exposed. The ellipsoid's lower half counts as fully inside vertically.
/// OutRing, when given, receives that ring radius.
inline float TreeCoverAt(const TreeCoverInput& In, float* OutRing = 0) {
	if (OutRing) *OutRing = 0.0f;
	TreeCoverShape Shape;
	if (!TreeCoverShapeOf(In, Shape)) return 0.0f;
	if (OutRing) *OutRing = Shape.Ring;
	return TreeCoverShapeAt(Shape, In.QX, In.QY, In.QZ);
}

/// Narrows [T0, T1] to where Start + t·Delta lies within [Lo, Hi] on one axis. Returns false when empty.
inline bool TreeCoverClipSlab(float Start, float Delta, float Lo, float Hi, float& T0, float& T1) {
	if (fabsf(Delta) < 1e-6f) return Start >= Lo && Start <= Hi;
	float TA = (Lo - Start) / Delta;
	float TB = (Hi - Start) / Delta;
	if (TA > TB) { float Swap = TA; TA = TB; TB = Swap; }
	if (TA > T0) T0 = TA;
	if (TB < T1) T1 = TB;
	return T0 < T1;
}

/// Returns the effective foliage depth, in world units, that one tree puts on the segment A → B: its
/// coverage integrated along the part of the segment inside the ellipsoid's bounding box. The Q fields of
/// Tree are ignored.
inline float TreeCoverRayDepth(const TreeCoverInput& Tree, float AX, float AY, float AZ, float BX, float BY, float BZ) {
	TreeCoverShape Shape;
	if (!TreeCoverShapeOf(Tree, Shape)) return 0.0f;

	float DX = BX - AX;
	float DY = BY - AY;
	float DZ = BZ - AZ;
	float T0 = 0.0f;
	float T1 = 1.0f;
	float Axis = Shape.HorizontalAxis;
	if (!TreeCoverClipSlab(AX, DX, Shape.CX - Axis, Shape.CX + Axis, T0, T1) ||
		!TreeCoverClipSlab(AY, DY, Shape.CY - Axis, Shape.CY + Axis, T0, T1) ||
		!TreeCoverClipSlab(AZ, DZ, Shape.BaseZ, Shape.TopZ, T0, T1)) return 0.0f;

	float Step = (T1 - T0) / kTreeCoverRaySamples;
	float Sum = 0.0f;
	for (int i = 0; i < kTreeCoverRaySamples; i++) {
		float T = T0 + (i + 0.5f) * Step;
		Sum += TreeCoverShapeAt(Shape, AX + DX * T, AY + DY * T, AZ + DZ * T);
	}
	return Sum * Step * sqrtf(DX * DX + DY * DY + DZ * DZ);
}
