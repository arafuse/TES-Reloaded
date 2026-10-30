// Stock skinning for the multipass vertex shader overrides. The texture pass keeps its stock vertex
// shader and later passes test depth against the first, so SkinPosition keeps the stock instruction
// order for a bit-identical position.

float4 Bones[54] : register(c42);

// Bone row offsets of the four blend indices, in stock order.
float4 SkinOffsets(float4 BlendIndices) {
	return BlendIndices.zyxw * 765.01001f;
}

// Skinned model-space position with w = 1.
float4 SkinPosition(float4 Position, float3 Weights, float4 Offset) {
	float4 P = (Position.xyzx * float4(1.0f, 1.0f, 1.0f, 0.0f)) + float4(0.0f, 0.0f, 0.0f, 1.0f);
	float3 Py = mul(float3x4(Bones[0 + Offset.y], Bones[1 + Offset.y], Bones[2 + Offset.y]), P);
	float3 Px = mul(float3x4(Bones[0 + Offset.x], Bones[1 + Offset.x], Bones[2 + Offset.x]), P);
	float3 Pz = mul(float3x4(Bones[0 + Offset.z], Bones[1 + Offset.z], Bones[2 + Offset.z]), P);
	float3 Pw = mul(float3x4(Bones[0 + Offset.w], Bones[1 + Offset.w], Bones[2 + Offset.w]), P);
	float3 Sum = (Weights.z * Pz) + ((Weights.x * Px) + (Py * Weights.y));
	return float4(((1.0f - dot(Weights, 1.0f)) * Pw) + Sum, 1.0f);
}

// Skinned, normalized direction (tangent, binormal or normal).
float3 SkinVector(float3 V, float3 Weights, float4 Offset) {
	float3 Vy = mul(float3x3(Bones[0 + Offset.y].xyz, Bones[1 + Offset.y].xyz, Bones[2 + Offset.y].xyz), V);
	float3 Vx = mul(float3x3(Bones[0 + Offset.x].xyz, Bones[1 + Offset.x].xyz, Bones[2 + Offset.x].xyz), V);
	float3 Vz = mul(float3x3(Bones[0 + Offset.z].xyz, Bones[1 + Offset.z].xyz, Bones[2 + Offset.z].xyz), V);
	float3 Vw = mul(float3x3(Bones[0 + Offset.w].xyz, Bones[1 + Offset.w].xyz, Bones[2 + Offset.w].xyz), V);
	float3 Sum = (Weights.z * Vz) + ((Weights.x * Vx) + (Vy * Weights.y));
	return normalize(((1.0f - dot(Weights, 1.0f)) * Vw) + Sum);
}
