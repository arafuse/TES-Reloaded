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
samplerCUBE TESR_ShadowCubeMapBuffer0 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer1 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer2 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer3 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_POMDepthBuffer : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float nearZ = TESR_ProjectionTransform._43 / TESR_ProjectionTransform._33;
static const float farZ = (TESR_ProjectionTransform._33 * nearZ) / (TESR_ProjectionTransform._33 - 1.0f);
static const float Zmul = nearZ * farZ;
static const float Zdiff = farZ - nearZ;
static const float texelSize = TESR_ShadowPointData.z;
static const float bias = TESR_ShadowPointData.w;

// 4x4 grid minus its corners: the corners contribute least and cost the same as the rest.
static const int SAMPLE_COUNT = 12;
static const float2 PoissonDisk[12] = {
	float2(-0.5f, -1.5f), float2( 0.5f, -1.5f),
	float2(-1.5f, -0.5f), float2(-0.5f, -0.5f), float2( 0.5f, -0.5f), float2( 1.5f, -0.5f),
	float2(-1.5f,  0.5f), float2(-0.5f,  0.5f), float2( 0.5f,  0.5f), float2( 1.5f,  0.5f),
	float2(-0.5f,  1.5f), float2( 0.5f,  1.5f),
};

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
	// Out of reach, or so close that the PCF basis below would be NaN.
	if (dist >= 1.0f || len < 0.001f) return 1.0f;

	// Negate Z to match GetCubeFaceAtUp's swapped Z faces; change both together.
	float3 lookup = float3(dir.x, dir.y, -dir.z);

	// PCF offsets are perpendicular to the lookup, else the kernel collapses where
	// Z dominates. One texel of a 90-degree face spans ~len * (pi/2) * texelSize.
	float3 n = lookup / len;
	float3 up = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
	float3 tangent = normalize(cross(up, n));
	float3 bitangent = cross(n, tangent);
	float spread = len * 1.6f * texelSize;

	float lit = 0.0f;
	for (int i = 0; i < SAMPLE_COUNT; i++) {
		float3 offset = (tangent * PoissonDisk[i].x + bitangent * PoissonDisk[i].y) * spread;
		float occluder = texCUBE(cubeMap, lookup + offset).r;
		lit += (occluder < dist - bias) ? 0.0f : 1.0f;
	}
	lit /= SAMPLE_COUNT;

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
