// Wet actors (WetActors.cpp): darkening and a water-film sheen for base-pass actor pixel shaders.
// TESR_GEOM_Wetness is uploaded per draw, TESR_WetActorData per shader bind. Wetness 0 leaves the
// caller's colour exactly unchanged.

float4 TESR_GEOM_Wetness : register(c102);	// x = wetness 0..1, y = 1 to keep the legacy base-pass highlight
float4 TESR_WetActorData : register(c103);	// x = Darkening, y = Sheen, z = Glossiness

// Darker albedo where water soaks in. SpecMask (normal map alpha) marks shiny, non-porous parts.
float3 WetAlbedo(float3 Albedo, float SpecMask) {
	float Soak = TESR_GEOM_Wetness.x * (1.0f - saturate(SpecMask));
	return Albedo * (1.0f - TESR_WetActorData.x * Soak);
}

// Light reflected by the water film, added to the lit colour: a sky sheen at glancing angles tinted
// by the ambient light, and a tight sun glint. N, V and L are tangent-space and need not be unit length.
float3 WetSheen(float3 N, float3 V, float3 L, float3 SunColor, float3 Ambient) {
	float Wet = TESR_GEOM_Wetness.x * TESR_WetActorData.y;
	[branch] if (Wet <= 0.0f) return 0.0f;
	N = normalize(N);
	V = normalize(V);
	L = normalize(L);
	float Fresnel = pow(1.0f - saturate(dot(N, V)), 5.0f);
	float3 H = normalize(L + V);
	float Glint = pow(saturate(dot(N, H)), max(TESR_WetActorData.z, 1.0f)) * saturate(dot(N, L) * 4.0f);
	return Wet * (Fresnel * Ambient + Glint * SunColor);
}

// The view direction from a half vector H = normalize(L + V) and the light direction L.
float3 WetViewFromHalf(float3 H, float3 L) {
	H = normalize(H);
	L = normalize(L);
	return 2.0f * dot(H, L) * H - L;
}
