// Multipass main-light highlight pass (stock SLS2037), added after the texture pass. Wet actors: adds
// the water-film sheen and glint for the main light.

float4 PSLightColor[4] : register(c2);
float4 Toggles : register(c7);
sampler2D NormalMap : register(s0);
#include "Includes/WetActor.hlsl"

struct VS_OUTPUT {
	float2 BaseUV : TEXCOORD0;
	float3 LightDir : TEXCOORD1_centroid;
	float3 HalfDir : TEXCOORD3_centroid;
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
	float3 Light = Spec * PSLightColor[0].rgb;
	Light += WetSheenFromHalf(N, IN.HalfDir, IN.LightDir, PSLightColor[0].rgb, 0.5f * PSLightColor[0].rgb);

	OUT.Color.rgb = saturate(Light);
	OUT.Color.a = dot(Light, 1.0f);
	return OUT;
}
