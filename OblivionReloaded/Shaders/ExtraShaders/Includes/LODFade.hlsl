// Distant LOD fade-in dither. x = remaining fade (0 = opaque), y = per-frame seed.
float4 TESR_GEOM_FadeParams : register(c110);

// Clips the pixel while its dither threshold is below the remaining fade. Zero, the D3D9 initial
// register value, is fully opaque, and settled draws skip the hash through a uniform branch.
// Interleaved gradient noise, not a sin hash: ps_3_0 expands sin past the 512-slot limit.
void LODFadeClip(float2 vpos) {
    float f = TESR_GEOM_FadeParams.x;
    [branch]
    if (f > 0.0) {
        float n = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))) + TESR_GEOM_FadeParams.y);
        clip(n - f);
    }
}
