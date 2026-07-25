// Optimized Parallax Occlusion Mapping core.
//
// Shared by every POM-loop permutation (PAR2002/2006/2010/2018/2024 and
// POMExterior/PAR2022). Replaces the old up-to-160-tap linear search + piecewise
// intersection with an adaptive steep search + fixed binary refinement, and uses
// Welsh offset-limiting to keep the march bounded (no close-up warp / swimming).

float4 TESR_ParallaxData : register(c8);
float4 TESR_TextureData  : register(c9);

#define g_fHeightMapScale       TESR_ParallaxData.x
#define g_fSelfShadowStrength   TESR_ParallaxData.y   // 0 = self-shadowing off (INI-gated)
#define g_nMinSamples           TESR_ParallaxData.z
#define g_nMaxSamples           TESR_ParallaxData.w

#define g_nLODThreshold     10                     // mip level at which POM fully fades to flat
#define g_nRefineSteps      5                      // binary refinement iterations
#define g_nSelfShadowSteps  5                      // light-march taps for self-shadowing

// BaseUV    : the geometric (un-displaced) texture coordinate
// CameraDir : tangent-space eye->fragment vector (need not be normalized)
// LightDirTS : tangent-space direction toward the sun (pass 0 to skip self-shadow)
// uv        : [out] displaced texture coordinate
// ao        : [out] self-shadow term in (0..1]; every POM permutation multiplies by it
void psParallax(in float2 BaseUV, in float3 CameraDir, in float3 LightDirTS, inout float2 uv, inout float ao) {

	ao = 1.0;

	float3 viewTS = normalize(CameraDir);

	// Gradients for tex2Dgrad so mip selection stays continuous inside the loop.
	float2 dx = ddx(BaseUV);
	float2 dy = ddy(BaseUV);

	// Adaptive LOD: estimate the mip level from the texel-space derivatives and
	// skip / fade the effect at distance (cheap far away, no shimmer).
	float2 texelUV  = BaseUV * TESR_TextureData.xy;
	float2 dTexdx   = ddx(texelUV);
	float2 dTexdy   = ddy(texelUV);
	float  maxDelta = max(dot(dTexdx, dTexdx), dot(dTexdy, dTexdy));
	float  mipLevel = max(0.5 * log2(maxDelta), 0.0);

	float2 texSample = BaseUV;

	if (mipLevel < (float)g_nLODThreshold) {

		// Offset limiting (Welsh): the maximum lateral march is the tangent-space
		// view xy scaled by heightScale. |viewTS.xy| <= 1, so the march is
		// inherently bounded and continuous -- no clamp discontinuity to slither
		// across the surface as the view angle changes.
		float2 maxOffset = viewTS.xy * g_fHeightMapScale;

		// Fewer samples head-on (view.z -> 1), more at grazing (view.z -> 0).
		int   numSteps = (int)lerp(g_nMaxSamples, g_nMinSamples, viewTS.z);
		float stepSize = 1.0 / (float)numSteps;
		float2 offsetStep = maxOffset * stepSize;

		// Linear steep search: walk down the ray until we dip below the surface.
		float2 curOffset  = 0.0,  prevOffset = 0.0;
		float  curBound   = 1.0,  prevBound  = 1.0;
		float  curHeight  = 1.0,  prevHeight = 1.0;

		[loop]
		for (int i = 0; i < numSteps; ++i) {
			prevOffset = curOffset;  prevBound = curBound;  prevHeight = curHeight;

			curOffset += offsetStep;
			curBound  -= stepSize;
			curHeight  = tex2Dgrad(TESR_samplerBaseMap, BaseUV - curOffset, dx, dy).a;

			if (curHeight > curBound) break;
		}

		// Binary refinement between the last "above" and "below" samples.
		[unroll]
		for (int j = 0; j < g_nRefineSteps; ++j) {
			float2 midOffset = 0.5 * (prevOffset + curOffset);
			float  midBound  = 0.5 * (prevBound  + curBound);
			float  midHeight = tex2Dgrad(TESR_samplerBaseMap, BaseUV - midOffset, dx, dy).a;

			if (midHeight > midBound) { curOffset = midOffset;  curBound = midBound;  curHeight = midHeight; }
			else                      { prevOffset = midOffset; prevBound = midBound; prevHeight = midHeight; }
		}

		// Line-intersect the last segment for the sub-step hit point.
		float distCur  = curHeight  - curBound;
		float distPrev = prevHeight - prevBound;
		float w = distCur / max(distCur - distPrev, 1e-5);

		texSample = BaseUV - lerp(curOffset, prevOffset, w);

		// Gentle fade to flat at the far edge of the POM range to avoid a hard pop.
		float fade = saturate((mipLevel - (g_nLODThreshold - 2)) * 0.5);
		texSample  = lerp(texSample, BaseUV, fade);
	}

	uv = texSample;

	// Self-shadowing: march the height field from the shaded point toward the sun.
	// If a taller feature blocks the light ray, darken. Routed through `ao` (every
	// POM permutation already multiplies its output by ao). Gated by
	// SelfShadowStrength (0 = off) and skipped when the sun is at/below the surface
	// horizon. At distance the height field flattens, so occ -> 0 and it self-fades.
	if (g_fSelfShadowStrength > 0.0 && LightDirTS.z > 0.0) {
		float2 lightRay = LightDirTS.xy * g_fHeightMapScale;   // longer at low sun -> longer shadows
		float  h0  = tex2Dgrad(TESR_samplerBaseMap, texSample, dx, dy).a;
		float  occ = 0.0;

		[unroll]
		for (int s = 1; s <= g_nSelfShadowSteps; ++s) {
			float k  = (float)s / (float)(g_nSelfShadowSteps + 1);
			float hs = tex2Dgrad(TESR_samplerBaseMap, texSample + lightRay * k, dx, dy).a;
			occ = max(occ, (hs - h0 - k) * (1.0 - k));   // near occluders weighted more
		}

		ao = 1.0 - saturate(occ * g_fSelfShadowStrength);
	}
}
