#pragma once

#if defined(OBLIVION)
static NiNode** RenderWindowNode = (NiNode**)0x00B42CF4;
#elif defined(NEWVEGAS)
static NiNode** RenderWindowNode = (NiNode**)0x011FA008;
#endif

class NiD3DVertexShaderEx : public NiD3DVertexShader {
public:
	ShaderRecord*			ShaderProg;
	IDirect3DVertexShader9*	ShaderHandleBackup;
	char*					ShaderName;
	bool					isSkin;
	bool					isEyePosition;
	bool					isSun;
	bool					isTree;
	bool					isGrass;
};

class NiD3DPixelShaderEx : public NiD3DPixelShader {
public:
	ShaderRecord*			ShaderProg;
	IDirect3DPixelShader9*	ShaderHandleBackup;
	char*					ShaderName;
	bool					isSkin;
	bool					isRefraction;
};

class RenderManager: public NiDX9Renderer {
public:
	void				Initialize();
	void				ResolveDepthBuffer();
	void				ResolvePreWaterDepthBuffer();
	void				ResolveDepthInto(IDirect3DTexture9* Target);
	void				GetSceneCameraData();
	void				SetupSceneCamera();
	void				SetSceneGraph();

	// --- Near shell via depth clear ------------------------------------------------------------
	// Pass 1 renders frustum [M, F] with the depth range untouched; depth is resolved; the depth
	// buffer is cleared; pass 2 renders frustum [n, M], also untouched, compositing over pass 1.
	// That is correct because anything inside M is geometrically in front of everything beyond it.
	// See docs/superpowers/specs/2026-08-08-near-shell-depth-clear-design.md
	//
	// ALL state here is static ON PURPOSE. TheRenderManager is the engine's own NiDX9Renderer
	// reinterpret-cast to RenderManager (Framework/Game.cpp:37), and sizeof(NiDX9Renderer) is 0xB00
	// (GameNi.h:3198), so every non-static member this class declares lands past the end of the
	// engine's allocation. Statics leave the object layout untouched.
	enum ScenePass {
		PassFull = 0,	// not inside a shell pass - the real (n, F) frustum
		PassFar  = 1,	// [M, F]
		PassNear = 2,	// [n, M], drawn on a cleared depth buffer
	};

	static void			UpdateNearShell();
	static void			ApplyPass(ScenePass Pass);
	static void			StampPassProjection(D3DMATRIX* Proj);

	// The shell's depth clear value: 1 - 2^-24, the largest float32 strictly below 1.0, and exactly
	// D24 0xFFFFFE. Sky, cloud and sun shaders pin z == w and land at depth exactly 1.0 (0xFFFFFF);
	// clearing one ULP below 1.0 fails their LESSEQUAL test and keeps them out of the shell without
	// any name matching. Real shell geometry loses only its final ~0.00001 units before M.
	static constexpr float	ShellClearDepth = 0.99999994f;
	// Coverage threshold for the depth flatten (ShaderManager::FlattenShellDepth). A resolved
	// post-shell depth sample strictly below this means the shell wrote depth at that pixel; a pixel
	// the shell never touched reads back at ShellClearDepth. Backed off ~16 D24 units so the compare
	// cannot be upset by rounding in the resolve. The shell geometry that falls inside that band
	// sits within ~0.0002 units of M, i.e. exactly where flattening to M is already a no-op.
	static constexpr float	ShellCoveredMax = 0.99999894f;

	static ScenePass	CurrentPass;
	static bool			ShellActive;
	static float		ShellBoundary;	// M
	static float		RealNear;		// n, latched at frame start
	static float		RealFar;		// F, latched at frame start
	// Shell-pass draws that could have written depth: every draw submitted at PassNear EXCEPT the
	// SKY* ones, which the sub-1.0 clear rejects or the shell's far plane clips (see the increment in
	// RenderHook::TrackSetupShaderPrograms). Gates the depth flatten, and reported by NearShellDebug.
	// An upper bound on coverage, not a measure of it - a counted draw may still be depth-rejected.
	static int			ShellDraws;

	// Published to shaders as TESR_DepthProjectionTransform. A copy of projMatrix whose depth row
	// carries the encoding of whatever is currently in TESR_DepthBuffer - (M, F) while the shell is
	// active, since that buffer always holds the far pass's resolve - so a shader can linearize the
	// depth buffer no matter which pass it is rasterising in. projMatrix cannot serve both jobs: the
	// shell MUST rasterise with (n, M) or nothing inside M draws at all. Identical to projMatrix when
	// the shell is off, so the constant is correct and inert there. Written by SetupSceneCamera.
	static D3DMATRIX	DepthProjMatrix;

	D3DXMATRIX			InvViewProjMatrix;
	D3DXMATRIX			WorldViewProjMatrix;
	D3DXVECTOR4			CameraForward;
	D3DXVECTOR4			CameraPosition;
	IDirect3DSurface9*	BackBuffer;
	IDirect3DSurface9*	DepthSurface;
	IDirect3DTexture9*	DepthTexture;
	IDirect3DTexture9*	DepthTexturePreWater;
	IDirect3DTexture9*	DepthTextureINTZ;
	RECT				SaveGameScreenShotRECT;
	bool				IsSaveGameScreenShot;
	bool				FirstPersonView;
	bool				RESZ;
};