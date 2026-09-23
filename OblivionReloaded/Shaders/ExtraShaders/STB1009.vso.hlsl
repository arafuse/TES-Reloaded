// SpeedTree branch fog pass (SRCALPHA blend, ZFUNC EQUAL). Port of stock STB1009.vso (vs_1_1)
// with the small-tree collision bend; pairs with the STB1009.pso override.

float3 FogColor : register(c24);
float4 FogParam : register(c23);
row_major float4x4 ModelViewProj : register(c0);
float4 WindMatrices[16] : register(c38);

#include "Includes/TreeCollision.hlsl"

struct VS_INPUT {
    float4 position : POSITION;
    float4 blendindices : BLENDINDICES;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float4 color_0 : COLOR0;
};

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float4 r0 = TreeBranchPosition(IN.position, IN.blendindices);
    float4 clip = mul(ModelViewProj, r0);

    OUT.position = clip;
    OUT.color_0.rgb = FogColor.rgb;
    OUT.color_0.a = 1 - saturate((FogParam.x - length(clip.xyz)) / FogParam.y);
    return OUT;
};
