// PAR2016.pso: parallax lighting-only first pass, sun + two point lights + ambient.
// Stock-equivalent (paired with PAR2020.vso); the interior twin of PAR2018, which
// has no vanilla shadow term to drop. Overridden so interior multipass writes the depth offset.

float4 AmbientColor : register(c1);
float4 PSLightColor[4] : register(c2);

sampler2D BaseMap : register(s0);
sampler2D NormalMap : register(s1);
sampler2D AttenuationMap : register(s4);

struct VS_OUTPUT {
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1_centroid;
    float3 Light1Dir : TEXCOORD2_centroid;
    float3 Light2Dir : TEXCOORD3_centroid;
    float4 Att1UV : TEXCOORD4;
    float4 Att2UV : TEXCOORD5;
    float3 CameraDir : TEXCOORD7_centroid;
    float2 DepthData : TEXCOORD8;
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
    float3 normal = normalize(expand(tex2D(NormalMap, uv).rgb));

    float att1 = saturate((1 - tex2D(AttenuationMap, IN.Att1UV.xy).x) - tex2D(AttenuationMap, IN.Att1UV.zw).x);
    float att2 = saturate((1 - tex2D(AttenuationMap, IN.Att2UV.xy).x) - tex2D(AttenuationMap, IN.Att2UV.zw).x);

    float3 sun = shades(normal, IN.Light0Dir) * PSLightColor[0].rgb;
    float3 point1 = att1 * shades(normal, normalize(IN.Light1Dir)) * PSLightColor[1].rgb;
    float3 point2 = att2 * shades(normal, normalize(IN.Light2Dir)) * PSLightColor[2].rgb;

    OUT.Color.rgb = sun + point1 + point2 + AmbientColor.rgb;
    OUT.Color.a = 1;
    OUT.POM = ParallaxShadowDepth(IN.DepthData, height, IN.CameraDir);
    return OUT;
};
