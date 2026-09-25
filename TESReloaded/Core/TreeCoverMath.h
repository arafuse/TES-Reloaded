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
/// Maximum ring radius when tree bends (prevents artifacts at extreme push).
static const float kTreeCoverMaxRing	= 0.85f;
/// Minimum ring radius; below this the coverage uses outer half-circle only.
static const float kTreeCoverMinRing	= 0.001f;

/// Clamps X to [0, 1].
inline float TreeCoverSaturate(float X) { return X < 0.0f ? 0.0f : (X > 1.0f ? 1.0f : X); }

/// Hermite smoothstep: 3t² - 2t³ applied to saturated X.
inline float TreeCoverSmoothstep(float X) { X = TreeCoverSaturate(X); return X * X * (3.0f - 2.0f * X); }

/// Returns how concealed the player point is by one tree, in [0, 1].
/// Unbent, cover peaks at the centre of an ellipsoid running from the ground to the bound top. As
/// the tree bends away from the player, the peak moves out to a ring of normalised radius Ring
/// (the shader's push at torso height over the horizontal semi-axis) and the centre is exposed.
/// OutRing, when given, receives that ring radius.
inline float TreeCoverAt(const TreeCoverInput& In, float* OutRing = 0) {
	if (OutRing) *OutRing = 0.0f;
	float Top = In.CZ + In.R;
	float Height = Top - In.BaseZ;
	if (In.R <= 0.0f || Height <= 0.0f) return 0.0f;

	float CentreZ = (In.BaseZ + Top) * 0.5f;
	float VerticalAxis = Height * 0.5f;
	float Rise = In.CZ - In.BaseZ;
	float MinAxis = 0.25f * In.R;
	float HorizontalSq = In.R * In.R - Rise * Rise;
	float HorizontalAxis = sqrtf(HorizontalSq > MinAxis * MinAxis ? HorizontalSq : MinAxis * MinAxis);

	float Bend = TreeCoverSaturate((In.QZ - In.BaseZ) / In.R);
	float Ring = TreeCoverSaturate(In.PushStrength * kTreeCoverPushPeak * Bend * Bend / HorizontalAxis);
	if (Ring > kTreeCoverMaxRing) Ring = kTreeCoverMaxRing;
	if (OutRing) *OutRing = Ring;

	float DX = In.QX - In.CX;
	float DY = In.QY - In.CY;
	float U = sqrtf(DX * DX + DY * DY) / HorizontalAxis;
	float V = (In.QZ - CentreZ) / VerticalAxis;
	float Radial = (Ring < kTreeCoverMinRing || U >= Ring) ? (U - Ring) / (1.0f - Ring) : (Ring - U) / Ring;
	return 1.0f - TreeCoverSmoothstep(sqrtf(Radial * Radial + V * V));
}
