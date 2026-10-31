// Multipass point-light highlight pass, skinned (stock SLS2036, feeds SLS2041): one point light and
// the half vector in tangent space, plus the light attenuation coordinates.

row_major float4x4 SkinModelViewProj : register(c1);
float4 LightPosition[3] : register(c16);
float4 EyePosition : register(c25);
#include "Includes/Skinning.hlsl"

struct VS_INPUT {
	float4 Position : POSITION;
	float3 Tangent : TANGENT;
	float3 Binormal : BINORMAL;
	float3 Normal : NORMAL;
	float4 TexCoord : TEXCOORD0;
	float3 BlendWeight : BLENDWEIGHT;
	float4 BlendIndices : BLENDINDICES;
};

struct VS_OUTPUT {
	float4 Position : POSITION;
	float2 BaseUV : TEXCOORD0;
	float3 LightDir : TEXCOORD1;
	float3 HalfDir : TEXCOORD3;
	float4 AttUV : TEXCOORD5;
};

VS_OUTPUT main(VS_INPUT IN) {
	VS_OUTPUT OUT;

	float4 Offset = SkinOffsets(IN.BlendIndices);
	float4 Position = SkinPosition(IN.Position, IN.BlendWeight, Offset);
	float3x3 TBN = float3x3(SkinVector(IN.Tangent, IN.BlendWeight, Offset), SkinVector(IN.Binormal, IN.BlendWeight, Offset), SkinVector(IN.Normal, IN.BlendWeight, Offset));
	float3 Light = LightPosition[0].xyz - Position.xyz;
	float3 L = normalize(Light);
	float3 Half = normalize(normalize(EyePosition.xyz - Position.xyz) + L);

	OUT.Position = mul(SkinModelViewProj, Position);
	OUT.BaseUV = IN.TexCoord.xy;
	OUT.LightDir = normalize(mul(TBN, L));
	OUT.HalfDir = mul(TBN, Half);
	OUT.AttUV = float4(((Light / LightPosition[0].w) * 0.5f) + 0.5f, 0.5f);
	return OUT;
}
