// Near-shell masked colour copy - pixel stage.
//
// Rewrites TESR_RenderedBuffer over the near shell's coverage ONLY, leaving every other pixel as the
// far pass captured it before its water went down.
//
// The shell cannot use a blind full-frame capture of the live scene target, which is what it did
// originally: by the instant the shell reaches its own first near-water draw, the far pass has
// already shaded ITS water into that target, everywhere beyond the boundary M. Shell water then
// refracts that buffer through the +0.01 UV tap in WATER*.pso, and for the ~11 px either side of the
// boundary line where the tap crosses it, the sample lands on already-shaded far water: extinction
// and volume colour get applied a second time, over themselves. The result is a thin strip along the
// boundary in which red is attenuated hardest, then green, then blue - the ordering of
// TESR_WaterCoefficients - and which vanishes wherever the bottom is no longer visible, because
// re-extinguishing fully extinct water is a fixed point.
//
// The shell only ever NEEDED the live target for the pixels it drew itself (the player's arms while
// treading water, which near water covers and which the far pass clipped away). So take those and
// nothing else. Outside the mask the buffer keeps the far pass's pre-water frame, which is precisely
// what the far pass's own water refracted - so both halves of a surface crossing M now refract the
// same content.
//
// ShellDepthMap is the post-geometry / pre-water resolve of the shell's depth buffer, the same
// coverage mask ShellFlatten.pso uses and produced by the same resolve: the shell starts from a clear
// to ShellClearDepth, so anything strictly below that threshold is a pixel the shell wrote depth at.
// Sky, cloud and sun shaders pin z == w and are rejected by that clear, so they never read as
// coverage and their pixels correctly keep the far pass's content.
//
// Shares ShellFlatten.vso as its vertex stage. That shader pins the vertex to NDC depth 0 for the
// flatten's benefit; here depth testing and depth writes are both off, so the value is inert.
//
// No TESR_ names on purpose: these are bound by hand from ShaderManager::CaptureShellRenderedBuffer,
// so the ShaderRecord constant-table machinery has nothing to do here.

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
