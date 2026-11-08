// Instanced variant of ShadowCubeMap.vso for opaque, non-skinned statics: many copies of one mesh
// drawn into a cube face in a single call. The per-object world transform comes from a per-instance
// stream instead of TESR_ShadowWorldTransform, laid out as in ShadowMapInstanced.vso: three float4
// columns of the camera-relative row_major world matrix,
//   iCol0 = (_11, _21, _31, _41), iCol1 = (_12, _22, _32, _42), iCol2 = (_13, _23, _33, _43).
// Pairs with ShadowCubeMap.pso, which is opaque here (TESR_ShadowCubeBakeData.y = 0) and so never
// reads texcoord_1.
row_major float4x4 TESR_ShadowViewProjTransform : register(c4);
float4 TESR_ShadowCubeMapLightPosition : register(c63);

struct VS_INPUT {
	float4 position : POSITION;
	float4 iCol0 : TEXCOORD5;
	float4 iCol1 : TEXCOORD6;
	float4 iCol2 : TEXCOORD7;
};

struct VS_OUTPUT {
	float4 position : POSITION;
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

	OUT.position = mul(world, TESR_ShadowViewProjTransform);
	OUT.texcoord_0 = float4(TESR_ShadowCubeMapLightPosition.xyz - world.xyz, 0.0f);
	OUT.texcoord_1 = 0.0f;
	return OUT;
};
