// Image space shadows shader for Oblivion Reloaded

float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ViewTransform;
float4x4 TESR_ProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformSkin;
float4x4 TESR_ShadowCameraToLightTransformNearPrev;
float4x4 TESR_ShadowCameraToLightTransformFarPrev;
float4 TESR_CameraPosition;
float4 TESR_ShadowData;
float4 TESR_ShadowLightDir;
float4 TESR_ReciprocalResolution;
float4 TESR_ShadowBiasDeferred; // adaptive: xy = near/far normal offset (world), zw = near/far depth bias (normalized)
// w is the SUN-ACTIVE flag: 1 while the sun shadow maps are being updated this frame, 0 otherwise
// (published every exterior frame by ShadowManager::RenderExteriorShadows, including sunless ones).
// The apply effect runs on a broader condition than the shadow pass does, so the terminator ramp
// below uses it to switch itself off when the maps are frozen. If nothing ever publishes it, 0 makes
// the ramp disable itself -- the safe failure mode.
float4 TESR_ShadowBiasAdaptive; // x = terminator width, y = max slope clamp, z = adaptive enable, w = sun active (0/1)
// x = static-map crossfade progress: 0 = fully on the previous bake, 1 = fully on the current one.
// 1 is the steady state and the value this shader branches on to skip the previous map set entirely.
float4 TESR_ShadowFadeData;
float4 TESR_FogData;

// TESR_DepthBuffer (s1) and TESR_SourceBuffer (s4) are NOT sampled by this effect any more -- the
// sky guard reads TESR_DepthBufferPreWater instead. They stay declared regardless, because for an
// EFFECT the runtime binds textures by ORDINAL, not by the declared register: EffectRecord::CreateCT
// passes the Nth TESR_ sampler parameter's index to TextureManager::LoadTexture, which then matches
// it against the register(sN) it parses out of this source. Deleting a sampler from the middle
// renumbers every ordinal after it while the registers here stay put, and every later sampler binds
// the wrong texture. Removing these two means renumbering the whole block contiguously; until that
// is done and tested they cost two SetTexture calls a frame and nothing else.
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNear : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFar : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBufferPreWater : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferSkin : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNearPrev : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFarPrev : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_POMDepthBuffer : register(s9) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float nearZ = TESR_ProjectionTransform._43 / TESR_ProjectionTransform._33;
static const float farZ = (TESR_ProjectionTransform._33 * nearZ) / (TESR_ProjectionTransform._33 - 1.0f);
static const float Zmul = nearZ * farZ;
static const float Zdiff = farZ - nearZ;
static const float darkness = TESR_ShadowData.y; // INI [Exteriors] Darkness (lower = darker shadows). Preshader.

struct VSOUT
{
	float4 vertPos : POSITION;
	float4 normal : TEXCOORD1;
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
	// Resolved mid-scene before the first near-water draw, so it holds every
	// receiver (land, grass, submerged floor) but no near-water surface.
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

float3 getPosition(in float2 tex, in float depth)
{
	return (TESR_CameraPosition.xyz + toWorld(tex) * depth);
}

// Screen-space normal reconstruction: the world-space cross of the depth derivatives. The SIGN is
// deliberately left unresolved -- the cross product's orientation depends on tap winding, and the two
// bias paths disambiguate it differently (the adaptive path flips toward the camera, the legacy path
// mirrors z and encodes). Both derive from this single result so the depth taps happen only once.
float3 getRawNormal(float2 UVCoord)
{
	float depth = readDepth(UVCoord);
	float3 pos = getPosition(UVCoord, depth);

	float3 left = pos - getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(-1, 0), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(-1, 0)));
	float3 right = getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(1, 0), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(1, 0))) - pos;
	float3 up = pos - getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(0, -1), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(0, -1)));
	float3 down = getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(0, 1), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(0, 1))) - pos;
	// Shorter derivative wins: at a silhouette the far side spans a depth jump.
	float3 dx = length(left) < length(right) ? left : right;
	float3 dy = length(up) < length(down) ? up : down;

	return normalize(cross(dx, dy));
}


float AddProximityLight(float4 WorldPos, float4 ExternalLightPos) {

	if (ExternalLightPos.w) {
		float distToExternalLight = distance(WorldPos.xyz, ExternalLightPos.xyz);
		return (saturate(1.000f - (distToExternalLight / (ExternalLightPos.w))));
	}
	return 0.0f;
}

#include "ShadowsExteriorsLookup.hlsl"

float4 Shadow(VSOUT IN) : COLOR0{
	float3 color = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;

	// Sky guard on depth, not brightness, so sun glare on receivers is shadowed.
	float rawDepth = tex2D(TESR_DepthBufferPreWater, IN.UVCoord).x;
	bool isSky = rawDepth >= 0.9999f;
	if (isSky) {
		return float4(color, 1.0f);
	}

	// The receiver follows the parallax relief; the normal keeps geometric depth.
	float depth = reliefDepth(IN.UVCoord, readDepth(IN.UVCoord));
	float3 camera_vector = toWorld(IN.UVCoord) * depth;
	float4 world_pos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);

	// Maps and matrices freeze below SunUpThreshold; sampling them then makes the
	// shadows swim. A branch, not an early return: that would put the PCF loops in
	// another dynamic branch (fxc X3570 unroll count 285 -> 497).
	bool sunMapsLive = TESR_ShadowBiasAdaptive.w >= 0.5f;
	if (sunMapsLive) {
		float fogCoeff = (saturate((distance(world_pos, TESR_CameraPosition.xyz) - ((TESR_FogData.y - 2000))) / 1000)) + 1.0f;
		float3 raw = getRawNormal(IN.UVCoord);

		float4 posNear;
		float4 posFar;
		float biasNear;
		float biasFar;
		float facing;

		if (TESR_ShadowBiasAdaptive.z > 0.5f) {
			// Resolve the reconstruction's sign: a visible surface must face the camera.
			float3 viewRay = normalize(toWorld(IN.UVCoord));
			float3 N = (dot(raw, viewRay) > 0.0f) ? -raw : raw;
			// Already normalized on the CPU; normalize() would turn the zero moon
			// direction published when !MoonsExist into NaN.
			float3 toSun = TESR_ShadowLightDir.xyz;
			float ndl = dot(N, toSun);

			// Optional terminator ramp against acne, off by default: N is the per-triangle
			// geometric normal, so any ramp flat-shades smooth meshes. The depth compare
			// already shadows sun-away faces along the real silhouette.
			facing = TESR_ShadowBiasAdaptive.x > 0.0f ? smoothstep(0.0f, TESR_ShadowBiasAdaptive.x, ndl) : 1.0f;


			// Clamped tan(acos(|ndl|)). abs(), since max(ndl, ..) gave sun-away faces the
			// peak bias and thin walls stopped shadowing their own inner face.
			float ndlSafe = max(abs(ndl), 0.05f);
			float slope = min(sqrt(saturate(1.0f - ndlSafe * ndlSafe)) / ndlSafe, TESR_ShadowBiasAdaptive.y);
			biasNear = TESR_ShadowBiasDeferred.z * (1.0f + slope);
			biasFar = TESR_ShadowBiasDeferred.w * (1.0f + slope);

			// World-space normal offset, growing with tilt, applied before the transforms.
			// TESR_ShadowBiasDeferred.xy arrive pre-scaled to world units per cascade.
			float sinT = sqrt(saturate(1.0f - ndl * ndl));
			posNear = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.x * sinT, 1.0f), TESR_WorldViewProjectionTransform);
			posFar = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.y * sinT, 1.0f), TESR_WorldViewProjectionTransform);
		}
		else {
			// Legacy path, bit-exact with the pre-adaptive shader: z-mirrored encoded
			// normal, abs()'d light dir, unclamped slope, flat far bias, and a clip-space
			// normal offset.
			float4 normal = float4((float3(raw.x, raw.y, -raw.z) + 1) / 2, 1);
			float4 lightDir = abs(TESR_ShadowLightDir);
			float3 n = normalize(normal);
			float3 l = normalize(lightDir);
			float cosTheta = clamp(dot(n, l), 0, 1);
			biasNear = TESR_ShadowBiasDeferred.z * tan(acos(cosTheta));
			biasFar = TESR_ShadowBiasDeferred.w;

			float4 pos = mul(world_pos, TESR_WorldViewProjectionTransform);
			posNear = pos;
			posFar = pos;
			posNear.xyz = posNear.xyz + (normal.xyz * TESR_ShadowBiasDeferred.x);
			posFar.xyz = posFar.xyz + (normal.xyz * TESR_ShadowBiasDeferred.y);
			facing = 1.0f;
		}

		float4 ShadowSkin = mul(posNear, TESR_ShadowCameraToLightTransformSkin);

		float Shadow = StaticTerm(TESR_ShadowMapBufferNear, TESR_ShadowMapBufferFar,
		                          TESR_ShadowCameraToLightTransformNear, TESR_ShadowCameraToLightTransformFar,
		                          posNear, posFar, ShadowSkin, biasNear, biasFar, facing);

		// Crossfade from the previous static bake while a rebake fades in over
		// [Exteriors] FadeTime; in steady state .x is 1 and the branch is skipped.
		if (TESR_ShadowFadeData.x < 1.0f) {
			float prevShadow = StaticTerm(TESR_ShadowMapBufferNearPrev, TESR_ShadowMapBufferFarPrev,
			                              TESR_ShadowCameraToLightTransformNearPrev, TESR_ShadowCameraToLightTransformFarPrev,
			                              posNear, posFar, ShadowSkin, biasNear, biasFar, facing);
			Shadow = lerp(prevShadow, Shadow, TESR_ShadowFadeData.x);
		}

		color.rgb *= saturate(Shadow * fogCoeff) * float3(1.0f, 1.0f, 1.0f);
	}
	return float4(color, 1.0f);

}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}

}
