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

float4 getNormals(float2 UVCoord)
{
	float depth = readDepth(UVCoord);
	float3 pos = getPosition(UVCoord, depth);

	float3 left = pos - getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(-1, 0), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(-1, 0)));
	float3 right = getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(1, 0), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(1, 0))) - pos;
	float3 up = pos - getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(0, -1), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(0, -1)));
	float3 down = getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(0, 1), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(0, 1))) - pos;
	float3 dx = length(left) < length(right) ? left : right;
	float3 dy = length(up) < length(down) ? up : down;
	float3 norm = normalize(cross(dx, dy));

	norm.z *= -1;

	return float4((norm + 1) / 2, 1);
}


float LookupFar(float4 ShadowPos, float2 OffSet) {
	float Shadow = tex2D(TESR_ShadowMapBufferFar, ShadowPos.xy + float2(OffSet.x * TESR_ShadowData.w, OffSet.y * TESR_ShadowData.w)).r;
	if (Shadow < ShadowPos.z - TESR_ShadowBiasDeferred.w) return darkness;
	return clamp(TESR_ShadowLightDir.w, darkness, 1.0f);
}

float GetLightAmountFar(float4 ShadowPos) {

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
			Shadow += LookupFar(ShadowPos, float2(x, y));
		}
	}
	Shadow /= 4.0f;
	return Shadow;

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

float GetLightAmount(float4 WorldPos, float4 ShadowPos, float4 ShadowPosFar, float4 ShadowPosSkin, float bias) {

	float Shadow = 0.0f;
	float x;
	float y;
	float distToExternalLight;

	ShadowPos.xyz /= ShadowPos.w;
	if (ShadowPos.x < -1.0f || ShadowPos.x > 1.0f ||
		ShadowPos.y < -1.0f || ShadowPos.y > 1.0f ||
		ShadowPos.z < 0.0f || ShadowPos.z > 1.0f)
		return GetLightAmountFar(ShadowPosFar);

	ShadowPos.x = ShadowPos.x * 0.5f + 0.5f;
	ShadowPos.y = ShadowPos.y * -0.5f + 0.5f;

	// Perf: 4x4 = 16 taps (was 6x6 = 36). Aggressive quality trade.
	for (y = -1.5f; y <= 1.5f; y += 1.0f) {
		for (x = -1.5f; x <= 1.5f; x += 1.0f) {
			Shadow += Lookup(ShadowPos, float2(x, y), bias);
		}
	}
	Shadow /= 16.0f;

	// Per-frame actor overlay: MapSkin is drawn dynamically each frame in its own camera-relative light
	// space; min-combine its shadow term with the cached static near term.
	Shadow = min(Shadow, GetLightAmountSkin(ShadowPosSkin, bias));

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

	if (world_pos.z > -2147483000.0f) { // pre-water depth excludes the water surface; gate effectively off (sky handled by the length(color) early-out + GetLightAmount frustum bounds)
		float fogCoeff = (saturate((distance(world_pos, TESR_CameraPosition.xyz) - ((TESR_FogData.y - 2000))) / 1000)) + 1.0f;
		float4 pos = mul(world_pos, TESR_WorldViewProjectionTransform);
		float4 farPos = pos;
		float4 world_pos_trans = mul(world_pos, TESR_WorldTransform);
		float4 normal = getNormals(IN.UVCoord);
		float4 lightDir = abs(TESR_ShadowLightDir);

		float3 n = normalize(normal);
		float3 l = normalize(lightDir);
		float cosTheta = clamp(dot(n, l), 0, 1);
		float bias = TESR_ShadowBiasDeferred.z * tan(acos(cosTheta));

		pos.xyz = pos.xyz + (normal.xyz * TESR_ShadowBiasDeferred.x);
		farPos.xyz = farPos.xyz + (normal.xyz * TESR_ShadowBiasDeferred.y);
		float4 ShadowNear = mul(pos, TESR_ShadowCameraToLightTransformNear);
		float4 ShadowFar = mul(farPos, TESR_ShadowCameraToLightTransformFar);
		float4 ShadowSkin = mul(pos, TESR_ShadowCameraToLightTransformSkin);
		float Shadow = GetLightAmount(world_pos_trans, ShadowNear, ShadowFar, ShadowSkin, bias);
		color.rgb *= saturate(Shadow*fogCoeff) * float3(1.0f, 1.0f, 1.0f);
	}
	return float4(color, 1.0f);

}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Shadow();
	}

}
