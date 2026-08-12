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
// x = static-map crossfade progress: 0 = fully on the previous bake, 1 = fully on the current one.
// 1 is the steady state and the value this shader branches on to skip the previous map set entirely.
float4 TESR_ShadowFadeData;
float4 TESR_FogData;

sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNear : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFar : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_SourceBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBufferPreWater : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferSkin : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNearPrev : register(s7) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFarPrev : register(s8) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

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
	// Perf: 2x2 = 4 taps (was 3x3 = 9). Aggressive quality trade.
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

	// Perf: 4x4 = 16 taps (was 6x6 = 36). Aggressive quality trade.
	for (y = -1.5f; y <= 1.5f; y += 1.0f) {
		for (x = -1.5f; x <= 1.5f; x += 1.0f) {
			Shadow += Lookup(mapNear, ShadowPos, float2(x, y), biasNear);
		}
	}
	Shadow /= 16.0f;

	// Both crossfade sets apply the same current-frame skin overlay -- see the function comment.
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

			// Optional terminator ramp: a surface pointing away from the sun is self-shadowed by its
			// own geometry, so ramping it to the shadow term directly keeps it away from the depth
			// compare, where no bias value can stop acne.
			//
			// OFF BY DEFAULT (BiasTerminatorWidth = 0), because it FLAT-SHADES the scene. N here is
			// reconstructed from depth derivatives, so it is the GEOMETRIC face normal: constant
			// across each triangle. Any visible term driven by it is therefore constant across each
			// triangle too, and this one multiplies whole triangles by exactly `darkness` while the
			// engine's own Gouraud (vertex-normal) shading still lights them -- smooth meshes break
			// up into hard-edged facets. The depth compare handles these surfaces correctly anyway:
			// a surface turned away from the sun has its object's lit side in the shadow map, so it
			// tests as occluded, and the resulting boundary follows the real silhouette instead of
			// triangle edges. Set a small width (0.15 was the old default) only to trade facets back
			// for suppressing terminator-band acne.
			facing = TESR_ShadowBiasAdaptive.x > 0.0f ? smoothstep(0.0f, TESR_ShadowBiasAdaptive.x, ndl) : 1.0f;


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

		float4 ShadowSkin = mul(posNear, TESR_ShadowCameraToLightTransformSkin);

		// Current map set. The comments that used to sit on the lerps below now live in StaticTerm:
		// the terminator ramp collapses to the raw map term at facing == 1, and the coverage ramp
		// fades the whole term out where the cascades hold no data (LOD terrain, distant statics,
		// tree billboards all sit outside the far box, where a screen-space normal is meaningless).
		float Shadow = StaticTerm(TESR_ShadowMapBufferNear, TESR_ShadowMapBufferFar,
		                          TESR_ShadowCameraToLightTransformNear, TESR_ShadowCameraToLightTransformFar,
		                          posNear, posFar, ShadowSkin, biasNear, biasFar, facing);

		// Crossfade against the previous static bake while one is in flight, so a rebake -- whatever
		// triggered it: cell load/unload, drift past the guard band, sun rotation -- ramps in over
		// [Exteriors] FadeTime instead of switching in one frame. TESR_ShadowFadeData.x is 1 in
		// steady state, so this branch is not taken and the previous set costs nothing but the test.
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
