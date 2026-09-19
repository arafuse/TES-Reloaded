// PAR2006.pso: parallax, sun + one point light + ambient, fog.
// Stock-equivalent (paired with PAR2006.vso), minus the vanilla projected
// shadow term: sun shadows come from the Oblivion Reloaded shadow pass.

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
    float4 Att1UV : TEXCOORD4;
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

    float att1 = saturate((1 - tex2D(AttenuationMap, IN.Att1UV.xy).x) - tex2D(AttenuationMap, IN.Att1UV.zw).x);
    float3 point1 = att1 * shades(normal, normalize(IN.Light1Dir)) * PSLightColor[1].rgb;
    float3 sun = shades(normal, IN.Light0Dir) * PSLightColor[0].rgb;

    float3 light = max(sun + point1 + AmbientColor.rgb, 0);
    float3 albedo = (Toggles.x <= 0.0 ? base : (base * IN.Color.rgb));
    float3 color = light * albedo;

    OUT.Color.rgb = (Toggles.y <= 0.0 ? color : lerp(color, IN.Fog.rgb, IN.Fog.a));
    OUT.Color.a = AmbientColor.a;
    OUT.POM = ParallaxShadowDepth(IN.DepthData, height, IN.CameraDir);
    return OUT;
};
