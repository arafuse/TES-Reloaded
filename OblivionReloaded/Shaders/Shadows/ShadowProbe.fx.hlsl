// Sun shadow probe for Oblivion Reloaded: samples the exterior sun shadow maps at the player probe
// points built by SunShadowStealth.cpp, writing one raw visibility (0 = occluded, 1 = lit) per pixel
// of an 8x1 target.

float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformSkin;
float4x4 TESR_ShadowCameraToLightTransformNearPrev;
float4x4 TESR_ShadowCameraToLightTransformFarPrev;
float4 TESR_ShadowData;
float4 TESR_ShadowBiasDeferred; // zw = near/far depth bias (normalized)
float4 TESR_ShadowFadeData;

// World-space probe points, uploaded by SunShadowStealth.cpp with SetVectorArray. No TESR_ prefix,
// so EffectRecord::CreateCT leaves it alone.
float4 SunShadowProbePoints[8];

// Bound by ordinal (see ShadowsExteriors.fx.hlsl), so the registers stay contiguous from s0.
sampler2D TESR_ShadowMapBufferNear : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFar : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferSkin : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNearPrev : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFarPrev : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

// The lookup returns darkness for an occluded tap and clamp(TESR_ShadowLightDir.w, darkness, 1) for
// a lit one; pinning those to 0 and 1 makes it return raw visibility.
static const float darkness = 0.0f;
static const float4 TESR_ShadowLightDir = float4(0.0f, 0.0f, 0.0f, 1.0f);

#include "ShadowsExteriorsLookup.hlsl"

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

float4 Probe(float2 vpos : VPOS) : COLOR0 {
	float4 probePoint = SunShadowProbePoints[0];
	[unroll] for (int i = 1; i < 8; i++) {
		if (vpos.x > i - 0.5f) probePoint = SunShadowProbePoints[i];
	}

	// Points have no surface normal: depth bias only, no normal offset or terminator ramp.
	float4 pos = mul(float4(probePoint.xyz, 1.0f), TESR_WorldViewProjectionTransform);
	float4 ShadowSkin = mul(pos, TESR_ShadowCameraToLightTransformSkin);
	float biasNear = TESR_ShadowBiasDeferred.z;
	float biasFar = TESR_ShadowBiasDeferred.w;

	float visibility = StaticTerm(TESR_ShadowMapBufferNear, TESR_ShadowMapBufferFar,
	                              TESR_ShadowCameraToLightTransformNear, TESR_ShadowCameraToLightTransformFar,
	                              pos, pos, ShadowSkin, biasNear, biasFar, 1.0f);
	if (TESR_ShadowFadeData.x < 1.0f) {
		float prevVisibility = StaticTerm(TESR_ShadowMapBufferNearPrev, TESR_ShadowMapBufferFarPrev,
		                                  TESR_ShadowCameraToLightTransformNearPrev, TESR_ShadowCameraToLightTransformFarPrev,
		                                  pos, pos, ShadowSkin, biasNear, biasFar, 1.0f);
		visibility = lerp(prevVisibility, visibility, TESR_ShadowFadeData.x);
	}
	return float4(saturate(visibility), 0.0f, 0.0f, 1.0f);
}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Probe();
	}

}
