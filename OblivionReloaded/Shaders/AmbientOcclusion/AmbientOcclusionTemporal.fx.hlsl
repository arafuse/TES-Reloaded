// Temporal ambient occlusion for Oblivion Reloaded E3
//
// The AmbientOcclusion.fx estimator with noise that changes every frame, accumulated over frames in
// a dedicated history. The temporal technique (R2-offset spatio-temporal noise soaked up by
// accumulation) follows Intel's XeGTAO (MIT): https://github.com/GameTechDev/XeGTAO
//
// Driven by ShaderManager::RenderAmbientOcclusionTemporal: technique Compute runs pass by pass into
// the AO targets, technique Combine runs in the effect chain.

#define viewao 0
#define viewhistory 0

float4x4 TESR_ProjectionTransform;
float4x4 TESR_ViewTransform;
float4x4 TESR_PrevWorldViewProjectionTransform;
float4 TESR_CameraPosition;
float4 TESR_ReciprocalResolution;
float4 TESR_AmbientOcclusionAOData;
float4 TESR_AmbientOcclusionData;
float4 TESR_AmbientOcclusionTemporal;
float4 TESR_FogData;

// Registers must follow declaration order: EffectRecord::CreateCT numbers the TESR_ samplers in
// parameter order and binds each at its ordinal. C++ binds s2 and s3 before every pass.
sampler2D TESR_SourceBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_DepthBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AOBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_AOHistoryBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

static const int samples = 5;
static const int rings = 2;
static const float nearZ = TESR_ProjectionTransform._43 / TESR_ProjectionTransform._33;
static const float farZ = (TESR_ProjectionTransform._33 * nearZ) / (TESR_ProjectionTransform._33 - 1.0f);
static const float Zmul = nearZ * farZ;
static const float Zdiff = farZ - nearZ;
static const float farDistance = abs(farZ);
static const float2 g_InvFocalLen = { tan(0.5 * radians(TESR_ReciprocalResolution.w)) * (1.0f / TESR_ReciprocalResolution.z), tan(0.5 * radians(TESR_ReciprocalResolution.w)) };
static const float depthRange = nearZ - farZ;
static const float depthConstant = depthRange / 20000;
static const float aoMultiplier = 500.0;
static const float angleBiasRadians = radians(TESR_AmbientOcclusionData.x);
static const float cosAngleBias = cos(angleBiasRadians);
static const float sinAngleBias = sin(angleBiasRadians);
static const float2 OffsetMaskH = float2(1.0f, 0.0f);
static const float2 OffsetMaskV = float2(0.0f, 1.0f);
static const float PI = 3.14159265;
static const float MaxHistoryCount = 32.0f;

// 7-tap Gaussian (sigma 2.0), identical to AmbientOcclusion.fx.
static const int cKernelSize = 6;
static const float cCenterWeight = 0.21609f;

static const float BlurWeights[cKernelSize] =
{
	0.07017f,
	0.13107f,
	0.19071f,
	0.19071f,
	0.13107f,
	0.07017f
};

static const float2 BlurOffsets[cKernelSize] =
{
	float2(-3.0f * TESR_ReciprocalResolution.x, -3.0f * TESR_ReciprocalResolution.y),
	float2(-2.0f * TESR_ReciprocalResolution.x, -2.0f * TESR_ReciprocalResolution.y),
	float2(-1.0f * TESR_ReciprocalResolution.x, -1.0f * TESR_ReciprocalResolution.y),
	float2( 1.0f * TESR_ReciprocalResolution.x,  1.0f * TESR_ReciprocalResolution.y),
	float2( 2.0f * TESR_ReciprocalResolution.x,  2.0f * TESR_ReciprocalResolution.y),
	float2( 3.0f * TESR_ReciprocalResolution.x,  3.0f * TESR_ReciprocalResolution.y)
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

/// Interleaved gradient noise (Jimenez 2014): a per-pixel value in [0, 1) built for temporal accumulation.
float InterleavedGradientNoise(float2 Pixel)
{
	return frac(52.9829189f * frac(dot(Pixel, float2(0.06711056f, 0.00583715f))));
}

/// Two per-pixel noise values in [0, 1), offset each frame by the R2 sequence term in TESR_AmbientOcclusionTemporal.xy.
float2 TemporalNoise(float2 Pixel)
{
	float2 Spatial = float2(InterleavedGradientNoise(Pixel), InterleavedGradientNoise(Pixel + float2(47.0f, 17.0f)));
	return frac(Spatial + TESR_AmbientOcclusionTemporal.xy);
}

/// The legacy AO depth: 1 at the far plane, used by the estimator and the blur exactly as in AmbientOcclusion.fx.
float readDepth(in float2 coord : TEXCOORD0)
{
	float posZ = tex2D(TESR_DepthBuffer, coord).x;
	return (2.0f * nearZ) / (nearZ + farZ - posZ * (farZ - nearZ));
}

/// Signed view distance along the camera axis, as TAA.fx decodes it.
float readDepthWorld(in float2 coord : TEXCOORD0)
{
	float posZ = tex2D(TESR_DepthBuffer, coord).x;
	return Zmul / ((posZ * Zdiff) - farZ);
}

/// World-space view ray through a screen UV, from TAA.fx.
float3 toWorld(float2 tex)
{
	float3 v = float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]);
	v += (1 / TESR_ProjectionTransform[0][0] * (2 * tex.x - 1)).xxx * float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]);
	v += (-1 / TESR_ProjectionTransform[1][1] * (2 * tex.y - 1)).xxx * float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]);
	return v;
}

/// World position of the pixel at a screen UV, from TAA.fx.
float4 getWorldPos(in float2 coord : TEXCOORD0)
{
	float depthWorld = readDepthWorld(coord);
	float3 cameraVector = toWorld(coord) * depthWorld;
	return float4(TESR_CameraPosition.xyz + cameraVector, 1.0f);
}

float3 getPosition(in float2 uv, in float eye_z)
{
	uv = (uv * float2(2.0, -2.0) - float2(1.0, -1.0));
	float3 pos = float3(uv * g_InvFocalLen * eye_z, eye_z);
	return pos;
}

float compareDepths(float depth1, float depth2)
{
	float diff = sqrt(saturate(1.0 - (depth1 - depth2) / (TESR_AmbientOcclusionAOData.w / depthRange)));
	float dist = (depth1 - depth2) * depthConstant;
	float ao = min(1, dist * aoMultiplier) * diff;
	float depth1Pos = depth1 * depthRange;

	if (depth1Pos >= 8000) ao *= saturate(lerp(1, 0, (depth1Pos - 8000) / 12500));

	return float(ao);
}

/// The legacy estimator with the ring rotated and its radius drawn per pixel per frame.
/// Output: r = final AO term, g = view depth / far plane (0 marks sky).
float4 AOPass(VSOUT IN, float2 Pixel : VPOS) : COLOR0
{
	float depth = readDepth(IN.UVCoord);
	float3 pos = getPosition(IN.UVCoord, depth);
	float depthPos = depth * depthRange;
	float dx = normalize(ddx(pos)).z * g_InvFocalLen.x;
	float dy = normalize(ddy(pos)).z * g_InvFocalLen.y;
	float cosdx = dx * cosAngleBias;
	float cosdy = dy * cosAngleBias;
	float sindx = sqrt(1 - dx * dx) * sinAngleBias;
	float sindy = sqrt(1 - dy * dy) * sinAngleBias;
	float ao = 1.0f;
	float viewDepth = 0.0f;

	if (depth < 1) {
		float2 noise = TemporalNoise(Pixel);
		float rotation = noise.x * 2.0f * PI;
		float radius = noise.y * 0.004f;
		float jitter = radius * (1.0f - radius);
		float w = jitter / (g_InvFocalLen.x * depthPos / (TESR_ReciprocalResolution.x * 200000));
		float h = jitter / (g_InvFocalLen.y * depthPos / (TESR_ReciprocalResolution.y * 200000));
		float occlusion = 0.0f;
		float s = 0.0f;

		for (int i = 0; i < rings; i++)
		{
			[unroll]
			for (int j = 0; j < samples * i; j++)
			{
				float angle = float(j) * PI * 2.0 / float(samples * i) + rotation;
				float pw = (cos(angle) * float(i)) * TESR_AmbientOcclusionAOData.x;
				float ph = (sin(angle) * float(i)) * TESR_AmbientOcclusionAOData.x * TESR_ReciprocalResolution.z;
				float anglebias_dx = cosdx + sign(pw) * sindx;
				float anglebias_dy = cosdy + sign(ph) * sindy;
				float depth_pred = depth + depth * anglebias_dx * pw * w + depth * anglebias_dy * ph * h;
				float d = readDepth(float2(IN.UVCoord.x + pw * w, IN.UVCoord.y + ph * h));
				occlusion += compareDepths(depth_pred, d);
				s += 1;
			}
		}
		occlusion /= s;
		if (TESR_FogData.y > TESR_FogData.x && depthPos >= TESR_FogData.x) occlusion *= saturate(lerp(1, 0, (depthPos - TESR_FogData.x) / (TESR_FogData.y - TESR_FogData.x)));
		ao = 1.0 - occlusion * TESR_AmbientOcclusionAOData.y;
		ao = clamp(ao * ao, TESR_AmbientOcclusionAOData.z, 1);
		viewDepth = abs(readDepthWorld(IN.UVCoord)) / farDistance;
	}

	return float4(ao, viewDepth, 0.0f, 1.0f);
}

/// Blends this frame's AO (s2) into the reprojected history (s3).
/// Output: r = accumulated AO, g = view depth / far plane, b = frames accumulated (0 = sky).
float4 AccumulatePS(VSOUT IN) : COLOR0
{
	float2 current = tex2D(TESR_AOBuffer, IN.UVCoord).rg;
	float4 prevClip = mul(getWorldPos(IN.UVCoord), TESR_PrevWorldViewProjectionTransform);
	float2 prevUV = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5f + 0.5f;
	float expectedDepth = abs(prevClip.w) / farDistance;
	float3 history = tex2D(TESR_AOHistoryBuffer, prevUV).rgb;

	bool onScreen = all(prevUV == saturate(prevUV));
	bool depthMatches = abs(history.g - expectedDepth) <= TESR_AmbientOcclusionTemporal.w * expectedDepth;
	bool valid = TESR_AmbientOcclusionTemporal.z > 0.0f && history.b > 0.0f && prevClip.w > 0.0f && onScreen && depthMatches;

	float count = valid ? min(history.b + 1.0f, MaxHistoryCount) : 1.0f;
	float weight = max(1.0f / count, 1.0f - TESR_AmbientOcclusionTemporal.z);
	float ao = valid ? lerp(history.r, current.r, weight) : current.r;

	return current.g > 0.0f ? float4(ao, current.g, count, 1.0f) : float4(1.0f, 0.0f, 0.0f, 1.0f);
}

/// The legacy depth-aware blur over s2. Sky writes 1 instead of clipping: these targets persist
/// across frames, so a clipped pixel would keep stale AO.
float4 BlurPS(VSOUT IN, uniform float2 OffsetMask) : COLOR0
{
	float Depth1 = readDepth(IN.UVCoord);
	float WeightSum = cCenterWeight;
	float ao = tex2D(TESR_AOBuffer, IN.UVCoord).r * WeightSum;

	for (int i = 0; i < cKernelSize; i++)
	{
		float2 uv = IN.UVCoord + (BlurOffsets[i] * OffsetMask) * TESR_AmbientOcclusionData.w;
		float diff = abs((Depth1 - readDepth(uv)) * depthRange);
		float weight = (diff <= TESR_AmbientOcclusionData.z) ? BlurWeights[i] : 0.0f;
		ao += weight * tex2D(TESR_AOBuffer, uv).r;
		WeightSum += weight;
	}

	return float4(Depth1 < 0.9999 ? ao / WeightSum : 1.0f, 0.0f, 0.0f, 1.0f);
}

/// The legacy combine: darkens the scene (s0) by the blurred AO (s2), fading it out on bright pixels.
float4 CombinePS(VSOUT IN) : COLOR0
{
	float3 color = tex2D(TESR_SourceBuffer, IN.UVCoord).rgb;
	float ao = tex2D(TESR_AOBuffer, IN.UVCoord).r;
	float luminance = color.r * 0.3 + color.g * 0.59 + color.b * 0.11;
	float white = 1.0;
	float black = 0.0;
	float lt = luminance - TESR_AmbientOcclusionData.y;
	luminance = clamp(max(black, lt) + max(black, lt) + max(black, lt), 0.0, 1.0);
	ao = lerp(ao, white, luminance);
	color *= ao;

	#if viewhistory
		float frames = tex2D(TESR_AOHistoryBuffer, IN.UVCoord).b / MaxHistoryCount;
		return float4(frames, frames, frames, 1);
	#endif

	#if viewao
		return float4(ao, ao, ao, 1);
	#endif

	return float4(color, 1.0f);
}

technique Compute
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 AOPass();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 AccumulatePS();
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurPS(OffsetMaskH);
	}

	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BlurPS(OffsetMaskV);
	}
}

technique Combine
{
	pass
	{
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 CombinePS();
	}
}
