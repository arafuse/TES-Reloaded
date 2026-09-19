// Stock Oblivion parallax, shared by every PAR pixel shader override.
//
// Vanilla parallax is a single-tap offset: read the height from the base map's
// alpha at the un-displaced UV, then shift the UV along the tangent-space view
// vector by (height - 0.5) * scale. Stock scale is 0.04 (POM.ini HeightMapScale).
//
// The first-pass PSOs (ZWrite on) also output a shadow side channel on COLOR1, which the
// plugin binds to TESR_POMDepthBuffer around those draws only: (geometric view depth,
// view depth lifted toward the eye by the height). The screen-space shadow passes use the
// lifted depth for the receiver where the geometric depth still matches the depth buffer.
// The main depth buffer is never touched: Oblivion's follow-up passes depth-test EQUAL.

float4 TESR_ParallaxData : register(c8);    // x = height scale, y = -0.5 * scale, z = shadow relief (world units at height 1)

// HeightMap : sampler whose alpha channel holds the height (the base map)
// BaseUV    : un-displaced texture coordinate
// CameraDir : tangent-space surface->eye vector (need not be normalized)
// Height    : receives the height sampled at BaseUV
// Returns the displaced texture coordinate.
float2 ParallaxUV(sampler2D HeightMap, float2 BaseUV, float3 CameraDir, out float Height) {
    Height = tex2D(HeightMap, BaseUV).a;
    return BaseUV + (Height * TESR_ParallaxData.x + TESR_ParallaxData.y) * normalize(CameraDir).xy;
}

float2 ParallaxUV(sampler2D HeightMap, float2 BaseUV, float3 CameraDir) {
    float height;
    return ParallaxUV(HeightMap, BaseUV, CameraDir, height);
}

// VSO side of the shadow side channel; emit the result as TEXCOORD8 (free in every PAR layout).
// MVP      : ModelViewProj
// Position : model-space vertex position
// Eye      : model-space eye position
// Returns (view depth, eye distance).
float2 ParallaxDepthData(row_major float4x4 MVP, float4 Position, float3 Eye) {
    return float2(mul(MVP, Position).w, length(Eye - Position.xyz));
}

// PSO side: (geometric view depth, view depth of the relief point) for TESR_POMDepthBuffer.
// The relief point sits Height * scale above the geometry (toward the eye, so the flat wall in
// the shadow map never occludes it), walked back along the view ray and capped at half the way.
// DepthData : interpolated ParallaxDepthData
// Height    : height from ParallaxUV
// CameraDir : tangent-space surface->eye vector (need not be normalized)
float4 ParallaxShadowDepth(float2 DepthData, float Height, float3 CameraDir) {
    float dist = Height * TESR_ParallaxData.z / max(normalize(CameraDir).z, 0.1);
    return float4(DepthData.x, DepthData.x * (1 - min(dist / DepthData.y, 0.5)), 0, 1);
}
