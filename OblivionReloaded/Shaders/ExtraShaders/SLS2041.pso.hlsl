// Multipass point-light highlight pass (stock SLS2041), added after the texture pass, once per point
// light. Wet actors: adds the water-film sheen and glint for this light, faded by its attenuation.

float4 PSLightColor[4] : register(c2);
float4 Toggles : register(c7);
sampler2D NormalMap : register(s0);
sampler2D AttenuationMap : register(s3);
#include "Includes/WetActor.hlsl"

struct VS_OUTPUT {
	float2 BaseUV : TEXCOORD0;
	float3 LightDir : TEXCOORD1_centroid;
	float3 HalfDir : TEXCOORD3_centroid;
	float4 AttUV : TEXCOORD5;
};

struct PS_OUTPUT {
	float4 Color : COLOR0;
};

PS_OUTPUT main(VS_OUTPUT IN) {
	PS_OUTPUT OUT;

	float4 Normal = tex2D(NormalMap, IN.BaseUV);
	float3 N = normalize((Normal.xyz - 0.5f) * 2.0f);
	float NdotL = dot(N, normalize(IN.LightDir));
	float Spec = Normal.a * pow(saturate(dot(N, normalize(IN.HalfDir))), Toggles.z);
	Spec = (0.2f - NdotL >= 0.0f) ? Spec * max(NdotL + 0.5f, 0.0f) : Spec;
	float Att = saturate(1.0f - tex2D(AttenuationMap, IN.AttUV.xy).x - tex2D(AttenuationMap, IN.AttUV.zw).x);
	float3 LightColor = PSLightColor[0].rgb * Att;
	float3 Light = Spec * LightColor;
	Light += WetSheenFromHalf(N, IN.HalfDir, IN.LightDir, LightColor, 0.5f * LightColor);

	OUT.Color.rgb = saturate(Light);
	OUT.Color.a = dot(Light, 1.0f);
	return OUT;
}
