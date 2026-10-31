// Multipass extra-lights pass, rigid (stock SLS2027, feeds SLS2035): two more point lights in tangent
// space, plus their attenuation coordinates.

row_major float4x4 ModelViewProj : register(c0);
float4 LightPosition[3] : register(c16);

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
	float3 Light0Dir : TEXCOORD1;
	float3 Light1Dir : TEXCOORD2;
	float4 Att0UV : TEXCOORD4;
	float4 Att1UV : TEXCOORD5;
};

VS_OUTPUT main(VS_INPUT IN) {
	VS_OUTPUT OUT;

	float3x3 TBN = float3x3(IN.Tangent, IN.Binormal, IN.Normal);
	float3 Light0 = LightPosition[0].xyz - IN.Position.xyz;
	float3 Light1 = LightPosition[1].xyz - IN.Position.xyz;

	OUT.Position = mul(ModelViewProj, IN.Position);
	OUT.BaseUV = IN.TexCoord.xy;
	OUT.Light0Dir = mul(TBN, normalize(Light0));
	OUT.Light1Dir = mul(TBN, normalize(Light1));
	OUT.Att0UV = float4(((Light0 / LightPosition[0].w) * 0.5f) + 0.5f, 0.5f);
	OUT.Att1UV = float4(((Light1 / LightPosition[1].w) * 0.5f) + 0.5f, 0.5f);
	return OUT;
}
