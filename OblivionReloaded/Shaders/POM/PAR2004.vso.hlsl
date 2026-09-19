// PAR2004.vso: parallax, sun + one point light, fog. Stock-equivalent output layout.

row_major float4x4 ModelViewProj : register(c0);
float3 LightDirection[3] : register(c13);
float4 LightPosition[3] : register(c16);
float4 FogParam : register(c23);
float3 FogColor : register(c24);
float4 EyePosition : register(c25);

struct VS_INPUT {
    float4 Position : POSITION;
    float3 Tangent : TANGENT;
    float3 BiNormal : BINORMAL;
    float3 Normal : NORMAL;
    float4 BaseUV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VS_OUTPUT {
    float4 Position : POSITION;
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1;
    float3 Light1Dir : TEXCOORD2;
    float4 Att1UV : TEXCOORD4;
    float3 CameraDir : TEXCOORD6;
    float2 DepthData : TEXCOORD8;
    float4 Color : COLOR0;
    float4 Fog : COLOR1;
};

#include "Includes/PAR.hlsl"

#define	TanSpaceProj	float3x3(IN.Tangent.xyz, IN.BiNormal.xyz, IN.Normal.xyz)
#define	compress(v)		(((v) * 0.5) + 0.5)

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float4 mdl = mul(ModelViewProj, IN.Position);
    float3 eye = normalize(EyePosition.xyz - IN.Position.xyz);
    float3 lit1 = LightPosition[1].xyz - IN.Position.xyz;

    OUT.Position = mdl;
    OUT.BaseUV = IN.BaseUV.xy;
    OUT.Light0Dir = normalize(mul(TanSpaceProj, LightDirection[0].xyz));
    OUT.Light1Dir = mul(TanSpaceProj, normalize(lit1));
    OUT.Att1UV.xyz = compress(lit1 / LightPosition[1].w);
    OUT.Att1UV.w = 0.5;
    OUT.CameraDir = normalize(mul(TanSpaceProj, eye));
    OUT.Color = IN.Color;
    OUT.Fog.rgb = FogColor.rgb;
    OUT.Fog.a = 1 - saturate((FogParam.x - length(mdl.xyz)) / FogParam.y);
    OUT.DepthData = ParallaxDepthData(ModelViewProj, IN.Position, EyePosition.xyz);
    return OUT;
};
