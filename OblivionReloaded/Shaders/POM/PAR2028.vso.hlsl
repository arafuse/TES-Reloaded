// PAR2028.vso: parallax, unlit vertex color. Stock-equivalent output layout.

row_major float4x4 ModelViewProj : register(c0);
float4 EyePosition : register(c25);

struct VS_INPUT {
    float4 Position : POSITION;
    float3 Tangent : TANGENT;
    float3 BiNormal : BINORMAL;
    float3 Normal : NORMAL;
    float4 BaseUV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VS_OUTPUT {
    float4 Position : POSITION;
    float2 BaseUV : TEXCOORD0;
    float3 CameraDir : TEXCOORD6;
    float4 Color : COLOR0;
};

#define	TanSpaceProj	float3x3(IN.Tangent.xyz, IN.BiNormal.xyz, IN.Normal.xyz)

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    OUT.Position = mul(ModelViewProj, IN.Position);
    OUT.BaseUV = IN.BaseUV.xy;
    OUT.CameraDir = mul(TanSpaceProj, normalize(EyePosition.xyz - IN.Position.xyz));
    OUT.Color = IN.Color;
    return OUT;
};
