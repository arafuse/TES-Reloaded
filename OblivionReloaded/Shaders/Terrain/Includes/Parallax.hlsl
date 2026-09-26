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
