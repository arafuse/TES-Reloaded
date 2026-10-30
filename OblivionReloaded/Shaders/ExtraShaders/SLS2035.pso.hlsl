// Multipass extra-lights pass (stock SLS2035): two more point lights, added to the lighting before the
// texture pass multiplies in the texture. Wet actors: the lighting darkens where porous.

float4 PSLightColor[4] : register(c2);
sampler2D NormalMap : register(s0);
sampler2D AttenuationMap : register(s3);
#include "Includes/WetActor.hlsl"

struct VS_OUTPUT {
	float2 BaseUV : TEXCOORD0;
	float3 Light0Dir : TEXCOORD1_centroid;
	float3 Light1Dir : TEXCOORD2_centroid;
	float4 Att0UV : TEXCOORD4;
	float4 Att1UV : TEXCOORD5;
};

struct PS_OUTPUT {
	float4 Color : COLOR0;
};

PS_OUTPUT main(VS_OUTPUT IN) {
	PS_OUTPUT OUT;

	float4 Normal = tex2D(NormalMap, IN.BaseUV);
	float3 N = normalize((Normal.xyz - 0.5f) * 2.0f);
	float Att0 = saturate(1.0f - tex2D(AttenuationMap, IN.Att0UV.xy).x - tex2D(AttenuationMap, IN.Att0UV.zw).x);
	float Att1 = saturate(1.0f - tex2D(AttenuationMap, IN.Att1UV.xy).x - tex2D(AttenuationMap, IN.Att1UV.zw).x);
	float3 Lit = (saturate(dot(N, normalize(IN.Light1Dir))) * PSLightColor[1].rgb) * Att1;
	Lit = ((saturate(dot(N, normalize(IN.Light0Dir))) * PSLightColor[0].rgb) * Att0) + Lit;

	OUT.Color.rgb = Lit * WetLightScale(Normal.a);
	OUT.Color.a = 1.0f;
	return OUT;
}
