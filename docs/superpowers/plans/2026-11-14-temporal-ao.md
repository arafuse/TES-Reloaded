# Temporal Ambient Occlusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the static grain from the existing AO by giving it per-frame noise and accumulating it over frames in a dedicated history, independent of the TAA effect.

**Architecture:** A new effect, `AmbientOcclusionTemporal.fx`, holds the legacy estimator with spatio-temporal noise, an accumulation pass (TAA-style reprojection plus depth rejection), the legacy blur and the legacy combine. C++ (`ShaderManager::RenderAmbientOcclusionTemporal`) drives its `Compute` technique pass by pass into four dedicated render targets (two of them a ping-pong history), then runs its `Combine` technique through the normal effect chain. With `TemporalEnabled = 0`, or on any failure, the untouched legacy `AmbientOcclusion.fx` runs exactly as today.

**Tech Stack:** C++ (MSVC v145, x86), Direct3D 9 + D3DX effects (fx_2_0, vs_3_0/ps_3_0), Win32 INI APIs.

**Spec:** `docs/superpowers/specs/2026-11-14-temporal-ao-design.md`

## Global Constraints

- `OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusion.fx.hlsl` (and `.hlsl.2`) must NOT change. `git diff --exit-code` on it must pass at every commit.
- In `AmbientOcclusionTemporal.fx.hlsl`, sampler `register(sN)` numbers MUST equal declaration order (s0 `TESR_SourceBuffer`, s1 `TESR_DepthBuffer`, s2 `TESR_AOBuffer`, s3 `TESR_AOHistoryBuffer`). `EffectRecord::CreateCT` numbers TESR_ samplers by parameter order, and `TextureManager::LoadTexture` matches each one by its `register(sN)` text.
- No `clip`/`discard` anywhere in the new effect. The AO targets persist across frames, so a discarded pixel keeps stale data (memory `discard-illegal-in-chained-effects`).
- Target formats: `AORaw` and `AOBlur` are `D3DFMT_G16R16F`; `History0` and `History1` are `D3DFMT_A16B16G16R16F`. All are full resolution, created lazily on the first temporal frame.
- Engine frame index: `*(UInt32*)((UInt8*)TheRenderManager->vertexBufferMgr + 0x3C)` (memory `skinning-bone-matrix-cache`). Do NOT use `TESR_Tick.x`/`FrameRateManager::Time`, which is never assigned.
- `TESR_AmbientOcclusionTemporal` layout: `.xy` R2 offset `frac(0.5 + (frame % 64) * (0.75487766624669276, 0.56984029099805326))`; `.z` `TemporalBlend`, or 0 on a reset frame; `.w` `TemporalDepthReject`.
- History count cap: 32. Teleport threshold: 1024 world units (same as `kGrassTeleportDist`).
- INI defaults in code: `TemporalEnabled` 0, `TemporalBlend` 0.9, `TemporalDepthReject` 0.05. The shipped `AmbientOcclusion.ini` sets `TemporalEnabled = 1` in both sections.
- Build ONLY via the PowerShell tool (memory `build-via-powershell-not-bash`):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  The post-build step copies the DLL into the game folder, so the game must not be running.
- Compiled effects (`*.fx`) are gitignored and are never committed. Produce `AmbientOcclusionTemporal.fx` either with fxc (`/T fx_2_0`, memory `fxc-verify-shader-edits`) into the repo shader folder, which reaches the game through the directory symlink, or with `[Develop] CompileShaders = 1`.
- Stage `AmbientOcclusion.ini` deliberately (`git add <path>`), never through a blanket add of the shader folder (memory `shader-deployment-workflow`).
- Coding style (AGENTS.md): public symbols get documentation comments; inline comments are at most 1-3 lines. Commit subjects follow the repo style, e.g. `feat(AmbientOcclusion): ...`. Memory and design-doc changes go in their own `docs:` commits.
- Recorded refinements of the spec, decided during planning:
  - The blur reads depth per tap from `TESR_DepthBuffer` with the legacy `readDepth`, rather than from a channel of its input. This is the same depth `packDepth` stored, at full float precision.
  - Sky pixels write AO = 1 instead of being clipped.
  - `CombinePS` gains a `viewhistory` debug define (history count as grayscale) beside `viewao`.

**User decisions (already made):**
- "I only care about the noise reduction via temporal soaking - the current AO looks static and grainy."
- "Maybe we can adopt the technique for the current AO implementation?" Keep the legacy estimator, blur and combine; do not port the XeGTAO horizon search.
- "I also want this to work without actually enabling the TAA effect": a dedicated AO accumulation pass, never the color TAA.
- Approach A (dedicated AO targets driven from C++) was chosen over in-chain variants.
- Spec sections 1-4 were approved as presented: architecture, shaders, settings/constants/reset, verification.

## Review Focus

1. **Reprojection convention mismatch.** If `prevClip.w` from `TESR_PrevWorldViewProjectionTransform` is not positive view depth, every pixel fails the depth test, and the AO stays grainy while looking "enabled". Expected: on a still camera, history counts saturate to 32 (white in the `viewhistory` build). Pinned by Task 4 step 3.
2. **Sky and skyline.** Stale target content or a missing sky case would darken the sky or halo the skyline. Expected: the sky stays exactly as with AO off. Pinned by Task 4 step 4.
3. **Toggling from the in-game menu.** `AmbientOcclusion` effect on/off and `TemporalEnabled` on/off must neither crash (a dangling `AmbientOcclusionTemporalEffect`) nor show stale AO for a frame. Pinned by Task 4 step 6.
4. **`EffectChainPingPong = 0` (legacy Render path).** `Combine` must also work through `EffectRecord::Render`, not only `RenderChained`. Pinned by Task 4 step 7.
5. **Effect chain not running exactly once per engine frame** (for example `RenderEffectsBeforeHdr = 1`). That would reset history every frame. Expected: counts still saturate under both HDR orders. Pinned by Task 4 step 3.

---

### Task 1: Temporal AO effect shader

**Goal:** Create `AmbientOcclusionTemporal.fx.hlsl`: the `Compute` technique (AO with temporal noise, accumulate, blur H, blur V) and the `Combine` technique. It must compile with fxc.

**Files:**
- Create: `OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusionTemporal.fx.hlsl`

**Acceptance Criteria:**
- [ ] fxc `/T fx_2_0` compiles the file with exit code 0, both as-is and with `/D viewhistory=1` and `/D viewao=1`
- [ ] Sampler registers s0..s3 follow declaration order, and no `clip`/`discard` appears in the file
- [ ] `AmbientOcclusion.fx.hlsl` is unchanged (`git diff --exit-code` passes)

**Verify:** `& $fxc /nologo /T fx_2_0 /Fo $env:TEMP\aot.fxo OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx.hlsl` → `compilation succeeded`

**Steps:**

- [ ] **Step 1: Create the effect file** with exactly this content (it was compile-checked with fxc during planning):

```hlsl
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
	bool valid = TESR_AmbientOcclusionTemporal.z > 0.0f && history.b > 0.0f && onScreen && depthMatches;

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
```

Write the file as UTF-8 without a BOM. fxc fails on a BOM with "compilation failed; no code produced".

- [ ] **Step 2: Compile all three variants with fxc** (PowerShell, from the repo root)

```powershell
$fxc = 'C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe'
$src = 'OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx.hlsl'
& $fxc /nologo /T fx_2_0 /Fo "$env:TEMP\aot.fxo" $src; "default exit=$LASTEXITCODE"
& $fxc /nologo /T fx_2_0 /D viewhistory=1 /Fo "$env:TEMP\aot_h.fxo" $src; "viewhistory exit=$LASTEXITCODE"
& $fxc /nologo /T fx_2_0 /D viewao=1 /Fo "$env:TEMP\aot_a.fxo" $src; "viewao exit=$LASTEXITCODE"
```

Expected: `compilation succeeded` three times, all `exit=0`.

- [ ] **Step 3: Check the constraints**

```powershell
git diff --exit-code -- OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusion.fx.hlsl; "legacy unchanged exit=$LASTEXITCODE"
Select-String -Path OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx.hlsl -Pattern '\bclip\(|\bdiscard\b'
```

Expected: `legacy unchanged exit=0` and no Select-String output.

- [ ] **Step 4: Commit**

```powershell
git add OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusionTemporal.fx.hlsl
git commit -m "feat(AmbientOcclusion): Add the temporal AO effect"
```

---

### Task 2: Temporal AO settings and shader constant

**Goal:** Add `TemporalEnabled`, `TemporalBlend` and `TemporalDepthReject` to each AO INI section (read, save, menu), and add the `TESR_AmbientOcclusionTemporal` constant with its `.zw` filled per frame. Rendering does not change yet.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h` (`SettingsAmbientOcclusionStruct`, ~line 555)
- Modify: `TESReloaded/Core/SettingManager.cpp` (AO load ~line 816, save ~line 1494, menu read ~line 2246, menu write ~line 2933)
- Modify: `TESReloaded/Core/GameMenuManager.cpp:35` (`BoolValues`)
- Modify: `TESReloaded/Core/ShaderManager.h` (`AmbientOcclusionStruct`, ~line 164)
- Modify: `TESReloaded/Core/ShaderManager.cpp` (constant binding ~line 410, `UpdateAmbientOcclusion` ~line 2341)
- Modify: `OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusion.ini`

**Acceptance Criteria:**
- [ ] Code defaults are `TemporalEnabled` 0, `TemporalBlend` 0.9, `TemporalDepthReject` 0.05; the shipped INI has `TemporalEnabled = 1` in `[Exterior]` and `[Interior]`
- [ ] `TESR_AmbientOcclusionTemporal` binds to `ShaderConst.AmbientOcclusion.Temporal`; `UpdateAmbientOcclusion` sets `TemporalEnabled`, `.z` = `TemporalBlend` and `.w` = `TemporalDepthReject`
- [ ] The three keys are written by the save path and readable/writable from the in-game menu; `TemporalEnabled` is in `BoolValues`
- [ ] The Release x86 build succeeds

**Verify:** the MSBuild command from Global Constraints → exit code 0, and the output ends with `OblivionReloaded.vcxproj -> ...\OblivionReloaded.dll`

**Steps:**

- [ ] **Step 1: Add the settings fields.** In `TESReloaded/Core/SettingManager.h`, change `SettingsAmbientOcclusionStruct` to:

```cpp
struct SettingsAmbientOcclusionStruct {
	bool Enabled;
	float RadiusMultiplier;
	float StrengthMultiplier;
	float ClampStrength;
	float AngleBias;
	float Range;
	float LumThreshold;
	float BlurDropThreshold;
	float BlurRadiusMultiplier;
	bool TemporalEnabled;		///< Run AmbientOcclusionTemporal.fx (per-frame noise + accumulation) instead of AmbientOcclusion.fx
	float TemporalBlend;		///< Steady-state history weight of the temporal accumulation
	float TemporalDepthReject;	///< Relative depth mismatch above which the reprojected history is dropped
};
```

- [ ] **Step 2: Read the keys.** In `SettingManager.cpp`, in the `AmbientOcclusion.ini` loop, directly after `SA.BlurRadiusMultiplier = atof(value);` and before `SettingsAmbientOcclusion[std::string(pNextSection)] = SA;`, insert:

```cpp
		SA.TemporalEnabled = GetPrivateProfileIntA(pNextSection, "TemporalEnabled", 0, Filename);
		GetPrivateProfileStringA(pNextSection, "TemporalBlend", "0.9", value, SettingStringBuffer, Filename);
		SA.TemporalBlend = atof(value);
		GetPrivateProfileStringA(pNextSection, "TemporalDepthReject", "0.05", value, SettingStringBuffer, Filename);
		SA.TemporalDepthReject = atof(value);
```

- [ ] **Step 3: Save the keys.** In the `"AmbientOcclusion"` save branch, directly after the line writing `"StrengthMultiplier"` (`WritePrivateProfileStringA(v->first.c_str(), "StrengthMultiplier", ...)`), insert:

```cpp
				WritePrivateProfileStringA(v->first.c_str(), "TemporalBlend", ToString(v->second.TemporalBlend).c_str(), Filename);
				WritePrivateProfileStringA(v->first.c_str(), "TemporalDepthReject", ToString(v->second.TemporalDepthReject).c_str(), Filename);
				WritePrivateProfileStringA(v->first.c_str(), "TemporalEnabled", ToString(v->second.TemporalEnabled).c_str(), Filename);
```

- [ ] **Step 4: Expose the keys to the menu.** After `Settings["StrengthMultiplier"] = sas->StrengthMultiplier;` (the AO block that starts with `SettingsAmbientOcclusionStruct* sas = GetSettingsAmbientOcclusion(Section);` and `Settings["AngleBias"]`), insert:

```cpp
			Settings["TemporalBlend"] = sas->TemporalBlend;
			Settings["TemporalDepthReject"] = sas->TemporalDepthReject;
			Settings["TemporalEnabled"] = sas->TemporalEnabled;
```

In the menu setter, replace

```cpp
			else if (!strcmp(Setting, "StrengthMultiplier"))
				sas->StrengthMultiplier = Value;
```

(the one in the AO block, whose first branch is `if (!strcmp(Setting, "AngleBias"))`) with

```cpp
			else if (!strcmp(Setting, "StrengthMultiplier"))
				sas->StrengthMultiplier = Value;
			else if (!strcmp(Setting, "TemporalBlend"))
				sas->TemporalBlend = Value;
			else if (!strcmp(Setting, "TemporalDepthReject"))
				sas->TemporalDepthReject = Value;
			else if (!strcmp(Setting, "TemporalEnabled"))
				sas->TemporalEnabled = Value;
```

- [ ] **Step 5: Mark the bool for the menu.** In `TESReloaded/Core/GameMenuManager.cpp:35`, append ` TemporalEnabled` to `BoolValues`:

```cpp
#define BoolValues "Enabled DistantBlur SunGlareEnabled TimeEnabled FPSOverlay SleepingEquipment SwimmingEquipment PurgeCells PurgeTextures FatigueEnabled HealthEnabled InfoEnabled AlphaEnabled DirectionalLightOverride RenderEffectsBeforeHdr UsePostProcessing UseVanillaShaders AnimatedFog JitterEnabled NearShellEnabled TemporalEnabled"
```

- [ ] **Step 6: Add the constant.** In `TESReloaded/Core/ShaderManager.h`, change `ShaderConstants::AmbientOcclusionStruct` to:

```cpp
	struct AmbientOcclusionStruct {
		bool			Enabled;
		bool			TemporalEnabled;
		D3DXVECTOR4		AOData;
		D3DXVECTOR4		Data;
		D3DXVECTOR4		Temporal;	///< TESR_AmbientOcclusionTemporal: xy R2 noise offset, z history weight (0 = reset), w depth reject
	};
```

In `ShaderManager.cpp`, directly after

```cpp
	else if (!strcmp(Name, "TESR_AmbientOcclusionData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.AmbientOcclusion.Data;
```

insert:

```cpp
	else if (!strcmp(Name, "TESR_AmbientOcclusionTemporal"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.AmbientOcclusion.Temporal;
```

- [ ] **Step 7: Fill it per frame.** Replace `ShaderManager::UpdateAmbientOcclusion` with:

```cpp
void ShaderManager::UpdateAmbientOcclusion(ShaderConstants& ShaderConst, SettingsAmbientOcclusionStruct* sas) {
	ShaderConst.AmbientOcclusion.Enabled = sas->Enabled;
	ShaderConst.AmbientOcclusion.TemporalEnabled = sas->TemporalEnabled;
	if (ShaderConst.AmbientOcclusion.Enabled) {
		ShaderConst.AmbientOcclusion.AOData.x = sas->RadiusMultiplier;
		ShaderConst.AmbientOcclusion.AOData.y = sas->StrengthMultiplier;
		ShaderConst.AmbientOcclusion.AOData.z = sas->ClampStrength;
		ShaderConst.AmbientOcclusion.AOData.w = sas->Range;
		ShaderConst.AmbientOcclusion.Data.x = sas->AngleBias;
		ShaderConst.AmbientOcclusion.Data.y = sas->LumThreshold * ShaderConst.SunAmount.y;
		ShaderConst.AmbientOcclusion.Data.z = sas->BlurDropThreshold;
		ShaderConst.AmbientOcclusion.Data.w = sas->BlurRadiusMultiplier;
		ShaderConst.AmbientOcclusion.Temporal.z = sas->TemporalBlend;
		ShaderConst.AmbientOcclusion.Temporal.w = sas->TemporalDepthReject;
	}
}
```

- [ ] **Step 8: Ship the INI.** Replace `OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusion.ini` with the content below. Before replacing, run `git diff` on the file: if it shows uncommitted tuning, keep those values and only add the three keys.

```ini
[Exterior]
Enabled              = 1
RadiusMultiplier     = 15.0
StrengthMultiplier   = 1.2
ClampStrength        = 0.5
AngleBias            = 15.0
Range                = 50.0
LumThreshold         = 0.2
BlurDropThreshold    = 10.0
BlurRadiusMultiplier = 1.0
TemporalEnabled      = 1
TemporalBlend        = 0.9
TemporalDepthReject  = 0.05

[Interior]
Enabled              = 1
RadiusMultiplier     = 15.0
StrengthMultiplier   = 1.2
ClampStrength        = 0.5
AngleBias            = 15.0
Range                = 50.0
LumThreshold         = 0.1
BlurDropThreshold    = 10.0
BlurRadiusMultiplier = 1.0
TemporalEnabled      = 1
TemporalBlend        = 0.9
TemporalDepthReject  = 0.05
```

- [ ] **Step 9: Build** with the MSBuild command from Global Constraints. Expected: exit code 0 and `OblivionReloaded.vcxproj -> ...\OblivionReloaded.dll`.

- [ ] **Step 10: Commit**

```powershell
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp TESReloaded/Core/GameMenuManager.cpp TESReloaded/Core/ShaderManager.h TESReloaded/Core/ShaderManager.cpp OblivionReloaded/Shaders/AmbientOcclusion/AmbientOcclusion.ini
git commit -m "feat(AmbientOcclusion): Add the temporal AO settings and constant"
```

---

### Task 3: Temporal AO orchestration

**Goal:** Load `AmbientOcclusionTemporal.fx` beside the legacy effect. Drive its `Compute` passes into four lazily created targets with ping-pong history, frame-gap and teleport resets, then run `Combine` through the effect chain, falling back to the legacy effect on any failure.

**Files:**
- Modify: `TESReloaded/Core/ShaderManager.h` (`EffectRecord`, `ShaderManager` public methods and members)
- Modify: `TESReloaded/Core/ShaderManager.cpp` (`EffectRecord::RenderPass`, constructor, `CreateEffect`, `DisposeEffect`, `RenderEffects`, `SwitchShaderStatus`, new functions)

**Acceptance Criteria:**
- [ ] With `ShaderConst.AmbientOcclusion.TemporalEnabled` false, or when `RenderAmbientOcclusionTemporal` returns false, the existing `RunEffect(AmbientOcclusionEffect, ...)` call runs unchanged
- [ ] The targets are created on the first temporal frame only; creation failure or a missing technique logs one warning, latches `AOTemporalFailed`, and returns false
- [ ] Reset (`Temporal.z = 0`) when not yet run, when the engine frame is not last + 1, or when the camera moved more than 1024 units; `.xy` gets the R2 offset of `frame % 64`
- [ ] The pass bindings follow the table in Step 5; s2/s3 are unbound before pass 0 and after Combine
- [ ] Toggling the AmbientOcclusion effect from the menu disposes the temporal effect too (no dangling pointer)
- [ ] The Release x86 build succeeds

**Verify:** the MSBuild command from Global Constraints → exit code 0; `OblivionReloaded.log` then contains `Effect loaded: ...AmbientOcclusionTemporal.fx` after the game starts (checked in Task 4)

**Steps:**

- [ ] **Step 1: Declare `RenderPass`.** In `ShaderManager.h`, in `class EffectRecord`, directly after the `RenderChained` declaration, add:

```cpp
	/// Draws one pass of the current technique into Target and leaves Target bound. The caller selects
	/// the technique, calls SetCT, and binds any sampler the pass reads that SetCT does not bind.
	void						RenderPass(IDirect3DDevice9* Device, UINT Pass, IDirect3DSurface9* Target);
```

- [ ] **Step 2: Declare the ShaderManager API and state.** In `ShaderManager.h`, directly after the `void RunEffect(...)` declaration, add:

```cpp
	/// Runs the temporal ambient occlusion (AmbientOcclusionTemporal.fx) in place of AmbientOcclusion.fx.
	/// Returns false when it cannot run (effect not loaded, targets or techniques unavailable); the
	/// caller then runs the legacy effect.
	bool					RenderAmbientOcclusionTemporal(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget);
	/// Creates the temporal AO render targets; on failure releases any it made and returns false.
	bool					CreateAOTemporalTargets();
```

Directly after `IDirect3DSurface9*		TAASurface;`, add:

```cpp
	/// Temporal AO render targets, indexed by AOTemporalTarget. NULL until the first temporal frame.
	enum					AOTemporalTarget { AOTarget_Raw, AOTarget_Blur, AOTarget_History0, AOTarget_History1, AOTarget_Count };
	IDirect3DTexture9*		AOTemporalTexture[AOTarget_Count];
	IDirect3DSurface9*		AOTemporalSurface[AOTarget_Count];
	bool					AOTemporalFailed;		///< Targets or techniques unavailable; the legacy AO runs for the session
	bool					AOTemporalHasRun;		///< A temporal frame has rendered since the targets were created
	int						AOTemporalHistory;		///< 0 or 1: which History target holds the newest accumulation
	UInt32					AOTemporalLastFrame;	///< Engine frame of the last temporal run
	D3DXVECTOR3				AOTemporalLastCamera;	///< Camera position at the last temporal run
```

Directly after `EffectRecord*			AmbientOcclusionEffect;`, add:

```cpp
	EffectRecord*			AmbientOcclusionTemporalEffect;
```

- [ ] **Step 3: Implement `RenderPass` and initialise the state.** In `ShaderManager.cpp`, directly after the closing brace of `EffectRecord::RenderChained`, add:

```cpp
void EffectRecord::RenderPass(IDirect3DDevice9* Device, UINT Pass, IDirect3DSurface9* Target) {

	UINT Passes;

	Device->SetRenderTarget(0, Target);
	Effect->Begin(&Passes, NULL);
	Effect->BeginPass(Pass);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	Effect->EndPass();
	Effect->End();
	EffCountPass();

}
```

In the `ShaderManager` constructor, directly after `AmbientOcclusionEffect = NULL;`, add:

```cpp
	AmbientOcclusionTemporalEffect = NULL;
	for (int i = 0; i < AOTarget_Count; i++) { AOTemporalTexture[i] = NULL; AOTemporalSurface[i] = NULL; }
	AOTemporalFailed = false;
	AOTemporalHasRun = false;
	AOTemporalHistory = 0;
	AOTemporalLastFrame = 0;
	AOTemporalLastCamera = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
```

- [ ] **Step 4: Load and dispose the effect.** In `ShaderManager::CreateEffect`, replace the `EffectRecordType_AmbientOcclusion` case with:

```cpp
		case EffectRecordType_AmbientOcclusion:
			strcat(Filename, "AmbientOcclusion\\AmbientOcclusion.fx");
			AmbientOcclusionEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.AmbientOcclusion = LoadEffect(AmbientOcclusionEffect, Filename, NULL);
			if (TheSettingManager->SettingsMain.Effects.AmbientOcclusion) {
				char TemporalFilename[MAX_PATH];
				strcpy(TemporalFilename, EffectsPath);
				strcat(TemporalFilename, "AmbientOcclusion\\AmbientOcclusionTemporal.fx");
				AmbientOcclusionTemporalEffect = new EffectRecord();
				if (!LoadEffect(AmbientOcclusionTemporalEffect, TemporalFilename, NULL))
					Logger::Log("WARNING: AmbientOcclusionTemporal.fx not loaded; the legacy ambient occlusion runs. Enable CompileShaders once to build it.");
			}
			break;
```

(`LoadEffect` already calls `DisposeEffect` on failure, which leaves the pointer NULL once the next edit is in.)

In `ShaderManager::DisposeEffect`, directly after `if (TheEffect == AmbientOcclusionEffect) AmbientOcclusionEffect = NULL;`, add:

```cpp
	else if (TheEffect == AmbientOcclusionTemporalEffect) AmbientOcclusionTemporalEffect = NULL;
```

In `ShaderManager::SwitchShaderStatus`, replace the `"AmbientOcclusion"` block with:

```cpp
	if (!strcmp(Name, "AmbientOcclusion")) {
		Value = !Effects->AmbientOcclusion;
		Effects->AmbientOcclusion = Value;
		DisposeEffect(AmbientOcclusionEffect);
		if (AmbientOcclusionTemporalEffect) DisposeEffect(AmbientOcclusionTemporalEffect);
		AOTemporalHasRun = false;
		if (Value) {
			CreateEffect(EffectRecordType_AmbientOcclusion);
		}
	}
```

- [ ] **Step 5: Implement the render path.** In `ShaderManager.cpp`, directly before `void ShaderManager::RenderEffects(IDirect3DSurface9* RenderTarget) {`, add the code below. Pass bindings:

| Pass | s2 | s3 | Target |
|---|---|---|---|
| 0 AO | NULL | NULL | Raw |
| 1 Accumulate | Raw | History[prev] | History[cur] |
| 2 Blur H | History[cur] | NULL | Blur |
| 3 Blur V | Blur | NULL | Raw |
| Combine (chain) | Raw | History[cur] | chain |

```cpp
// A jump this large between temporal frames is a load door or fast travel (same as kGrassTeleportDist).
static const float kAOTemporalTeleportDist = 1024.0f;

// Engine display-frame counter at NiDX9VertexBufferManager + 0x3C: bumped once per frame by 0x777A40
// from the BeginFrame path 0x762600, shared by every pass of the frame.
static UInt32 GetEngineFrame() {

	return *(UInt32*)((UInt8*)TheRenderManager->vertexBufferMgr + 0x3C);

}

bool ShaderManager::CreateAOTemporalTargets() {

	static const D3DFORMAT Formats[AOTarget_Count] = { D3DFMT_G16R16F, D3DFMT_G16R16F, D3DFMT_A16B16G16R16F, D3DFMT_A16B16G16R16F };
	IDirect3DDevice9* Device = TheRenderManager->device;

	for (int i = 0; i < AOTarget_Count; i++) {
		if (FAILED(Device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, Formats[i], D3DPOOL_DEFAULT, &AOTemporalTexture[i], NULL)) ||
			FAILED(AOTemporalTexture[i]->GetSurfaceLevel(0, &AOTemporalSurface[i]))) {
			for (int j = 0; j <= i; j++) {
				if (AOTemporalSurface[j]) { AOTemporalSurface[j]->Release(); AOTemporalSurface[j] = NULL; }
				if (AOTemporalTexture[j]) { AOTemporalTexture[j]->Release(); AOTemporalTexture[j] = NULL; }
			}
			return false;
		}
	}
	return true;

}

bool ShaderManager::RenderAmbientOcclusionTemporal(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget) {

	EffectRecord* Effect = AmbientOcclusionTemporalEffect;
	if (!Effect || AOTemporalFailed) return false;

	if (!AOTemporalSurface[AOTarget_Raw]) {
		if (!CreateAOTemporalTargets()) {
			AOTemporalFailed = true;
			Logger::Log("WARNING: The temporal ambient occlusion targets could not be created; the legacy ambient occlusion runs.");
			return false;
		}
		AOTemporalHasRun = false;
	}
	D3DXHANDLE Compute = Effect->Effect->GetTechniqueByName("Compute");
	D3DXHANDLE Combine = Effect->Effect->GetTechniqueByName("Combine");
	if (!Compute || !Combine) {
		AOTemporalFailed = true;
		Logger::Log("WARNING: AmbientOcclusionTemporal.fx lacks the Compute or Combine technique; the legacy ambient occlusion runs.");
		return false;
	}

	UInt32 Frame = GetEngineFrame();
	D3DXVECTOR3 Camera(TheRenderManager->CameraPosition.x, TheRenderManager->CameraPosition.y, TheRenderManager->CameraPosition.z);
	D3DXVECTOR3 Moved = Camera - AOTemporalLastCamera;
	bool Reset = !AOTemporalHasRun || Frame != AOTemporalLastFrame + 1 ||
		D3DXVec3LengthSq(&Moved) > kAOTemporalTeleportDist * kAOTemporalTeleportDist;

	// R2 sequence offset, cycling every 64 frames as XeGTAO's NoiseIndex does.
	D3DXVECTOR4& Temporal = ShaderConst.AmbientOcclusion.Temporal;
	double NoiseIndex = (double)(Frame % 64);
	Temporal.x = (float)fmod(0.5 + NoiseIndex * 0.75487766624669276, 1.0);
	Temporal.y = (float)fmod(0.5 + NoiseIndex * 0.56984029099805326, 1.0);
	if (Reset) Temporal.z = 0.0f;

	const int Cur = AOTemporalHistory ^ 1;
	IDirect3DTexture9* HistoryPrev = AOTemporalTexture[AOTarget_History0 + AOTemporalHistory];
	IDirect3DTexture9* HistoryCur = AOTemporalTexture[AOTarget_History0 + Cur];

	EffCountEffect();
	EffMark(Effect->ProfileName.c_str());
	Effect->Effect->SetTechnique(Compute);
	Effect->SetCT();
	Device->SetTexture(2, NULL);
	Device->SetTexture(3, NULL);
	Effect->RenderPass(Device, 0, AOTemporalSurface[AOTarget_Raw]);
	Device->SetTexture(2, AOTemporalTexture[AOTarget_Raw]);
	Device->SetTexture(3, HistoryPrev);
	Effect->RenderPass(Device, 1, AOTemporalSurface[AOTarget_History0 + Cur]);
	Device->SetTexture(2, HistoryCur);
	Device->SetTexture(3, NULL);
	Effect->RenderPass(Device, 2, AOTemporalSurface[AOTarget_Blur]);
	Device->SetTexture(2, AOTemporalTexture[AOTarget_Blur]);
	Effect->RenderPass(Device, 3, AOTemporalSurface[AOTarget_Raw]);

	// Combine joins the chain like any effect; SetCT inside RunEffect leaves s2/s3 alone because
	// their texture records are empty.
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetTexture(2, AOTemporalTexture[AOTarget_Raw]);
	Device->SetTexture(3, HistoryCur);
	Effect->Effect->SetTechnique(Combine);
	RunEffect(Effect, Device, RenderTarget, true, false);
	Device->SetTexture(2, NULL);
	Device->SetTexture(3, NULL);

	AOTemporalHistory = Cur;
	AOTemporalLastFrame = Frame;
	AOTemporalLastCamera = Camera;
	AOTemporalHasRun = true;
	return true;

}
```

- [ ] **Step 6: Branch in `RenderEffects`.** Replace

```cpp
		if (Effects->AmbientOcclusion && ShaderConst.AmbientOcclusion.Enabled) {
			RunEffect(AmbientOcclusionEffect, Device, RenderTarget, true, false);
		}
```

with

```cpp
		if (Effects->AmbientOcclusion && ShaderConst.AmbientOcclusion.Enabled) {
			if (!ShaderConst.AmbientOcclusion.TemporalEnabled || !RenderAmbientOcclusionTemporal(Device, RenderTarget))
				RunEffect(AmbientOcclusionEffect, Device, RenderTarget, true, false);
		}
```

- [ ] **Step 7: Build** with the MSBuild command from Global Constraints. Expected: exit code 0 and `OblivionReloaded.vcxproj -> ...\OblivionReloaded.dll`. If `EffCountPass`, `EffCountEffect` or `EffMark` are reported undeclared at `RenderPass` or `RenderAmbientOcclusionTemporal`, they are defined near the top of `ShaderManager.cpp` (~line 100); keep the new code below them.

- [ ] **Step 8: Deploy the compiled effect** (gitignored, reaches the game through the shader-folder symlink):

```powershell
$fxc = 'C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe'
& $fxc /nologo /T fx_2_0 /Fo OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx.hlsl
git status --short OblivionReloaded/Shaders/AmbientOcclusion
```

Expected: `compilation succeeded`. `git status` does not list `AmbientOcclusionTemporal.fx`, because it is ignored.

- [ ] **Step 9: Commit**

```powershell
git add TESReloaded/Core/ShaderManager.h TESReloaded/Core/ShaderManager.cpp
git commit -m "feat(AmbientOcclusion): Accumulate the AO over frames in its own targets"
```

---

### Task 4: In-game verification and findings

**Goal:** Show in-game that the temporal AO converges, handles motion, resets and toggles, leaves the legacy path intact, and costs what was expected. Then record the findings in memory.

**Files:**
- Create: `memory/temporal-ao.md`
- Modify: `memory/MEMORY.md`

**Acceptance Criteria:**
- [ ] `OblivionReloaded.log` shows `Effect loaded: ...AmbientOcclusionTemporal.fx` and no temporal AO warning
- [ ] `viewhistory` build: a still camera turns the screen white (count 32) within about 1 s, under both `RenderEffectsBeforeHdr = 0` and `1`
- [ ] `viewao` build: the sky stays white, the AO grain settles on a still camera, and no dark trails remain after pans or behind walking NPCs
- [ ] `TemporalEnabled = 0` looks like the legacy AO; toggling the AmbientOcclusion effect and `TemporalEnabled` from the menu neither crashes nor flashes stale AO
- [ ] Combine works with `EffectChainPingPong = 0` and `= 1`
- [ ] `ProfileFrame` AO cost is measured for temporal vs legacy, and the numbers are reported to the user
- [ ] `memory/temporal-ao.md` is committed in its own `docs:` commit

**Verify:** the user confirms each in-game check. The `ProfileFrame` log lines for AO (temporal vs legacy) are quoted in the report.

**Steps:**

- [ ] **Step 1: Confirm the load.** Ask the user to start the game and load a save. Then search `C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log` for `AmbientOcclusionTemporal` and `WARNING`. Expected: `Effect loaded: ...AmbientOcclusionTemporal.fx`, and no `temporal ambient occlusion` warning.

- [ ] **Step 2: Build the `viewhistory` debug effect.** Compile it into the deployed name (the user reloads shaders by restarting, or with the effect toggle in the menu):

```powershell
$fxc = 'C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe'
& $fxc /nologo /T fx_2_0 /D viewhistory=1 /Fo OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx OblivionReloaded\Shaders\AmbientOcclusion\AmbientOcclusionTemporal.fx.hlsl
```

- [ ] **Step 3: History check (Review Focus 1 and 5).** Ask the user to stand still outdoors. Expected: the screen fades from dark to white within about 1 s; the sky stays black (count 0). When panning, newly revealed areas go dark and refill. Repeat with `RenderEffectsBeforeHdr = 1` in `OblivionReloaded.ini`, then restore the user's value (0). A screen that stays dark means every pixel is rejected or reset each frame: stop and debug with superpowers-extended-cc:systematic-debugging, starting from the `prevClip.w` sign and the frame-gap test.

- [ ] **Step 4: AO check (Review Focus 2).** Rebuild with `/D viewao=1` (same command as Step 2) and ask the user to compare a still view against the legacy AO (`TemporalEnabled = 0` in the menu). Expected: the same overall shading without the frozen grain, and a uniformly white sky. Then ask the user to strafe past pillars and doorways and to watch NPCs walk. Expected: brief noise at revealed edges that settles; no dark trails or ground smears.

- [ ] **Step 5: Restore the shipping effect** (same command without `/D`) so later checks run the real combine.

- [ ] **Step 6: Toggle check (Review Focus 3).** In the in-game menu, ask the user to toggle the AmbientOcclusion effect off and on twice, and `TemporalEnabled` off and on in both `[Exterior]` and `[Interior]`. Then take a load door and fast travel. Expected: no crash, no frame of stale or misplaced AO.

- [ ] **Step 7: Chain path check (Review Focus 4).** Ask the user to set `EffectChainPingPong = 0` in `OblivionReloaded.ini` and restart. Expected: the AO looks the same as with `= 1`. Restore the user's value (1).

- [ ] **Step 8: Cost.** Ask the user to set `ProfileFrame = 1` (memory `gpu-frame-breakdown` explains how to read it), stand at a fixed spot, and capture the AO effect bucket with `TemporalEnabled = 1` and `= 0`. Report both numbers to the user (expected: about +0.1-0.2 ms GPU).

- [ ] **Step 9: Record findings.** Write `memory/temporal-ao.md` with the measured cost, whether `prevClip.w` worked as positive view depth, any tuning the user settled on, and anything that surprised during Steps 3-8. Use this frontmatter:

```markdown
---
name: temporal-ao
description: <one line naming the measured cost and the key gotcha found>
metadata:
  type: project
---
```

Link the memory to [[effect-quad-uv-and-aspect-bugs]] and [[discard-illegal-in-chained-effects]]. Then add a one-line pointer to `memory/MEMORY.md`:

```markdown
- [Temporal AO](temporal-ao.md) — <same hook as the description>
```

- [ ] **Step 10: Commit the memory**

```powershell
git add memory/temporal-ao.md memory/MEMORY.md
git commit -m "docs: Record the temporal AO verification results"
```
