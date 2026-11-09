// Sun shadow probe for Oblivion Reloaded: samples the exterior sun shadow maps at the player probe
// points built by SunShadowStealth.cpp. Each point owns a run of ProbeThresholds pixels in a 128x1
// target, and pixel j of the run survives only if the point's raw visibility (0 = occluded, 1 = lit)
// exceeds (j + 0.5) / ProbeThresholds. An occlusion query per point counts the survivors, so
// count / ProbeThresholds is the visibility, with no surface readback.

float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformSkin;
float4x4 TESR_ShadowCameraToLightTransformNearPrev;
float4x4 TESR_ShadowCameraToLightTransformFarPrev;
float4 TESR_ShadowData;
float4 TESR_ShadowBiasDeferred; // zw = near/far depth bias (normalized)
float4 TESR_ShadowBiasSkin; // x = actor (skin map) depth bias (normalized)
float4 TESR_ShadowSkinBounds; // skin map UV rect holding every caster (xy min, zw max)
float4 TESR_ShadowFadeData;

// World-space probe points, uploaded by SunShadowStealth.cpp with SetVectorArray. No TESR_ prefix,
// so EffectRecord::CreateCT leaves it alone.
float4 SunShadowProbePoints[8];

// Bound by ordinal (see ShadowsExteriors.fx.hlsl), so the registers stay contiguous from s0.
sampler2D TESR_ShadowMapBufferNear : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowMapBufferFar : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowMapBufferSkin : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowMapBufferNearPrev : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_ShadowMapBufferFarPrev : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

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

// Pixels per probe point; must match kThresholdsPerPoint in SunShadowStealth.cpp.
static const float ProbeThresholds = 16.0f;

float4 Probe(float2 vpos : VPOS) : COLOR0 {
	float pixel = floor(vpos.x + 0.25f);
	float4 probePoint = SunShadowProbePoints[0];
	[unroll] for (int i = 1; i < 8; i++) {
		if (pixel >= i * ProbeThresholds) probePoint = SunShadowProbePoints[i];
	}
	float threshold = (pixel - floor(pixel / ProbeThresholds) * ProbeThresholds + 0.5f) / ProbeThresholds;

	// Points have no surface normal: depth bias only, no normal offset or terminator ramp.
	float4 pos = mul(float4(probePoint.xyz, 1.0f), TESR_WorldViewProjectionTransform);
	float4 ShadowSkin = mul(pos, TESR_ShadowCameraToLightTransformSkin);
	float biasNear = TESR_ShadowBiasDeferred.z;
	float biasFar = TESR_ShadowBiasDeferred.w;
	float biasSkin = TESR_ShadowBiasSkin.x;

	float visibility = StaticTerm(TESR_ShadowMapBufferNear, TESR_ShadowMapBufferFar,
	                              TESR_ShadowCameraToLightTransformNear, TESR_ShadowCameraToLightTransformFar,
	                              pos, pos, ShadowSkin, biasNear, biasFar, biasSkin, 1.0f);
	if (TESR_ShadowFadeData.x < 1.0f) {
		float prevVisibility = StaticTerm(TESR_ShadowMapBufferNearPrev, TESR_ShadowMapBufferFarPrev,
		                                  TESR_ShadowCameraToLightTransformNearPrev, TESR_ShadowCameraToLightTransformFarPrev,
		                                  pos, pos, ShadowSkin, biasNear, biasFar, biasSkin, 1.0f);
		visibility = lerp(prevVisibility, visibility, TESR_ShadowFadeData.x);
	}
	visibility = saturate(visibility);
	clip(visibility - threshold);
	return float4(visibility, 0.0f, 0.0f, 1.0f);
}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Probe();
	}

}
