// Small-tree collision, shared by the SpeedTree vertex shader overrides and Shadows/ShadowMap.vso.
//
// Bends shrubs away from actors by the shrub layer of the grass trail map (see GrassTrail.hlsl): one
// bilinear fetch at the vertex's world XY gives the push vector, flatten peak and press time, so a
// shrub stays bent behind an actor and springs back over the recovery time. TreeCollision.cpp uploads
// c240-c243 per tree. They are deliberately not TESR_ constants: for a batch's first tree the
// per-tree hook runs before SetCT, which would overwrite a TESR_ value with a global one.
// A zero live flag (c243.x) skips the whole block.
//
// The includer declares TESR_GrassTrailWindow (c244), TESR_GrassTrailTime (c245) and the vertex
// sampler TESR_GrassTrailShrub (s0): TextureManager binds a TESR_ sampler only when it is declared
// in the top-level shader file.

float4 TreeCollisionRow0 : register(c240);		// row 0 of scale * rot, w = world x
float4 TreeCollisionRow1 : register(c241);		// row 1 of scale * rot, w = world y
float4 TreeCollisionRow2 : register(c242);		// row 2 of scale * rot
float4 TreeCollisionParams : register(c243);	// x live, y push / scale^2, z flatten / scale^2, w = 1 / bend height

#include "../Grass/Includes/GrassTrail.hlsl"

// ModelPos : model-space vertex, or leaf cluster centre before billboarding
// Returns the model-space displacement to add to ModelPos.
float3 TreeCollisionDisplacement(float3 ModelPos) {
    float3 disp = 0;
    [branch] if (TreeCollisionParams.x > 0.5) {
        float2 worldXY = float2(dot(TreeCollisionRow0.xyz, ModelPos) + TreeCollisionRow0.w, dot(TreeCollisionRow1.xyz, ModelPos) + TreeCollisionRow1.w);
        float4 cell = tex2Dlod(TESR_GrassTrailShrub, GrassTrailUV(worldXY));
        float w = GrassTrailSpring(cell.a) * GrassTrailEdgeFade(worldXY);
        float2 push = (cell.rg * 2.0 - 1.0) * (w * TreeCollisionParams.y);
        float sink = -cell.b * w * TreeCollisionParams.z;
        // Transposed scaled rotation; the strengths already carry the second 1 / scale.
        disp = push.x * TreeCollisionRow0.xyz + push.y * TreeCollisionRow1.xyz + sink * TreeCollisionRow2.xyz;
        float bend = saturate(ModelPos.z * TreeCollisionParams.w);
        disp *= bend * bend;
    }
    return disp;
}

// Collision plus the stock SpeedTree wind blend. Every branch override must get its model-space
// position ONLY through this function: the follow-up passes (STB2015/2016/1009) depth-test EQUAL
// against the first pass, so all of them have to compile to the same instruction chain.
// The includer declares WindMatrices before including this file.
float4 TreeBranchPosition(float4 Position, float4 BlendIndices) {
    float4 p = Position;
    p.xyz += TreeCollisionDisplacement(p.xyz);
    float4 wind = mul(float4x4(WindMatrices[0 + BlendIndices.y].xyzw, WindMatrices[1 + BlendIndices.y].xyzw, WindMatrices[2 + BlendIndices.y].xyzw, WindMatrices[3 + BlendIndices.y].xyzw), p);
    return (BlendIndices.x * (wind - p)) + p;
}
