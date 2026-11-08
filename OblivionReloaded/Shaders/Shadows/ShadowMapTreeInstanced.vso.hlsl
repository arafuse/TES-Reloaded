// Instanced variant of ShadowMap.vso's SpeedTree paths, for the dynamic-tree shadow composites.
// Each tree's world transform comes from the per-instance stream instead of
// TESR_ShadowWorldTransform, so every copy of one tree mesh draws in a single call. The wind palette
// and billboard axes are global and the leaf table is per mesh, so all of them stay constants.
//
// Tree collision is per tree and is NOT applied here: the CPU draws any tree with a collision source
// in reach through ShadowMap.vso instead. c240-c243 still hold the last such tree's values, which is
// why this shader must never read them.
//
// The instance columns match ShadowMapInstanced.vso: three float4 columns of the row_major world
// matrix, iCol0 = (_11, _21, _31, _41) and so on.

row_major float4x4 TESR_ShadowViewProjTransform : register(c4);
float4 TESR_ShadowData : register(c8); // x = 2 leaves, 3 branches
float4 BillboardRight : register(c63);
float4 BillboardUp : register(c64);
float4 RockParams : register(c65);
float4 RustleParams : register(c66);
float4 WindMatrices[16] : register(c67);
float4 LeafBase[48] : register(c83);

#include "ShadowMapTree.hlsl"

struct VS_INPUT {
	float4 position     : POSITION;
	float4 texcoord_0   : TEXCOORD0;
	float4 blendindexes : BLENDINDICES;
	float4 iCol0        : TEXCOORD5;
	float4 iCol1        : TEXCOORD6;
	float4 iCol2        : TEXCOORD7;
};

struct VS_OUTPUT {
	float4 position   : POSITION;
	float4 texcoord_0 : TEXCOORD0;
	float4 texcoord_1 : TEXCOORD1;
};

VS_OUTPUT main(VS_INPUT IN) {
	VS_OUTPUT OUT;

	float4 r0 = IN.position;
	if (TESR_ShadowData.x == 2.0f)
		r0 = ShadowTreeLeafPosition(r0, IN.blendindexes);
	else
		r0 = ShadowTreeBranchPosition(r0, IN.blendindexes);

	// mul(r0, World) for an affine World, written against its columns.
	float4 world;
	world.x = dot(r0, IN.iCol0);
	world.y = dot(r0, IN.iCol1);
	world.z = dot(r0, IN.iCol2);
	world.w = r0.w;

	r0 = mul(world, TESR_ShadowViewProjTransform);
	OUT.position = r0;
	OUT.texcoord_0 = r0;
	OUT.texcoord_1 = IN.texcoord_0;
	return OUT;
};
