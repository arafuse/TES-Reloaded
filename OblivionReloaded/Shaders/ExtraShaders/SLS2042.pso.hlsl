// Multipass anisotropic highlight for one point light (stock SLS2042), rewritten unchanged so it can
// run with the shader model 3 SLS2035/SLS2036 vertex shader overrides. No wet look.

float4 PSLightColor[4] : register(c2);
sampler2D NormalMap : register(s0);
sampler2D AttenuationMap : register(s3);

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
	float3 Bent = normalize((N * 0.5f) + float3(0.0f, 0.0f, 1.0f));
	float3 L = normalize(IN.LightDir);
	float Aniso = 1.0f - saturate(abs(dot(Bent, L) - dot(Bent, normalize(IN.HalfDir))));
	float NdotL = dot(N, L);
	float Spec = (Normal.a * 0.7f) * pow(Aniso, 30.0f);
	Spec = (0.2f - NdotL >= 0.0f) ? Spec * max(NdotL + 0.5f, 0.0f) : Spec;
	float Att = saturate(1.0f - tex2D(AttenuationMap, IN.AttUV.xy).x - tex2D(AttenuationMap, IN.AttUV.zw).x);
	float3 Light = (Spec * PSLightColor[0].rgb) * Att;

	OUT.Color.rgb = saturate(Light);
	OUT.Color.a = dot(Light, 1.0f);
	return OUT;
}
