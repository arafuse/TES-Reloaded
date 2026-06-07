
// Instanced variant of ShadowMap.vso for opaque, non-skinned, non-SpeedTree statics.
// The per-object world transform comes from a per-instance vertex stream (stream 1) instead
// of the TESR_ShadowWorldTransform constant, so many copies of the same mesh can be drawn in
// a single DrawIndexedPrimitive call via D3D9 hardware instancing (SetStreamSourceFreq).
//
// The instance data is three float4 columns of the camera-relative, row_major world matrix:
//   iCol0 = (_11, _21, _31, _41)
//   iCol1 = (_12, _22, _32, _42)
//   iCol2 = (_13, _23, _33, _43)
// so that mul(float4(pos.xyz, 1), World) == ( dot(posH, iCol0), dot(posH, iCol1), dot(posH, iCol2), 1 ).

row_major float4x4 TESR_ShadowViewProjTransform : register(c4);

// Only POSITION and the instance columns are read, so the combined declaration never needs to
// guarantee TEXCOORD0 exists (some static meshes have no UVs). Instanced geometry is opaque, so
// the pixel shader's alpha path (which would use texcoord_1) is never taken.
struct VS_INPUT {
    float4 position   : POSITION;
    float4 iCol0      : TEXCOORD5;
    float4 iCol1      : TEXCOORD6;
    float4 iCol2      : TEXCOORD7;
};

struct VS_OUTPUT {
    float4 position   : POSITION;
    float4 texcoord_0 : TEXCOORD0;
    float4 texcoord_1 : TEXCOORD1;
};

VS_OUTPUT main(VS_INPUT IN) {
    VS_OUTPUT OUT;

    float4 posH = float4(IN.position.xyz, 1.0f);
    float4 world;
    world.x = dot(posH, IN.iCol0);
    world.y = dot(posH, IN.iCol1);
    world.z = dot(posH, IN.iCol2);
    world.w = 1.0f;

    float4 r0 = mul(world, TESR_ShadowViewProjTransform);
    OUT.position   = r0;
    OUT.texcoord_0 = r0;
    OUT.texcoord_1 = 0.0f;
    return OUT;
};
