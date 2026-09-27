// Single-tap parallax for the near-land layer passes (SLS2048 base layer, SLS2049 blended layers).
//
// Each pass reads the height from its own diffuse map's alpha at the un-displaced UV and shifts the
// UV along the tangent-space view vector by (height - 0.5) * scale, like the stock PAR shaders,
// faded out with eye distance. Layer blend weights come from vertex colors and are not affected.
//
// Both passes also output the POM shadow side channel on COLOR1 (TESR_POMDepthBuffer, see
// POM/Includes/PAR.hlsl). The layer passes blend it by their layer weight like the diffuse, so the
// shadow receiver follows the weighted mix of the layers' heights.

float4 TESR_TerrainParallaxData : register(c7);    // x = scale, y = -0.5 * scale, z = fade slope, w = fade bias
float4 TESR_TerrainReliefData : register(c8);      // x = shadow relief (world units at height 1)

// HeightMap    : sampler whose alpha holds the height (the layer's base map)
// BaseUV       : un-displaced texture coordinate
// ParallaxView : xyz = tangent-space surface->eye vector (need not be normalized), w = eye distance
// Relief       : receives the height faded with eye distance, for TerrainShadowDepth
// Returns the displaced texture coordinate.
float2 TerrainParallaxUV(sampler2D HeightMap, float2 BaseUV, float4 ParallaxView, out float Relief) {
    float height = tex2D(HeightMap, BaseUV).a;
    float fade = saturate(ParallaxView.w * TESR_TerrainParallaxData.z + TESR_TerrainParallaxData.w);
    Relief = height * fade;
    return BaseUV + (height * TESR_TerrainParallaxData.x + TESR_TerrainParallaxData.y) * fade * normalize(ParallaxView.xyz).xy;
}

// PSO side of the shadow side channel: (geometric view depth, view depth of the relief point, 0, Weight),
// lifted toward the eye like the POM ParallaxShadowDepth.
// ViewDepth    : interpolated clip-space w
// Relief       : from TerrainParallaxUV
// ParallaxView : as for TerrainParallaxUV
// Weight       : the pass's layer blend weight (1 for the opaque base layer)
float4 TerrainShadowDepth(float ViewDepth, float Relief, float4 ParallaxView, float Weight) {
    float dist = Relief * TESR_TerrainReliefData.x / max(normalize(ParallaxView.xyz).z, 0.1);
    return float4(ViewDepth, ViewDepth * (1 - min(dist / ParallaxView.w, 0.5)), 0, Weight);
}

// VSO side: the eye position in model space, solved from ModelViewProj as the point whose clip x, y
// and w are all zero. The engine does not upload EyePosition (c25) for land draws (the stock land
// shaders never use it), so that register holds a stale value from an earlier draw.
// An orthographic MVP has no eye (w = 0); it is clamped to a very distant point so the fade zeroes the offset.
// MVP : ModelViewProj
float3 TerrainEyePosition(row_major float4x4 MVP) {
    float4 a = MVP[0];
    float4 b = MVP[1];
    float4 c = MVP[3];
    float4 eye = float4(determinant(float3x3(a.yzw, b.yzw, c.yzw)), -determinant(float3x3(a.xzw, b.xzw, c.xzw)),
                        determinant(float3x3(a.xyw, b.xyw, c.xyw)), -determinant(float3x3(a.xyz, b.xyz, c.xyz)));
    return eye.xyz / (abs(eye.w) < 1e-6 ? 1e-6 : eye.w);
}
