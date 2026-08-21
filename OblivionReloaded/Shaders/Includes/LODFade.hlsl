// Screen-space dither fade for LOD and full-model load transitions.
// x = fade alpha, y = per-frame seed, z = invert flag. c100 is TESR_GEOM_Toggles.
float4 TESR_GEOM_FadeParams : register(c101);

// Clips the pixel unless it falls inside the current fade coverage. The invert flag selects the
// complementary threshold, so a paired in-fade and out-fade together always cover exactly 100%.
void LODFadeClip(float2 vpos) {
    float a = TESR_GEOM_FadeParams.x;
    float n = frac(sin(dot(vpos + TESR_GEOM_FadeParams.y, float2(12.9898, 78.233))) * 43758.5453);
    clip(TESR_GEOM_FadeParams.z > 0.5 ? (n - a) : (a >= 1.0 ? 1.0 : a - n));
}
