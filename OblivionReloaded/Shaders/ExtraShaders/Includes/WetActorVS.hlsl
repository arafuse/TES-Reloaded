// Wet actors: the eye position in the space a model-view-projection matrix maps from (model space,
// or skin space for SkinModelViewProj), solved as the null vector of rows 0, 1 and 3. Same method
// as TerrainEyePosition (memory terrain-land-passes); needs no engine eye constant.
float3 WetEyePosition(row_major float4x4 MVP) {
	float4 a = MVP[0];
	float4 b = MVP[1];
	float4 c = MVP[3];
	float4 eye = float4(determinant(float3x3(a.yzw, b.yzw, c.yzw)), -determinant(float3x3(a.xzw, b.xzw, c.xzw)),
	                    determinant(float3x3(a.xyw, b.xyw, c.xyw)), -determinant(float3x3(a.xyz, b.xyz, c.xyz)));
	return eye.xyz / (abs(eye.w) < 1e-6 ? 1e-6 : eye.w);
}
