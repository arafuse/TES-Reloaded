#pragma once
#define SamplerStatesMax 12
#define WordSourceBuffer "TESR_SourceBuffer"
#define WordRenderedBuffer "TESR_RenderedBuffer"
#define WordTAABuffer "TESR_TAABuffer"
#define WordDepthBuffer "TESR_DepthBuffer"
#define WordDepthBufferPreWater "TESR_DepthBufferPreWater"
#define WordShadowMapBufferNear "TESR_ShadowMapBufferNear"
#define WordShadowMapBufferFar "TESR_ShadowMapBufferFar"
#define WordShadowMapBufferSkin "TESR_ShadowMapBufferSkin"
#define WordShadowMapBufferNearPrev "TESR_ShadowMapBufferNearPrev"
#define WordShadowMapBufferFarPrev "TESR_ShadowMapBufferFarPrev"
#define WordOrthoMapBuffer "TESR_OrthoMapBuffer"
#define WordShadowCubeMapBuffer0 "TESR_ShadowCubeMapBuffer0"
#define WordShadowCubeMapBuffer1 "TESR_ShadowCubeMapBuffer1"
#define WordShadowCubeMapBuffer2 "TESR_ShadowCubeMapBuffer2"
#define WordShadowCubeMapBuffer3 "TESR_ShadowCubeMapBuffer3"

enum TextureRecordType {
	TextureRecordType_None,
	TextureRecordType_PlanarBuffer,
	TextureRecordType_VolumeBuffer,
	TextureRecordType_CubeBuffer,
	TextureRecordType_SourceBuffer,
	TextureRecordType_RenderedBuffer,
	TextureRecordType_TAABuffer,
	TextureRecordType_DepthBuffer,
	TextureRecordType_DepthBufferPreWater,
	TextureRecordType_ShadowMapBufferNear,
	TextureRecordType_ShadowMapBufferFar,
	TextureRecordType_ShadowMapBufferSkin,
	TextureRecordType_ShadowMapBufferNearPrev,
	TextureRecordType_ShadowMapBufferFarPrev,
	TextureRecordType_OrthoMapBuffer,
	TextureRecordType_ShadowCubeMapBuffer0,
	TextureRecordType_ShadowCubeMapBuffer1,
	TextureRecordType_ShadowCubeMapBuffer2,
	TextureRecordType_ShadowCubeMapBuffer3,
	TextureRecordType_Max,
};

// One sampler state the shader source asked for, in the form the per-bind loop wants it.
struct SamplerStateEntry {
	D3DSAMPLERSTATETYPE		Type;
	DWORD					Value;
};

class TextureRecord
{
public:
	TextureRecord();

	bool					LoadTexture(TextureRecordType Type, const char* Filename);
	void					SetSamplerState(D3DSAMPLERSTATETYPE SamplerType, DWORD Value);
	void					PackSamplerStates();	// rebuild PackedStates from SamplerStates; call once after parsing

	IDirect3DBaseTexture9*	Texture;
	DWORD					SamplerStates[SamplerStatesMax];
	// The same states, compacted. ShaderRecord::SetCT runs on every shader-handle change and used to
	// scan all 12 sparse slots per texture to find the two or three that are set; it now walks this
	// instead. Rebuilt from the sparse array rather than appended to as states are parsed, so a state
	// the parser writes twice cannot land here twice.
	SamplerStateEntry		PackedStates[SamplerStatesMax];
	UInt32					PackedStateCount;

};

typedef std::map<std::string, TextureRecord*> TextureList;

class TextureManager // Never disposed
{
public:
	TextureManager();

	TextureRecord*			LoadTexture(const char* ShaderSource, UInt32 RegisterIndex);
	
	TextureList				Textures;
};