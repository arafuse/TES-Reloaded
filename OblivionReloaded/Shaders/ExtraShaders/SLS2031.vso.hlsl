// Multipass main-light highlight pass, rigid (stock SLS2031, feeds SLS2037): the main light and the
// half vector in tangent space.

row_major float4x4 ModelViewProj : register(c0);
float3 LightDirection[3] : register(c13);
float4 EyePosition : register(c25);

struct VS_INPUT {
	float4 Position : POSITION;
	float3 Tangent : TANGENT;
	float3 Binormal : BINORMAL;
	float3 Normal : NORMAL;
	float4 TexCoord : TEXCOORD0;
};

struct VS_OUTPUT {
	float4 Position : POSITION;
	float2 BaseUV : TEXCOORD0;
	float3 LightDir : TEXCOORD1;
	float3 HalfDir : TEXCOORD3;
};

VS_OUTPUT main(VS_INPUT IN) {
	VS_OUTPUT OUT;

	float3x3 TBN = float3x3(IN.Tangent, IN.Binormal, IN.Normal);
	float3 Half = normalize(normalize(EyePosition.xyz - IN.Position.xyz) + LightDirection[0]);

	OUT.Position = mul(ModelViewProj, IN.Position);
	OUT.BaseUV = IN.TexCoord.xy;
	OUT.LightDir = normalize(mul(TBN, LightDirection[0]));
	OUT.HalfDir = mul(TBN, Half);
	return OUT;
}
