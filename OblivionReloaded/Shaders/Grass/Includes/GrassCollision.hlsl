// Grass collision, shared by every GRASS vertex shader override.
//
// Up to three sources bend the blades around them. Source 0 is the player at full strength.
// Sources 1 and 2 are fading footprints or nearby actors and carry a spring recovery weight,
// which dips negative near the end of the recovery so the blade whips just past upright
// before settling. The includer declares the TESR_GrassCollision* constants (c253-c255).
//
// The bend is limited by the vertex's height above the root, as a blade can only lean and sink
// as far as it is tall. Short groundcover is pressed in place instead of sliding as one patch.

// Fraction of its height a vertex can lean sideways or sink; below 1 keeps flattened grass off the terrain.
static const float GrassCollisionMaxBend = 0.8;

// BladeXY     : world XY of the grass instance
// TipWeight   : bend weight of this vertex (0 at the root, 1 at the tip)
// BladeHeight : scaled model-space height of this vertex above the root
// Returns the model-space displacement to add to the vertex position.
float3 GrassCollisionDisplacement(float2 BladeXY, float TipWeight, float BladeHeight) {
    float radius = TESR_GrassCollisionParams.x;
    float pushStr = TESR_GrassCollisionParams.y;
    float flatStr = TESR_GrassCollisionParams.z;
    int numSources = (int)TESR_GrassCollisionParams.w;
    float invRadius = 1.0 / max(radius, 0.001);

    float3 collisionDisp = 0;
    float2 sources[3] = {
        TESR_GrassCollisionXY0.xy, TESR_GrassCollisionXY0.zw,
        TESR_GrassCollisionXY1.xy
    };
    float weights[3] = { 1.0, TESR_GrassCollisionXY1.z, TESR_GrassCollisionXY1.w };

    [unroll]
    for (int i = 0; i < 3; i++) {
        if (i >= numSources) break;
        float2 diff = BladeXY - sources[i];
        float dist = length(diff);
        float t = saturate(dist * invRadius);
        float influence = smoothstep(1.0, 0.0, t) * weights[i];

        float2 pushDir = (dist > 0.001) ? (diff / dist) : float2(1, 0);
        float pushFade = smoothstep(0.0, 0.3, t);
        collisionDisp.xy += pushDir * influence * pushFade * pushStr * TipWeight;
        collisionDisp.z -= influence * flatStr * TipWeight;
    }

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
