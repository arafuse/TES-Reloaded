#ifndef __SHADOW_EXTERIOR_DEPENDENCY__
#define __SHADOW_EXTERIOR_DEPENDENCY__

float4 TESR_SunAmount : register(c223);
float4 TESR_ShadowLightDir : register(c222);
float4 TESR_ShadowBiasForward : register(c221);
float4 TESR_ShadowCullLightPosition[18] : register(c203);

#include "../Shadows/Includes/DirectionalSamples.hlsl"

#endif // __SHADOW_EXTERIOR_DEPENDENCY__


float LookupFar(float4 ShadowPos) {
	float Shadow = tex2D(TESR_ShadowMapBufferFar, ShadowPos.xy).r;
	if (Shadow < ShadowPos.z - TESR_ShadowBiasForward.w) return TESR_ShadowData.y;
	return TESR_ShadowLightDir.w;
}

float LookupLeaves(float4 ShadowPos) {
	float Shadow = tex2D(TESR_ShadowMapBufferNear, ShadowPos.xy).r;
	if (Shadow < ShadowPos.z - TESR_ShadowBiasForward.z) return min(TESR_ShadowLightDir.w + 0.4f, TESR_ShadowData.y + 0.6f);
	return saturate(TESR_ShadowLightDir.w + 0.4f);
}

float LookupFarLeaves(float4 ShadowPos) {
	float Shadow = tex2D(TESR_ShadowMapBufferFar, ShadowPos.xy).r;
	if (Shadow < ShadowPos.z - TESR_ShadowBiasForward.w) return min(TESR_ShadowLightDir.w + 0.4f, TESR_ShadowData.y + 0.6f);
	return saturate(TESR_ShadowLightDir.w + 0.4f);
}

float GetLightAmountFar(float4 ShadowPos) {
	return 1.0f; // SHADOWS DISABLED: directional shadow sampling dummied out (reference code below)

	float Shadow = 0.0f;
	float x;
	float y;

	ShadowPos.xyz /= ShadowPos.w;
	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
	for (uint s = 0; s < SAMPLE_NUM_FAR; s++) {
		ShadowPos.xy += (POISSON_SAMPLES[s] * RADIUS_FAR);
		Shadow += LookupFar(ShadowPos);
	}

	Shadow /= SAMPLE_NUM_FAR;
	return Shadow;

}

float GetLightAmountFarLeaves(float4 ShadowPos) {
	return 1.0f; // SHADOWS DISABLED: directional shadow sampling dummied out (reference code below)

	float Shadow = 0.0f;
	float x;
	float y;

	ShadowPos.xyz /= ShadowPos.w;
	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
	
    for (uint s = 0; s < SAMPLE_NUM_LEAVES_FAR; s++)
    {
        ShadowPos.xy += (POISSON_SAMPLES[s] * RADIUS_LEAVES_FAR);
        Shadow += LookupFarLeaves(ShadowPos);
    }
    Shadow /= SAMPLE_NUM_LEAVES_FAR;

	return Shadow;

}

float Lookup(float4 ShadowPos) {
	float Shadow = tex2D(TESR_ShadowMapBufferNear, ShadowPos.xy).r;
	if (Shadow < ShadowPos.z - TESR_ShadowBiasForward.z) return TESR_ShadowData.y;
	return TESR_ShadowLightDir.w;
}

// --- Specular-only sun shadow -------------------------------------------------
// Returns 1 where the sun reaches the surface, 0 where it is occluded. Used ONLY to gate
// the in-shader sun SPECULAR ("glare") in exterior object shaders so glare is not emitted in
// shadow. Diffuse is deliberately NOT gated here -- it is shadowed by the image-space pass
// (ShadowsExteriors.fx); gating it here too would double-darken. Without this gate, a bright
// specular highlight pushes the pixel past that pass's `length(color) > 1.4` bright-pixel
// early-out, so the glare survives on top of shadowed ground as a washed-out blob.
// ShadowPos/ShadowPosFar are the near/far light-space coords the object VS already computes
// (mul(clipPos, TESR_ShadowCameraToLightTransform[0]/[1])). Cheap: 4 near taps + far 1-tap.
float GetSpecularShadow(float4 ShadowPos, float4 ShadowPosFar) {
	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z <  0.0f || ShadowPos.z > 1.0f) {
		// Outside the near frustum: fall back to the far map (single tap).
		ShadowPosFar.xyz /= ShadowPosFar.w;
		if (ShadowPosFar.x < -1.0f || ShadowPosFar.x > 1.0f ||
			ShadowPosFar.y < -1.0f || ShadowPosFar.y > 1.0f ||
			ShadowPosFar.z <  0.0f || ShadowPosFar.z > 1.0f)
			return 1.0f;
		ShadowPosFar.x = ShadowPosFar.x * 0.5f + 0.5f;
		ShadowPosFar.y = ShadowPosFar.y * -0.5f + 0.5f;
		float df = tex2D(TESR_ShadowMapBufferFar, ShadowPosFar.xy).r;
		return (df < ShadowPosFar.z - TESR_ShadowBiasForward.w) ? 0.0f : 1.0f;
	}
	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
	// 4-tap PCF on the near map to soften the specular cutoff along shadow edges.
	float lit = 0.0f;
	for (uint s = 0; s < 4; s++) {
		float d = tex2D(TESR_ShadowMapBufferNear, ShadowPos.xy + POISSON_SAMPLES[s] * RADIUS).r;
		lit += (d < ShadowPos.z - TESR_ShadowBiasForward.z) ? 0.0f : 1.0f;
	}
	return lit * 0.25f;
}

float GetLightAmount(float4 ShadowPos, float4 ShadowPosFar, float4 InvPos) {
	return 1.0f; // SHADOWS DISABLED: directional shadow sampling dummied out (reference code below)

	float Shadow = 0.0f;
	float x;
	float y;
	float distToExternalLight = 0.0f;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return GetLightAmountFar(ShadowPosFar);

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;

	for (uint s = 0; s < SAMPLE_NUM; s++) {
		ShadowPos.xy += (POISSON_SAMPLES[s] * RADIUS);
		Shadow += Lookup(ShadowPos);
	}
	Shadow /= SAMPLE_NUM;

	for (int i = 0; i < 12; i++) {
		if (TESR_ShadowLightPosition[i].w) {
			distToExternalLight = distance(InvPos.xyz, TESR_ShadowLightPosition[i].xyz);
			Shadow += (saturate(1.000f - (distToExternalLight / (TESR_ShadowLightPosition[i].w))) * TESR_SunAmount.w);
		}
	}

	for (int j = 0; j < 18; j++) {
		if (TESR_ShadowCullLightPosition[j].w) {
			distToExternalLight = distance(InvPos.xyz, TESR_ShadowCullLightPosition[j].xyz);
			Shadow += (saturate(1.000f - (distToExternalLight / (TESR_ShadowCullLightPosition[j].w))) * TESR_SunAmount.w);
		}
	}
	return saturate(Shadow);

}

float GetLightAmountLeaves(float4 ShadowPos, float4 ShadowPosFar, float4 InvPos) {
	return 1.0f; // SHADOWS DISABLED: directional shadow sampling dummied out (reference code below)

	float Shadow = 0.0f;
	float x;
	float y;
	float distToExternalLight = 0.0f;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return GetLightAmountFarLeaves(ShadowPosFar);

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
	
    for (uint s = 0; s < SAMPLE_NUM_LEAVES; s++)
    {
        ShadowPos.xy += (POISSON_SAMPLES[s] * RADIUS_LEAVES);
        Shadow += LookupLeaves(ShadowPos);
    }
    Shadow /= SAMPLE_NUM_LEAVES;

	for (int i = 0; i < 12; i++) {
		if (TESR_ShadowLightPosition[i].w) {
			distToExternalLight = distance(InvPos.xyz, TESR_ShadowLightPosition[i].xyz);
			Shadow += (saturate(1.000f - (distToExternalLight / (TESR_ShadowLightPosition[i].w))) * TESR_SunAmount.w);
		}
	}

	for (int j = 0; j < 18; j++) {
		if (TESR_ShadowCullLightPosition[j].w) {
			distToExternalLight = distance(InvPos.xyz, TESR_ShadowCullLightPosition[j].xyz);
			Shadow += (saturate(1.000f - (distToExternalLight / (TESR_ShadowCullLightPosition[j].w))) * TESR_SunAmount.w);
		}
	}
	return saturate(Shadow);

}


float GetLightAmountFarGrass(float4 ShadowPos) {
	return 1.0f; // SHADOWS DISABLED: directional shadow sampling dummied out (reference code below)

	float Shadow = 0.0f;
	float x;
	float y;

	ShadowPos.xyz /= ShadowPos.w;
	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
    for (uint s = 0; s < SAMPLE_NUM_GRASS_FAR; s++)
    {
        ShadowPos.xy += (POISSON_SAMPLES[s] * RADIUS_GRASS_FAR);
        Shadow += LookupFar(ShadowPos);
    }

    Shadow /= SAMPLE_NUM_GRASS_FAR;
	
	return Shadow;

}

float GetLightAmountGrass(float4 ShadowPos, float4 ShadowPosFar, float4 InvPos) {
	return 1.0f; // SHADOWS DISABLED: directional shadow sampling dummied out (reference code below)

	float Shadow = 0.0f;
	float x;
	float y;
	float distToExternalLight = 0.0f;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return GetLightAmountFarGrass(ShadowPosFar);

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
    for (uint s = 0; s < SAMPLE_NUM_GRASS; s++)
    {
        ShadowPos.xy += (POISSON_SAMPLES[s] * RADIUS_GRASS);
        Shadow += Lookup(ShadowPos);
    }
    Shadow /= SAMPLE_NUM_GRASS;

	for (int i = 0; i < 12; i++) {
		if (TESR_ShadowLightPosition[i].w) {
			distToExternalLight = distance(InvPos.xyz, TESR_ShadowLightPosition[i].xyz);
			Shadow += (saturate(1.000f - (distToExternalLight / (TESR_ShadowLightPosition[i].w))) * TESR_SunAmount.w);
		}
	}

	for (int j = 0; j < 18; j++) {
		if (TESR_ShadowCullLightPosition[j].w) {
			distToExternalLight = distance(InvPos.xyz, TESR_ShadowCullLightPosition[j].xyz);
			Shadow += (saturate(1.000f - (distToExternalLight / (TESR_ShadowCullLightPosition[j].w))) * TESR_SunAmount.w);
		}
	}
	return saturate(Shadow);

}