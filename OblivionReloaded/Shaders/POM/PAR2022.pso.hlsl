// PAR2022.pso: parallax, unlit base * vertex color.
// Stock-equivalent (paired with PAR2028.vso).

float4 Toggles : register(c7);

sampler2D BaseMap : register(s0);

struct VS_OUTPUT {
    float2 BaseUV : TEXCOORD0;
    float3 CameraDir : TEXCOORD6_centroid;
    float3 Color : COLOR0;
};

struct PS_OUTPUT {
    float4 Color : COLOR0;
};

#include "Includes/PAR.hlsl"

PS_OUTPUT main(VS_OUTPUT IN) {
    PS_OUTPUT OUT;

    float2 uv = ParallaxUV(BaseMap, IN.BaseUV, IN.CameraDir);
    float3 base = tex2D(BaseMap, uv).rgb;

    OUT.Color.rgb = (Toggles.x <= 0.0 ? base : (base * IN.Color.rgb));
    OUT.Color.a = 1;
    return OUT;
};
