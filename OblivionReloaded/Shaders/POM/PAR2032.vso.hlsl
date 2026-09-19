// PAR2032.vso: parallax sun specular-only pass, projected shadow. Stock-equivalent output layout.

row_major float4x4 ModelViewProj : register(c0);
float3 LightDirection[3] : register(c13);
float4 EyePosition : register(c25);
row_major float4x4 ShadowProj : register(c28);
float4 ShadowProjData : register(c32);
float4 ShadowProjTransform : register(c33);

struct VS_INPUT {
    float4 Position : POSITION;
    float3 Tangent : TANGENT;
    float3 BiNormal : BINORMAL;
    float3 Normal : NORMAL;
    float4 BaseUV : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 Position : POSITION;
    float2 BaseUV : TEXCOORD0;
    float3 Light0Dir : TEXCOORD1;
    float3 Light0Half : TEXCOORD3;
    float4 ShadowUV : TEXCOORD6;
    float3 CameraDir : TEXCOORD7;
};

#define	TanSpaceProj	float3x3(IN.Tangent.xyz, IN.BiNormal.xyz, IN.Normal.xyz)

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float4 shw = mul(ShadowProj, IN.Position);
    float3 eye = normalize(EyePosition.xyz - IN.Position.xyz);

    OUT.Position = mul(ModelViewProj, IN.Position);
    OUT.BaseUV = IN.BaseUV.xy;
    OUT.Light0Dir = normalize(mul(TanSpaceProj, LightDirection[0].xyz));
    OUT.Light0Half = mul(TanSpaceProj, normalize(eye + LightDirection[0].xyz));
    OUT.ShadowUV.xy = ((shw.w * ShadowProjTransform.xy) + shw.xy) / (shw.w * ShadowProjTransform.w);
    OUT.ShadowUV.zw = ((shw.xy - ShadowProjData.xy) / ShadowProjData.w) * float2(1, -1) + float2(0, 1);
    OUT.CameraDir = normalize(mul(TanSpaceProj, eye));
    return OUT;
};
