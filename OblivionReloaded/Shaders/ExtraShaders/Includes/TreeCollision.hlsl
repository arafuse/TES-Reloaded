// Small-tree collision, shared by the SpeedTree vertex shader overrides and Shadows/ShadowMap.vso.
//
// Bends shrubs away from up to three actors in the tree's model space. TreeCollision.cpp uploads
// c240-c243 per tree. They are deliberately not TESR_ constants: for a batch's first tree the
// per-tree hook runs before SetCT, which would overwrite a TESR_ value with a global one.
// Unused source slots carry weight 0, and count 0 skips the whole block.

float4 TreeCollisionParams : register(c240);	// x radius, y push, z flatten, w source count
float4 TreeCollisionXY0 : register(c241);		// source 0 xy, source 1 xy
float4 TreeCollisionXY1 : register(c242);		// source 2 xy, z = 1 / bend height
float4 TreeCollisionWeights : register(c243);	// xyz = source weights

// ModelPos : model-space vertex, or leaf cluster centre before billboarding
// Returns the model-space displacement to add to ModelPos.
float3 TreeCollisionDisplacement(float3 ModelPos) {
    float3 disp = 0;
    [branch] if (TreeCollisionParams.w > 0) {
        float invRadius = 1.0 / max(TreeCollisionParams.x, 0.001);
        float bend = saturate(ModelPos.z * TreeCollisionXY1.z);
        float2 sources[3] = { TreeCollisionXY0.xy, TreeCollisionXY0.zw, TreeCollisionXY1.xy };

        [unroll]
        for (int i = 0; i < 3; i++) {
            float2 diff = ModelPos.xy - sources[i];
            float dist = length(diff);
            float t = saturate(dist * invRadius);
            float influence = smoothstep(1.0, 0.0, t) * TreeCollisionWeights[i];
            float2 pushDir = (dist > 0.001) ? (diff / dist) : float2(1, 0);
            disp.xy += pushDir * (influence * smoothstep(0.0, 0.3, t) * TreeCollisionParams.y);
            disp.z -= influence * TreeCollisionParams.z;
        }
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
