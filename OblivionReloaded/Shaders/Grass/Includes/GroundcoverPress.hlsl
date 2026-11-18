// Groundcover press, shared by the GRASS pixel shader overrides.
//
// Groundcover is a few large leaf-textured cards, too coarse to bend around a foot, so actors press
// it instead of moving it: inside a foot-sized radius the diffuse texture is warped so the leaves
// squash down toward the root and shrink toward the foot, and the area darkens. The includer
// declares TESR_GroundcoverPressParams (c32), TESR_GrassTrailWindow (c33), TESR_GrassTrailTime (c34)
// and the ground trail sampler TESR_GrassTrailGround (s4); the strength in
// TESR_GroundcoverPressParams.y is zero for every draw that is not groundcover. The press, its pull
// and its recovery come from one fetch of the grass trail map (see GrassTrail.hlsl).

// Pos          : batch-relative XY (xy) and model-space height (z) of the pixel
// PosDx, PosDy : screen-space derivatives of Pos
// UvDx, UvDy   : screen-space derivatives of the diffuse UV
// Darken       : returns the fraction of light to remove
// Above        : returns 1 where the squashed texture would be sampled above the model's top
// Returns the offset to add to the diffuse UV.
float2 GroundcoverPressOffset(float3 Pos, float3 PosDx, float3 PosDy, float2 UvDx, float2 UvDy, out float Darken, out float Above) {
    float strength = TESR_GroundcoverPressParams.y;
    float4 cell = tex2Dlod(TESR_GrassTrailGround, GrassTrailUV(Pos.xy));
    float w = max(GrassTrailSpring(cell.a), 0.0) * GrassTrailEdgeFade(Pos.xy);
    float2 pull = (cell.rg * 2.0 - 1.0) * GrassTrailPullScale * w;
    float press = cell.b * w;
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
