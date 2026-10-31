// Rain probe for Oblivion Reloaded: tests the precipitation occlusion map at the player probe points
// built by RainProbe.cpp, writing one exposure (1 = open to the rain, 0 = sheltered) per pixel of a
// 56x1 target (8 player points, then 2 per probed NPC). Uses the same compare as Rain.fx.hlsl, so it agrees with the rain that is drawn.

float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformOrtho;
float4 TESR_OrthoData;

// World-space probe points, uploaded by RainProbe.cpp with SetVectorArray. No TESR_ prefix, so
// EffectRecord::CreateCT leaves it alone.
float4 RainProbePoints[56];

sampler2D TESR_OrthoMapBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

static const float4x4 OrthoCombined = mul(TESR_WorldViewProjectionTransform, TESR_ShadowCameraToLightTransformOrtho);

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

// Outside the map counts as open sky, as in Rain.fx.hlsl.
float GetExposure(float4 OrthoPos) {
	OrthoPos.xyz /= OrthoPos.w;
	if (OrthoPos.x < -1.0f || OrthoPos.x > 1.0f ||
		OrthoPos.y < -1.0f || OrthoPos.y > 1.0f ||
		OrthoPos.z <  0.0f || OrthoPos.z > 1.0f)
		return 1.0f;
	OrthoPos.x = OrthoPos.x *  0.5f + 0.5f;
	OrthoPos.y = OrthoPos.y * -0.5f + 0.5f;
	float Ortho = tex2Dlod(TESR_OrthoMapBuffer, float4(OrthoPos.xy, 0.0f, 0.0f)).r;
	return Ortho < OrthoPos.z + TESR_OrthoData.x ? 0.0f : 1.0f;
}

float4 Probe(float2 vpos : VPOS) : COLOR0 {
	float4 probePoint = RainProbePoints[0];
	[unroll] for (int i = 1; i < 56; i++) {
		if (vpos.x > i - 0.5f) probePoint = RainProbePoints[i];
	}
	return float4(GetExposure(mul(float4(probePoint.xyz, 1.0f), OrthoCombined)), 0.0f, 0.0f, 1.0f);
}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Probe();
	}

}
