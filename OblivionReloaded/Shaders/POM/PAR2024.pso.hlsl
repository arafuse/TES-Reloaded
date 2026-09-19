// PAR2024.pso: parallax, sun specular-only pass (alpha = specular weight).
// Stock-equivalent (paired with PAR2032.vso), including the vanilla projected
// shadow term. Note this pass binds the normal map to s0 and the base to s1.

float4 PSLightColor[4] : register(c2);
float4 Toggles : register(c7);

sampler2D NormalMap : register(s0);
sampler2D BaseMap : register(s1);
sampler2D ShadowMap : register(s4);
sampler2D ShadowMaskMap : register(s5);

struct VS_OUTPUT {
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1_centroid;
    float3 Light0Half : TEXCOORD3_centroid;
    float4 ShadowUV : TEXCOORD6;
    float3 CameraDir : TEXCOORD7_centroid;
};

struct PS_OUTPUT {
    float4 Color : COLOR0;
};

#include "Includes/PAR.hlsl"

#define	expand(v)		(((v) - 0.5) / 0.5)
#define	shades(n, l)	saturate(dot(n, l))

PS_OUTPUT main(VS_OUTPUT IN) {
    PS_OUTPUT OUT;

    float2 uv = ParallaxUV(BaseMap, IN.BaseUV, IN.CameraDir);
    float4 normalMap = tex2D(NormalMap, uv);
    float3 normal = normalize(expand(normalMap.rgb));

    // Vanilla specular: gloss in normal alpha, faded out as N.L drops below 0.2.
    float NdotL = dot(normal, normalize(IN.Light0Dir));
    float spec = normalMap.a * pow(shades(normal, normalize(IN.Light0Half)), Toggles.z);
    spec = (0.2 >= NdotL ? (spec * max(NdotL + 0.5, 0)) : spec);

    float3 shadow = lerp(1, tex2D(ShadowMap, IN.ShadowUV.xy).rgb, tex2D(ShadowMaskMap, IN.ShadowUV.zw).r);
    float3 specular = spec * PSLightColor[0].rgb * shadow;

    OUT.Color.rgb = saturate(specular);
    OUT.Color.a = dot(specular, 1);
    return OUT;
};
