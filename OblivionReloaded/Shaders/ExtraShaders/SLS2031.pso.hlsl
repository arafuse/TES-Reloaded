// Multipass lighting pass (stock SLS2031): ambient, the main light and two point lights, without the
// texture, which the texture pass multiplies in afterwards. Wet actors: the lighting darkens where porous.

float4 AmbientColor : register(c1);
float4 PSLightColor[4] : register(c2);
sampler2D BaseMap : register(s0);
sampler2D NormalMap : register(s1);
sampler2D AttenuationMap : register(s4);
#include "Includes/WetActor.hlsl"

struct VS_OUTPUT {
	float2 BaseUV : TEXCOORD0;
	float3 Light0Dir : TEXCOORD1_centroid;
	float3 Light1Dir : TEXCOORD2_centroid;
	float3 Light2Dir : TEXCOORD3_centroid;
	float4 Att1UV : TEXCOORD4;
	float4 Att2UV : TEXCOORD5;
};

struct PS_OUTPUT {
	float4 Color : COLOR0;
};

PS_OUTPUT main(VS_OUTPUT IN) {
	PS_OUTPUT OUT;

	float4 Base = tex2D(BaseMap, IN.BaseUV);
	float4 Normal = tex2D(NormalMap, IN.BaseUV);
	float3 N = normalize((Normal.xyz - 0.5f) * 2.0f);
	float Att1 = saturate(1.0f - tex2D(AttenuationMap, IN.Att1UV.xy).x - tex2D(AttenuationMap, IN.Att1UV.zw).x);
	float Att2 = saturate(1.0f - tex2D(AttenuationMap, IN.Att2UV.xy).x - tex2D(AttenuationMap, IN.Att2UV.zw).x);
	float3 Lit = (saturate(dot(N, normalize(IN.Light1Dir))) * PSLightColor[1].rgb) * Att1;
	Lit = (PSLightColor[0].rgb * saturate(dot(N, IN.Light0Dir))) + Lit;
	Lit = ((saturate(dot(N, normalize(IN.Light2Dir))) * PSLightColor[2].rgb) * Att2) + Lit;

	OUT.Color.rgb = (Lit + AmbientColor.rgb) * WetLightScale(Normal.a);
	OUT.Color.a = Base.a;
	return OUT;
}
