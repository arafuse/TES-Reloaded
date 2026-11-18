// Grass collision, shared by every GRASS vertex shader override.
//
// Blades are bent by the grass trail map (see GrassTrail.hlsl): one bilinear fetch of the tall-grass
// layer at the blade's world XY gives the push vector, flatten peak and press time, and the spring
// weight turns those into the current bend. The includer declares TESR_GrassTrailWindow (c249),
// TESR_GrassTrailTime (c250), TESR_GrassCollisionParams (c253: y push strength, z flatten strength,
// w = 1 when the trail reaches the batch) and TESR_GrassTrailTall on vertex sampler 0.
//
// The bend is limited by the vertex's height above the root, as a blade can only lean and sink
// as far as it is tall. Short groundcover is pressed in place instead of sliding as one patch.

// Fraction of its height a vertex can lean sideways or sink; below 1 keeps flattened grass off the terrain.
static const float GrassCollisionMaxBend = 0.8;

// The body of GrassCollisionDisplacement, for a batch the trail reaches.
float3 GrassCollisionDisplacementLive(float2 BladeXY, float TipWeight, float BladeHeight) {
    float pushStr = TESR_GrassCollisionParams.y;
    float flatStr = TESR_GrassCollisionParams.z;

    float4 cell = tex2Dlod(TESR_GrassTrailTall, GrassTrailUV(BladeXY));
    float w = GrassTrailSpring(cell.a) * GrassTrailEdgeFade(BladeXY);
    float2 push = cell.rg * 2.0 - 1.0;

    float3 collisionDisp;
    collisionDisp.xy = push * w * pushStr * TipWeight;
    collisionDisp.z = -cell.b * w * flatStr * TipWeight;

    float h = max(BladeHeight, 0.0);
    float maxBend = h * GrassCollisionMaxBend;
    float leanSq = dot(collisionDisp.xy, collisionDisp.xy);
    float leanScale = min(1.0, maxBend * rsqrt(max(leanSq, 0.001)));
    collisionDisp.xy *= leanScale;

    // The tip travels on an arc around the root, so leaning also lowers it.
    float arcDrop = h - sqrt(max(h * h - leanSq * leanScale * leanScale, 0.0));
    collisionDisp.z = max(min(collisionDisp.z, -arcDrop), -maxBend);
    return collisionDisp;
}

// BladeXY     : world XY of the grass instance
// TipWeight   : bend weight of this vertex (0 at the root, 1 at the tip)
// BladeHeight : scaled model-space height of this vertex above the root
// Returns the model-space displacement to add to the vertex position.
//
// A batch the trail does not reach gets c253.w = 0; a real branch on it (uniform across the draw)
// skips the texture fetch.
float3 GrassCollisionDisplacement(float2 BladeXY, float TipWeight, float BladeHeight) {
    float3 collisionDisp = 0;
    [branch]
    if (TESR_GrassCollisionParams.w > 0.5)
        collisionDisp = GrassCollisionDisplacementLive(BladeXY, TipWeight, BladeHeight);
    return collisionDisp;
}
