// Point-light shadow cube bake PS. Stores radial distance from the light, normalized by the
// light's far plane (radius), into the R32F cube face. Distance (not depth) storage means no
// projection-dependent bias is needed here; the apply shader biases at sample time.
// TESR_ShadowCubeBakeData: x = skinned flag, y = alpha flag, z = far plane (light radius).
float4 TESR_ShadowCubeBakeData : register(c0);
sampler2D DiffuseMap : register(s0);

struct VS_OUTPUT {
	float4 texcoord_0 : TEXCOORD0;
	float4 texcoord_1 : TEXCOORD1;
};

struct PS_OUTPUT {
	float4 color_0 : COLOR0;
};


PS_OUTPUT main(VS_OUTPUT IN) {
	PS_OUTPUT OUT;

	if (TESR_ShadowCubeBakeData.y == 1.0f) { // Alpha is required
		float4 diffuse = tex2D(DiffuseMap, IN.texcoord_1.xy);
		if (diffuse.a <= 0.2f) discard;
	}

	OUT.color_0 = length(IN.texcoord_0.xyz) / TESR_ShadowCubeBakeData.z;
	return OUT;

};
