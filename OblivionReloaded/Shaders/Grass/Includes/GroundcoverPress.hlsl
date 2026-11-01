// Groundcover press, shared by the GRASS pixel shader overrides.
//
// Groundcover is a few large leaf-textured cards, too coarse to bend around a foot, so actors press
// it instead of moving it: inside a foot-sized radius the diffuse texture is warped so the leaves
// squash down toward the root and shrink toward the foot, and the area darkens. The includer
// declares the TESR_GroundcoverPress* constants (c32-c34); the strength in
// TESR_GroundcoverPressParams.y is zero for every draw that is not groundcover.

// Pos          : batch-relative XY (xy) and model-space height (z) of the pixel
// PosDx, PosDy : screen-space derivatives of Pos
// UvDx, UvDy   : screen-space derivatives of the diffuse UV
// Darken       : returns the fraction of light to remove
// Above        : returns 1 where the squashed texture would be sampled above the model's top
// Returns the offset to add to the diffuse UV.
float2 GroundcoverPressOffset(float3 Pos, float3 PosDx, float3 PosDy, float2 UvDx, float2 UvDy, out float Darken, out float Above) {
    float radius = TESR_GroundcoverPressParams.x;
    float strength = TESR_GroundcoverPressParams.y;
    float2 sources[3] = {
        TESR_GroundcoverPressXY0.xy, TESR_GroundcoverPressXY0.zw,
        TESR_GroundcoverPressXY1.xy
    };

    // Unused sources are parked far away by the CPU, so all three are always evaluated.
    float2 pull = 0.0;
    float press = 0.0;
    [unroll]
    for (int i = 0; i < 3; i++) {
        float2 fromSource = Pos.xy - sources[i];
        float d = length(fromSource);
        float spread = saturate(1.0 - d / radius);
        pull += fromSource * spread * spread;
        press = max(press, smoothstep(radius, radius * 0.35, d));
    }
    Darken = press * TESR_GroundcoverPressParams.z;

    // Sampling farther from the foot and higher up the card shrinks the leaves into a dent and squashes
    // them down; sampling closer would magnify them into a bulge.
    float squash = strength * press;
    float3 offset = float3(pull * strength, max(Pos.z, 0.0) * squash / (1.0 - squash));

    // Least-squares fit of the 3D offset onto the card's plane, as a screen-space step.
    float a = dot(PosDx, PosDx);
    float b = dot(PosDx, PosDy);
    float c = dot(PosDy, PosDy);
    float det = a * c - b * b;
    float2 rhs = float2(dot(PosDx, offset), dot(PosDy, offset));
    float2 step = float2(c * rhs.x - b * rhs.y, a * rhs.y - b * rhs.x) * (det / max(det * det, 1e-30));

    // Edge-on cards have no usable plane, so the warp fades out on them.
    step *= smoothstep(0.01, 0.05, det / max(a * c, 1e-30));

    Above = Pos.z + PosDx.z * step.x + PosDy.z * step.y > TESR_GroundcoverPressParams.w ? 1.0 : 0.0;
    return UvDx * step.x + UvDy * step.y;
}
