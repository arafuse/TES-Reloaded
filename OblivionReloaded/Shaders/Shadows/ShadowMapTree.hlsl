// SpeedTree model-space motion for the sun shadow vertex shaders, shared by ShadowMap.vso and
// ShadowMapTreeInstanced.vso so a batched tree lands exactly where a single draw would put it.
// Declares nothing: the includer declares BillboardRight (c63), BillboardUp (c64), RockParams (c65),
// RustleParams (c66), WindMatrices[16] (c67) and LeafBase[48] (c83). Tree collision, which is per
// tree, stays with the includer and is applied to the position before these.

#define	ShadowTreePI			3.14159274
#define	ShadowTreeAngler(v)		(((v) * (2 * ShadowTreePI)) - ShadowTreePI)
#define	ShadowTreeSqr(v)		((v) * (v))

// Leaf cluster: expands the cluster centre r0 into its billboard corner (LeafBase, facing the light
// via BillboardRight/Up), rocks and rustles it, then blends toward the wind-matrix pose.
float4 ShadowTreeLeafPosition(float4 r0, float4 blendindexes) {
	float4 offset;
	float4 q1;
	float4 q2;
	float4 q3;
	float4 q5;
	float4 q6;
	float4 q7;
	float4 q8;
	float4 q9;
	float4 q10;
	float4 q11;
	float4 q28;
	float4 q59;
	float4 r1;
	float4 r2;
	float4 r3;
	float4 r4;
	float4 r5;
	const float4 const_4 = {(1.0 / (ShadowTreePI * 2)), 0.25, 0.5, 0};

	q1.x = ShadowTreeSqr(ShadowTreeAngler(frac((((blendindexes.z / 48)) * 0.499999553) + 0.25)));
	q6.x = ShadowTreeAngler(frac((((blendindexes.z / 48)) * 0.499999553) + 0.25));
	q7.x = ShadowTreeSqr(q6.x);
	offset.x = blendindexes.y;
	r1.zw = BillboardRight.zw;
	q10.z = 0;
	r3.z = 0;
	q8.x = (q7.x * ((q7.x * ((q7.x * -2.52398507e-007) + 2.47609005e-005)) - (1.0 / 72))) + (1.0 / 24);
	q2.x = (q1.x * ((q1.x * ((q1.x * -2.52398507e-007) + 2.47609005e-005)) - (1.0 / 72))) + (1.0 / 24);
	q9.xy = (((((q7.x * ((q7.x * q8.x) - 0.5)) + 1) * RustleParams.z) * RustleParams.x) / (ShadowTreePI * 2)) + const_4.yz;
	r1.xy = ShadowTreeAngler(frac(q9.xy)) * ShadowTreeAngler(frac(q9.xy));
	q3.xy = (((((q1.x * ((q1.x * q2.x) - 0.5)) + 1) * RockParams.z) * RockParams.x) / (ShadowTreePI * 2)) + const_4.yz;
	q10.xy = ShadowTreeAngler(frac(q3.xy)) * ShadowTreeAngler(frac(q3.xy));
	q11.xy = (((((-2.52398507e-007 * r1.xy) + 2.47609005e-005) * r1.xy) - (1.0 / 72)) * r1.xy) + (1.0 / 24);
	r3.xy = (q11.xy * r1.xy) - 0.5;
	r3.xw = (r1.yx * r3.yx) + 1;
	r3.y = -r3.w;
	r1.y = dot(r3.wxz, BillboardRight.xyz);
	r1.x = dot(r3.xyz, BillboardRight.xyz);
	q5.xy = (((((-2.52398507e-007 * q10.xy) + 2.47609005e-005) * q10.xy) - (1.0 / 72)) * q10.xy) + (1.0 / 24);
	r2.xy = (q5.xy * q10.xy) - 0.5;
	q10.xw = (q10.yx * r2.yx) + 1;
	r2.xyzw = blendindexes.w * LeafBase[blendindexes.z];
	q10.y = -q10.w;
	r4.x = dot(q10.zwx, r2.xyz);
	r5.x = dot(q10.zxy, r2.xyz);
	q10.y = dot(r3.wxz, BillboardUp.xyz);
	q10.x = dot(r3.xyz, BillboardUp.xyz);
	q10.zw = BillboardUp.zw;
	q59.xyzw = ((r4.x * q10.xyzw) + (r5.x * r1.xyzw)) + r0.xyzw;
	q28.xyzw = mul(float4x4(WindMatrices[0 + offset.x].xyzw, WindMatrices[1 + offset.x].xyzw, WindMatrices[2 + offset.x].xyzw, WindMatrices[3 + offset.x].xyzw), q59.xyzw);
	return (blendindexes.x * (q28.xyzw - q59.xyzw)) + q59.xyzw;
}

// Branch/trunk: the same bend the stock branch shaders apply (STB2005 and friends), blending the
// vertex toward its wind-matrix transform. Without it the trunk shadow stays at rest pose while the
// visible tree sways.
float4 ShadowTreeBranchPosition(float4 r0, float4 blendindexes) {
	float4 offset;
	float4 q28;
	offset.x = blendindexes.y;
	q28.xyzw = mul(float4x4(WindMatrices[0 + offset.x].xyzw, WindMatrices[1 + offset.x].xyzw, WindMatrices[2 + offset.x].xyzw, WindMatrices[3 + offset.x].xyzw), r0.xyzw);
	return (blendindexes.x * (q28.xyzw - r0.xyzw)) + r0.xyzw;
}
