// Wind slant shared by Rain.fx.hlsl and Snow.fx.hlsl. The including effect must declare
// TESR_WindData (x,y = direction the wind blows toward, z = speed 0-1; see Wind.h).
//
// Both effects draw particles that fall along +Z of a camera-centred cylinder. Rotating the view
// ray by this matrix before the cylinder mapping makes them fall downwind instead. Call it from a
// static const initializer so D3DX folds it into the preshader.

// Slant : fall-line tilt (horizontal per vertical) at full wind speed
// Returns the rotation taking the wind-tilted fall axis onto +Z (Rodrigues).
float3x3 WindSlantRotation(float Slant) {
	float3 Axis = normalize(float3(-Slant * TESR_WindData.z * TESR_WindData.xy, 1.0f));
	float2 Turn = float2(Axis.y, -Axis.x);
	float3x3 Skew = float3x3(0.0f, 0.0f, Turn.y, 0.0f, 0.0f, -Turn.x, -Turn.y, Turn.x, 0.0f);
	return float3x3(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f) + Skew + mul(Skew, Skew) / (1.0f + Axis.z);
}
