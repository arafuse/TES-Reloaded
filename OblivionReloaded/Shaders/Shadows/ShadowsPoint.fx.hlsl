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
float4 TESR_ShadowPointData;      // y = darkness, z = 1 / cube map size, w = depth bias
float4 TESR_ShadowLightPosition0; // xyz = camera-relative light pos, w = far plane (0 = slot empty)
float4 TESR_ShadowLightPosition1;
float4 TESR_ShadowLightPosition2;
float4 TESR_ShadowLightPosition3;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBufferPreWater : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
samplerCUBE TESR_ShadowCubeMapBuffer0 : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer1 : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer2 : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
samplerCUBE TESR_ShadowCubeMapBuffer3 : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; ADDRESSW = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };

static const float nearZ = TESR_ProjectionTransform._43 / TESR_ProjectionTransform._33;
static const float farZ = (TESR_ProjectionTransform._33 * nearZ) / (TESR_ProjectionTransform._33 - 1.0f);
static const float Zmul = nearZ * farZ;
static const float Zdiff = farZ - nearZ;
static const float darkness = TESR_ShadowPointData.y; // INI [Point] Darkness. Preshader.
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
	// Pre-water depth: resolved just before the near-water draw, so it holds every shadow receiver
	// including the submerged floor, and never the water surface itself.
	float posZ = tex2D(TESR_DepthBufferPreWater, coord).x;
	return Zmul / ((posZ * Zdiff) - farZ);
}

// One light's contribution. Returns the factor to multiply scene color by: 1 = unshadowed.
// `dir` is the camera-relative vector from the light to this pixel.
float GetPointShadow(samplerCUBE cubeMap, float4 lightPos, float3 pixelPos)
{
	if (lightPos.w == 0.0f) return 1.0f; // empty slot

	float3 dir = pixelPos - lightPos.xyz;
	float len = length(dir);
	float dist = len / lightPos.w;
	if (dist >= 1.0f) return 1.0f; // beyond this light's reach

	// The cube was rendered with GetCubeFaceAtUp's swapped Z faces; negating Z here is what makes
	// the lookup agree with it. Change one and you must change the other.
	float3 lookup = float3(dir.x, dir.y, -dir.z);

	// PCF offsets must be perpendicular to the lookup direction, otherwise the kernel collapses
	// wherever the direction is dominated by Z. One texel of a 90-degree face subtends
	// ~(pi/2)/size radians, which at this distance is len * 1.57 * texelSize.
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

	float shadow = lerp(darkness, 1.0f, lit);
	// Fade the shadow out as the receiver approaches the light's radius, so the shadowed region
	// does not end in a hard circle where the cube's coverage stops.
	return lerp(shadow, 1.0f, smoothstep(0.85f, 1.0f, dist));
}

float4 Shadow(VSOUT IN) : COLOR0{
	float3 color = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;

	// Sky and other very bright pixels receive no point shadow (same early-out as the sun apply).
	if (length(color) > 1.4f) {
		return float4(color, 1.0f);
	}

	float depth = readDepth(IN.UVCoord);
	float3 pixelPos = toWorld(IN.UVCoord) * depth; // camera-relative, same space as the light positions

	float shadow = GetPointShadow(TESR_ShadowCubeMapBuffer0, TESR_ShadowLightPosition0, pixelPos);
	shadow *= GetPointShadow(TESR_ShadowCubeMapBuffer1, TESR_ShadowLightPosition1, pixelPos);
	shadow *= GetPointShadow(TESR_ShadowCubeMapBuffer2, TESR_ShadowLightPosition2, pixelPos);
	shadow *= GetPointShadow(TESR_ShadowCubeMapBuffer3, TESR_ShadowLightPosition3, pixelPos);

	color.rgb *= saturate(shadow);
	return float4(color, 1.0f);

}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}

}
