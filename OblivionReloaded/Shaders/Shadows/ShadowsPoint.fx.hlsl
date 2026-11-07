// Image-space point-light shadows for Oblivion Reloaded.
//
// Runs in interiors AND exteriors, immediately after the exterior sun-shadow apply (which may not
// run at all) and before the near-water surface draw, so water composites over the result.
//
// Each occupied slot owns a shadow cube storing radial distance from the light, normalized by that
// light's far plane. Both the cubes and TESR_ShadowLightPositionN are CAMERA-RELATIVE, and this
// shader reconstructs camera-relative positions too, so sampling needs no matrix at all - which is
// what lets a cached cube stay valid while the camera moves.

float4x4 TESR_ViewTransform;
float4x4 TESR_ProjectionTransform;
float4 TESR_ShadowPointData;      // x = shadow strength scale, z = 1 / cube map size, w = depth bias
float4 TESR_ShadowLightPosition0; // xyz = camera-relative light pos, w = far plane (0 = slot empty)
float4 TESR_ShadowLightPosition1;
float4 TESR_ShadowLightPosition2;
float4 TESR_ShadowLightPosition3;
float4 TESR_ShadowLightLuminance; // one component per slot (x=0..w=3): luma(Diff) * Dimmer

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBufferPreWater : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer0 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer1 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer2 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer3 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_POMDepthBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float nearZ = TESR_ProjectionTransform._43 / TESR_ProjectionTransform._33;
static const float farZ = (TESR_ProjectionTransform._33 * nearZ) / (TESR_ProjectionTransform._33 - 1.0f);
static const float Zmul = nearZ * farZ;
static const float Zdiff = farZ - nearZ;
static const float texelSize = TESR_ShadowPointData.z;
static const float bias = TESR_ShadowPointData.w;

// Half the PCF kernel width in texels: a 4x4 kernel, read as a 5x5 texel grid.
static const int PCF_HALF_WIDTH = 2;

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

float3 toWorld(float2 tex)
{
	float3 v = float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]);
	v += (1 / TESR_ProjectionTransform[0][0] * (2 * tex.x - 1)).xxx * float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]);
	v += (-1 / TESR_ProjectionTransform[1][1] * (2 * tex.y - 1)).xxx * float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]);
	return v;
}

float readDepth(in float2 coord : TEXCOORD0)
{
	// Pre-water depth holds every receiver, submerged floor included, but no
	// water surface.
	float posZ = tex2D(TESR_DepthBufferPreWater, coord).x;
	return Zmul / ((posZ * Zdiff) - farZ);
}

// POM shadow side channel: x = geometric view depth, y = view depth of the parallax relief, written
// by the PAR first-pass shaders. Used only where x still matches this pixel's depth, i.e. where that
// PAR draw is still the visible surface; anything drawn over it later fails the match.
float reliefDepth(in float2 coord, in float depth)
{
	float2 pom = tex2D(TESR_POMDepthBuffer, coord).xy;
	return (abs(pom.x - depth) < depth * 0.001f) ? pom.y : depth;
}

// Weight of a texel `d` texels from the kernel centre in a PCF kernel of `2 * halfWidth` taps spaced
// one texel apart, each tap's compare bilinearly interpolated between its two nearest texels. The
// overlapping tents sum to 1 across the kernel and fall to 0 over its last texel.
float PCFWeight(float d, float halfWidth)
{
	return saturate(halfWidth + 0.5f - abs(d));
}

// Fraction of a bilinearly weighted 4x4 PCF kernel around `lookup` that passes the depth compare, so
// the shadow edge moves continuously instead of in texel steps. The kernel is laid out on the texel
// grid of the cube face `lookup` lands on, and each tap is a direction through a texel centre of that
// face. Taps that spill past the face edge land on the neighbouring face, just off its texel centres,
// which only blurs the seams slightly. The cube must be POINT-sampled: the weights do the filtering.
float CubeLitFraction(samplerCUBE cubeMap, float3 lookup, float receiverDist)
{
	float3 a = abs(lookup);
	float3 major;
	float3 axisU;
	float3 axisV;
	if (a.x >= a.y && a.x >= a.z) {
		major = float3(sign(lookup.x), 0.0f, 0.0f); axisU = float3(0.0f, 1.0f, 0.0f); axisV = float3(0.0f, 0.0f, 1.0f);
	}
	else if (a.y >= a.z) {
		major = float3(0.0f, sign(lookup.y), 0.0f); axisU = float3(1.0f, 0.0f, 0.0f); axisV = float3(0.0f, 0.0f, 1.0f);
	}
	else {
		major = float3(0.0f, 0.0f, sign(lookup.z)); axisU = float3(1.0f, 0.0f, 0.0f); axisV = float3(0.0f, 1.0f, 0.0f);
	}

	float2 face = float2(dot(lookup, axisU), dot(lookup, axisV)) / dot(a, abs(major));
	float2 p = (face * 0.5f + 0.5f) / texelSize;
	float2 centre = floor(p);
	float2 offset = p - centre - 0.5f;
	float2 centreFace = (centre + 0.5f) * (2.0f * texelSize) - 1.0f;

	float lit = 0.0f;
	[unroll] for (int y = -PCF_HALF_WIDTH; y <= PCF_HALF_WIDTH; y++) {
		float wy = PCFWeight(y - offset.y, PCF_HALF_WIDTH);
		[unroll] for (int x = -PCF_HALF_WIDTH; x <= PCF_HALF_WIDTH; x++) {
			float2 f = centreFace + float2(x, y) * (2.0f * texelSize);
			float occluder = texCUBElod(cubeMap, float4(major + axisU * f.x + axisV * f.y, 0.0f)).r;
			lit += (occluder < receiverDist - bias) ? 0.0f : PCFWeight(x - offset.x, PCF_HALF_WIDTH) * wy;
		}
	}
	return lit / (4.0f * PCF_HALF_WIDTH * PCF_HALF_WIDTH);
}

// One light's contribution. Returns the factor to multiply scene color by: 1 = unshadowed.
// Brightness only -- the shadow darkens all three channels equally and never tints them. This is an
// approximation, not a true light subtraction: the result multiplies the already-composited scene
// colour, so a shadowed pixel loses a fraction of ambient and of every other light too, not just this
// one -- it over-darkens where ambient dominates. It also ignores N.L. Still, a dim light casts a
// correspondingly faint shadow and a bright one a deep shadow.
float GetPointShadow(samplerCUBE cubeMap, float4 lightPos, float lum, float3 pixelPos)
{
	float radius = lightPos.w;
	if (radius == 0.0f) return 1.0f;

	float3 dir = pixelPos - lightPos.xyz;
	float len = length(dir);
	float dist = len / radius;
	// Out of reach, or so close that the cube face lookup below would divide by zero.
	if (dist >= 1.0f || len < 0.001f) return 1.0f;

	// Negate Z to match GetCubeFaceAtUp's swapped Z faces; change both together.
	float3 lookup = float3(dir.x, dir.y, -dir.z);

	float lit = CubeLitFraction(cubeMap, lookup, dist);

	// The engine's own quadratic falloff. dist is normalized by the cube far plane:
	// the authored radius, except for carried torches (fixed 257), whose shadow
	// therefore fades out before their light does.
	float att = saturate(1.0f - dist * dist);
	// Clamp lum, not the product: a Dimmer > 1 would flatten unlit to black across
	// much of the radius.
	float unlit = 1.0f - saturate(lum) * att;
	return lerp(unlit, 1.0f, lit);
}

float4 Shadow(VSOUT IN) : COLOR0{
	float3 color = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;

	// Sky guard on depth, not brightness, so sun glare on receivers is shadowed.
	float rawDepth = tex2D(TESR_DepthBufferPreWater, IN.UVCoord).x;
	bool isSky = rawDepth >= 0.9999f;
	if (isSky) {
		return float4(color, 1.0f);
	}

	float depth = reliefDepth(IN.UVCoord, readDepth(IN.UVCoord));
	float3 pixelPos = toWorld(IN.UVCoord) * depth; // camera-relative, like the lights

	float shadow = GetPointShadow(TESR_ShadowCubeMapBuffer0, TESR_ShadowLightPosition0, TESR_ShadowLightLuminance.x, pixelPos);
	shadow *= GetPointShadow(TESR_ShadowCubeMapBuffer1, TESR_ShadowLightPosition1, TESR_ShadowLightLuminance.y, pixelPos);
	shadow *= GetPointShadow(TESR_ShadowCubeMapBuffer2, TESR_ShadowLightPosition2, TESR_ShadowLightLuminance.z, pixelPos);
	shadow *= GetPointShadow(TESR_ShadowCubeMapBuffer3, TESR_ShadowLightPosition3, TESR_ShadowLightLuminance.w, pixelPos);

	// Scale the combined term (lighter under volumetric fog) so overlapping lights
	// lighten together.
	color.rgb *= lerp(1.0f, saturate(shadow), TESR_ShadowPointData.x);
	return float4(color, 1.0f);

}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}

}
