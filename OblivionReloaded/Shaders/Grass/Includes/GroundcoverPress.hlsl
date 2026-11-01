// Groundcover press, shared by the GRASS pixel shader overrides.
//
// Groundcover is a few large leaf-textured sheets, too coarse to bend around a foot, so actors
// press it instead of moving it: inside a foot-sized radius the leaves thin out in blotches and
// darken. The includer declares the TESR_GroundcoverPress* constants (c32-c34); the strength in
// TESR_GroundcoverPressParams.y is zero for every draw that is not groundcover.
//
// The thinning is a noise dissolve, not a raised alpha-test reference: grass alpha is effectively
// binary, so a reference below 1 never removes an opaque texel.

// World units per blotch of the thinning pattern, about one leaf across.
static const float GroundcoverBlotchSize = 3.0;

// Sine-free hash (Dave Hoskins' hash12), much cheaper than a sincos per lookup.
float GroundcoverHash(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float GroundcoverBlotch(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = GroundcoverHash(i);
    float b = GroundcoverHash(i + float2(1, 0));
    float c = GroundcoverHash(i + float2(0, 1));
    float d = GroundcoverHash(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Pos     : batch-relative XY of the pixel
// Darken  : returns the fraction of light to remove
// Cleared : returns 1 where the pressed leaves are thinned away, else 0
void GroundcoverPress(float2 Pos, out float Darken, out float Cleared) {
    float radius = TESR_GroundcoverPressParams.x;
    float2 sources[3] = {
        TESR_GroundcoverPressXY0.xy, TESR_GroundcoverPressXY0.zw,
        TESR_GroundcoverPressXY1.xy
    };

    // Unused sources are parked far away by the CPU, so all three are always evaluated.
    float press = 0.0;
    [unroll]
    for (int i = 0; i < 3; i++)
        press = max(press, smoothstep(radius, radius * 0.35, distance(Pos, sources[i])));

    Darken = press * TESR_GroundcoverPressParams.z;
    Cleared = 0.0;
    [branch]
    if (press > 0.0)
        Cleared = GroundcoverBlotch(Pos / GroundcoverBlotchSize) < press * TESR_GroundcoverPressParams.y ? 1.0 : 0.0;
}
