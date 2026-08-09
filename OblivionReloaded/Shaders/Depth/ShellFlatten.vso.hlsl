// Near-shell depth flatten - vertex stage.
//
// Draws ShaderManager's full-screen effect quad (FVF XYZ|TEX1, TRIANGLESTRIP, 2 primitives) with the
// vertex pinned to NDC depth 0. Depth 0 is the nearest value the far pass's (M, F) projection can
// express - it decodes to exactly z = M - and writing it at shell-covered pixels is the whole point
// of this pass, so the constant is here in the transform rather than in a pixel-shader oDepth.
// The quad's own z (1.0) is deliberately discarded.

struct VSIN {
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSOUT {
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

VSOUT main(VSIN IN) {

	VSOUT OUT = (VSOUT)0.0f;

	OUT.vertPos = float4(IN.vertPos.xy, 0.0f, 1.0f);
	OUT.UVCoord = IN.UVCoord;
	return OUT;

}
