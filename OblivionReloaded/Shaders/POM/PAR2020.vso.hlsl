// PAR2020.vso: parallax lighting-only pass, sun + two point lights. Stock-equivalent output layout.

row_major float4x4 ModelViewProj : register(c0);
float3 LightDirection[3] : register(c13);
float4 LightPosition[3] : register(c16);
float4 EyePosition : register(c25);

struct VS_INPUT {
    float4 Position : POSITION;
    float3 Tangent : TANGENT;
    float3 BiNormal : BINORMAL;
    float3 Normal : NORMAL;
    float4 BaseUV : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 Position : POSITION;
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1;
    float3 Light1Dir : TEXCOORD2;
    float3 Light2Dir : TEXCOORD3;
    float4 Att1UV : TEXCOORD4;
    float4 Att2UV : TEXCOORD5;
    float3 CameraDir : TEXCOORD7;
    float2 DepthData : TEXCOORD8;
};

#include "Includes/PAR.hlsl"

#define	TanSpaceProj	float3x3(IN.Tangent.xyz, IN.BiNormal.xyz, IN.Normal.xyz)
#define	compress(v)		(((v) * 0.5) + 0.5)

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float3 eye = normalize(EyePosition.xyz - IN.Position.xyz);
    float3 lit1 = LightPosition[1].xyz - IN.Position.xyz;
    float3 lit2 = LightPosition[2].xyz - IN.Position.xyz;

    OUT.Position = mul(ModelViewProj, IN.Position);
    OUT.BaseUV = IN.BaseUV.xy;
    OUT.Light0Dir = normalize(mul(TanSpaceProj, LightDirection[0].xyz));
    OUT.Light1Dir = mul(TanSpaceProj, normalize(lit1));
    OUT.Light2Dir = mul(TanSpaceProj, normalize(lit2));
    OUT.Att1UV.xyz = compress(lit1 / LightPosition[1].w);
    OUT.Att1UV.w = 0.5;
    OUT.Att2UV.xyz = compress(lit2 / LightPosition[2].w);
    OUT.Att2UV.w = 0.5;
    OUT.CameraDir = normalize(mul(TanSpaceProj, eye));
    OUT.DepthData = ParallaxDepthData(ModelViewProj, IN.Position, EyePosition.xyz);
    return OUT;
};
