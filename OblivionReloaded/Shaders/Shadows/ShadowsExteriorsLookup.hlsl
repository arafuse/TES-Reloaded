// Sun shadow cascade lookup shared by ShadowsExteriors.fx.hlsl and ShadowProbe.fx.hlsl.
// Declares nothing: the including effect must declare TESR_ShadowData, TESR_ShadowLightDir,
// TESR_ShadowMapBufferSkin and a `darkness` float before including it. Samplers must stay in the
// including file, because TextureManager::LoadTexture parses only the top-level source.

// Explicit LOD 0 rather than tex2D: the shadow maps are created with a single mip level
// (CreateShadowMapSurfaces passes Levels=1), so LOD 0 is the only LOD that exists and this is
// lossless -- MAGFILTER/MINFILTER=LINEAR still give bilinear filtering within it, and MIPFILTER has
// nothing to interpolate between. tex2D's implicit derivatives are what forced fxc to flatten the
// crossfade's `if (TESR_ShadowFadeData.x < 1.0f)` into an unconditional double evaluation (ddx/ddy
// cannot be computed inside divergent flow control); tex2Dlod has no derivative dependency, so it
// lets that branch survive as real flow control instead.
float LookupFar(sampler2D mapFar, float4 ShadowPos, float2 OffSet, float bias) {
	float Shadow = tex2Dlod(mapFar, float4(ShadowPos.xy + float2(OffSet.x * TESR_ShadowData.w, OffSet.y * TESR_ShadowData.w), 0, 0)).r;
	if (Shadow < ShadowPos.z - bias) return darkness;
	return clamp(TESR_ShadowLightDir.w, darkness, 1.0f);
}

float GetLightAmountFar(sampler2D mapFar, float4 ShadowPos, float bias) {

	float Shadow = 0.0f;
	float x;
	float y;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return 1.0f;

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
	for (x = -0.5f; x <= 0.5f; x += 1.0f) {
		for (y = -0.5f; y <= 0.5f; y += 1.0f) {
			Shadow += LookupFar(mapFar, ShadowPos, float2(x, y), bias);
		}
	}
	Shadow /= 4.0f;
	return Shadow;

}

// How much of this cascade's ortho box the receiver is inside: 1 well within, ramping to 0 at the
// border. Mirrors the bounds tests in GetLightAmount/GetLightAmountFar, which return "lit" outside
// the box, but as a smooth ramp so the effect fades out instead of ending on a hard circle.
float CascadeCoverage(float4 ShadowPos) {
	float3 ndc = ShadowPos.xyz / ShadowPos.w;
	if (ndc.z < 0.0f || ndc.z > 1.0f) return 0.0f;
	return 1.0f - smoothstep(0.9f, 1.0f, max(abs(ndc.x), abs(ndc.y)));
}

float Lookup(sampler2D mapNear, float4 ShadowPos, float2 OffSet, float bias) {
	float Shadow = tex2Dlod(mapNear, float4(ShadowPos.xy + float2(OffSet.x * TESR_ShadowData.z, OffSet.y * TESR_ShadowData.z), 0, 0)).r;
	if (Shadow < ShadowPos.z - bias) return darkness;
	return clamp(TESR_ShadowLightDir.w, darkness, 1.0f);
}

// Actor overlay: project the receiver into the SKIN map's OWN (camera-relative) light space and PCF it,
// then return the shadow term (1 = lit) so it can be min-combined with the static term. Receivers outside
// the skin coverage return lit (no actor overlay there). Called only on the near-in-bounds path.
float GetLightAmountSkin(float4 ShadowPosSkin, float bias) {
	ShadowPosSkin.xyz /= ShadowPosSkin.w;
	if (ShadowPosSkin.x < -1.0f || ShadowPosSkin.x > 1.0f ||
		ShadowPosSkin.y < -1.0f || ShadowPosSkin.y > 1.0f ||
		ShadowPosSkin.z < 0.0f || ShadowPosSkin.z > 1.0f)
		return 1.0f;
	ShadowPosSkin.x = ShadowPosSkin.x * 0.5f + 0.5f;
	ShadowPosSkin.y = ShadowPosSkin.y * -0.5f + 0.5f;
	float Shadow = 0.0f;
	float x;
	float y;
	for (y = -1.5f; y <= 1.5f; y += 1.0f)
		for (x = -1.5f; x <= 1.5f; x += 1.0f) {
			// The skin map is allocated at the near resolution, so the near texel size
			// (TESR_ShadowData.z) is its PCF step too.
			float s = tex2Dlod(TESR_ShadowMapBufferSkin, float4(ShadowPosSkin.xy + float2(x, y) * TESR_ShadowData.z, 0, 0)).r;
			Shadow += (s < ShadowPosSkin.z - bias) ? darkness : 1.0f;
		}
	return Shadow / 16.0f;
}

// The cascade term for ONE map set. There is no previous-bake skin map -- MapSkin is redrawn every
// frame in its own camera-relative light space, and both the current and previous StaticTerm calls
// are handed the SAME current-frame ShadowSkin/GetLightAmountSkin term, so it applies to both and is
// always min-combined below (on the NEAR-IN-BOUNDS PATH ONLY, since the out-of-bounds branch returns
// before it).
float GetLightAmount(sampler2D mapNear, sampler2D mapFar, float4 ShadowPos, float4 ShadowPosFar, float4 ShadowPosSkin, float biasNear, float biasFar) {

	float Shadow = 0.0f;
	float x;
	float y;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return GetLightAmountFar(mapFar, ShadowPosFar, biasFar);

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;

	for (y = -1.5f; y <= 1.5f; y += 1.0f) {
		for (x = -1.5f; x <= 1.5f; x += 1.0f) {
			Shadow += Lookup(mapNear, ShadowPos, float2(x, y), biasNear);
		}
	}
	Shadow /= 16.0f;

	Shadow = min(Shadow, GetLightAmountSkin(ShadowPosSkin, biasNear));

	return saturate(Shadow);

}

// The complete static shadow term for one map set: project into that set's light space, cascade
// lookup, terminator ramp, then the coverage fade-out -- the same order, and for the current set the
// same arithmetic, the shader has always applied. Factored out so the crossfade can evaluate it once
// per map set without duplicating the cascade logic.
float StaticTerm(sampler2D mapNear, sampler2D mapFar, float4x4 toNear, float4x4 toFar,
                 float4 posNear, float4 posFar, float4 ShadowPosSkin,
                 float biasNear, float biasFar, float facing) {
	float4 ShadowNear = mul(posNear, toNear);
	float4 ShadowFar  = mul(posFar,  toFar);
	float mapShadow = GetLightAmount(mapNear, mapFar, ShadowNear, ShadowFar, ShadowPosSkin, biasNear, biasFar);
	float s = lerp(darkness, mapShadow, facing);
	float coverage = max(CascadeCoverage(ShadowNear), CascadeCoverage(ShadowFar));
	return lerp(1.0f, s, coverage);
}
