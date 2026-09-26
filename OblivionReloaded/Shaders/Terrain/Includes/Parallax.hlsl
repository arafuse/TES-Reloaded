// Single-tap parallax for the near-land layer passes (SLS2048 base layer, SLS2049 blended layers).
//
// Each pass reads the height from its own diffuse map's alpha at the un-displaced UV and shifts the
// UV along the tangent-space view vector by (height - 0.5) * scale, like the stock PAR shaders,
// faded out with eye distance. Layer blend weights come from vertex colors and are not affected.

float4 TESR_TerrainParallaxData : register(c7);    // x = scale, y = -0.5 * scale, z = fade slope, w = fade bias

// HeightMap    : sampler whose alpha holds the height (the layer's base map)
// BaseUV       : un-displaced texture coordinate
// ParallaxView : xyz = tangent-space surface->eye vector (need not be normalized), w = eye distance
// Returns the displaced texture coordinate.
float2 TerrainParallaxUV(sampler2D HeightMap, float2 BaseUV, float4 ParallaxView) {
    float height = tex2D(HeightMap, BaseUV).a;
    float fade = saturate(ParallaxView.w * TESR_TerrainParallaxData.z + TESR_TerrainParallaxData.w);
    return BaseUV + (height * TESR_TerrainParallaxData.x + TESR_TerrainParallaxData.y) * fade * normalize(ParallaxView.xyz).xy;
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
