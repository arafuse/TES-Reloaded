// Grass collision, shared by every GRASS vertex shader override.
//
// Up to three sources bend the blades around them, picked per batch by the CPU from the ones that
// reach it. Source 0 is at full strength with its radius in c253.x: the player whenever the player
// or a footprint reaches the batch, otherwise an actor. Sources 1 and 2 are described by c255.zw: a
// fading footprint sends its spring recovery weight and shares source 0's radius, while an actor
// sends GrassCollisionActorTag plus its own radius at full strength. The recovery weight dips
// negative near the end so the blade whips just past upright before settling. The includer
// declares the TESR_GrassCollision* constants (c253-c255).
//
// The bend is limited by the vertex's height above the root, as a blade can only lean and sink
// as far as it is tall. Short groundcover is pressed in place instead of sliding as one patch.

// Fraction of its height a vertex can lean sideways or sink; below 1 keeps flattened grass off the terrain.
static const float GrassCollisionMaxBend = 0.8;

// c255.zw values at or above this are an actor's radius offset by the tag; must match
// kGrassCollisionActorTag in ShaderManager.cpp.
static const float GrassCollisionActorTag = 2.0;

// The body of GrassCollisionDisplacement, for a batch with at least one live source.
float3 GrassCollisionDisplacementLive(float2 BladeXY, float TipWeight, float BladeHeight) {
    float playerRadius = TESR_GrassCollisionParams.x;
    float pushStr = TESR_GrassCollisionParams.y;
    float flatStr = TESR_GrassCollisionParams.z;
    int numSources = (int)TESR_GrassCollisionParams.w;

    float2 slotData = TESR_GrassCollisionXY1.zw;
    float2 isActor = step(GrassCollisionActorTag, slotData);
    float2 slotRadii = lerp(playerRadius, slotData - GrassCollisionActorTag, isActor);
    float2 slotWeights = lerp(slotData, 1.0, isActor);
    float3 invRadii = 1.0 / max(float3(playerRadius, slotRadii), 0.001);

    float3 collisionDisp = 0;
    float2 sources[3] = {
        TESR_GrassCollisionXY0.xy, TESR_GrassCollisionXY0.zw,
        TESR_GrassCollisionXY1.xy
    };
    float weights[3] = { 1.0, slotWeights.x, slotWeights.y };

    [unroll]
    for (int i = 0; i < 3; i++) {
        if (i >= numSources) break;
        float2 diff = BladeXY - sources[i];
        float dist = length(diff);
        float t = saturate(dist * invRadii[i]);
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

// BladeXY     : world XY of the grass instance
// TipWeight   : bend weight of this vertex (0 at the root, 1 at the tip)
// BladeHeight : scaled model-space height of this vertex above the root
// Returns the model-space displacement to add to the vertex position.
//
// A batch no source reaches gets c253.w = 0, but the unrolled loop pays every slot regardless, so a
// real branch on that per-batch count (uniform across the draw) skips the whole body.
float3 GrassCollisionDisplacement(float2 BladeXY, float TipWeight, float BladeHeight) {
    float3 collisionDisp = 0;
    [branch]
    if (TESR_GrassCollisionParams.w > 0.5)
        collisionDisp = GrassCollisionDisplacementLive(BladeXY, TipWeight, BladeHeight);
    return collisionDisp;
}
