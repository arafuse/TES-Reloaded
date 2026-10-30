// Wet actors (WetActors.cpp): darkening and a water-film sheen for base-pass actor pixel shaders.
// TESR_GEOM_Wetness is uploaded per draw, TESR_WetActorData per shader bind. Wetness 0 leaves the
// caller's colour exactly unchanged.

float4 TESR_GEOM_Wetness : register(c102);	// x = wetness 0..1, y = 1 to keep the legacy base-pass highlight
float4 TESR_WetActorData : register(c103);	// x = Darkening, y = Sheen, z = Glossiness

// Scale for light that a later pass multiplies by the texture: darker where water soaks in, exactly 1
// when dry. SpecMask (normal map alpha) marks shiny, non-porous parts.
float WetLightScale(float SpecMask) {
	float Soak = TESR_GEOM_Wetness.x * (1.0f - saturate(SpecMask));
	return 1.0f - TESR_WetActorData.x * Soak;
}

// Darker albedo where water soaks in. SpecMask (normal map alpha) marks shiny, non-porous parts.
float3 WetAlbedo(float3 Albedo, float SpecMask) {
	return Albedo * WetLightScale(SpecMask);
}

// The unscaled water-film light: a sky sheen at glancing angles tinted by the ambient light, and a
// tight sun glint. N, V and L are tangent-space and unit length. Call only inside a wetness branch.
float3 WetSheenTerms(float3 N, float3 V, float3 L, float3 SunColor, float3 Ambient) {
	float Fresnel = pow(1.0f - saturate(dot(N, V)), 5.0f);
	float3 H = normalize(L + V);
	float Glint = pow(saturate(dot(N, H)), max(TESR_WetActorData.z, 1.0f)) * saturate(dot(N, L) * 4.0f);
	return Fresnel * Ambient + Glint * SunColor;
}

// Water-film light added to the lit colour from a view vector. Inputs need not be unit length.
float3 WetSheen(float3 N, float3 V, float3 L, float3 SunColor, float3 Ambient) {
	float Wet = TESR_GEOM_Wetness.x * TESR_WetActorData.y;
	float3 Sheen = 0.0f;
	[branch] if (Wet > 0.0f) {
		Sheen = Wet * WetSheenTerms(normalize(N), normalize(V), normalize(L), SunColor, Ambient);
	}
	return Sheen;
}

// As WetSheen, with the view vector derived from the half vector H = normalize(L + V) inside the branch.
float3 WetSheenFromHalf(float3 N, float3 H, float3 L, float3 SunColor, float3 Ambient) {
	float Wet = TESR_GEOM_Wetness.x * TESR_WetActorData.y;
	float3 Sheen = 0.0f;
	[branch] if (Wet > 0.0f) {
		H = normalize(H);
		L = normalize(L);
		Sheen = Wet * WetSheenTerms(normalize(N), 2.0f * dot(H, L) * H - L, L, SunColor, Ambient);
	}
	return Sheen;
}
