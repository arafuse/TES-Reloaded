// Sun shadow cascade lookup shared by ShadowsExteriors.fx.hlsl and ShadowProbe.fx.hlsl.
// Declares nothing: the including effect must declare TESR_ShadowData, TESR_ShadowLightDir,
// TESR_ShadowMapBufferSkin and a `darkness` float before including it. Samplers must stay in the
// including file, because TextureManager::LoadTexture parses only the top-level source.
//
// The shadow map samplers must be POINT-filtered: LitFraction does its own filtering of the compare
// results, and hardware LINEAR filtering would interpolate the stored depths across silhouettes.

// Weight of a texel `d` texels from the kernel centre in a PCF kernel of `2 * halfWidth` taps spaced
// one texel apart, each tap's compare bilinearly interpolated between its two nearest texels. The
// overlapping tents sum to 1 across the kernel and fall to 0 over its last texel.
float PCFWeight(float d, float halfWidth) {
	return saturate(halfWidth + 0.5f - abs(d));
}

// Fraction of a (2 * halfWidth)^2 PCF kernel centred on uv that passes the depth compare, with each
// tap bilinearly weighted between texels so the edge moves continuously instead of in texel steps.
// Neighbouring taps share texels, so the whole kernel reads one (2 * halfWidth + 1)^2 grid centred on
// the texel under uv. halfWidth must be a compile-time constant for the loops to unroll.
//
// Explicit LOD 0 rather than tex2D: the shadow maps are created with a single mip level
// (CreateShadowMapSurfaces passes Levels=1), so this is lossless. tex2D's implicit derivatives would
// force fxc to flatten the crossfade's `if (TESR_ShadowFadeData.x < 1.0f)` into an unconditional
// double evaluation (ddx/ddy cannot be computed inside divergent flow control).
float LitFraction(sampler2D map, float2 uv, float texelSize, float receiverZ, float bias, int halfWidth) {
	float2 p = uv / texelSize;
	float2 centre = floor(p);
	float2 offset = p - centre - 0.5f;
	float2 centreUV = (centre + 0.5f) * texelSize;
	float lit = 0.0f;
	[unroll] for (int y = -halfWidth; y <= halfWidth; y++) {
		float wy = PCFWeight(y - offset.y, halfWidth);
		[unroll] for (int x = -halfWidth; x <= halfWidth; x++) {
			float depth = tex2Dlod(map, float4(centreUV + float2(x, y) * texelSize, 0, 0)).r;
			lit += (depth < receiverZ - bias) ? 0.0f : PCFWeight(x - offset.x, halfWidth) * wy;
		}
	}
	return lit / (4.0f * halfWidth * halfWidth);
}

// Projects a receiver into one cascade's shadow map UV, or returns false when it falls outside it.
bool ToShadowUV(inout float4 ShadowPos) {
	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return false;
	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;
	return true;
}

// Far cascade: a 2x2 kernel of the far map's coarser texels.
float GetLightAmountFar(sampler2D mapFar, float4 ShadowPos, float bias) {
	if (!ToShadowUV(ShadowPos)) return 1.0f;
	float lit = LitFraction(mapFar, ShadowPos.xy, TESR_ShadowData.w, ShadowPos.z, bias, 1);
	return lerp(darkness, clamp(TESR_ShadowLightDir.w, darkness, 1.0f), lit);
}

// How much of this cascade's ortho box the receiver is inside: 1 well within, ramping to 0 at the
// border. Mirrors the bounds tests in GetLightAmount/GetLightAmountFar, which return "lit" outside
// the box, but as a smooth ramp so the effect fades out instead of ending on a hard circle.
float CascadeCoverage(float4 ShadowPos) {
	float3 ndc = ShadowPos.xyz / ShadowPos.w;
	if (ndc.z < 0.0f || ndc.z > 1.0f) return 0.0f;
	return 1.0f - smoothstep(0.9f, 1.0f, max(abs(ndc.x), abs(ndc.y)));
}

// Actor overlay: project the receiver into the SKIN map's OWN (camera-relative) light space and PCF it,
// then return the shadow term (1 = lit) so it can be min-combined with the static term. Receivers outside
// the skin coverage return lit (no actor overlay there). Called only on the near-in-bounds path.
// The skin map is allocated at the near resolution, so the near texel size (TESR_ShadowData.z) is its
// PCF step too.
float GetLightAmountSkin(float4 ShadowPosSkin, float bias) {
	if (!ToShadowUV(ShadowPosSkin)) return 1.0f;
	float lit = LitFraction(TESR_ShadowMapBufferSkin, ShadowPosSkin.xy, TESR_ShadowData.z, ShadowPosSkin.z, bias, 2);
	return lerp(darkness, 1.0f, lit);
}

// The cascade term for ONE map set: a 4x4 kernel of the near map, falling back to the far cascade
// outside it. There is no previous-bake skin map -- MapSkin is redrawn every frame in its own
// camera-relative light space, and both the current and previous StaticTerm calls are handed the
// SAME current-frame ShadowSkin/GetLightAmountSkin term, so it applies to both and is always
// min-combined below (on the NEAR-IN-BOUNDS PATH ONLY, since the out-of-bounds branch returns
// before it).
float GetLightAmount(sampler2D mapNear, sampler2D mapFar, float4 ShadowPos, float4 ShadowPosFar, float4 ShadowPosSkin, float biasNear, float biasFar) {
	if (!ToShadowUV(ShadowPos)) return GetLightAmountFar(mapFar, ShadowPosFar, biasFar);

	float lit = LitFraction(mapNear, ShadowPos.xy, TESR_ShadowData.z, ShadowPos.z, biasNear, 2);
	float Shadow = lerp(darkness, clamp(TESR_ShadowLightDir.w, darkness, 1.0f), lit);

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
