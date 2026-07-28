// Image space shadows shader for Oblivion Reloaded

float4x4 TESR_WorldTransform;
float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ViewTransform;
float4x4 TESR_ProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformSkin;
float4 TESR_CameraPosition;
float4 TESR_WaterSettings;
float4 TESR_ShadowData;
float4 TESR_ShadowLightPosition0;
float4 TESR_ShadowLightPosition1;
float4 TESR_ShadowLightPosition2;
float4 TESR_ShadowLightPosition3;
float4 TESR_ShadowLightPosition4;
float4 TESR_ShadowLightPosition5;
float4 TESR_ShadowLightPosition6;
float4 TESR_ShadowLightPosition7;
float4 TESR_ShadowLightPosition8;
float4 TESR_ShadowLightPosition9;
float4 TESR_ShadowLightPosition10;
float4 TESR_ShadowLightPosition11;
float4 TESR_ShadowCullLightPosition0;
float4 TESR_ShadowCullLightPosition1;
float4 TESR_ShadowCullLightPosition2;
float4 TESR_ShadowCullLightPosition3;
float4 TESR_ShadowCullLightPosition4;
float4 TESR_ShadowCullLightPosition5;
float4 TESR_ShadowCullLightPosition6;
float4 TESR_ShadowCullLightPosition7;
float4 TESR_ShadowCullLightPosition8;
float4 TESR_ShadowCullLightPosition9;
float4 TESR_ShadowCullLightPosition10;
float4 TESR_ShadowCullLightPosition11;
float4 TESR_ShadowCullLightPosition12;
float4 TESR_ShadowCullLightPosition13;
float4 TESR_ShadowCullLightPosition14;
float4 TESR_ShadowCullLightPosition15;
float4 TESR_ShadowCullLightPosition16;
float4 TESR_ShadowCullLightPosition17;
float4 TESR_SunAmount;
float4 TESR_ShadowLightDir;
float4 TESR_ReciprocalResolution;
float4 TESR_ShadowBiasDeferred;
// w is the SUN-ACTIVE flag: 1 while the sun shadow maps are being updated this frame, 0 otherwise
// (published every exterior frame by ShadowManager::RenderExteriorShadows, including sunless ones).
// The apply effect runs on a broader condition than the shadow pass does, so the terminator ramp
// below uses it to switch itself off when the maps are frozen. If nothing ever publishes it, 0 makes
// the ramp disable itself -- the safe failure mode.
float4 TESR_ShadowBiasAdaptive; // x = terminator width, y = max slope clamp, z = adaptive enable, w = sun active (0/1)
float4 TESR_FogData;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNear : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFar : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBufferPreWater : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferSkin : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

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
	// The apply now runs MID-SCENE, right before the first near-water surface draw (engine order:
	// opaque -> LOD water -> sky -> LOD terrain -> grass -> near water). TESR_DepthBufferPreWater is
	// resolved at that same moment, so this single snapshot holds every shadow receiver — land, grass,
	// and the submerged floor — with no near-water surface in it. No per-pixel waterline select needed.
	float posZ = tex2D(TESR_DepthBufferPreWater, coord).x;
	return Zmul / ((posZ * Zdiff) - farZ);
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
	// Shorter derivative wins: at a silhouette the far side spans a depth discontinuity.
	float3 dx = length(left) < length(right) ? left : right;
	float3 dy = length(up) < length(down) ? up : down;

	return normalize(cross(dx, dy));
}


float LookupFar(float4 ShadowPos, float2 OffSet, float bias) {
	float Shadow = tex2D(TESR_ShadowMapBufferFar, ShadowPos.xy + float2(OffSet.x * TESR_ShadowData.w, OffSet.y * TESR_ShadowData.w)).r;
	if (Shadow < ShadowPos.z - bias) return darkness;
	return clamp(TESR_ShadowLightDir.w, darkness, 1.0f);
}

float GetLightAmountFar(float4 ShadowPos, float bias) {

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
	// Perf: 2x2 = 4 taps (was 3x3 = 9). Aggressive quality trade.
	for (x = -0.5f; x <= 0.5f; x += 1.0f) {
		for (y = -0.5f; y <= 0.5f; y += 1.0f) {
			Shadow += LookupFar(ShadowPos, float2(x, y), bias);
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

float Lookup(float4 ShadowPos, float2 OffSet, float bias) {
	float Shadow = tex2D(TESR_ShadowMapBufferNear, ShadowPos.xy + float2(OffSet.x * TESR_ShadowData.z, OffSet.y * TESR_ShadowData.z)).r;
	if (Shadow < ShadowPos.z - bias) return darkness;
	return clamp(TESR_ShadowLightDir.w, darkness, 1.0f);
}

float AddProximityLight(float4 WorldPos, float4 ExternalLightPos) {

	if (ExternalLightPos.w) {
		float distToExternalLight = distance(WorldPos.xyz, ExternalLightPos.xyz);
		return (saturate(1.000f - (distToExternalLight / (ExternalLightPos.w))));
	}
	return 0.0f;
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
			// TESR_ShadowData.z is the near map's texel size; the skin overlay map is allocated at the same
			// (near) resolution (see CreateShadowMapSurfaces), so it is the correct PCF step here too.
			float s = tex2D(TESR_ShadowMapBufferSkin, ShadowPosSkin.xy + float2(x, y) * TESR_ShadowData.z).r;
			Shadow += (s < ShadowPosSkin.z - bias) ? darkness : 1.0f;
		}
	return Shadow / 16.0f;
}

float GetLightAmount(float4 WorldPos, float4 ShadowPos, float4 ShadowPosFar, float4 ShadowPosSkin, float biasNear, float biasFar) {

	float Shadow = 0.0f;
	float x;
	float y;
	float distToExternalLight;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return GetLightAmountFar(ShadowPosFar, biasFar);

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;

	// Perf: 4x4 = 16 taps (was 6x6 = 36). Aggressive quality trade.
	for (y = -1.5f; y <= 1.5f; y += 1.0f) {
		for (x = -1.5f; x <= 1.5f; x += 1.0f) {
			Shadow += Lookup(ShadowPos, float2(x, y), biasNear);
		}
	}
	Shadow /= 16.0f;

	// Per-frame actor overlay: MapSkin is drawn dynamically each frame in its own camera-relative light
	// space; min-combine its shadow term with the cached static near term.
	Shadow = min(Shadow, GetLightAmountSkin(ShadowPosSkin, biasNear));

	return saturate(Shadow);

}

float4 Shadow(VSOUT IN) : COLOR0{
	float3 color = tex2D(TESR_RenderedBuffer, IN.UVCoord).rgb;

	// Sky guard: pixels at the far plane are sky/backdrop and must never be shadowed (keep full brightness).
	// This REPLACES the old `length(color) > 1.4` bright-pixel early-out. That brightness test also skipped
	// the sun-specular "glare" on real geometry, letting it survive on top of shadowed ground as washed-out
	// blobs. Gating on depth instead lets bright glare on receivers flow through the shadow multiply below
	// (darkened when in shadow, preserved in sun) while still protecting the sky.
	float rawDepth = tex2D(TESR_DepthBufferPreWater, IN.UVCoord).x;
	if (rawDepth >= 0.9999f) {
		return float4(color, 1.0f);
	}

	float depth = readDepth(IN.UVCoord);
	float3 camera_vector = toWorld(IN.UVCoord) * depth;
	float4 world_pos = float4(TESR_CameraPosition.xyz + camera_vector, 1.0f);

	// Sun-active gate. This effect runs on a BROADER condition than the shadow pass that feeds it:
	// it needs only an exterior worldspace, while ShadowManager::SunShadowNeeded() also requires the
	// light to be above SunUpThreshold. Below that threshold the shadow maps AND the per-frame
	// camera->light sample matrices stop being republished while the map textures stay resident, so
	// sampling them with a frozen matrix drifts by exactly the camera delta and the shadows visibly
	// detach and swim with the camera. TESR_ShadowBiasAdaptive.w is published every exterior frame
	// from that same DoSun condition, so gating on it makes the two passes agree: when the maps stop
	// updating, we stop sampling them. That is what makes SunUpThreshold safe at ANY value rather
	// than a setting whose only correct value is the one that disables it.
	//
	// This REPLACES a dead `world_pos.z > -2147483000.0f` test that was always true. Reusing that
	// slot rather than adding an early return is deliberate: an extra return ahead of the PCF loops
	// puts their tex2D calls inside another dynamic branch, and fxc's X3570 unroll count jumps from
	// 285 to 497. Same branch count, no new sampling context.
	if (TESR_ShadowBiasAdaptive.w >= 0.5f) {
		float fogCoeff = (saturate((distance(world_pos, TESR_CameraPosition.xyz) - ((TESR_FogData.y - 2000))) / 1000)) + 1.0f;
		float4 world_pos_trans = mul(world_pos, TESR_WorldTransform);
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
			// Published already normalized by the C++ side (every assignment to ShadowLightDir copies a
			// normalized SunDir/MasserDir), so no normalize() here: besides the wasted ALU, it would turn
			// the zero moon-direction ShaderManager publishes when !MoonsExist into NaN, where the raw
			// dot below just yields a benign 0.
			float3 L = TESR_ShadowLightDir.xyz; // points TOWARD the sun; no abs()
			float ndl = dot(N, L);

			// A surface pointing away from the sun is self-shadowed by its own geometry. Ramp it to
			// the shadow term directly -- it never reaches the depth compare, so no bias value can
			// produce acne there. Smoothstep rather than a hard cutoff because the reconstructed
			// normals are noisy and a binary test would speckle along the terminator.
			// The max() guards BiasTerminatorWidth = 0, which would otherwise divide by zero inside
			// smoothstep and produce NaN; at 1e-4 it degenerates to a hard cutoff, as intended.
			facing = smoothstep(0.0f, max(TESR_ShadowBiasAdaptive.x, 1e-4f), ndl);


			// sqrt(1-x*x)/x == tan(acos(x)), without the transcendentals and clamped. The unclamped
			// form diverges at grazing angles, which is what the legacy path below still does.
			float ndlSafe = max(ndl, 0.05f);
			float slope = min(sqrt(saturate(1.0f - ndlSafe * ndlSafe)) / ndlSafe, TESR_ShadowBiasAdaptive.y);
			biasNear = TESR_ShadowBiasDeferred.z * (1.0f + slope);
			biasFar = TESR_ShadowBiasDeferred.w * (1.0f + slope);

			// World-space normal offset, growing with tilt, applied BEFORE the transform chain.
			// TESR_ShadowBiasDeferred.x/.y arrive pre-scaled from texels to world units per cascade.
			float sinT = sqrt(saturate(1.0f - ndl * ndl));
			posNear = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.x * sinT, 1.0f), TESR_WorldViewProjectionTransform);
			posFar = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.y * sinT, 1.0f), TESR_WorldViewProjectionTransform);
		}
		else {
			// Legacy path, bit-exact with the pre-adaptive shader: encoded normal mirrored in z, an
			// abs()'d light dir (both of which force everything into the positive octant, which is
			// why cosTheta could never tell sun-facing from sun-away), unclamped slope, flat far
			// bias, and a normal offset applied in CLIP space after the WVP multiply.
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
			facing = 1.0f; // no terminator ramp: lerp below collapses to the raw map term
		}

		float4 ShadowNear = mul(posNear, TESR_ShadowCameraToLightTransformNear);
		float4 ShadowFar = mul(posFar, TESR_ShadowCameraToLightTransformFar);
		float4 ShadowSkin = mul(posNear, TESR_ShadowCameraToLightTransformSkin);
		float mapShadow = GetLightAmount(world_pos_trans, ShadowNear, ShadowFar, ShadowSkin, biasNear, biasFar);

		// `darkness` is exactly what Lookup returns on its shadowed branch, so the forced-shadow
		// path and the map path agree. At facing == 1 this collapses to mapShadow to within a ULP
		// (the branch is dynamic, so fxc emits a real `lrp` rather than folding it away) -- far
		// below the quantization of the 8-bit output.
		float Shadow = lerp(darkness, mapShadow, facing);

		// Fade the whole term out where the maps have no data. The far cascade is an ortho box of
		// half-width ShadowMapRadius[MapFar] (8192 by default) around the player, so every distant-LOD
		// receiver -- LOD terrain, distant statics, tree billboards -- sits OUTSIDE it, and both bounds
		// tests above already return "lit" there. The terminator ramp did not: it kept darkening those
		// pixels straight from `facing`, i.e. purely from a screen-space normal, with no shadow map
		// involved at all. At LOD range that normal is meaningless. Depth quantization terraces the far
		// depth buffer (one 24-bit LSB is tens of world units out there, comparable to a pixel's own
		// lateral footprint), so getRawNormal collapses to +/-viewRay in contour bands; and billboards
		// are camera-facing by construction, so they reconstruct as facing the camera everywhere. Both
		// make N.L swing with camera PITCH rather than with the surface, which is the striped shadowing
		// that washes over distant mountains and their trees as you look up and down.
		//
		// Gating on coverage rather than on a distance constant keeps this exact under any
		// ShadowMapRadius, and the ramp also softens the hard edge the bounds tests already had.
		// max() because a receiver inside the near box is served by the near map even where it is at
		// the far box's border (the two cascades snap to independently drifting anchors).
		float coverage = max(CascadeCoverage(ShadowNear), CascadeCoverage(ShadowFar));
		Shadow = lerp(1.0f, Shadow, coverage);

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
