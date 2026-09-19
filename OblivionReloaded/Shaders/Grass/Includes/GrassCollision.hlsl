// Grass collision, shared by every GRASS vertex shader override.
//
// Up to three sources bend the blades around them. Source 0 is the player at full strength.
// Sources 1 and 2 are fading footprints or nearby actors and carry a spring recovery weight,
// which dips negative near the end of the recovery so the blade whips just past upright
// before settling. The includer declares the TESR_GrassCollision* constants (c253-c255).

// BladeXY   : world XY of the grass instance
// TipWeight : bend weight of this vertex (0 at the root, 1 at the tip)
// Returns the model-space displacement to add to the vertex position.
float3 GrassCollisionDisplacement(float2 BladeXY, float TipWeight) {
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
    return collisionDisp;
}
