// Scene light sums shared by ShadowsPoint.fx.hlsl and ShadowsExteriors.fx.hlsl, so each shadow darkens a
// pixel by only its own light's share. The engine shades albedo * (ambient + sum of light * N.L) with no
// clamp, so a light's share is an exact ratio and albedo cancels. Luma only: shadows never tint.
// Published by ShadowManager::PublishPointLightConstants and PublishPointBaseLight. Positions are
// camera-relative, like the pixel positions the including effects reconstruct.
// Declares constants only: samplers must stay in the including file, because
// TextureManager::LoadTexture parses only the top-level source.

float4 TESR_ShadowLightPosition0; // per point-shadow slot: xyz = camera-relative light pos, w = far plane (0 = slot empty)
float4 TESR_ShadowLightPosition1;
float4 TESR_ShadowLightPosition2;
float4 TESR_ShadowLightPosition3;
float4 TESR_ShadowSlotLuminance;  // one component per slot (x=0..w=3): luma(Diff) * Dimmer, without the fade
float4 TESR_ShadowBaseLight;      // x = ambient luma, y = directional light luma, z = 1 when any point light is published
float4 TESR_ShadowDirLight;       // xyz = unit vector toward the directional light
float4x4 TESR_ShadowFillLights0;  // unslotted scene lights, one per row: xyz camera-relative, w = radius
float4x4 TESR_ShadowFillLights1;
float4 TESR_ShadowFillLuminance0; // one component per TESR_ShadowFillLights0 row (0 = empty)
float4 TESR_ShadowFillLuminance1;

// A point light's falloff times N.L at a pixel: multiplied by its luminance, the light it adds there.
// The engine's own quadratic falloff over the light's radius.
float PointGeometry(float3 toLight, float radius, float3 N)
{
	float len = length(toLight);
	float dist = len / radius;
	return saturate(1.0f - dist * dist) * saturate(dot(N, toLight / max(len, 0.001f)));
}

// Ambient plus directional (sun, moon or interior) light at a surface with normal N.
float BaseLightAt(float3 N)
{
	return TESR_ShadowBaseLight.x + TESR_ShadowBaseLight.y * saturate(dot(N, TESR_ShadowDirLight.xyz));
}

float FillMatrixLight(float4x4 lights, float4 lum, float3 pixelPos, float3 N)
{
	float light = 0.0f;
	[unroll] for (int i = 0; i < 4; i++) {
		if (lum[i] > 0.0f) light += lum[i] * PointGeometry(lights[i].xyz - pixelPos, lights[i].w, N);
	}
	return light;
}

// Light the unslotted scene lights add at the pixel.
float FillLightAt(float3 pixelPos, float3 N)
{
	return FillMatrixLight(TESR_ShadowFillLights0, TESR_ShadowFillLuminance0, pixelPos, N) +
	       FillMatrixLight(TESR_ShadowFillLights1, TESR_ShadowFillLuminance1, pixelPos, N);
}

float SlotGeometry(float4 lightPos, float3 pixelPos, float3 N)
{
	return lightPos.w > 0.0f ? PointGeometry(lightPos.xyz - pixelPos, lightPos.w, N) : 0.0f;
}

// Light the shadow-slot lights add at the pixel, ignoring their own shadows.
float SlotLightAt(float3 pixelPos, float3 N)
{
	return SlotGeometry(TESR_ShadowLightPosition0, pixelPos, N) * TESR_ShadowSlotLuminance.x +
	       SlotGeometry(TESR_ShadowLightPosition1, pixelPos, N) * TESR_ShadowSlotLuminance.y +
	       SlotGeometry(TESR_ShadowLightPosition2, pixelPos, N) * TESR_ShadowSlotLuminance.z +
	       SlotGeometry(TESR_ShadowLightPosition3, pixelPos, N) * TESR_ShadowSlotLuminance.w;
}
