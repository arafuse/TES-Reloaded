// Near-shell masked colour copy - pixel stage. Rewrites TESR_RenderedBuffer over the shell's coverage
// only, so shell water refracts the far pass's PRE-water frame everywhere else instead of its
// already-shaded water. See the near-shell design doc, "A fresh TESR_RenderedBuffer capture".
//
// ShellDepthMap is the shell's coverage mask, the same one ShellFlatten.pso uses: the shell starts
// from a clear to ShellClearDepth, so anything strictly below the threshold is a pixel it wrote at.
// Shares ShellFlatten.vso; that shader pins depth 0 for the flatten's benefit, inert with Z off here.
// Bound by hand from ShaderManager::CaptureShellRenderedBuffer, hence no TESR_ names.

float4 ShellCopyData : register(c0);		// x = coverage threshold (RenderManager::ShellCoveredMax)
sampler2D ShellDepthMap : register(s0);		// shell coverage mask (INTZ resolve)
sampler2D ShellColorMap : register(s1);		// the live scene target, resolved into a plain texture

struct VSOUT {
	float2 UVCoord : TEXCOORD0;
};

float4 main(VSOUT IN) : COLOR0 {

	clip(ShellCopyData.x - tex2D(ShellDepthMap, IN.UVCoord).x);
	return tex2D(ShellColorMap, IN.UVCoord);

}
