// Near-shell depth flatten - pixel stage.
//
// ShellDepthMap is a resolve of the depth buffer taken immediately AFTER the near shell rendered, so
// it is the shell's coverage mask: the shell pass starts from a clear to ShellClearDepth (one ULP
// below 1.0), which means any pixel the shell wrote depth at reads back strictly below that value,
// and any pixel it did not reads back at exactly the clear value. Sky, cloud and sun shaders pin
// z == w and are rejected by that same clear, so they never appear as coverage.
//
// Covered pixels survive and take the vertex's depth 0 (== z of M) in the depth-stencil target,
// which is TESR_DepthBuffer's own texture. Uncovered pixels are killed and keep the far-pass depth
// the resolve left there.
//
// No TESR_ names on purpose: these are bound by hand from ShaderManager::FlattenShellDepth, so the
// ShaderRecord constant-table machinery has nothing to do here.

float4 ShellFlattenData : register(c0);		// x = coverage threshold (RenderManager::ShellCoveredMax)
sampler2D ShellDepthMap : register(s0);

struct VSOUT {
	float2 UVCoord : TEXCOORD0;
};

float4 main(VSOUT IN) : COLOR0 {

	clip(ShellFlattenData.x - tex2D(ShellDepthMap, IN.UVCoord).x);
	return 0.0f;

}
