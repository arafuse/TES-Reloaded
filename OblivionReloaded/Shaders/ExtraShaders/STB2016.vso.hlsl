// SpeedTree branch point-light specular pass (additive, ZFUNC EQUAL). Port of stock STB2016.vso
// with the small-tree collision bend. The output layout is stock: stock SLS2041.pso reads t0, t1,
// t3 and t5, and it also pairs with stock SLS2035/2036.

float4 LightPosition : register(c16);
float4 EyePosition : register(c25);
row_major float4x4 ModelViewProj : register(c0);
float4 WindMatrices[16] : register(c38);

#include "Includes/TreeCollision.hlsl"

struct VS_INPUT {
    float4 position : POSITION;
    float3 tangent : TANGENT;
    float3 binormal : BINORMAL;
    float3 normal : NORMAL;
    float4 texcoord_0 : TEXCOORD0;
    float4 blendindices : BLENDINDICES;
};

struct VS_OUTPUT {
    float4 position : POSITION;
    float2 texcoord_0 : TEXCOORD0;
    float3 texcoord_1 : TEXCOORD1;
    float3 texcoord_3 : TEXCOORD3;
    float4 texcoord_5 : TEXCOORD5;
};

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float3x3 tanSpaceProj = float3x3(IN.tangent.xyz, IN.binormal.xyz, IN.normal.xyz);
    float4 r0 = TreeBranchPosition(IN.position, IN.blendindices);
    float3 lightVec = LightPosition.xyz - r0.xyz;
    float3 lightDir = normalize(lightVec);
    float3 halfDir = normalize(normalize(EyePosition.xyz - r0.xyz) + lightDir);

    OUT.position = mul(ModelViewProj, r0);
    OUT.texcoord_0.xy = IN.texcoord_0.xy;
    OUT.texcoord_1.xyz = normalize(mul(tanSpaceProj, lightDir));
    OUT.texcoord_3.xyz = mul(tanSpaceProj, halfDir);
    OUT.texcoord_5.xyz = ((lightVec / LightPosition.w) * 0.5) + 0.5;
    OUT.texcoord_5.w = 0.5;
    return OUT;
};
