// PAR2000.pso: parallax, sun + ambient, fog.
// Stock-equivalent (paired with PAR2000.vso).

float4 AmbientColor : register(c1);
float4 PSLightColor[4] : register(c2);
float4 Toggles : register(c7);

sampler2D BaseMap : register(s0);
sampler2D NormalMap : register(s1);

struct VS_OUTPUT {
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1_centroid;
    float3 CameraDir : TEXCOORD6_centroid;
    float2 DepthData : TEXCOORD8;
    float3 Color : COLOR0;
    float4 Fog : COLOR1;
};

struct PS_OUTPUT {
    float4 Color : COLOR0;
    float4 POM : COLOR1;
};

#include "Includes/PAR.hlsl"

#define	expand(v)		(((v) - 0.5) / 0.5)
#define	shades(n, l)	saturate(dot(n, l))

PS_OUTPUT main(VS_OUTPUT IN) {
    PS_OUTPUT OUT;

    float height;
    float2 uv = ParallaxUV(BaseMap, IN.BaseUV, IN.CameraDir, height);
    float3 base = tex2D(BaseMap, uv).rgb;
    float3 normal = normalize(expand(tex2D(NormalMap, uv).rgb));

    float3 light = max((shades(normal, IN.Light0Dir) * PSLightColor[0].rgb) + AmbientColor.rgb, 0);
    float3 albedo = (Toggles.x <= 0.0 ? base : (base * IN.Color.rgb));
    float3 color = light * albedo;

    OUT.Color.rgb = (Toggles.y <= 0.0 ? color : lerp(color, IN.Fog.rgb, IN.Fog.a));
    OUT.Color.a = AmbientColor.a;
    OUT.POM = ParallaxShadowDepth(IN.DepthData, height, IN.CameraDir);
    return OUT;
};
