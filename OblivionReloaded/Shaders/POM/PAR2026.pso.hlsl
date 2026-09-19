// PAR2026.pso: parallax, sun + one point light (diffuse + specular) + ambient, fog.
// Stock-equivalent (paired with PAR2012.vso).

float4 AmbientColor : register(c1);
float4 PSLightColor[4] : register(c2);
float4 Toggles : register(c7);

sampler2D BaseMap : register(s0);
sampler2D NormalMap : register(s1);
sampler2D AttenuationMap : register(s5);

struct VS_OUTPUT {
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1_centroid;
    float3 Light1Dir : TEXCOORD2_centroid;
    float3 Light0Half : TEXCOORD3_centroid;
    float3 Light1Half : TEXCOORD4_centroid;
    float4 Att1UV : TEXCOORD5;
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

// Vanilla specular: gloss in normal alpha, faded out as N.L drops below 0.2.
float Specular(float gloss, float3 normal, float3 halfDir, float NdotL) {
    float spec = gloss * pow(shades(normal, normalize(halfDir)), Toggles.z);
    return (0.2 >= NdotL ? (spec * max(NdotL + 0.5, 0)) : spec);
}

PS_OUTPUT main(VS_OUTPUT IN) {
    PS_OUTPUT OUT;

    float height;
    float2 uv = ParallaxUV(BaseMap, IN.BaseUV, IN.CameraDir, height);
    float3 base = tex2D(BaseMap, uv).rgb;
    float4 normalMap = tex2D(NormalMap, uv);
    float3 normal = normalize(expand(normalMap.rgb));

    float att1 = saturate((1 - tex2D(AttenuationMap, IN.Att1UV.xy).x) - tex2D(AttenuationMap, IN.Att1UV.zw).x);
    float NdotL0 = dot(normal, IN.Light0Dir);
    float NdotL1 = dot(normal, normalize(IN.Light1Dir));

    float3 specular0 = saturate(Specular(normalMap.a, normal, IN.Light0Half, NdotL0) * PSLightColor[0].rgb);
    float3 specular1 = saturate(att1 * (Specular(normalMap.a, normal, IN.Light1Half, NdotL1) * PSLightColor[1].rgb));

    float3 diffuse = (saturate(NdotL0) * PSLightColor[0].rgb) + (att1 * saturate(NdotL1) * PSLightColor[1].rgb);
    float3 light = max(diffuse + AmbientColor.rgb, 0);
    float3 albedo = (Toggles.x <= 0.0 ? base : (base * IN.Color.rgb));
    float3 color = (light * albedo) + (specular0 + specular1);

    OUT.Color.rgb = (Toggles.y <= 0.0 ? color : lerp(color, IN.Fog.rgb, IN.Fog.a));
    OUT.Color.a = AmbientColor.a;
    OUT.POM = ParallaxShadowDepth(IN.DepthData, height, IN.CameraDir);
    return OUT;
};
