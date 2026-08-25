#include <fstream>
#include <ctime>
#include <math.h>
#include <cmath>
#include <WeatherMode.h>
#include <algorithm>
#include <iostream>
#include <filesystem>
#define EFFECTQUADFORMAT D3DFVF_XYZ | D3DFVF_TEX1

#if defined(NEWVEGAS)
#define CurrentBlend 0.0f
#define TerrainShaders ""
#define BloodShaders ""
#elif defined(OBLIVION)
#define CurrentBlend *WaterBlend
#define TerrainShaders "SLS2001.vso SLS2001.pso SLS2064.vso SLS2068.pso SLS2042.vso SLS2048.pso SLS2043.vso SLS2049.pso"
#define ExteriorPom "PAR2022.pso"
#define ExteriorExtraShaders "SM3LL003.pso SM3002.vso"
#define InteriorShadowShaders "SLS2022.pso SLS2021.pso SLS2016.vso SLS2015.vso SLS2015.pso SLS2012.vso SLS2011.vso SLS2010.pso SLS2008.vso SLS2007.vso SLS2002.vso SLS2002.pso SLS2000.vso SLS2000.pso SLS1006.vso SLS1005.vso SLS1004.pso SLS1S006.vso SLS1S005.vso SLS1003.pso SLS2009.pso SLS2035.vso SLS2036.vso SLS2041.pso SM3002.vso SM3001.vso SM3001.pso SM3000.vso SM3LL003.pso SM3LL001.pso SM3LL000.pso PAR2022.pso"
#define ExteriorDialogShaders "SLS2003.pso SLS2018.pso SLS2039.pso SKIN2001.pso SKIN2003.pso SKIN2007.pso"
#define BloodShaders "GDECALS.vso GDECAL.pso SLS2040.vso SLS2046.pso"
#elif defined(SKYRIM)
#define sunGlare general.sunGlare
#define windSpeed general.windSpeed
#define weatherType general.weatherType
#define CurrentBlend 0.0f
#define TerrainShaders ""
#define BloodShaders ""
#endif

// --- Lightweight post-processing profiler (Develop.ProfileEffects) -------------
// Mirrors the ShadowManager ProfileShadows pattern. The effect chain is GPU
// fill-rate / bandwidth bound (full-screen passes + StretchRect blits on FP16
// targets), so a CPU wall-clock timer alone can't tell whether the frame is
// GPU- or CPU-bound. We therefore measure BOTH: a QPC wall-clock around the
// chain (CPU submission cost) and a D3D9 timestamp query (actual GPU time),
// plus structural counts (active effects, full-screen passes, blits). The GPU
// timer is double-buffered and read one frame late so it never stalls the CPU.
namespace {
	struct EffPhaseAccum { double Ms; UInt32 Calls; };

	bool     gEffProfilingEnabled = false;
	LONGLONG gEffQpcFreq = 0;
	int      gEffFrames = 0;
	const int gEffReportFrames = 600;

	// CPU wall-clock of the chain, plus per-frame structural counters (accumulated
	// over gEffReportFrames then averaged).
	double   gEffCpuMs = 0.0;
	UInt32   gEffCount = 0;   // EffectRecord::Render invocations (active effects)
	UInt32   gPassCount = 0;  // full-screen passes (DrawPrimitive quads)
	UInt32   gBlitCount = 0;  // full-screen StretchRect blits within the chain
	// Per-frame snapshots reset at chain begin (so spikes are visible via max too).
	UInt32   gEffCountFrame = 0, gPassCountFrame = 0, gBlitCountFrame = 0;
	UInt32   gEffMax = 0, gPassMax = 0, gBlitMax = 0;
	LONGLONG gEffCpuStart = 0;

	// --- GPU timing (D3D9 timestamp queries), double-buffered ---
	// Within one disjoint block we issue a SEQUENCE of timestamps ("marks"): one at
	// chain start, one just before each effect (labelled with the effect name), and
	// one at chain end. The interval between mark i and mark i+1 is the GPU time of
	// whatever ran in it, attributed to Label[i] — so a single run ranks every effect.
	static const int EFF_MAXMARKS = 40;
	struct GpuTimerSlot {
		IDirect3DQuery9* Disjoint = NULL;
		IDirect3DQuery9* Freq     = NULL;
		IDirect3DQuery9* Ts[EFF_MAXMARKS] = { NULL };
		const char*      Label[EFF_MAXMARKS] = { NULL };
		int              MarkCount = 0;
		bool             Pending  = false;
	};
	GpuTimerSlot gGpuSlot[2];
	int      gGpuActiveSlot = -1;     // slot issued this frame (-1 = none free, timing skipped)
	bool     gGpuAvailable = false;   // queries created OK and driver supports them
	bool     gGpuTried = false;       // attempted creation already
	double   gEffGpuMs = 0.0;
	UInt32   gEffGpuSamples = 0;       // frames that yielded a valid (non-disjoint) reading
	// Why a polled slot didn't produce a sample (diagnostics for the "no samples" case):
	UInt32   gGpuRejNotReady = 0;      // GetData returned S_FALSE (GPU hadn't finished)
	UInt32   gGpuRejDisjoint = 0;      // disjoint flag set / freq==0 / bad timestamps

	// Per-effect GPU time accumulated over the report window, keyed by label-pointer
	// identity (labels are string literals, so their addresses are stable).
	struct EffBucket { const char* Name; double Ms; };
	EffBucket gEffBuckets[EFF_MAXMARKS];
	int       gEffBucketCount = 0;
	void EffBucketAdd(const char* name, double ms) {
		for (int i = 0; i < gEffBucketCount; i++)
			if (gEffBuckets[i].Name == name) { gEffBuckets[i].Ms += ms; return; }
		if (gEffBucketCount < EFF_MAXMARKS) gEffBuckets[gEffBucketCount++] = { name, ms };
	}

	inline LONGLONG EffQpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
	inline void EffCountEffect() { if (gEffProfilingEnabled) gEffCountFrame++; }
	inline void EffCountPass()   { if (gEffProfilingEnabled) gPassCountFrame++; }
	inline void EffCountBlit()   { if (gEffProfilingEnabled) gBlitCountFrame++; }

	// Place a timestamp mark on the active slot, labelling the interval that follows.
	void EffMarkRaw(GpuTimerSlot& s, const char* label) {
		if (s.MarkCount >= EFF_MAXMARKS) return;
		s.Ts[s.MarkCount]->Issue(D3DISSUE_END);
		s.Label[s.MarkCount] = label;
		s.MarkCount++;
	}
	// Public hook used from RenderEffects, right before each effect renders.
	inline void EffMark(const char* label) {
		if (!gEffProfilingEnabled || !gGpuAvailable || gGpuActiveSlot < 0) return;
		EffMarkRaw(gGpuSlot[gGpuActiveSlot], label);
	}

	void EffGpuEnsureQueries(IDirect3DDevice9* Device) {
		if (gGpuTried) return;
		gGpuTried = true;
		bool ok = true;
		for (int i = 0; i < 2 && ok; i++) {
			GpuTimerSlot& s = gGpuSlot[i];
			ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.Disjoint) == D3D_OK;
			ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,     &s.Freq)     == D3D_OK;
			for (int m = 0; m < EFF_MAXMARKS && ok; m++)
				ok = ok && Device->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.Ts[m]) == D3D_OK;
		}
		gGpuAvailable = ok;
		Logger::Log(ok ? "[EffectProfile] GPU timestamp queries created OK."
		              : "[EffectProfile] GPU timestamp queries unavailable (CreateQuery failed); reporting CPU only.");
	}

	// Poll a pending slot. We flush on the first GetData so a 1-frame-old query
	// reliably completes; GetData still returns S_FALSE (non-blocking) if the GPU
	// isn't done, so this never stalls the CPU. A slot that isn't ready stays
	// Pending and is retried next frame — it is NEVER re-issued while pending
	// (doing so corrupts the query so it never signals again).
	void EffGpuTryCollect(GpuTimerSlot& s) {
		if (!s.Pending) return;
		BOOL   disjoint = FALSE;
		UINT64 freq = 0;
		if (s.Disjoint->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) != S_OK) { gGpuRejNotReady++; return; }
		if (s.Freq->GetData(&freq, sizeof(freq), 0)             != S_OK) { gGpuRejNotReady++; return; }
		// Read every mark's timestamp; if any isn't ready yet, retry next frame.
		UINT64 ts[EFF_MAXMARKS];
		for (int m = 0; m < s.MarkCount; m++)
			if (s.Ts[m]->GetData(&ts[m], sizeof(ts[m]), 0) != S_OK) { gGpuRejNotReady++; return; }
		s.Pending = false;
		if (disjoint || freq == 0 || s.MarkCount < 2) { gGpuRejDisjoint++; return; }
		gEffGpuMs += (double)(ts[s.MarkCount - 1] - ts[0]) * 1000.0 / (double)freq;
		gEffGpuSamples++;
		// Interval [i, i+1] is the GPU time of the work labelled at mark i.
		for (int i = 0; i + 1 < s.MarkCount; i++) {
			if (ts[i + 1] < ts[i]) continue;
			EffBucketAdd(s.Label[i], (double)(ts[i + 1] - ts[i]) * 1000.0 / (double)freq);
		}
	}

	void EffectProfileChainBegin(IDirect3DDevice9* Device) {
		gEffProfilingEnabled = TheSettingManager->SettingsMain.Develop.ProfileEffects != 0;
		if (!gEffProfilingEnabled) return;
		if (gEffQpcFreq == 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); gEffQpcFreq = f.QuadPart; }
		gEffCountFrame = gPassCountFrame = gBlitCountFrame = 0;
		gEffCpuStart = EffQpcNow();

		EffGpuEnsureQueries(Device);
		gGpuActiveSlot = -1;
		if (gGpuAvailable) {
			// Only start a measurement on a slot whose previous result has been
			// collected. If both are still pending (a stalled readback), skip timing
			// this frame rather than re-issue and corrupt a pending query.
			for (int i = 0; i < 2; i++) {
				if (!gGpuSlot[i].Pending) { gGpuActiveSlot = i; break; }
			}
			if (gGpuActiveSlot >= 0) {
				GpuTimerSlot& s = gGpuSlot[gGpuActiveSlot];
				s.MarkCount = 0;
				s.Disjoint->Issue(D3DISSUE_BEGIN);
				EffMarkRaw(s, "(setup)"); // mark 0: start of chain
			}
		}
	}

	void EffectProfileChainEnd() {
		if (!gEffProfilingEnabled) return;
		gEffCpuMs += (double)(EffQpcNow() - gEffCpuStart) * 1000.0 / (double)gEffQpcFreq;
		gEffCount += gEffCountFrame; gPassCount += gPassCountFrame; gBlitCount += gBlitCountFrame;
		if (gEffCountFrame  > gEffMax)  gEffMax  = gEffCountFrame;
		if (gPassCountFrame > gPassMax) gPassMax = gPassCountFrame;
		if (gBlitCountFrame > gBlitMax) gBlitMax = gBlitCountFrame;

		if (gGpuAvailable) {
			if (gGpuActiveSlot >= 0) {
				GpuTimerSlot& cur = gGpuSlot[gGpuActiveSlot];
				EffMarkRaw(cur, "(end)"); // final mark closes the last effect's interval
				cur.Freq->Issue(D3DISSUE_END);
				cur.Disjoint->Issue(D3DISSUE_END);
				cur.Pending = true;
			}
			// Collect any slot pending from a previous frame (not the one just issued).
			for (int i = 0; i < 2; i++)
				if (i != gGpuActiveSlot) EffGpuTryCollect(gGpuSlot[i]);
			gGpuActiveSlot = -1; // chain over: EffMark from outside the chain (e.g. the mid-scene shadow apply) must not mark a pending slot
		}

		if (++gEffFrames < gEffReportFrames) return;
		double inv = 1.0 / gEffFrames;
		Logger::Log("[EffectProfile] avg over %d frames:", gEffFrames);
		Logger::Log("[EffectProfile]   CPU chain   %8.4f ms/frame", gEffCpuMs * inv);
		if (gEffGpuSamples)
			Logger::Log("[EffectProfile]   GPU chain   %8.4f ms/frame  (%u samples)", gEffGpuMs / gEffGpuSamples, gEffGpuSamples);
		else
			Logger::Log("[EffectProfile]   GPU chain   (no samples: available=%d notReady=%u disjoint=%u)",
				(int)gGpuAvailable, gGpuRejNotReady, gGpuRejDisjoint);
		Logger::Log("[EffectProfile]   Effects     %6.2f /frame  (max %u)", gEffCount * inv, gEffMax);
		Logger::Log("[EffectProfile]   Passes      %6.2f /frame  (max %u)", gPassCount * inv, gPassMax);
		Logger::Log("[EffectProfile]   Blits       %6.2f /frame  (max %u)", gBlitCount * inv, gBlitMax);
		// Per-effect GPU breakdown, averaged over measured frames, sorted by cost desc.
		if (gEffGpuSamples && gEffBucketCount) {
			Logger::Log("[EffectProfile]   per-effect GPU (ms/frame, measured frames):");
			double gpuInv = 1.0 / gEffGpuSamples;
			for (int a = 0; a < gEffBucketCount; a++) {
				int best = a;
				for (int b = a + 1; b < gEffBucketCount; b++)
					if (gEffBuckets[b].Ms > gEffBuckets[best].Ms) best = b;
				EffBucket t = gEffBuckets[a]; gEffBuckets[a] = gEffBuckets[best]; gEffBuckets[best] = t;
				Logger::Log("[EffectProfile]     %-18s %8.4f", gEffBuckets[a].Name, gEffBuckets[a].Ms * gpuInv);
			}
		}
		gEffFrames = 0;
		gEffCpuMs = gEffGpuMs = 0.0; gEffGpuSamples = 0;
		gGpuRejNotReady = gGpuRejDisjoint = 0;
		gEffCount = gPassCount = gBlitCount = 0;
		gEffMax = gPassMax = gBlitMax = 0;
		for (int i = 0; i < gEffBucketCount; i++) gEffBuckets[i].Ms = 0.0;
		gEffBucketCount = 0;
	}
}

ShaderProgram::ShaderProgram() {

	FloatShaderValues = NULL;
	TextureShaderValues = NULL;
	FloatShaderValuesCount = 0;
	TextureShaderValuesCount = 0;

}

ShaderProgram::~ShaderProgram() {

	if (FloatShaderValues) free(FloatShaderValues);
	if (TextureShaderValues) free(TextureShaderValues);

}

bool ShaderProgram::SetConstantTableValue1(LPCSTR Name, UInt32 Index) {

	if (!strcmp(Name, "TESR_Tick"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Tick;
	else if (!strcmp(Name, "TESR_ParallaxData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.POM.ParallaxData;
	else if (!strcmp(Name, "TESR_GrassScale"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Grass.Scale;
	else if (!strcmp(Name, "TESR_GrassCollisionParams"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Grass.CollisionParams;
	else if (!strcmp(Name, "TESR_GrassCollisionXY0"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Grass.CollisionXY[0];
	else if (!strcmp(Name, "TESR_GrassCollisionXY1"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Grass.CollisionXY[1];
	else if (!strcmp(Name, "TESR_TerrainData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Terrain.Data;
	else if (!strcmp(Name, "TESR_SkinData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Skin.SkinData;
	else if (!strcmp(Name, "TESR_SkinColor"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Skin.SkinColor;
	else if (!strcmp(Name, "TESR_ShadowData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Shadow.Data;
	else if (!strcmp(Name, "TESR_ShadowSkinData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Shadow.ShadowSkinData;
	else if (!strcmp(Name, "TESR_ShadowCubeData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowCube.Data;
	else if (!strcmp(Name, "TESR_OrthoData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Shadow.OrthoData;
	else if (!strcmp(Name, "TESR_RainData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Precipitations.RainData;
	else if (!strcmp(Name, "TESR_SnowData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Precipitations.SnowData;
	else if (!strcmp(Name, "TESR_WorldTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheRenderManager->worldMatrix;
	else if (!strcmp(Name, "TESR_ViewTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheRenderManager->viewMatrix;
	else if (!strcmp(Name, "TESR_ProjectionTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheRenderManager->projMatrix;
	// Same matrix, but with the depth row of whatever is currently in TESR_DepthBuffer rather than of
	// the matrix we are rasterising with. Use it to linearize that buffer; use the one above for
	// rasterisation-space work. The two differ only inside the near shell's second pass.
	else if (!strcmp(Name, "TESR_DepthProjectionTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&RenderManager::DepthProjMatrix;
	else if (!strcmp(Name, "TESR_WorldViewProjectionTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheRenderManager->WorldViewProjMatrix;
	else if (!strcmp(Name, "TESR_InvViewProjectionTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheRenderManager->InvViewProjMatrix;
	else if (!strcmp(Name, "TESR_ShadowWorldTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowWorld;
	else if (!strcmp(Name, "TESR_ShadowViewProjTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowViewProj;
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight;
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransformNear"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight[0];
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransformFar"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight[1];
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransformOrtho"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight[2];
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransformSkin"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLight[3];
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransformNearPrev"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLightPrev[0];
	else if (!strcmp(Name, "TESR_ShadowCameraToLightTransformFarPrev"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCameraToLightPrev[1];
	else if (!strcmp(Name, "TESR_ShadowFadeData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowFadeData;
	else if (!strcmp(Name, "TESR_PointLightPosition"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.PointLights.LightPosition;
	else if (!strcmp(Name, "TESR_PointLightColor"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.PointLights.LightColor;
	else if (!strcmp(Name, "TESR_ShadowCubeMapLightPosition"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowCubeMapLightPosition;
	else if (!strcmp(Name, "TESR_ShadowLightPosition"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition;
	else if (!strcmp(Name, "TESR_ShadowLightPosition0"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[0];
	else if (!strcmp(Name, "TESR_ShadowLightPosition1"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[1];
	else if (!strcmp(Name, "TESR_ShadowLightPosition2"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[2];
	else if (!strcmp(Name, "TESR_ShadowLightPosition3"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightPosition[3];
	else if (!strcmp(Name, "TESR_ShadowLightLuminance"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowCastLightLuminance;
	else if (!strcmp(Name, "TESR_ShadowBiasDeferred"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowBiasDeferred;
	else if (!strcmp(Name, "TESR_ShadowBiasAdaptive"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowBiasAdaptive;
	else if (!strcmp(Name, "TESR_ReciprocalResolution"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ReciprocalResolution;
	else if (!strcmp(Name, "TESR_ReciprocalResolutionWater"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ReciprocalResolutionWater;
	else if (!strcmp(Name, "TESR_CameraForward"))
		FloatShaderValues[Index].Value = &TheRenderManager->CameraForward;
	else if (!strcmp(Name, "TESR_CameraPosition"))
		FloatShaderValues[Index].Value = &TheRenderManager->CameraPosition;
	else if (!strcmp(Name, "TESR_SunDirection"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.SunDir;
	else if (!strcmp(Name, "TESR_ShadowLightDir"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
	else if (!strcmp(Name, "TESR_SunAmount"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.SunAmount;
	else if (!strcmp(Name, "TESR_MasserDirection"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.MasserDir;
	else if (!strcmp(Name, "TESR_MasserAmount"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.MasserAmount;
	else if (!strcmp(Name, "TESR_SecundaDirection"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.SecundaDir;
	else if (!strcmp(Name, "TESR_SecundaAmount"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.SecundaAmount;
	else if (!strcmp(Name, "TESR_RaysPhaseCoeff"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.RaysPhaseCoeff;
	else if (!strcmp(Name, "TESR_GameTime"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.GameTime;
	else if (!strcmp(Name, "TESR_InteriorDimmer"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.InteriorDimmer;
	else if (!strcmp(Name, "TESR_TextureData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.TextureData;
	else if (!strcmp(Name, "TESR_WaterCoefficients"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Water.waterCoefficients;
	else if (!strcmp(Name, "TESR_WaveParams"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Water.waveParams;
	else if (!strcmp(Name, "TESR_WaterVolume"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Water.waterVolume;
	else if (!strcmp(Name, "TESR_WaterSettings"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Water.waterSettings;
	else if (!strcmp(Name, "TESR_WaterShorelineParams"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Water.shorelineParams;
	else if (!strcmp(Name, "TESR_FogColor"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.fogColor;
	else if (!strcmp(Name, "TESR_SunColor"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.sunColor;
	else if (!strcmp(Name, "TESR_FogData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.fogData;
	else if (!strcmp(Name, "TESR_AmbientOcclusionAOData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.AmbientOcclusion.AOData;
	else if (!strcmp(Name, "TESR_AmbientOcclusionData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.AmbientOcclusion.Data;
	else if (!strcmp(Name, "TESR_BloomData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Bloom.BloomData;
	else if (!strcmp(Name, "TESR_BloomValues"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Bloom.BloomValues;
	else if (!strcmp(Name, "TESR_CinemaData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Cinema.Data;
	else if (!strcmp(Name, "TESR_ColoringColorCurve"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Coloring.ColorCurve;
	else if (!strcmp(Name, "TESR_ColoringEffectGamma"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Coloring.EffectGamma;
	else if (!strcmp(Name, "TESR_ColoringData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Coloring.Data;
	else if (!strcmp(Name, "TESR_ColoringValues"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Coloring.Values;
	else if (!strcmp(Name, "TESR_DepthOfFieldBlur"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.DepthOfField.Blur;
	else if (!strcmp(Name, "TESR_DepthOfFieldData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.DepthOfField.Data;
	else if (!strcmp(Name, "TESR_GodRaysRay"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.GodRays.Ray;
	else if (!strcmp(Name, "TESR_GodRaysRayColor"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.GodRays.RayColor;
	else if (!strcmp(Name, "TESR_GodRaysData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.GodRays.Data;
	else if (!strcmp(Name, "TESR_MasserRaysRay"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.KhajiitRaysMasser.Ray;
	else if (!strcmp(Name, "TESR_MasserRaysRayColor"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.KhajiitRaysMasser.RayColor;
	else if (!strcmp(Name, "TESR_MasserRaysData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.KhajiitRaysMasser.Data;
	else if (!strcmp(Name, "TESR_SecundaRaysRay"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.KhajiitRaysSecunda.Ray;
	else if (!strcmp(Name, "TESR_SecundaRaysRayColor"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.KhajiitRaysSecunda.RayColor;
	else if (!strcmp(Name, "TESR_SecundaRaysData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.KhajiitRaysSecunda.Data;
	else if (!strcmp(Name, "TESR_MotionBlurParams"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.MotionBlur.BlurParams;
	else if (!strcmp(Name, "TESR_MotionBlurData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.MotionBlur.Data;
	else if (!strcmp(Name, "TESR_SharpeningData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Sharpening.Data;
	else if (!strcmp(Name, "TESR_SnowAccumulationParams"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.SnowAccumulation.Params;
	else if (!strcmp(Name, "TESR_VolumetricFogData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricFog.Data;
	else if (!strcmp(Name, "TESR_WaterLensData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.WaterLens.Time;
	else if (!strcmp(Name, "TESR_WetWorldCoeffs"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.WetWorld.Coeffs;
	else if (!strcmp(Name, "TESR_WetWorldData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.WetWorld.Data;
	else {
		return false;
	}
	return true;
}

bool ShaderProgram::SetConstantTableValue2(LPCSTR Name, UInt32 Index) {

	if (!strcmp(Name, "TESR_ShadowCubeBakeData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowPoint.BakeData;
	else if (!strcmp(Name, "TESR_ShadowPointData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowPoint.PointData;
	else if (!strcmp(Name, "TESR_VolumetricLightData1"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricLight.data1;
	else if (!strcmp(Name, "TESR_VolumetricLightData2"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricLight.data2;
	else if (!strcmp(Name, "TESR_VolumetricLightData3"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricLight.data3;
	else if (!strcmp(Name, "TESR_VolumetricLightData4"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricLight.data4;
	else if (!strcmp(Name, "TESR_VolumetricLightData5"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricLight.data5;
	else if (!strcmp(Name, "TESR_VolumetricLightData6"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.VolumetricLight.data6;
	else if (!strcmp(Name, "TESR_SpecularData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Specular.SpecularData;
	else if (!strcmp(Name, "TESR_TAAData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.TAA.Data;
	else if (!strcmp(Name, "TESR_PrevWorldViewProjectionTransform"))
		FloatShaderValues[Index].Value = (D3DXVECTOR4*)&TheShaderManager->PrevWorldViewProjMatrix;
	else {
		return false;
	}
	return true;
}

bool ShaderProgram::SetPerGeomConstantTableValue(LPCSTR Name, UInt32 Index) {

	if (!strcmp(Name, "TESR_GEOM_EyePosition"))
		PerGeomFloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Specular.EyePosition;
	else if (!strcmp(Name, "TESR_GEOM_Toggles"))
		PerGeomFloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Geometry.Toggles;
	else {
		return false;
	}
	return true;
}

void ShaderProgram::SetConstantTableCustom(LPCSTR Name, UInt32 Index) {
	Logger::Log("Custom constant found: %s", Name);
	D3DXVECTOR4 v; v.x = v.y = v.z = v.w = 0.0f;
	TheShaderManager->CustomConst[Name] = v;
	FloatShaderValues[Index].Value = &TheShaderManager->CustomConst[Name];
}

ShaderRecord::ShaderRecord() {
	
	Enabled		= false;
	HasCT		= false;
	HasRB		= false;
	HasDB		= false;
	Function	= NULL;
	Source		= NULL;
	Shader		= NULL;
	Table		= NULL;
	Errors		= NULL;

}

ShaderRecord::~ShaderRecord() {

	if (Shader) Shader->Release();
	if (Table) Table->Release();
	if (Errors) Errors->Release();
	if (Source) delete Source;

}

bool ShaderRecord::LoadShader(const char* Name, const char* DirPostFix) {
  
	char FileName[MAX_PATH];
	char FileNameBinary[MAX_PATH];
	
	strcpy(FileName, ShadersPath);
	if (!memcmp(Name, "WATER", 5)) {
		if (!TheSettingManager->SettingsMain.Shaders.Water) return false;
		strcat(FileName, "Water");
	}
	else if (!memcmp(Name, "GRASS", 5)) {
		if (!TheSettingManager->SettingsMain.Shaders.Grass) return false;
		strcat(FileName, "Grass");
	}
	else if (!memcmp(Name, "PART", 4)) {
		strcat(FileName, "ExtraShaders");
	}
	else if (!memcmp(Name, "PAR", 3)) {
		if (!TheSettingManager->SettingsMain.Shaders.POM) return false;
		strcat(FileName, "POM");
	}
	else if (!memcmp(Name, "SKIN", 4)) {
		if (!TheSettingManager->SettingsMain.Shaders.Skin) return false;

		if (TheSettingManager->SettingsSkin.UseVanillaShaders) {
			strcat(FileName, "SkinVanilla");
		}
		else {
			strcat(FileName, "Skin");
		}
	}
	else if (strstr(TerrainShaders, Name)) {
		if (!TheSettingManager->SettingsMain.Shaders.Terrain) return false;
		strcat(FileName, "Terrain");
	}
	else if (strstr(BloodShaders, Name)) {
		if (!TheSettingManager->SettingsMain.Shaders.Blood) return false;
		strcat(FileName, "Blood");
	}
	else if (!memcmp(Name, "NIGHTEYE", 8)) {
		if (!TheSettingManager->SettingsMain.Shaders.NightEye) return false;
		strcat(FileName, "NightEye");
	}
	else if (!memcmp(Name, "Shadow", 6)) {
		strcat(FileName, "Shadows");
	}
	else if (!memcmp(Name, "Shell", 5)) {
		strcat(FileName, "Depth");
	}
	else if (!memcmp(Name, "ISHIT", 5)) {
		if (!TheSettingManager->SettingsMain.Main.RenderEffectsBeforeHdr)
		{
			strcat(FileName, "ExtraShaders");
		}
		else {
			strcat(FileName, "PreHdrEffectPatches");
		}
	}
	else {
		strcat(FileName, "ExtraShaders");
	}

	strcat(FileName, DirPostFix);
	strcat(FileName, "\\");
	strcat(FileName, Name);
	strcpy(FileNameBinary, FileName);
	strcat(FileName, ".hlsl");
	std::ifstream FileSource(FileName, std::ios::in | std::ios::binary | std::ios::ate);
	if (FileSource.is_open()) {	
		size_t size = FileSource.tellg();
		Source = new char[size + 1];
		FileSource.seekg(0, std::ios::beg);
		FileSource.read(Source, size);
		Source[size] = 0;
		FileSource.close();
		if (strstr(Name, ".vso"))
			Type = ShaderType_Vertex;
		else if (strstr(Name, ".pso"))
			Type = ShaderType_Pixel;

		std::ifstream FileBinary(FileNameBinary, std::ios::in | std::ios::binary | std::ios::ate);
		if (FileBinary.is_open()) {
			size = FileBinary.tellg();
			D3DXCreateBuffer(size, &Shader);
			FileBinary.seekg(0, std::ios::beg);
			void* pShaderBuffer = Shader->GetBufferPointer();
			FileBinary.read((char*)pShaderBuffer, size);
			FileBinary.close();
			D3DXGetShaderConstantTable((const DWORD*)pShaderBuffer, &Table);
		}
		else {
			Logger::Log("ERROR: Shader %s not found. Try to enable the CompileShader option to recompile the shaders.", FileNameBinary);
		}

		if (Shader) {
			Function = Shader->GetBufferPointer();
			CreateCT();
			Logger::Log("Shader loaded: %s", FileNameBinary);
			return true;
		}
	}
	return false;
}

void ShaderRecord::CreateCT() {

	D3DXCONSTANTTABLE_DESC ConstantTableDesc;
	D3DXCONSTANT_DESC ConstantDesc;
	D3DXHANDLE Handle;
	UINT ConstantCount = 1;
	UInt32 FloatIndex = 0;
	UInt32 PerGeomFloatIndex = 0;
	UInt32 TextureIndex = 0;
	
	Table->GetDesc(&ConstantTableDesc);
    for (UINT c = 0; c < ConstantTableDesc.Constants; c++) {
		Handle = Table->GetConstant(NULL, c);
		Table->GetConstantDesc(Handle, &ConstantDesc, &ConstantCount);
		//if (ConstantDesc.RegisterSet == D3DXRS_FLOAT4 && !memcmp(ConstantDesc.Name, "TESR_", 5)) FloatShaderValuesCount += 1;
		if (ConstantDesc.RegisterSet == D3DXRS_FLOAT4 && !memcmp(ConstantDesc.Name, "TESR_GEOM_", 10)) { PerGeomFloatShaderValuesCount += 1; }
		else if(ConstantDesc.RegisterSet == D3DXRS_FLOAT4 && !memcmp(ConstantDesc.Name, "TESR_", 5)) { FloatShaderValuesCount += 1; }
		if (ConstantDesc.RegisterSet == D3DXRS_SAMPLER && !memcmp(ConstantDesc.Name, "TESR_", 5)) TextureShaderValuesCount += 1;
    }
	HasCT = FloatShaderValuesCount + TextureShaderValuesCount + PerGeomFloatShaderValuesCount;
    if (HasCT) {
		FloatShaderValues = (ShaderValue*)malloc(FloatShaderValuesCount * sizeof(ShaderValue));
		PerGeomFloatShaderValues = (ShaderValue*)malloc(PerGeomFloatShaderValuesCount * sizeof(ShaderValue));
		TextureShaderValues = (ShaderValue*)malloc(TextureShaderValuesCount * sizeof(ShaderValue));
		for (UINT c = 0; c < ConstantTableDesc.Constants; c++) {
			Handle = Table->GetConstant(NULL, c);
			Table->GetConstantDesc(Handle, &ConstantDesc, &ConstantCount);
			if (!memcmp(ConstantDesc.Name, "TESR_", 5)) {
				Logger::Log("%s", ConstantDesc.Name);
				switch (ConstantDesc.RegisterSet) {
					case D3DXRS_FLOAT4:
						if (SetPerGeomConstantTableValue(ConstantDesc.Name, PerGeomFloatIndex)) {
							PerGeomFloatShaderValues[PerGeomFloatIndex].RegisterIndex = ConstantDesc.RegisterIndex;
							PerGeomFloatShaderValues[PerGeomFloatIndex].RegisterCount = ConstantDesc.RegisterCount;
							PerGeomFloatIndex++;
						}
						else {
							if (!(SetConstantTableValue1(ConstantDesc.Name, FloatIndex) || SetConstantTableValue2(ConstantDesc.Name, FloatIndex))) {
								SetConstantTableCustom(ConstantDesc.Name, FloatIndex);
							}
							FloatShaderValues[FloatIndex].RegisterIndex = ConstantDesc.RegisterIndex;
							FloatShaderValues[FloatIndex].RegisterCount = ConstantDesc.RegisterCount;
							FloatIndex++;
						}
						break;
					case D3DXRS_SAMPLER:
						if (!strcmp(ConstantDesc.Name, WordRenderedBuffer)) HasRB = true;
						if (!strcmp(ConstantDesc.Name, WordDepthBuffer)) HasDB = true;
						TextureShaderValues[TextureIndex].Texture = TheTextureManager->LoadTexture(Source, ConstantDesc.RegisterIndex);
						TextureShaderValues[TextureIndex].RegisterIndex = ConstantDesc.RegisterIndex;
						TextureShaderValues[TextureIndex].RegisterCount = 1;
						TextureIndex++;
						break;
				}
			}
		}
	}

}

void ShaderRecord::SetCT() {
	
	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_HookSetCT);
	FrameProfiler::Count(FrameProfiler::Cnt_SetCT);

	ShaderValue* Value;

	if (HasCT) {
		if (HasRB && !TheShaderManager->RenderedBufferFilled) {
			FrameProfiler::Scope ResolveScope(FrameProfiler::Buck_HookResolve);
			FrameProfiler::Count(FrameProfiler::Cnt_RenderedBlits);
			TheRenderManager->device->StretchRect(TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface, NULL, TheShaderManager->RenderedSurface, NULL, D3DTEXF_NONE);
			TheShaderManager->RenderedBufferFilled = true;
		}
		// DepthBufferFilled alone is not enough while the near shell is active, because it is a
		// per-SCENE latch being asked to hold for the rest of the FRAME: BeginScene clears it, and the
		// game calls BeginScene again for the off-screen renders that follow the main pass - the water
		// reflection map among them, where numbered WATER* shaders bind and every one of them declares
		// TESR_DepthBuffer, so HasDB is true. Same trap, same shape of guard as the mid-scene pre-water
		// trigger in RenderHook: also require the main scene render to be on the stack.
		//
		// Why it only matters with the shell: before it, a second resolve merely duplicated a
		// depth-stencil that still held the whole scene. With the shell, the main depth-stencil is
		// resolved and then CLEARED at the end of the far pass and afterwards holds only the shell (in
		// first person, only the arms), and the post-shell flatten has already rewritten
		// TESR_DepthBuffer. A stray resolve after that overwrites the flattened buffer with a near-blank
		// one before the image-space effects read it. It is destructive on the NvAPI path in
		// particular, where ResolveDepthInto copies the CACHED main depth-stencil rather than whatever
		// the off-screen render currently has bound - the RESZ path's size mismatch would hide it.
		//
		// Gated on ShellActive so the shell-off path keeps vanilla's behaviour exactly.
		if (HasDB && !TheShaderManager->DepthBufferFilled &&
			(!RenderManager::ShellActive || TheShaderManager->InMainScenePass)) {
			FrameProfiler::Scope ResolveScope(FrameProfiler::Buck_HookResolve);
			FrameProfiler::Count(FrameProfiler::Cnt_DepthResolves);
			TheRenderManager->ResolveDepthBuffer();
			TheShaderManager->DepthBufferFilled = true;
		}
		IDirect3DDevice9* Device = TheRenderManager->device;
		for (UInt32 c = 0; c < TextureShaderValuesCount; c++) {
			Value = &TextureShaderValues[c];
			TextureRecord* Tex = Value->Texture;
			if (Tex->Texture) Device->SetTexture(Value->RegisterIndex, Tex->Texture);
			// Compacted at load time by TextureRecord::PackSamplerStates rather than rescanning all 12
			// sparse slots on every bind. Same states, same order, same skip of zero-valued entries -
			// see PackSamplerStates for why that skip is preserved rather than fixed.
			for (UInt32 s = 0; s < Tex->PackedStateCount; s++) {
				TheRenderManager->SetSamplerState(Value->RegisterIndex, Tex->PackedStates[s].Type, Tex->PackedStates[s].Value);
			}
		}
		// Type is fixed for the life of the record, so the test is hoisted out rather than re-run per
		// constant. Coalescing adjacent constants into single multi-register uploads was measured and
		// rejected: a merge needs consecutive REGISTERS and contiguous storage in ShaderConst at the
		// same time, and those two orderings are set independently - fxc's declaration order on one
		// side, the header's grouping on the other. Across every raw shader here they never coincide.
		if (Type == ShaderType_Vertex) {
			for (UInt32 c = 0; c < FloatShaderValuesCount; c++) {
				Value = &FloatShaderValues[c];
				Device->SetVertexShaderConstantF(Value->RegisterIndex, (const float *)Value->Value, Value->RegisterCount);
			}
		}
		else {
			for (UInt32 c = 0; c < FloatShaderValuesCount; c++) {
				Value = &FloatShaderValues[c];
				Device->SetPixelShaderConstantF(Value->RegisterIndex, (const float *)Value->Value, Value->RegisterCount);
			}
		}
	}

}

void ShaderRecord::SetPerGeomCT() {

	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_HookSetCT);
	FrameProfiler::Count(FrameProfiler::Cnt_SetPerGeomCT);

	ShaderValue* Value;

	if (HasCT) {
		for (UInt32 c = 0; c < PerGeomFloatShaderValuesCount; c++) {
			Value = &PerGeomFloatShaderValues[c];
			if (Type == ShaderType_Vertex)
				TheRenderManager->device->SetVertexShaderConstantF(Value->RegisterIndex, (const float*)Value->Value, Value->RegisterCount);
			else
				TheRenderManager->device->SetPixelShaderConstantF(Value->RegisterIndex, (const float*)Value->Value, Value->RegisterCount);
		}
	}

}

EffectRecord::EffectRecord() {

	Enabled = false;
	HasSB = false;
	HasRB = false;
	SBRegister = 0;
	RBRegister = 0;
	Source = NULL;
	Effect = NULL;
	Errors = NULL;

}

EffectRecord::~EffectRecord() {

	if (Effect) Effect->Release();
	if (Errors) Errors->Release();
	if (Source) delete Source;

}

bool EffectRecord::LoadEffect(const char* Name) {

	char FileName[MAX_PATH];
	strcpy(FileName, Name);
	strcat(FileName, ".hlsl");
	std::ifstream FileSource(FileName, std::ios::in | std::ios::binary | std::ios::ate);
	if (FileSource.is_open()) {
		size_t size = FileSource.tellg();
		Source = new char[size + 1];
		FileSource.seekg(0, std::ios::beg);
		FileSource.read(Source, size);
		Source[size] = 0;
		FileSource.close();

		D3DXCreateEffectFromFileA(TheRenderManager->device, Name, NULL, NULL, NULL, NULL, &Effect, &Errors);
		if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
		if (Effect) {
			CreateCT();
			Logger::Log("Effect loaded: %s", Name);
			return true;
		}
	}
	return false;
}

void ShaderManager::CompileShader(char* FileName, char* FileNameBinary, char* Source, ShaderType Type, ID3DXBuffer* Errors, ID3DXBuffer* Shader, ID3DXConstantTable* Table) {

	bool useFlowControl = false;
	useFlowControl = strstr(Source, "Includes/ShadowCube") != nullptr;
	if (useFlowControl) {
		Logger::Log("%s will be optimized for flow control", FileName);
	}
	D3DXCompileShaderFromFileA(FileName, NULL, NULL, "main", (Type == ShaderType_Vertex ? "vs_3_0" : "ps_3_0"), (useFlowControl ? D3DXSHADER_PREFER_FLOW_CONTROL : NULL), &Shader, &Errors, &Table);
	if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
	if (Shader) {
		std::ofstream FileBinary(FileNameBinary, std::ios::out | std::ios::binary);
		FileBinary.write((char*)Shader->GetBufferPointer(), Shader->GetBufferSize());
		FileBinary.flush();
		FileBinary.close();
		Logger::Log("Shader compiled: %s", FileNameBinary);
	}
}

void ShaderManager::CompileEffect(char* FileName, char* FileNameBinary, char* Source, ID3DXBuffer* Errors) {

	bool useFlowControl = false;
	std::string s = Source;
	useFlowControl = strstr(Source, "/Gfp") != nullptr;
	if (useFlowControl) {
		Logger::Log("%s will be optimized for flow control", FileName);
	}
	ID3DXEffectCompiler* Compiler = NULL;
	ID3DXBuffer* EffectBuffer = NULL;
	D3DXCreateEffectCompilerFromFileA(FileName, NULL, NULL, NULL, &Compiler, &Errors);
	if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
	if (Compiler) {
		Compiler->CompileEffect((useFlowControl ? D3DXSHADER_PREFER_FLOW_CONTROL : NULL), &EffectBuffer, &Errors);
		if (Errors) Logger::Log((char*)Errors->GetBufferPointer());
	}
	if (EffectBuffer) {
		std::ofstream FileBinary(FileNameBinary, std::ios::out | std::ios::binary);
		FileBinary.write((char*)EffectBuffer->GetBufferPointer(), EffectBuffer->GetBufferSize());
		FileBinary.flush();
		FileBinary.close();
		Logger::Log("Effect compiled: %s", FileNameBinary);
	}
	if (EffectBuffer) EffectBuffer->Release();
	if (Compiler) Compiler->Release();
}

void ShaderManager::CompileShaders(const std::filesystem::path& path){
	for (const auto& entry : std::filesystem::directory_iterator(path)) {
		if (entry.is_directory()) {
			Logger::Log("Compiling Directory: %s", entry.path().string());
			CompileShaders(entry.path()); 
		}
		else if (entry.is_regular_file()) {

			std::string FileNameStr = entry.path().string();
			
			ShaderType Type;
			bool validFile = false;
			bool isEffect = false;
			if (FileNameStr.ends_with(".vso.hlsl")) {
				Type = ShaderType_Vertex;
				validFile = true;
			}
			else if (FileNameStr.ends_with(".pso.hlsl")) {
				Type = ShaderType_Pixel;
				validFile = true;
			}
			else if (FileNameStr.ends_with(".fx.hlsl")) {
				validFile = true;
				isEffect = true;
			}

			if (validFile) {
				char* FileName = FileNameStr.data();
				Logger::Log("Compiling File: %s", entry.path().string());
				std::ifstream FileSource(FileName, std::ios::in | std::ios::binary | std::ios::ate);
				if (FileSource.is_open()) {
					size_t size = FileSource.tellg();
					char* Source = new char[size+1];
					FileSource.seekg(0, std::ios::beg);
					FileSource.read(Source, size);
					Source[size] = 0;
					FileSource.close();
					std::string FileNameBinaryStr = FileNameStr.substr(0, FileNameStr.length() - 5);
					char* FileNameBinary = FileNameBinaryStr.data();
					ID3DXBuffer* Errors = NULL;
					ID3DXBuffer* Shader = NULL;
					ID3DXConstantTable* Table = NULL;
					if (!isEffect) {
						CompileShader(FileName, FileNameBinary, Source, Type, Errors, Shader, Table);
					}
					else {
						CompileEffect(FileName, FileNameBinary, Source, Errors);
					}
				}			
			}			
		}
	}
}

void EffectRecord::CreateCT() {

	D3DXEFFECT_DESC ConstantTableDesc;
	D3DXPARAMETER_DESC ConstantDesc;
	D3DXHANDLE Handle;
	UINT ConstantCount = 1;
	UInt32 FloatIndex = 0;
	UInt32 PerGeomFloatIndex = 0;
	UInt32 TextureIndex = 0;

	Effect->GetDesc(&ConstantTableDesc);
	for (UINT c = 0; c < ConstantTableDesc.Parameters; c++) {
		Handle = Effect->GetParameter(NULL, c);
		Effect->GetParameterDesc(Handle, &ConstantDesc);
		if ((ConstantDesc.Class == D3DXPC_VECTOR || ConstantDesc.Class == D3DXPC_MATRIX_ROWS) && !memcmp(ConstantDesc.Name, "TESR_", 5)) FloatShaderValuesCount += 1;
		if (ConstantDesc.Class == D3DXPC_OBJECT && ConstantDesc.Type >= D3DXPT_SAMPLER && ConstantDesc.Type <= D3DXPT_SAMPLERCUBE && !memcmp(ConstantDesc.Name, "TESR_", 5)) TextureShaderValuesCount += 1;
	}
	FloatShaderValues = (ShaderValue*)malloc(FloatShaderValuesCount * sizeof(ShaderValue));
	TextureShaderValues = (ShaderValue*)malloc(TextureShaderValuesCount * sizeof(ShaderValue));
	for (UINT c = 0; c < ConstantTableDesc.Parameters; c++) {
		Handle = Effect->GetParameter(NULL, c);
		Effect->GetParameterDesc(Handle, &ConstantDesc);
		if (!memcmp(ConstantDesc.Name, "TESR_", 5)) {
			switch (ConstantDesc.Class) {
				case D3DXPC_VECTOR:
				case D3DXPC_MATRIX_ROWS:
					if (!(SetConstantTableValue1(ConstantDesc.Name, FloatIndex) || SetConstantTableValue2(ConstantDesc.Name, FloatIndex))) {
							SetConstantTableCustom(ConstantDesc.Name, FloatIndex);
					}
					FloatShaderValues[FloatIndex].RegisterIndex = (UInt32)Handle;
					FloatShaderValues[FloatIndex].RegisterCount = ConstantDesc.Rows;
					FloatIndex++;
					break;
				case D3DXPC_OBJECT:
					if (ConstantDesc.Class == D3DXPC_OBJECT && ConstantDesc.Type >= D3DXPT_SAMPLER && ConstantDesc.Type <= D3DXPT_SAMPLERCUBE) {
						if (!strcmp(ConstantDesc.Name, WordSourceBuffer)) { HasSB = true; SBRegister = TextureIndex; }
						if (!strcmp(ConstantDesc.Name, WordRenderedBuffer)) { HasRB = true; RBRegister = TextureIndex; }
						TextureShaderValues[TextureIndex].Texture = TheTextureManager->LoadTexture(Source, TextureIndex);
						TextureShaderValues[TextureIndex].RegisterIndex = TextureIndex;
						TextureShaderValues[TextureIndex].RegisterCount = 1;
						TextureIndex++;
					}
					break;
			}
		}
	}

}

void EffectRecord::SetCT() {

	ShaderValue* Value;

	for (UInt32 c = 0; c < TextureShaderValuesCount; c++) {
		Value = &TextureShaderValues[c];
		if (Value->Texture->Texture) TheRenderManager->device->SetTexture(Value->RegisterIndex, Value->Texture->Texture);
	}
	for (UInt32 c = 0; c < FloatShaderValuesCount; c++) {
		Value = &FloatShaderValues[c];
		if (Value->RegisterCount == 1)
			Effect->SetVector((D3DXHANDLE)Value->RegisterIndex, Value->Value);
		else
			Effect->SetMatrix((D3DXHANDLE)Value->RegisterIndex, (D3DXMATRIX*)Value->Value);
	}

}

void EffectRecord::Render(IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, IDirect3DSurface9* RenderedSurface, bool ClearRenderTarget) {

	UINT Passes;

	EffCountEffect();
	EffMark(ProfileName.c_str()); // open this effect's GPU-time interval (per-effect breakdown)
	Effect->Begin(&Passes, NULL);

	// Original per-pass-copy path: single-pass effects (nothing to ping-pong) and clear-based
	// effects (SMAA).
	//
	// CORRECTED 2026-08-24. This comment used to say SMAA was excluded because "SMAA's passes read
	// dedicated intermediate textures rather than the previous pass via TESR_RenderedBuffer". That is
	// false, and dangerously so: SMAA chains through TESR_RenderedBuffer exactly like everything else
	// - pass 0 (edge detection) reads it, pass 1 (blending weights) reads it, pass 2 (neighborhood
	// blending) reads it alongside TESR_SourceBuffer (SMAA.fx.hlsl:285, 297, 309). The per-pass
	// StretchRect below is what delivers each pass's output to the next one and is LOAD BEARING for
	// SMAA; anyone who trusts the old comment and elides it will silently break antialiasing.
	//
	// The two real reasons SMAA cannot take the ping-pong path:
	//   1. It needs the render target CLEARED before every pass. The ping-pong path never clears.
	//   2. The ping-pong path hardcodes Device->SetTexture(0, srcTex), i.e. it assumes
	//      TESR_RenderedBuffer sits at sampler 0. SMAA puts TESR_SourceBuffer at s0 and
	//      TESR_RenderedBuffer at s1 (SMAA.fx.hlsl:215-216), so that rebind would feed the wrong
	//      texture. Any future attempt to widen ping-ponging must read the register out of the
	//      effect's own parameter table instead of assuming s0.
	if (Passes <= 1 || ClearRenderTarget) {
		for (UINT p = 0; p < Passes; p++) {
			if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0L);
			Effect->BeginPass(p);
			Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Effect->EndPass();
			Device->StretchRect(RenderTarget, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
			EffCountPass(); EffCountBlit();
		}
		Effect->End();
		return;
	}

	// Multi-pass ping-pong. Every pass reads the previous pass's output via
	// TESR_RenderedBuffer (s0) -- the original code guaranteed this by copying the
	// render target into RenderedSurface after each pass. Instead we alternate the
	// render target between RenderedSurface (A, which also holds the s0 input) and the
	// scratch PingSurface (B), rebinding s0 to whichever texture holds the latest output.
	// This removes the per-pass full-screen FP16 StretchRect; only one reconciling copy
	// remains. (Effects needing the pre-effect image use TESR_SourceBuffer, untouched.)
	IDirect3DTexture9* texA = TheShaderManager->RenderedTexture; // backs RenderedSurface; holds the input
	IDirect3DSurface9* surfA = RenderedSurface;
	IDirect3DTexture9* texB = TheShaderManager->PingTexture;
	IDirect3DSurface9* surfB = TheShaderManager->PingSurface;

	for (UINT p = 0; p < Passes; p++) {
		// Pass p writes B when p is even, A when p is odd; it reads the buffer written by
		// pass p-1 (texA for pass 0, the input).
		IDirect3DSurface9* dstSurf = (p & 1) ? surfA : surfB;
		IDirect3DTexture9* srcTex  = (p == 0) ? texA : ((p & 1) ? texB : texA);
		Device->SetTexture(0, srcTex);
		Device->SetRenderTarget(0, dstSurf);
		Effect->BeginPass(p);
		Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		Effect->EndPass();
		EffCountPass();
	}
	Effect->End();

	// Final image is in B when the last pass index (Passes - 1) is even, else in A.
	if (!((Passes - 1) & 1)) {
		Device->StretchRect(surfB, NULL, surfA, NULL, D3DTEXF_NONE); // make RenderedSurface (s0 input for next effect) final
		EffCountBlit();
	}
	Device->StretchRect(surfA, NULL, RenderTarget, NULL, D3DTEXF_NONE); // restore the original contract: RenderTarget holds final
	EffCountBlit();

	// Restore the inter-effect state the chain expects (device RT = RenderTarget, s0 = RenderedTexture).
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetTexture(0, texA);

}

// Chain-rotation render. Reads TheShaderManager->ChainTex[ChainCur] and writes only the two scratch
// buffers, so the current buffer survives the whole effect and can serve TESR_SourceBuffer without a
// copy. On return ChainCur names whichever scratch holds the result. No StretchRect is issued.
//
// The pass walk mirrors Render()'s in-effect ping-pong, with one difference that matters: the source
// texture is rebound at RBRegister rather than sampler 0. Render() may assume s0 because it never
// runs for the one effect that breaks that assumption (SMAA, s1); this path runs for everything.
void EffectRecord::RenderChained(IDirect3DDevice9* Device, bool ClearRenderTarget) {

	UINT Passes;
	ShaderManager* SM = TheShaderManager;

	EffCountEffect();
	EffMark(ProfileName.c_str());
	Effect->Begin(&Passes, NULL);
	if (!Passes) { Effect->End(); return; }

	// The two buffers that are not currently live. Writing only these is the invariant the whole
	// design rests on - it is what keeps the source image intact across a multi-pass effect.
	const int Cur = SM->ChainCur;
	const int A = (Cur + 1) % 3;
	const int B = (Cur + 2) % 3;

	for (UINT p = 0; p < Passes; p++) {
		int Dst = (p & 1) ? B : A;
		IDirect3DTexture9* Src = (p == 0) ? SM->ChainTex[Cur] : SM->ChainTex[(p & 1) ? A : B];
		// Both bindings are refreshed every pass rather than set once outside the loop: Begin() was
		// called with flags 0, so D3DX saves and restores device state around the technique and may
		// put a sampler back between passes. The in-effect ping-pong in Render() rebinds per pass for
		// the same reason.
		//
		// The source bind is required, not an optimization: SetCT pointed this sampler at
		// SourceTexture, which the rotation never fills.
		if (HasSB) Device->SetTexture(SBRegister, SM->ChainTex[Cur]);
		if (HasRB) Device->SetTexture(RBRegister, Src);
		Device->SetRenderTarget(0, SM->ChainSurf[Dst]);
		// Per-pass clear (SMAA): the legacy path cleared the shared render target, this clears the
		// scratch the pass is about to write. Same guarantee, no shared surface involved.
		if (ClearRenderTarget) Device->Clear(0L, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0L);
		Effect->BeginPass(p);
		Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		Effect->EndPass();
		EffCountPass();
	}
	Effect->End();

	SM->ChainCur = ((Passes - 1) & 1) ? B : A;

}

// Adopt RenderedTexture as the live buffer. The callers of RenderEffects already blit the scene into
// RenderedSurface before the chain runs, so the rotation starts with no copy of its own.
void ShaderManager::ChainBegin() {

	ChainTex[0]  = RenderedTexture;  ChainSurf[0] = RenderedSurface;
	ChainTex[1]  = PingTexture;      ChainSurf[1] = PingSurface;
	ChainTex[2]  = EffectTexture;    ChainSurf[2] = EffectSurface;
	ChainCur = 0;
	ChainActive = true;

}

// The chain's single remaining copy: put the result where the caller expects it, and restore the
// device bindings the legacy path leaves behind (render target, sampler 0 = RenderedTexture) so
// nothing downstream can tell which path ran.
void ShaderManager::ChainEnd(IDirect3DSurface9* RenderTarget) {

	IDirect3DDevice9* Device = TheRenderManager->device;
	// In the pre-HDR chain the render target IS EffectSurface, so when the rotation happens to end
	// there the copy would be a surface onto itself - skip it rather than ask the driver.
	if (ChainSurf[ChainCur] != RenderTarget) {
		Device->StretchRect(ChainSurf[ChainCur], NULL, RenderTarget, NULL, D3DTEXF_NONE);
		EffCountBlit();
	}
	// Effects downstream of the chain (and the next frame's seed) still expect RenderedSurface to
	// hold the finished image.
	if (ChainCur != 0) {
		Device->StretchRect(ChainSurf[ChainCur], NULL, RenderedSurface, NULL, D3DTEXF_NONE);
		EffCountBlit();
	}
	Device->SetRenderTarget(0, RenderTarget);
	Device->SetTexture(0, RenderedTexture);
	ChainActive = false;

}

// One entry point for every effect in the chain, so the legacy and rotation paths cannot drift.
// BlitSource carries the call site's existing decision about TESR_SourceBuffer and is honoured
// exactly in legacy mode; the rotation needs no blit because it binds the live buffer instead.
void ShaderManager::RunEffect(EffectRecord* Effect, IDirect3DDevice9* Device, IDirect3DSurface9* RenderTarget, bool BlitSource, bool ClearRenderTarget) {

	if (ChainActive) {
		Effect->SetCT();
		Effect->RenderChained(Device, ClearRenderTarget);
		return;
	}
	if (BlitSource) ProfileBlitToSource(RenderTarget);
	Effect->SetCT();
	Effect->Render(Device, RenderTarget, RenderedSurface, ClearRenderTarget);

}

ShaderManager::ShaderManager() {

	Logger::Log("Starting the shaders manager...");
	TheShaderManager = this;

	float UAdj, VAdj;
	void* VertexPointer;

	SourceTexture = NULL;
	SourceSurface = NULL;
	RenderedTexture = NULL;
	RenderedSurface = NULL;
	RenderTextureSMAA = NULL;
	RenderSurfaceSMAA = NULL;
	EffectTexture = NULL;
	EffectSurface = NULL;
	PingTexture = NULL;
	PingSurface = NULL;
	RenderedBufferFilled = false;
	DepthBufferFilled = false;
	PreWaterDepthBufferFilled = false;
	InMainScenePass = false;
	EffectVertex = NULL;
	ShellMaskTexture = NULL;
	ShellFlattenDepthSurface = NULL;
	ShellFlattenPreWaterSurface = NULL;
	ShellFlattenVertex = NULL;
	ShellFlattenPixel = NULL;
	ShellCopyPixel = NULL;
	ShellFlattenVertexShader = NULL;
	ShellFlattenPixelShader = NULL;
	ShellCopyPixelShader = NULL;
	ShellFlattenFailed = false;
	CachedStateBlock = NULL;
	CachedStateBlockDevice = NULL;
	ChainActive = false;
	ChainCur = 0;
	for (int i = 0; i < 3; i++) { ChainTex[i] = NULL; ChainSurf[i] = NULL; }
	UnderwaterEffect = NULL;
	WaterLensEffect = NULL;
	GodRaysEffect = NULL;
	MasserRaysEffect = NULL;
	SecundaRaysEffect = NULL;
	DepthOfFieldEffect = NULL;
	AmbientOcclusionEffect = NULL;
	ColoringEffect = NULL;
	CinemaEffect = NULL;
	BloomEffect = NULL;
	SnowAccumulationEffect = NULL;
	SMAAEffect = NULL;
	TAAEffect = NULL;
	MotionBlurEffect = NULL;
	WetWorldEffect = NULL;
	SharpeningEffect = NULL;
	VolumetricFogEffect = NULL;
	VolumetricLightEffect = NULL;
	RainEffect = NULL;
	SnowEffect = NULL;
	ShadowsExteriorsEffect = NULL;
	ShadowsPointEffect = NULL;
	WaterHeightMapVertexShader = NULL;
	WaterHeightMapPixelShader = NULL;
	memset(WaterVertexShaders, NULL, sizeof(WaterVertexShaders));
	memset(WaterPixelShaders, NULL, sizeof(WaterPixelShaders));
	InitializeConstants();
	ShaderConst.ReciprocalResolution.x = 1.0f / TheRenderManager->width;
	ShaderConst.ReciprocalResolution.y = 1.0f / TheRenderManager->height;
	ShaderConst.ReciprocalResolution.z = TheRenderManager->width / TheRenderManager->height;
	ShaderConst.ReciprocalResolution.w = 0.0f; // Reserved to store the FoV
	ShaderConst.ReciprocalResolutionWater.x = 1.0f / WaterHeightMapSize;
	ShaderConst.ReciprocalResolutionWater.y = 1.0f / WaterHeightMapSize;
	ShaderConst.ReciprocalResolutionWater.z = 1.0f / WaterDisplacementMapSize;
	ShaderConst.ReciprocalResolutionWater.w = 1.0f / WaterDisplacementMapSize;
	UAdj = (1.0f / (float)TheRenderManager->width) * 0.5f;
	VAdj = (1.0f / (float)TheRenderManager->height) * 0.5f;
	EffectQuad ShaderVertices[] = {
		{ -1.0f,  1.0f, 1.0f, 0.0f + UAdj, 0.0f + VAdj },
		{ -1.0f, -1.0f, 1.0f, 0.0f + UAdj, 1.0f + VAdj },
		{  1.0f,  1.0f, 1.0f, 1.0f + UAdj, 0.0f + VAdj },
		{  1.0f, -1.0f, 1.0f, 1.0f + UAdj, 1.0f + VAdj }
	};
	TheRenderManager->device->CreateVertexBuffer(4 * sizeof(EffectQuad), D3DUSAGE_WRITEONLY, EFFECTQUADFORMAT, D3DPOOL_DEFAULT, &EffectVertex, 0);
	EffectVertex->Lock(0, 0, &VertexPointer, 0);
	CopyMemory(VertexPointer, ShaderVertices, sizeof(ShaderVertices));
	EffectVertex->Unlock();
	TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &SourceTexture, NULL);
	TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &RenderedTexture, NULL);
	TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &RenderTextureSMAA, NULL);
	TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &EffectTexture, NULL);
	TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &TAATexture, NULL);
	TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &PingTexture, NULL);
	SourceTexture->GetSurfaceLevel(0, &SourceSurface);
	RenderedTexture->GetSurfaceLevel(0, &RenderedSurface);
	RenderTextureSMAA->GetSurfaceLevel(0, &RenderSurfaceSMAA);
	EffectTexture->GetSurfaceLevel(0, &EffectSurface);
	TAATexture->GetSurfaceLevel(0, &TAASurface);
	PingTexture->GetSurfaceLevel(0, &PingSurface);
	UseIntervalUpdate = TheSettingManager->SettingsShadows.Exteriors.UseIntervalUpdate;
	if (TheSettingManager->SettingsMain.Develop.CompileShaders) {
		CompileShaders(ShadersPath);
	}
}

void ShaderManager::CreateEffects() {
	
	SettingsMainStruct::EffectsStruct* Effects = &TheSettingManager->SettingsMain.Effects;

	if (Effects->AmbientOcclusion) CreateEffect(EffectRecordType_AmbientOcclusion);
	if (Effects->Bloom) CreateEffect(EffectRecordType_Bloom);
	if (Effects->Cinema) CreateEffect(EffectRecordType_Cinema);
	if (Effects->Coloring) CreateEffect(EffectRecordType_Coloring);
	if (Effects->DepthOfField) CreateEffect(EffectRecordType_DepthOfField);
	if (Effects->GodRays) CreateEffect(EffectRecordType_GodRays);
	if (Effects->KhajiitRays) CreateEffect(EffectRecordType_KhajiitRays);
	if (Effects->MotionBlur) CreateEffect(EffectRecordType_MotionBlur);
	if (Effects->Sharpening) CreateEffect(EffectRecordType_Sharpening);
	if (Effects->SMAA) CreateEffect(EffectRecordType_SMAA);
	if (Effects->TAA) CreateEffect(EffectRecordType_TAA);
	if (Effects->SnowAccumulation) CreateEffect(EffectRecordType_SnowAccumulation);
	if (Effects->Underwater) CreateEffect(EffectRecordType_Underwater);
	if (Effects->VolumetricFog) CreateEffect(EffectRecordType_VolumetricFog);
	if (Effects->VolumetricLight) CreateEffect(EffectRecordType_VolumetricLight);
	if (Effects->WaterLens) CreateEffect(EffectRecordType_WaterLens);
	if (Effects->WetWorld) CreateEffect(EffectRecordType_WetWorld);
	if (Effects->Precipitations) CreateEffect(EffectRecordType_Precipitations);
	if (Effects->Extra) CreateEffect(EffectRecordType_Extra);
	if (TheSettingManager->SettingsShadows.Exteriors.UsePostProcessing) CreateEffect(EffectRecordType_ShadowsExteriors);
	if (TheSettingManager->SettingsShadows.Point.UsePostProcessing) CreateEffect(EffectRecordType_ShadowsPoint);

}

void ShaderManager::InitializeConstants() {

	ShaderConst.pWeather = NULL;
	ShaderConst.WaterLens.Percent = 0.0f;
	ShaderConst.SnowAccumulation.Params.w = 0.0f;
	ShaderConst.WetWorld.Data.x = 0.0f;
	GameDay = 0;
	ShaderConst.EveningTransLightDirSet = false;
	isFullyInitialized = false;
	InitFrameCount = 0;
	InitFrameTarget = 10;
}

void ShaderManager::UpdateShaderStates() {

	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_UpdateShaderStates);

	if (Player->IsExteriorLike()) {
		if (LocationState != CellLocation::Exterior) {
			LocationState = CellLocation::Exterior;
			DisposeShader("InteriorShadows");
			CreateShader("ExteriorPom"); //no disposal needed since they are disposed as part of "InteriorShadows"
			CreateShader("ExteriorExtraShaders");//no disposal needed since they are disposed as part of "InteriorShadows"
		}

		if (MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog) || MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) {
			if (!DialogState) {
				DialogState = true;
				DisposeShader("ExteriorDialog");
				CreateShader("ExteriorDialogActive");
			}
		}
		else if (DialogState) {
			DialogState = false;
			DisposeShader("ExteriorDialog");
			CreateShader("ExteriorDialogInactive");
		}

	}
	else {
		if (LocationState != CellLocation::Interior) {
			LocationState = CellLocation::Interior;
			DisposeShader("InteriorShadows");
			CreateShader("InteriorShadows");
		}

	}

}

void ShaderManager::UpdateGameTime(ShaderConstants& ShaderConst) {
	ShaderConst.GameTime.x = TimeGlobals::GetGameTime();
	ShaderConst.GameTime.y = ShaderConst.GameTime.x / 3600.0f;
	ShaderConst.GameTime.z = ShaderConst.Tick.x = TheFrameRateManager->Time;
	ShaderConst.Tick.y = TheFrameRateManager->GetPerformance();
}

void ShaderManager::UpdateCelestialDirections(ShaderConstants& ShaderConst, NiNode* SunRoot, Moon* Masser, Moon* Secunda, TESClimate* climate, float lastGameTime) {
	ShaderConst.SunTiming.x = climate->sunriseBegin / 6.0f - 1.0f;
	ShaderConst.SunTiming.y = climate->sunriseEnd / 6.0f;
	ShaderConst.SunTiming.z = climate->sunsetBegin / 6.0f;
	ShaderConst.SunTiming.w = climate->sunsetEnd / 6.0f + 1.0f;

	if (lastGameTime != ShaderConst.GameTime.y) {
		float deltaz = ShaderConst.SunDir.z;
		ShaderConst.SunDir.x = SunRoot->m_localTransform.pos.x;
		ShaderConst.SunDir.y = SunRoot->m_localTransform.pos.y;
		ShaderConst.SunDir.z = SunRoot->m_localTransform.pos.z;
		((NiVector4*)&ShaderConst.SunDir)->Normalize();
		if (ShaderConst.GameTime.y > ShaderConst.SunTiming.w || ShaderConst.GameTime.y < ShaderConst.SunTiming.x)
		{
			ShaderConst.SunDir.z = -ShaderConst.SunDir.z;
		}
		else if (ShaderConst.GameTime.y > ShaderConst.SunTiming.z && fabs(deltaz) - ShaderConst.SunDir.z < 0.0f)
		{
			ShaderConst.SunDir.z = -ShaderConst.SunDir.z;
		}

		//TODO: use kClimate_Masser and kClimate_Secunda but not sure what to compare against?
		if (Masser && Secunda) {
			//TODO: set x properly
			ShaderConst.MasserDir.x = 0.7f;
			ShaderConst.MasserDir.y = cos(Masser->degree * M_PI / 180.0);
			ShaderConst.MasserDir.z = sin(Masser->degree * M_PI / 180.0);

			ShaderConst.SecundaDir.x = 1.2f;
			ShaderConst.SecundaDir.y = cos(Secunda->degree * M_PI / 180.0);
			ShaderConst.SecundaDir.z = sin(Secunda->degree * M_PI / 180.0);

			((NiVector4*)&ShaderConst.MasserDir)->Normalize();
			((NiVector4*)&ShaderConst.SecundaDir)->Normalize();

			//TODO: make configurable
			Masser->AngleFadeStart = 0.0f;
			Masser->AngleFadeEnd = 0.0f;
			Secunda->AngleFadeStart = 0.0f;
			Secunda->AngleFadeEnd = 0.0f;

			ShaderConst.MasserFade = Masser->fadeValue();
			ShaderConst.SecundaFade = Secunda->fadeValue();
			ShaderConst.MoonsExist = true;
		}
		else {
			ShaderConst.MasserDir.x = 0.0f;
			ShaderConst.MasserDir.y = 0.0f;
			ShaderConst.MasserDir.z = 0.0f;

			ShaderConst.SecundaDir.x = 0.0f;
			ShaderConst.SecundaDir.y = 0.0f;
			ShaderConst.SecundaDir.z = 0.0f;

			ShaderConst.MasserFade = 0.0f;
			ShaderConst.SecundaFade = 0.0f;
			ShaderConst.MoonsExist = false;
		}
	}
}

void ShaderManager::UpdateMoonPhaseCoeff(ShaderConstants& ShaderConst, TESClimate* climate, int& GameDay) {
	//set moon phase Coeff during day to avoid jump in luminance when phases shift
	if (GameDay == 0 || (GameDay != TimeGlobals::GetGameDaysPassed() && ShaderConst.GameTime.y > 12.00)) {
		int phaseLength = (climate->phaseLength & climate->kClimate_PhaseLengthMask);
		int phase = TimeGlobals::GetGameDaysPassed() % (phaseLength * 8);
		TheShaderManager->SetPhaseLumCoeff(phaseLength, phase);
		GameDay = TimeGlobals::GetGameDaysPassed();
	}
}

float ShaderManager::UpdateExteriorLighting(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent) {
	float dayPercent = 0;
	if (currentWeather) {
		ShaderConst.SunDir.w = 1.0f;
		ShaderConst.MasserDir.w = 1.0f;
		ShaderConst.SecundaDir.w = 1.0f;
		ShaderConst.ShadowMap.ShadowLightDir.w = 1.0f;
		if (ShaderConst.GameTime.y >= ShaderConst.SunTiming.y && ShaderConst.GameTime.y <= ShaderConst.SunTiming.z) {
			ShaderConst.SunAmount.x = 0.0f;
			ShaderConst.SunAmount.y = 1.0f;
			ShaderConst.SunAmount.z = 0.0f;
			ShaderConst.SunAmount.w = 0.0f;
			ShaderConst.MasserAmount.x = (ShaderConst.MasserFade - ShaderConst.SunAmount.y);
			ShaderConst.SecundaAmount.x = (ShaderConst.SecundaFade - ShaderConst.SunAmount.y);
			ShaderConst.OverrideVanillaDirectionalLight = false;
			ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;
			ShaderConst.DayPhase = Day;
			dayPercent = 1.0f;
			ShaderConst.EveningTransLightDirSet = false;
		}
		else if ((ShaderConst.GameTime.y >= ShaderConst.SunTiming.w && ShaderConst.GameTime.y <= 23.99) || (ShaderConst.GameTime.y >= 0 && ShaderConst.GameTime.y <= ShaderConst.SunTiming.x)) {
			ShaderConst.SunAmount.x = 0.0f;
			ShaderConst.SunAmount.y = 0.0f;
			ShaderConst.SunAmount.z = 0.0f;
			ShaderConst.SunAmount.w = 1.0f;
			ShaderConst.MasserAmount.x = ShaderConst.MasserFade;
			ShaderConst.SecundaAmount.x = ShaderConst.SecundaFade;
			ShaderConst.DayPhase = Night;
			dayPercent = 0.0f;
			ShaderConst.EveningTransLightDirSet = false;
			if (TheSettingManager->SettingsMain.Main.DirectionalLightOverride) {
				ShaderConst.DirectionalLight.x = TheShaderManager->ShaderConst.MasserDir.x * -1;
				ShaderConst.DirectionalLight.y = TheShaderManager->ShaderConst.MasserDir.y * -1;
				ShaderConst.DirectionalLight.z = TheShaderManager->ShaderConst.MasserDir.z * -1;
				ShaderConst.OverrideVanillaDirectionalLight = true;
				ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.MasserDir;
			}
			else {
				ShaderConst.OverrideVanillaDirectionalLight = false;
				ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;
			}
		}
		else if (ShaderConst.GameTime.y >= ShaderConst.SunTiming.x && ShaderConst.GameTime.y <= ShaderConst.SunTiming.y) {
			if ((ShaderConst.GameTime.y - ShaderConst.SunTiming.x) * 2 <= ShaderConst.SunTiming.y - ShaderConst.SunTiming.x) {
				ShaderConst.SunAmount.x = (ShaderConst.GameTime.y - ShaderConst.SunTiming.x ) * 2 / (ShaderConst.SunTiming.y - ShaderConst.SunTiming.x);
				ShaderConst.SunAmount.y = 0.0f;
				ShaderConst.SunAmount.z = 0.0f;
				ShaderConst.SunAmount.w = 1.0f - (ShaderConst.GameTime.y - ShaderConst.SunTiming.x ) * 2 / (ShaderConst.SunTiming.y - ShaderConst.SunTiming.x);
				ShaderConst.MasserAmount.x = (ShaderConst.MasserFade - (ShaderConst.SunAmount.x / 0.4f));
				ShaderConst.SecundaAmount.x = (ShaderConst.SecundaFade - (ShaderConst.SunAmount.x / 0.4f));
				ShaderConst.OverrideVanillaDirectionalLight = false;
				ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;

				float start = 0.2f;
				float end = 0.6f;
				float diff = end - start;
				float scale = (ShaderConst.SunAmount.x - start) / diff;

				ShaderConst.ShadowMap.ShadowLightDir.w = std::clamp(scale, ShaderConst.Shadow.Data.y, 1.0f);
				ShaderConst.DayPhase = Dawn;
				dayPercent = ShaderConst.ShadowMap.ShadowLightDir.w;
				ShaderConst.EveningTransLightDirSet = false;

				if (ShaderConst.MasserAmount.x > 0.0f) {
					ShaderConst.OverrideVanillaDirectionalLight = true;
					if (!ShaderConst.MorningTransLightDirSet) {
						ShaderConst.DirectionalLight.x = std::lerp(ShaderConst.SunDir.x * -1, TheShaderManager->ShaderConst.MasserDir.x * -1, ShaderConst.MasserAmount.x);
						ShaderConst.DirectionalLight.y = std::lerp(ShaderConst.SunDir.y * -1, TheShaderManager->ShaderConst.MasserDir.y * -1, ShaderConst.MasserAmount.x);
						ShaderConst.DirectionalLight.z = std::lerp(ShaderConst.SunDir.z * -1, TheShaderManager->ShaderConst.MasserDir.z * -1, ShaderConst.MasserAmount.x);
					}
					else {
						ShaderConst.DirectionalLight.x = std::lerp(ShaderConst.MorningTransLightDir.x, TheShaderManager->ShaderConst.MasserDir.x * -1, ShaderConst.MasserAmount.x);
						ShaderConst.DirectionalLight.y = std::lerp(ShaderConst.MorningTransLightDir.y, TheShaderManager->ShaderConst.MasserDir.y * -1, ShaderConst.MasserAmount.x);
						ShaderConst.DirectionalLight.z = std::lerp(ShaderConst.MorningTransLightDir.z, TheShaderManager->ShaderConst.MasserDir.z * -1, ShaderConst.MasserAmount.x);
					}
					ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.MasserDir;
					ShaderConst.ShadowMap.ShadowLightDir.w = std::clamp(ShaderConst.MasserAmount.x, ShaderConst.Shadow.Data.y, 1.0f);
				}
				else {
					//Override the interval update here only to update the shadow map immediately, sometimes the refresh period transitions too late, showing the shadow map rotation
					TheSettingManager->SettingsShadows.Exteriors.UseIntervalUpdate = false;

					if (!ShaderConst.MorningTransLightDirSet && ShaderConst.MasserAmount.x > -0.1f) {
						ShaderConst.MorningTransLightDir = D3DXVECTOR4(Tes->niDirectionalLight->m_direction.x, Tes->niDirectionalLight->m_direction.y, Tes->niDirectionalLight->m_direction.z, 1);
						if (fabs(ShaderConst.MorningTransLightDir.x) > 1.0f) {
							ShaderConst.MorningTransLightDirSet = true;
							((NiVector4*)&ShaderConst.MorningTransLightDir)->Normalize();
						}
					}
				}
			}
			else {
				ShaderConst.SunAmount.x = 2.0f - (ShaderConst.GameTime.y - ShaderConst.SunTiming.x) * 2 / (ShaderConst.SunTiming.y - ShaderConst.SunTiming.x);
				ShaderConst.SunAmount.y = (ShaderConst.GameTime.y - ShaderConst.SunTiming.x) * 2 / (ShaderConst.SunTiming.y - ShaderConst.SunTiming.x) - 1.0f;
				ShaderConst.SunAmount.z = 0.0f;
				ShaderConst.SunAmount.w = 0.0f;
				ShaderConst.MasserAmount.x = (ShaderConst.MasserFade - (ShaderConst.SunAmount.x + ShaderConst.SunAmount.y));
				ShaderConst.SecundaAmount.x = (ShaderConst.SecundaFade - (ShaderConst.SunAmount.x + ShaderConst.SunAmount.y));
				ShaderConst.OverrideVanillaDirectionalLight = false;
				ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;
				ShaderConst.DayPhase = Sunrise;
				dayPercent = 1.0f;
				ShaderConst.EveningTransLightDirSet = false;
			}
		}
		else if (ShaderConst.GameTime.y >= ShaderConst.SunTiming.z && ShaderConst.GameTime.y <= ShaderConst.SunTiming.w) {
			if ((ShaderConst.GameTime.y - ShaderConst.SunTiming.z) * 2 <= ShaderConst.SunTiming.w - ShaderConst.SunTiming.z) {
				ShaderConst.SunAmount.x = 0.0f;
				ShaderConst.SunAmount.y = 1.0f - (ShaderConst.GameTime.y - ShaderConst.SunTiming.z) * 2 / (ShaderConst.SunTiming.w - ShaderConst.SunTiming.z);
				ShaderConst.SunAmount.z = (ShaderConst.GameTime.y - ShaderConst.SunTiming.z) * 2 / (ShaderConst.SunTiming.w - ShaderConst.SunTiming.z);
				ShaderConst.SunAmount.w = 0.0f;
				ShaderConst.MasserAmount.x = (ShaderConst.MasserFade - (ShaderConst.SunAmount.y + ShaderConst.SunAmount.z));
				ShaderConst.SecundaAmount.x = (ShaderConst.SecundaFade - (ShaderConst.SunAmount.y + ShaderConst.SunAmount.z));
				ShaderConst.OverrideVanillaDirectionalLight = false;
				ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;
				ShaderConst.DayPhase = Sunset;
				dayPercent = 1.0f;
				ShaderConst.EveningTransLightDirSet = false;
			}
			else {
				ShaderConst.SunAmount.x = 0.0f;
				ShaderConst.SunAmount.y = 0.0f;
				ShaderConst.SunAmount.z = 2.0f - (ShaderConst.GameTime.y - ShaderConst.SunTiming.z) * 2 / (ShaderConst.SunTiming.w - ShaderConst.SunTiming.z);
				ShaderConst.SunAmount.w = (ShaderConst.GameTime.y - ShaderConst.SunTiming.z) * 2 / (ShaderConst.SunTiming.w - ShaderConst.SunTiming.z) - 1.0f;
				ShaderConst.MasserAmount.x = (ShaderConst.MasserFade - (ShaderConst.SunAmount.z / 0.3f));
				ShaderConst.SecundaAmount.x = (ShaderConst.SecundaFade - (ShaderConst.SunAmount.z / 0.3f));
				ShaderConst.OverrideVanillaDirectionalLight = false;
				ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;
				ShaderConst.DayPhase = Dusk;
				dayPercent = 1.0f;

				if (ShaderConst.SunAmount.z < .5) {
					float start = 0.5f;
					float end = 0.3f;
					float diff = start - end;
					float scale = (ShaderConst.SunAmount.z - end) / diff;
					ShaderConst.ShadowMap.ShadowLightDir.w = std::clamp(scale, ShaderConst.Shadow.Data.y, 1.0f);
					dayPercent = ShaderConst.ShadowMap.ShadowLightDir.w;
				}
				if (ShaderConst.MasserAmount.x > 0.0f) {
					ShaderConst.OverrideVanillaDirectionalLight = true;

					if (!ShaderConst.EveningTransLightDirSet) {
						ShaderConst.EveningTransLightDir = D3DXVECTOR4(Tes->niDirectionalLight->m_direction.x, Tes->niDirectionalLight->m_direction.y, Tes->niDirectionalLight->m_direction.z, 1);
						((NiVector4*)&ShaderConst.EveningTransLightDir)->Normalize();
						ShaderConst.EveningTransLightDirSet = true;
					}
					ShaderConst.DirectionalLight.x = std::lerp(ShaderConst.EveningTransLightDir.x, TheShaderManager->ShaderConst.MasserDir.x * -1, ShaderConst.MasserAmount.x);
					ShaderConst.DirectionalLight.y = std::lerp(ShaderConst.EveningTransLightDir.y, TheShaderManager->ShaderConst.MasserDir.y * -1, ShaderConst.MasserAmount.x);
					ShaderConst.DirectionalLight.z = std::lerp(ShaderConst.EveningTransLightDir.z, TheShaderManager->ShaderConst.MasserDir.z * -1, ShaderConst.MasserAmount.x);
					ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.MasserDir;
					//Override the interval update here only to update the shadow map immediately, sometimes the refresh period transitions too late, showing the shadow map rotation
					TheSettingManager->SettingsShadows.Exteriors.UseIntervalUpdate = false;
					ShaderConst.ShadowMap.ShadowLightDir.w = std::clamp(ShaderConst.MasserAmount.x, ShaderConst.Shadow.Data.y, 1.0f);
					dayPercent = 0.0f;
				}
			}
		}

		TESWeatherEx* currentWeatherEx = ((TESWeatherEx*)currentWeather);
		currentWeather->colors[TESWeather::eColor_Sunlight].colors[TESWeather::eTime_Night].r = currentWeatherEx->colorsb[TESWeather::eColor_Sunlight].colors[TESWeather::eTime_Night].r * ShaderConst.MoonPhaseCoeff;
		currentWeather->colors[TESWeather::eColor_Sunlight].colors[TESWeather::eTime_Night].g = currentWeatherEx->colorsb[TESWeather::eColor_Sunlight].colors[TESWeather::eTime_Night].g * ShaderConst.MoonPhaseCoeff;
		currentWeather->colors[TESWeather::eColor_Sunlight].colors[TESWeather::eTime_Night].b = currentWeatherEx->colorsb[TESWeather::eColor_Sunlight].colors[TESWeather::eTime_Night].b * ShaderConst.MoonPhaseCoeff;
		currentWeather->colors[TESWeather::eColor_Ambient].colors[TESWeather::eTime_Night].r = currentWeatherEx->colorsb[TESWeather::eColor_Ambient].colors[TESWeather::eTime_Night].r * ShaderConst.MoonPhaseCoeff;
		currentWeather->colors[TESWeather::eColor_Ambient].colors[TESWeather::eTime_Night].g = currentWeatherEx->colorsb[TESWeather::eColor_Ambient].colors[TESWeather::eTime_Night].g * ShaderConst.MoonPhaseCoeff;
		currentWeather->colors[TESWeather::eColor_Ambient].colors[TESWeather::eTime_Night].b = currentWeatherEx->colorsb[TESWeather::eColor_Ambient].colors[TESWeather::eTime_Night].b * ShaderConst.MoonPhaseCoeff;

		if (ShaderConst.pWeather == NULL) ShaderConst.pWeather = currentWeather;

		for (int t = TESWeather::eTime_Sunrise; t <= TESWeather::eTime_Night ; t++) {
			RGBA color = currentWeather->colors[TESWeather::eColor_Fog].colors[t];
			switch (t)
			{
				case TESWeather::eTime_Sunrise:
					ShaderConst.fogColor.x = color.r / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.fogColor.y = color.g / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.fogColor.z = color.b / 255.0f * ShaderConst.SunAmount.x;
					break;
				case TESWeather::eTime_Day:
					ShaderConst.fogColor.x += color.r / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.fogColor.y += color.g / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.fogColor.z += color.b / 255.0f * ShaderConst.SunAmount.y;
					break;
				case TESWeather::eTime_Sunset:
					ShaderConst.fogColor.x += color.r / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.fogColor.y += color.g / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.fogColor.z += color.b / 255.0f * ShaderConst.SunAmount.z;
					break;
				case TESWeather::eTime_Night:
					ShaderConst.fogColor.x += color.r / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.fogColor.y += color.g / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.fogColor.z += color.b / 255.0f * ShaderConst.SunAmount.w;
					break;
			}
		}
		for (int t = TESWeather::eTime_Sunrise; t <= TESWeather::eTime_Night ; t++) {
			RGBA color = ShaderConst.pWeather->colors[TESWeather::eColor_Fog].colors[t];
			switch (t)
			{
				case TESWeather::eTime_Sunrise:
					ShaderConst.oldfogColor.x = color.r / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.oldfogColor.y = color.g / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.oldfogColor.z = color.b / 255.0f * ShaderConst.SunAmount.x;
					break;
				case TESWeather::eTime_Day:
					ShaderConst.oldfogColor.x += color.r / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.oldfogColor.y += color.g / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.oldfogColor.z += color.b / 255.0f * ShaderConst.SunAmount.y;
					break;
				case TESWeather::eTime_Sunset:
					ShaderConst.oldfogColor.x += color.r / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.oldfogColor.y += color.g / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.oldfogColor.z += color.b / 255.0f * ShaderConst.SunAmount.z;
					break;
				case TESWeather::eTime_Night:
					ShaderConst.oldfogColor.x += color.r / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.oldfogColor.y += color.g / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.oldfogColor.z += color.b / 255.0f * ShaderConst.SunAmount.w;
					break;
			}
		}

		for (int t = TESWeather::eTime_Sunrise; t <= TESWeather::eTime_Night ; t++) {
			RGBA color = currentWeather->colors[TESWeather::eColor_Sun].colors[t];
			switch (t)
			{
				case TESWeather::eTime_Sunrise:
					ShaderConst.sunColor.x = color.r / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.sunColor.y = color.g / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.sunColor.z = color.b / 255.0f * ShaderConst.SunAmount.x;
					break;
				case TESWeather::eTime_Day:
					ShaderConst.sunColor.x += color.r / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.sunColor.y += color.g / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.sunColor.z += color.b / 255.0f * ShaderConst.SunAmount.y;
					break;
				case TESWeather::eTime_Sunset:
					ShaderConst.sunColor.x += color.r / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.sunColor.y += color.g / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.sunColor.z += color.b / 255.0f * ShaderConst.SunAmount.z;
					break;
				case TESWeather::eTime_Night:
					ShaderConst.sunColor.x += color.r / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.sunColor.y += color.g / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.sunColor.z += color.b / 255.0f * ShaderConst.SunAmount.w;
					break;
			}
		}
		for (int t = TESWeather::eTime_Sunrise; t <= TESWeather::eTime_Night ; t++) {
			RGBA color = ShaderConst.pWeather->colors[TESWeather::eColor_Sun].colors[t];
			switch (t)
			{
				case TESWeather::eTime_Sunrise:
					ShaderConst.oldsunColor.x = color.r / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.oldsunColor.y = color.g / 255.0f * ShaderConst.SunAmount.x;
					ShaderConst.oldsunColor.z = color.b / 255.0f * ShaderConst.SunAmount.x;
					break;
				case TESWeather::eTime_Day:
					ShaderConst.oldsunColor.x += color.r / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.oldsunColor.y += color.g / 255.0f * ShaderConst.SunAmount.y;
					ShaderConst.oldsunColor.z += color.b / 255.0f * ShaderConst.SunAmount.y;
					break;
				case TESWeather::eTime_Sunset:
					ShaderConst.oldsunColor.x += color.r / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.oldsunColor.y += color.g / 255.0f * ShaderConst.SunAmount.z;
					ShaderConst.oldsunColor.z += color.b / 255.0f * ShaderConst.SunAmount.z;
					break;
				case TESWeather::eTime_Night:
					ShaderConst.oldsunColor.x += color.r / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.oldsunColor.y += color.g / 255.0f * ShaderConst.SunAmount.w;
					ShaderConst.oldsunColor.z += color.b / 255.0f * ShaderConst.SunAmount.w;
					break;
			}
		}

		if (ShaderConst.SunAmount.w == 1.0f) {
			ShaderConst.currentfogStart = currentWeather->GetFogNightNear();
			ShaderConst.currentfogEnd = currentWeather->GetFogNightFar();
			ShaderConst.oldfogStart = ShaderConst.pWeather->GetFogNightNear();
			ShaderConst.oldfogEnd = ShaderConst.pWeather->GetFogNightFar();
		}
		else {
			ShaderConst.currentfogStart = currentWeather->GetFogDayNear();
			ShaderConst.currentfogEnd = currentWeather->GetFogDayFar();
			ShaderConst.oldfogStart = ShaderConst.pWeather->GetFogDayNear();
			ShaderConst.oldfogEnd = ShaderConst.pWeather->GetFogDayFar();
		}

		ShaderConst.oldsunGlare = ShaderConst.pWeather->sunGlare;
		ShaderConst.oldwindSpeed = ShaderConst.pWeather->windSpeed;
		ShaderConst.currentsunGlare = (ShaderConst.oldsunGlare - ((ShaderConst.oldsunGlare - currentWeather->sunGlare) * weatherPercent)) / 255.0f;
		ShaderConst.currentwindSpeed = (ShaderConst.oldwindSpeed - ((ShaderConst.oldwindSpeed - currentWeather->windSpeed) * weatherPercent)) / 255.0f;

		ShaderConst.fogColor.x = ShaderConst.oldfogColor.x - ((ShaderConst.oldfogColor.x - ShaderConst.fogColor.x) * weatherPercent);
		ShaderConst.fogColor.y = ShaderConst.oldfogColor.y - ((ShaderConst.oldfogColor.y - ShaderConst.fogColor.y) * weatherPercent);
		ShaderConst.fogColor.z = ShaderConst.oldfogColor.z - ((ShaderConst.oldfogColor.z - ShaderConst.fogColor.z) * weatherPercent);
		ShaderConst.fogColor.w = 1.0f;

		ShaderConst.sunColor.x = ShaderConst.oldsunColor.x - ((ShaderConst.oldsunColor.x - ShaderConst.sunColor.x) * weatherPercent);
		ShaderConst.sunColor.y = ShaderConst.oldsunColor.y - ((ShaderConst.oldsunColor.y - ShaderConst.sunColor.y) * weatherPercent);
		ShaderConst.sunColor.z = ShaderConst.oldsunColor.z - ((ShaderConst.oldsunColor.z - ShaderConst.sunColor.z) * weatherPercent);
		ShaderConst.sunColor.w = ShaderConst.SunAmount.w;

		ShaderConst.fogData.x = ShaderConst.oldfogStart - ((ShaderConst.oldfogStart - ShaderConst.currentfogStart) * weatherPercent);
		ShaderConst.fogData.y = ShaderConst.oldfogEnd - ((ShaderConst.oldfogEnd - ShaderConst.currentfogEnd) * weatherPercent);
		ShaderConst.fogData.z = ShaderConst.currentsunGlare;

		if (weatherPercent == 1.0f) ShaderConst.pWeather = currentWeather;
	}
	return dayPercent;
}

void ShaderManager::UpdateInteriorLighting(ShaderConstants& ShaderConst, TESObjectCELL* currentCell, ShaderConstants::SimpleLightingStruct& InteriorLighting, bool& isFullyInitialized, int& InitFrameCount) {
	ShaderConst.SunDir.w = 0.0f;
	ShaderConst.MasserDir.w = 0.0f;
	ShaderConst.SecundaDir.w = 0.0f;
	ShaderConst.SunAmount.x = 0.0f;
	ShaderConst.SunAmount.y = 1.0f;
	ShaderConst.SunAmount.z = 0.0f;
	ShaderConst.SunAmount.w = 1.0f;
	ShaderConst.currentsunGlare = 0.5f;
	ShaderConst.ShadowMap.ShadowLightDir = ShaderConst.SunDir;
	ShaderConst.OverrideVanillaDirectionalLight = false;
	ShaderConst.EveningTransLightDirSet = false;
	isFullyInitialized = false;
	InitFrameCount = 0;
	TESObjectCELL::LightingData* LightData = currentCell->lighting;

	if (!(currentCell->flags0 & currentCell->kFlags0_BehaveLikeExterior)) {
		ShaderConst.fogColor.x = LightData->fog.r / 255.0f;
		ShaderConst.fogColor.y = LightData->fog.g / 255.0f;
		ShaderConst.fogColor.z = LightData->fog.b / 255.0f;
	}
	else {
		//TODO: fog color causes issues in SKYT shader for these cells
		ShaderConst.fogColor.x = 0.0f;
		ShaderConst.fogColor.y = 0.0f;
		ShaderConst.fogColor.z = 0.0f;
	}
	ShaderConst.fogColor.w = 1.0f;

	ShaderConst.sunColor.x = LightData->ambient.r / 255.0f;
	ShaderConst.sunColor.y = LightData->ambient.g / 255.0f;
	ShaderConst.sunColor.z = LightData->ambient.b / 255.0f;
	ShaderConst.sunColor.w = 0.0f;

	ShaderConst.fogData.x = LightData->fogNear;
	ShaderConst.fogData.y = LightData->fogFar;
	ShaderConst.fogData.z = ShaderConst.currentsunGlare;

	//TODO do these
	ShaderConst.InteriorDimmerStart = 6.0f;
	ShaderConst.InteriorDimmerEnd = 9.0f;
	float dimmer;
	if (ShaderConst.GameTime.y > 12) {
		dimmer = ShaderConst.GameTime.y - (12 + ShaderConst.InteriorDimmerStart);
		dimmer = 1 - (dimmer / (ShaderConst.InteriorDimmerEnd - ShaderConst.InteriorDimmerStart));
		dimmer = std::clamp(dimmer, 0.0f, 1.0f);
	}
	else {
		dimmer = ShaderConst.GameTime.y - ShaderConst.InteriorDimmerStart;
		dimmer = (dimmer / (ShaderConst.InteriorDimmerEnd - ShaderConst.InteriorDimmerStart));
		dimmer = std::clamp(dimmer, 0.0f, 1.0f);
	}

	ShaderConst.InteriorDimmer.x = dimmer;

	if (TheSettingManager->SettingsMain.Main.InteriorDimmerCoeff < 1.0f) {
		// araf InteriorDimmerCoeff is now a dimmer switch
		// float dimmerAdj = std::clamp(dimmer, TheSettingManager->SettingsMain.Main.InteriorDimmerCoeff, 1.0f);
		float dimmerAdj = TheSettingManager->SettingsMain.Main.InteriorDimmerCoeff;

		LightData->ambient.r = InteriorLighting.r * dimmerAdj;
		LightData->ambient.g = InteriorLighting.g * dimmerAdj;
		LightData->ambient.b = InteriorLighting.b * dimmerAdj;
	}
}

void ShaderManager::UpdateWater(ShaderConstants& ShaderConst, TESObjectCELL* currentCell, SettingsWaterStruct* sws) {
	ShaderConst.Water.waterCoefficients.x = sws->inExtCoeff_R;
	ShaderConst.Water.waterCoefficients.y = sws->inExtCoeff_G;
	ShaderConst.Water.waterCoefficients.z = sws->inExtCoeff_B;
	ShaderConst.Water.waterCoefficients.w = sws->inScattCoeff;

	ShaderConst.Water.waveParams.x = sws->choppiness;
	ShaderConst.Water.waveParams.y = sws->waveWidth;
	ShaderConst.Water.waveParams.z = sws->waveSpeed;
	ShaderConst.Water.waveParams.w = sws->reflectivity;

	ShaderConst.Water.waterSettings.x = Tes->GetWaterHeight(Player);
	ShaderConst.Water.waterSettings.y = sws->depthDarkness;
	ShaderConst.Water.waterSettings.z = sws->LODdistance;
	ShaderConst.Water.waterSettings.w = sws->MinLOD;

	ShaderConst.Water.waterVolume.x = sws->causticsStrength * ShaderConst.currentsunGlare;
	ShaderConst.Water.waterVolume.y = sws->shoreFactor;
	ShaderConst.Water.waterVolume.z = sws->turbidity;
	ShaderConst.Water.waterVolume.w = sws->causticsStrengthS;

	ShaderConst.Water.shorelineParams.x = sws->shoreMovement;

	ShaderConst.HasWater = currentCell->flags0 & TESObjectCELL::kFlags0_HasWater;

	if (TheSettingManager->SettingsMain.Effects.Underwater && TheSettingManager->SettingsMain.Effects.WaterLens) {
		ShaderConst.WaterLens.Time.x = sws->LensTimeMultA;
		ShaderConst.WaterLens.Time.y = sws->LensTimeMultB;
		ShaderConst.WaterLens.Time.z = sws->LensViscosity;
		if (ShaderConst.WaterLens.Percent == -1.0f) {
			ShaderConst.WaterLens.TimeAmount = 0.0f;
			ShaderConst.WaterLens.Time.w = sws->LensAmount;
		}
		else if (ShaderConst.WaterLens.Percent > 0.0f) {
			ShaderConst.WaterLens.TimeAmount += 1.0f;
			ShaderConst.WaterLens.Percent = 1.0f - ShaderConst.WaterLens.TimeAmount / sws->LensTime;
			if (ShaderConst.WaterLens.Percent < 0.0f) ShaderConst.WaterLens.Percent = 0.0f;
			ShaderConst.WaterLens.Time.w = sws->LensAmount * ShaderConst.WaterLens.Percent;
		}
	}
}

void ShaderManager::UpdateSnowAccumulation(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather) {
	if (currentWeather->weatherType == TESWeather::WeatherType::kType_Snow) {
		if (ShaderConst.SnowAccumulation.Params.w < TheSettingManager->SettingsPrecipitations.SnowAccumulation.Amount) ShaderConst.SnowAccumulation.Params.w = ShaderConst.SnowAccumulation.Params.w + TheSettingManager->SettingsPrecipitations.SnowAccumulation.Increase;
	}
	else if (!previousWeather || (previousWeather && previousWeather->weatherType == TESWeather::WeatherType::kType_Snow)) {
		if (ShaderConst.SnowAccumulation.Params.w > 0.0f)
			ShaderConst.SnowAccumulation.Params.w = ShaderConst.SnowAccumulation.Params.w - TheSettingManager->SettingsPrecipitations.SnowAccumulation.Decrease;
		else if (ShaderConst.SnowAccumulation.Params.w < 0.0f)
			ShaderConst.SnowAccumulation.Params.w = 0.0f;
	}
	ShaderConst.SnowAccumulation.Params.x = TheSettingManager->SettingsPrecipitations.SnowAccumulation.BlurNormDropThreshhold;
	ShaderConst.SnowAccumulation.Params.y = TheSettingManager->SettingsPrecipitations.SnowAccumulation.BlurRadiusMultiplier;
	ShaderConst.SnowAccumulation.Params.z = TheSettingManager->SettingsPrecipitations.SnowAccumulation.SunPower;
}

void ShaderManager::UpdateWetWorld(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent) {
	if (currentWeather->weatherType == TESWeather::WeatherType::kType_Rainy) {
		ShaderConst.WetWorld.Data.y = 1.0f;
		if (ShaderConst.WetWorld.Data.x < TheSettingManager->SettingsPrecipitations.WetWorld.Amount) ShaderConst.WetWorld.Data.x = ShaderConst.WetWorld.Data.x + TheSettingManager->SettingsPrecipitations.WetWorld.Increase;
	}
	else if (!previousWeather || (previousWeather && previousWeather->weatherType == TESWeather::WeatherType::kType_Rainy)) {
		ShaderConst.WetWorld.Data.y = 0.3f - weatherPercent;
		if (ShaderConst.WetWorld.Data.y <= 0.0f) ShaderConst.WetWorld.Data.y = 0.05f;
		if (ShaderConst.WetWorld.Data.x > 0.0f)
			ShaderConst.WetWorld.Data.x = ShaderConst.WetWorld.Data.x - TheSettingManager->SettingsPrecipitations.WetWorld.Decrease;
		else if (ShaderConst.WetWorld.Data.x < 0.0f)
			ShaderConst.WetWorld.Data.x = 0.0f;
	}
	ShaderConst.WetWorld.Coeffs.x = TheSettingManager->SettingsPrecipitations.WetWorld.PuddleCoeff_R;
	ShaderConst.WetWorld.Coeffs.y = TheSettingManager->SettingsPrecipitations.WetWorld.PuddleCoeff_G;
	ShaderConst.WetWorld.Coeffs.z = TheSettingManager->SettingsPrecipitations.WetWorld.PuddleCoeff_B;
	ShaderConst.WetWorld.Coeffs.w = TheSettingManager->SettingsPrecipitations.WetWorld.PuddleSpecularMultiplier;
}

void ShaderManager::UpdatePrecipitation(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent) {
	// araf Stretch out rain transition time from 1.0 - 0.8 to 1.0 - 0.5
	if (currentWeather->weatherType == TESWeather::WeatherType::kType_Rainy) {
		if (weatherPercent > 0.5f) {
			ShaderConst.Precipitations.RainData.x = (weatherPercent - 0.5f) / (1.0f - 0.5f);
		}
		else {
			ShaderConst.Precipitations.RainData.x = 0.0f;
		}
	}
	else if (!previousWeather || (previousWeather && previousWeather->weatherType == TESWeather::WeatherType::kType_Rainy)) {
		if ((1.0f - weatherPercent) > 0.5f) {
			ShaderConst.Precipitations.RainData.x = ((1.0f - weatherPercent) - 0.5f) / (1.0f - 0.5f);
		}
		else {
			ShaderConst.Precipitations.RainData.x = 0.0f;
		}
	}
	if (currentWeather->weatherType == TESWeather::WeatherType::kType_Snow)
		ShaderConst.Precipitations.SnowData.x = weatherPercent;
	else if (!previousWeather || (previousWeather && previousWeather->weatherType == TESWeather::WeatherType::kType_Snow))
		ShaderConst.Precipitations.SnowData.x = 1.0f - weatherPercent;
	ShaderConst.Precipitations.SnowData.y = TheSettingManager->SettingsPrecipitations.Snow.DepthStep;
	ShaderConst.Precipitations.SnowData.z = TheSettingManager->SettingsPrecipitations.Snow.Flakes;
	ShaderConst.Precipitations.SnowData.w = TheSettingManager->SettingsPrecipitations.Snow.Speed;
}

namespace {

struct ActorDist { float x, y, distSq; };

static void ApplyGrassDensitySettings(int density) {
	static const int   kMinSize[]   = { 240, 240, 240, 240, 120, 120, 120, 120, 80, 80, 80, 80, 20, 20, 20, 20 };
	static const float kThreshold[] = { 0.3f, 0.2f, 0.1f, 0.0f, 0.3f, 0.2f, 0.1f, 0.0f,
	                                     0.3f, 0.2f, 0.1f, 0.0f, 0.3f, 0.2f, 0.1f, 0.0f };
	if (density >= 1 && density <= 16) {
		*SettingMinGrassSize        = kMinSize[density - 1];
		*SettingTexturePctThreshold = kThreshold[density - 1];
	}
}

static void TryInsertActor(ActorDist nearest[], int& count, float x, float y, float distSq) {
	if (count < 4) {
		nearest[count++] = { x, y, distSq };
	} else {
		int farthestIdx = 1;
		for (int i = 2; i < 4; i++)
			if (nearest[i].distSq > nearest[farthestIdx].distSq)
				farthestIdx = i;
		if (distSq < nearest[farthestIdx].distSq)
			nearest[farthestIdx] = { x, y, distSq };
	}
}

static void CollectActorsFromObjectList(TList<TESObjectREFR>::Entry* entry, TESObjectREFR* player,
                                        ActorDist nearest[], int& count, float maxTrackDistSq) {
	while (entry) {
		if (TESObjectREFR* ref = entry->item) {
			if (ref->baseForm && ref != player) {
				UInt8 formType = ref->baseForm->formType;
				if (formType >= TESForm::FormType::kFormType_NPC &&
				    formType <= TESForm::FormType::kFormType_LeveledCreature &&
				    !((Actor*)ref)->LifeState) {
					float dx = ref->pos.x - player->pos.x;
					float dy = ref->pos.y - player->pos.y;
					float distSq = dx * dx + dy * dy;
					if (distSq < maxTrackDistSq)
						TryInsertActor(nearest, count, ref->pos.x, ref->pos.y, distSq);
				}
			}
		}
		entry = entry->next;
	}
}

} // namespace

void ShaderManager::UpdateGrass(ShaderConstants& ShaderConst, GrassActorPos GrassCollisionActors[4], int& GrassCollisionActorCount) {
	ShaderConst.Grass.Scale.x = TheSettingManager->SettingsGrass.ScaleX;
	ShaderConst.Grass.Scale.y = TheSettingManager->SettingsGrass.ScaleY;
	ShaderConst.Grass.Scale.z = TheSettingManager->SettingsGrass.ScaleZ;
	ShaderConst.Grass.Scale.w = TheSettingManager->SettingsGrass.MinHeight;

	ApplyGrassDensitySettings(TheSettingManager->SettingsGrass.GrassDensity);
	*SettingGrassStartFadeDistance = TheSettingManager->SettingsGrass.MinDistance;
	*SettingGrassEndDistance = TheSettingManager->SettingsGrass.MaxDistance;

	if (TheSettingManager->SettingsGrass.WindEnabled) {
		*SettingGrassWindMagnitudeMax = *LocalGrassWindMagnitudeMax = TheSettingManager->SettingsGrass.WindCoefficient * ShaderConst.currentwindSpeed;
		*SettingGrassWindMagnitudeMin = *LocalGrassWindMagnitudeMin = *SettingGrassWindMagnitudeMax * 0.5f;
	}

	ShaderConst.Grass.CollisionParams.x = TheSettingManager->SettingsGrass.CollisionRadius;
	ShaderConst.Grass.CollisionParams.y = TheSettingManager->SettingsGrass.CollisionStrength;
	ShaderConst.Grass.CollisionParams.z = TheSettingManager->SettingsGrass.CollisionFlattenStrength;
	memset(ShaderConst.Grass.CollisionXY, 0, sizeof(ShaderConst.Grass.CollisionXY));

	ActorDist nearest[4] = {};
	int count = 0;
	float maxTrackDistSq = TheSettingManager->SettingsGrass.MaxDistance * TheSettingManager->SettingsGrass.MaxDistance;

	if (Player) {
		nearest[0] = { Player->pos.x, Player->pos.y, 0.0f };
		count = 1;
	}

	if (Player && Player->parentCell) {
		if (Player->GetWorldSpace()) {
			for (UInt32 x = 0; x < *SettingGridsToLoad; x++) {
				for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
					TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y);
					if (Cell)
						CollectActorsFromObjectList(&Cell->objectList.First, Player, nearest, count, maxTrackDistSq);
				}
			}
		} else {
			CollectActorsFromObjectList(&Player->parentCell->objectList.First, Player, nearest, count, maxTrackDistSq);
		}
	}

	ShaderConst.Grass.CollisionParams.w = (float)count;
	GrassCollisionActorCount = count;

	for (int i = 0; i < 4; i++) {
		GrassCollisionActors[i].x = nearest[i].x;
		GrassCollisionActors[i].y = nearest[i].y;
	}
}

void ShaderManager::UpdatePOM(ShaderConstants& ShaderConst) {
	ShaderConst.POM.ParallaxData.x = TheSettingManager->SettingsPOM.HeightMapScale;
	ShaderConst.POM.ParallaxData.y = TheSettingManager->SettingsPOM.SelfShadow != 0.0f ? TheSettingManager->SettingsPOM.SelfShadowStrength : 0.0f;
	ShaderConst.POM.ParallaxData.z = TheSettingManager->SettingsPOM.MinSamples;
	ShaderConst.POM.ParallaxData.w = TheSettingManager->SettingsPOM.MaxSamples;
}

void ShaderManager::UpdateTerrain(ShaderConstants& ShaderConst) {
	ShaderConst.Terrain.Data.x = TheSettingManager->SettingsTerrain.DistantSpecular;
	ShaderConst.Terrain.Data.y = TheSettingManager->SettingsTerrain.DistantNoise;
	ShaderConst.Terrain.Data.z = TheSettingManager->SettingsTerrain.NearSpecular;
	ShaderConst.Terrain.Data.w = TheSettingManager->SettingsTerrain.MiddleSpecular;
}

void ShaderManager::UpdateSkin(ShaderConstants& ShaderConst) {
	ShaderConst.Skin.SkinData.x = TheSettingManager->SettingsSkin.Attenuation;
	ShaderConst.Skin.SkinData.y = TheSettingManager->SettingsSkin.SpecularPower;
	ShaderConst.Skin.SkinData.z = TheSettingManager->SettingsSkin.MaterialThickness;
	ShaderConst.Skin.SkinData.w = TheSettingManager->SettingsSkin.RimScalar;
	ShaderConst.Skin.SkinColor.x = TheSettingManager->SettingsSkin.CoeffRed;
	ShaderConst.Skin.SkinColor.y = TheSettingManager->SettingsSkin.CoeffGreen;
	ShaderConst.Skin.SkinColor.z = TheSettingManager->SettingsSkin.CoeffBlue;
}

void ShaderManager::UpdateGodRays(ShaderConstants& ShaderConst) {
	ShaderConst.GodRays.Ray.x = TheSettingManager->SettingsGodRays.RayIntensity;
	ShaderConst.GodRays.Ray.y = TheSettingManager->SettingsGodRays.RayLength;
	if (TheSettingManager->SettingsGodRays.SunGlareEnabled) {
		ShaderConst.GodRays.Ray.z = TheSettingManager->SettingsGodRays.RayDensity * ShaderConst.currentsunGlare;
		ShaderConst.GodRays.Ray.w = TheSettingManager->SettingsGodRays.RayVisibility * ShaderConst.currentsunGlare;
	}
	else {
		ShaderConst.GodRays.Ray.z = TheSettingManager->SettingsGodRays.RayDensity;
		ShaderConst.GodRays.Ray.w = TheSettingManager->SettingsGodRays.RayVisibility;
	}
	ShaderConst.GodRays.RayColor.x = TheSettingManager->SettingsGodRays.RayR;
	ShaderConst.GodRays.RayColor.y = TheSettingManager->SettingsGodRays.RayG;
	ShaderConst.GodRays.RayColor.z = TheSettingManager->SettingsGodRays.RayB;
	ShaderConst.GodRays.RayColor.w = TheSettingManager->SettingsGodRays.Saturate;
	ShaderConst.GodRays.Data.x = TheSettingManager->SettingsGodRays.LightShaftPasses;
	ShaderConst.GodRays.Data.y = TheSettingManager->SettingsGodRays.Luminance;
	ShaderConst.GodRays.Data.z = TheSettingManager->SettingsGodRays.GlobalMultiplier;
	ShaderConst.GodRays.Data.w = TheSettingManager->SettingsGodRays.TimeEnabled;
}

void ShaderManager::UpdateKhajiitRays(ShaderConstants& ShaderConst) {
	ShaderConst.KhajiitRaysMasser.Ray.x = TheSettingManager->SettingsKhajiitRays.mRayIntensity;
	ShaderConst.KhajiitRaysMasser.Ray.y = TheSettingManager->SettingsKhajiitRays.mRayLength;
	ShaderConst.KhajiitRaysMasser.Ray.z = TheSettingManager->SettingsKhajiitRays.mRayDensity;
	ShaderConst.KhajiitRaysMasser.Ray.w = TheSettingManager->SettingsKhajiitRays.mRayVisibility;
	ShaderConst.KhajiitRaysMasser.RayColor.x = TheSettingManager->SettingsKhajiitRays.mRayR;
	ShaderConst.KhajiitRaysMasser.RayColor.y = TheSettingManager->SettingsKhajiitRays.mRayG;
	ShaderConst.KhajiitRaysMasser.RayColor.z = TheSettingManager->SettingsKhajiitRays.mRayB;
	ShaderConst.KhajiitRaysMasser.RayColor.w = TheSettingManager->SettingsKhajiitRays.mSaturate;
	ShaderConst.KhajiitRaysMasser.Data.x = TheSettingManager->SettingsKhajiitRays.mLightShaftPasses;
	ShaderConst.KhajiitRaysMasser.Data.y = TheSettingManager->SettingsKhajiitRays.mLuminance;
	ShaderConst.KhajiitRaysMasser.Data.z = TheSettingManager->SettingsKhajiitRays.mGlobalMultiplier;

	ShaderConst.KhajiitRaysSecunda.Ray.x = TheSettingManager->SettingsKhajiitRays.sRayIntensity;
	ShaderConst.KhajiitRaysSecunda.Ray.y = TheSettingManager->SettingsKhajiitRays.sRayLength;
	ShaderConst.KhajiitRaysSecunda.Ray.z = TheSettingManager->SettingsKhajiitRays.sRayDensity;
	ShaderConst.KhajiitRaysSecunda.Ray.w = TheSettingManager->SettingsKhajiitRays.sRayVisibility;
	ShaderConst.KhajiitRaysSecunda.RayColor.x = TheSettingManager->SettingsKhajiitRays.sRayR;
	ShaderConst.KhajiitRaysSecunda.RayColor.y = TheSettingManager->SettingsKhajiitRays.sRayG;
	ShaderConst.KhajiitRaysSecunda.RayColor.z = TheSettingManager->SettingsKhajiitRays.sRayB;
	ShaderConst.KhajiitRaysSecunda.RayColor.w = TheSettingManager->SettingsKhajiitRays.sSaturate;
	ShaderConst.KhajiitRaysSecunda.Data.x = TheSettingManager->SettingsKhajiitRays.sLightShaftPasses;
	ShaderConst.KhajiitRaysSecunda.Data.y = TheSettingManager->SettingsKhajiitRays.sLuminance;
	ShaderConst.KhajiitRaysSecunda.Data.z = TheSettingManager->SettingsKhajiitRays.sGlobalMultiplier;
}

void ShaderManager::UpdateAmbientOcclusion(ShaderConstants& ShaderConst, SettingsAmbientOcclusionStruct* sas) {
	ShaderConst.AmbientOcclusion.Enabled = sas->Enabled;
	if (ShaderConst.AmbientOcclusion.Enabled) {
		ShaderConst.AmbientOcclusion.AOData.x = sas->RadiusMultiplier;
		ShaderConst.AmbientOcclusion.AOData.y = sas->StrengthMultiplier;
		ShaderConst.AmbientOcclusion.AOData.z = sas->ClampStrength;
		ShaderConst.AmbientOcclusion.AOData.w = sas->Range;
		ShaderConst.AmbientOcclusion.Data.x = sas->AngleBias;
		ShaderConst.AmbientOcclusion.Data.y = sas->LumThreshold * ShaderConst.SunAmount.y;
		ShaderConst.AmbientOcclusion.Data.z = sas->BlurDropThreshold;
		ShaderConst.AmbientOcclusion.Data.w = sas->BlurRadiusMultiplier;
	}
}

void ShaderManager::UpdateBloom(ShaderConstants& ShaderConst, SettingsBloomStruct* sbs) {
	ShaderConst.Bloom.BloomData.x = sbs->Luminance;
	ShaderConst.Bloom.BloomData.y = sbs->MiddleGray;
	ShaderConst.Bloom.BloomData.z = sbs->WhiteCutOff;
	ShaderConst.Bloom.BloomValues.x = sbs->BloomIntensity;
	ShaderConst.Bloom.BloomValues.y = sbs->OriginalIntensity;
	ShaderConst.Bloom.BloomValues.z = sbs->BloomSaturation;
	ShaderConst.Bloom.BloomValues.w = sbs->OriginalSaturation;
}

void ShaderManager::UpdateColoring(ShaderConstants& ShaderConst, SettingsColoringStruct* scs) {
	ShaderConst.Coloring.Data.x = scs->Strength;
	ShaderConst.Coloring.Data.y = scs->BaseGamma;
	ShaderConst.Coloring.Data.z = scs->Fade;
	ShaderConst.Coloring.Data.w = scs->Contrast;
	ShaderConst.Coloring.Values.x = scs->Saturation;
	ShaderConst.Coloring.Values.y = scs->Bleach;
	ShaderConst.Coloring.Values.z = scs->BleachLuma;
	ShaderConst.Coloring.Values.w = scs->Linearization;
	ShaderConst.Coloring.ColorCurve.x = scs->ColorCurve;
	ShaderConst.Coloring.ColorCurve.y = scs->ColorCurveR;
	ShaderConst.Coloring.ColorCurve.z = scs->ColorCurveG;
	ShaderConst.Coloring.ColorCurve.w = scs->ColorCurveB;
	ShaderConst.Coloring.EffectGamma.x = scs->EffectGamma;
	ShaderConst.Coloring.EffectGamma.y = scs->EffectGammaR;
	ShaderConst.Coloring.EffectGamma.z = scs->EffectGammaG;
	ShaderConst.Coloring.EffectGamma.w = scs->EffectGammaB;
}

void ShaderManager::UpdateDepthOfField(ShaderConstants& ShaderConst, bool IsThirdPersonView) {
	SettingsDepthOfFieldStruct* sds = NULL;

	if (Player->IsVanityView())
		sds = TheSettingManager->GetSettingsDepthOfField("VanityView");
	else if (IsThirdPersonView)
		sds = TheSettingManager->GetSettingsDepthOfField("ThirdPersonView");
	else
		sds = TheSettingManager->GetSettingsDepthOfField("FirstPersonView");

	if (sds->Mode == 1) {
		if (MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog) || MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) sds->Enabled = false;
	}
	else if (sds->Mode == 2) {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog)) sds->Enabled = false;
	}
	else if (sds->Mode == 3) {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) sds->Enabled = false;
	}
	else if (sds->Mode == 4) {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog) && !MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) sds->Enabled = false;
	}
	if (ShaderConst.DepthOfField.Enabled = sds->Enabled) {
		ShaderConst.DepthOfField.Blur.x = sds->DistantBlur;
		ShaderConst.DepthOfField.Blur.y = sds->DistantBlurStartRange;
		ShaderConst.DepthOfField.Blur.z = sds->DistantBlurEndRange;
		ShaderConst.DepthOfField.Blur.w = sds->BaseBlurRadius;
		ShaderConst.DepthOfField.Data.x = sds->BlurFallOff;
		ShaderConst.DepthOfField.Data.y = sds->Radius;
		ShaderConst.DepthOfField.Data.z = sds->DiameterRange;
		ShaderConst.DepthOfField.Data.w = sds->NearBlurCutOff;
	}
}

void ShaderManager::UpdateCinema(ShaderConstants& ShaderConst) {
	UInt8 Mode = TheSettingManager->SettingsCinema.Mode;

	ShaderConst.Cinema.Data.x = TheSettingManager->SettingsCinema.AspectRatio;
	ShaderConst.Cinema.Data.y = TheSettingManager->SettingsCinema.VignetteRadius;
	ShaderConst.Cinema.Data.w = TheSettingManager->SettingsCinema.ChromaticAberrationPower;
	if (Mode == 1) {
		if (MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog) || MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) Mode = -1;
	}
	else if (Mode == 2) {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog)) Mode = -1;
	}
	else if (Mode == 3) {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) Mode = -1;
	}
	else if (Mode == 4) {
		if (!MenuManager->IsActive(Menu::MenuType::kMenuType_Dialog) && !MenuManager->IsActive(Menu::MenuType::kMenuType_Persuasion)) Mode = -1;
	}
	if (Mode == -1) {
		ShaderConst.Cinema.Data.x = 0.0f;
		ShaderConst.Cinema.Data.y = 0.0f;
	}
	ShaderConst.Cinema.Data.z = TheSettingManager->SettingsCinema.VignetteDarkness;
}

void ShaderManager::UpdateMotionBlur(ShaderConstants& ShaderConst, bool IsThirdPersonView) {
	SettingsMotionBlurStruct* sms = NULL;

	if (IsThirdPersonView)
		sms = TheSettingManager->GetSettingsMotionBlur("ThirdPersonView");
	else
		sms = TheSettingManager->GetSettingsMotionBlur("FirstPersonView");

	float AngleZ = D3DXToDegree(Player->rot.z);
	float AngleX = D3DXToDegree(Player->rot.x);
	float fMotionBlurAmtX = ShaderConst.MotionBlur.oldAngleZ - AngleZ;
	float fMotionBlurAmtY = ShaderConst.MotionBlur.oldAngleX - AngleX;
	float fBlurDistScratchpad = fMotionBlurAmtX + 360.0f;
	float fBlurDistScratchpad2 = (AngleZ - ShaderConst.MotionBlur.oldAngleZ + 360.0f) * -1.0f;

	if (abs(fMotionBlurAmtX) > abs(fBlurDistScratchpad))
		fMotionBlurAmtX = fBlurDistScratchpad;
	else if (abs(fMotionBlurAmtX) > abs(fBlurDistScratchpad2))
		fMotionBlurAmtX = fBlurDistScratchpad2;

	if (pow(fMotionBlurAmtX, 2) + pow(fMotionBlurAmtY, 2) < sms->BlurCutOff) {
		fMotionBlurAmtX = 0.0f;
		fMotionBlurAmtY = 0.0f;
	}

	if (sms->Enabled) {
		ShaderConst.MotionBlur.Data.x = (ShaderConst.MotionBlur.oldoldAmountX + ShaderConst.MotionBlur.oldAmountX + fMotionBlurAmtX) / 3.0f;
		ShaderConst.MotionBlur.Data.y = (ShaderConst.MotionBlur.oldoldAmountY + ShaderConst.MotionBlur.oldAmountY + fMotionBlurAmtY) / 3.0f;
	}
	else {
		ShaderConst.MotionBlur.Data.x = 0.0f;
		ShaderConst.MotionBlur.Data.y = 0.0f;
	}
	ShaderConst.MotionBlur.oldAngleZ = AngleZ;
	ShaderConst.MotionBlur.oldAngleX = AngleX;
	ShaderConst.MotionBlur.oldoldAmountX = ShaderConst.MotionBlur.oldAmountX;
	ShaderConst.MotionBlur.oldoldAmountY = ShaderConst.MotionBlur.oldAmountY;
	ShaderConst.MotionBlur.oldAmountX = fMotionBlurAmtX;
	ShaderConst.MotionBlur.oldAmountY = fMotionBlurAmtY;
	ShaderConst.MotionBlur.BlurParams.x = sms->GaussianWeight;
	ShaderConst.MotionBlur.BlurParams.y = sms->BlurScale;
	ShaderConst.MotionBlur.BlurParams.z = sms->BlurOffsetMax;
}

void ShaderManager::UpdateSharpening(ShaderConstants& ShaderConst) {
	ShaderConst.Sharpening.Data.x = TheSettingManager->SettingsSharpening.Strength;
	ShaderConst.Sharpening.Data.y = TheSettingManager->SettingsSharpening.Clamp;
	ShaderConst.Sharpening.Data.z = TheSettingManager->SettingsSharpening.Offset;
}

void ShaderManager::UpdateVolumetricFog(ShaderConstants& ShaderConst, float weatherPercent) {
	ShaderConst.VolumetricFog.Data.x = TheSettingManager->SettingsVolumetricFog.Exponent;
	ShaderConst.VolumetricFog.Data.y = TheSettingManager->SettingsVolumetricFog.ColorCoeff;
	ShaderConst.VolumetricFog.Data.z = TheSettingManager->SettingsVolumetricFog.Amount;
	ShaderConst.VolumetricFog.Data.w = 1.0f;
	if (weatherPercent == 1.0f && ShaderConst.fogData.y > TheSettingManager->SettingsVolumetricFog.MaxDistance) ShaderConst.VolumetricFog.Data.w = 0.0f;
}

void ShaderManager::UpdateTAA(ShaderConstants& ShaderConst, int& jitterIndex, const JitterPattern jitterPattern[2]) {
	ShaderConst.Jitter.x = 0.0f;
	ShaderConst.Jitter.y = 0.0f;
	if (TheSettingManager->SettingsMain.Effects.TAA) {
		ShaderConst.TAA.Data.x = TheSettingManager->SettingsTAA.BlendWeight;
		ShaderConst.TAA.Data.y = TheSettingManager->SettingsTAA.ClampRadius;
		ShaderConst.TAA.Data.z = TheSettingManager->SettingsTAA.Sharpening;

		if (TheSettingManager->SettingsTAA.JitterEnabled) {
			jitterIndex++;
			jitterIndex = jitterIndex % 2;
			ShaderConst.Jitter.x = (jitterPattern[TheSettingManager->SettingsTAA.JitterPattern].pattern[jitterIndex].x / TheRenderManager->width);
			ShaderConst.Jitter.y = (jitterPattern[TheSettingManager->SettingsTAA.JitterPattern].pattern[jitterIndex].y / TheRenderManager->height);
		}
	}
}

void ShaderManager::UpdateVolumetricLight(ShaderConstants& ShaderConst, TESWeather* currentWeather, TESWeather* previousWeather, float weatherPercent, float dayPercent) {
	SettingsVolumetricLightStruct* currentSettings = TheSettingManager->GetSettingsVolumetricLight(((TESWeatherEx*)currentWeather)->EditorName);
	SettingsVolumetricLightStruct* previousSettings;

	ShaderConstants::VolumetricLightStruct currentValues;
	ShaderConstants::VolumetricLightStruct previousValues;

	if (currentSettings == NULL) {
		currentSettings = TheSettingManager->GetSettingsVolumetricLight("Default");
	}

	if (!TheShaderManager->modifiersInitialzed) {
		TheShaderManager->SetVolumetricLightModifiers(currentSettings);
		TheShaderManager->previousModifier = TheShaderManager->currentModifier;
		TheShaderManager->previousFogHeight = TheShaderManager->currentFogHeight;
		TheShaderManager->previousAccumDistance = TheShaderManager->currentAccumDistance;
		TheShaderManager->modifiersInitialzed = true;
	}

	if (previousWeather == NULL) {
		previousSettings = TheSettingManager->GetSettingsVolumetricLight(((TESWeatherEx*)currentWeather)->EditorName);
		TheShaderManager->modifiersSet = false;
	}
	else {
		//we suspect the weather has changed, here we can determine the random modifier of the next (current) weather
		//set the random Modifer before first transition
		//if modifiers not set
		if (!TheShaderManager->modifiersSet) {
			TheShaderManager->SetVolumetricLightModifiers(currentSettings);
		}
		previousSettings = TheSettingManager->GetSettingsVolumetricLight(((TESWeatherEx*)previousWeather)->EditorName);
	}

	if (previousSettings == NULL) {
		previousSettings = TheSettingManager->GetSettingsVolumetricLight("Default");
	}

	if (ShaderConst.DayPhase < 2) {
		//sunrise -> midday
		currentValues.data1.x = std::lerp(currentSettings->accumSunriseR, currentSettings->accumMiddayR, ShaderConst.SunAmount.y);
		currentValues.data1.y = std::lerp(currentSettings->accumSunriseG, currentSettings->accumMiddayG, ShaderConst.SunAmount.y);
		currentValues.data1.z = std::lerp(currentSettings->accumSunriseB, currentSettings->accumMiddayB, ShaderConst.SunAmount.y);
		currentValues.data2.x = std::lerp(currentSettings->baseSunriseR, currentSettings->baseMiddayR, ShaderConst.SunAmount.y);
		currentValues.data2.y = std::lerp(currentSettings->baseSunriseG, currentSettings->baseMiddayG, ShaderConst.SunAmount.y);
		currentValues.data2.z = std::lerp(currentSettings->baseSunriseB, currentSettings->baseMiddayB, ShaderConst.SunAmount.y);
		currentValues.data6.x = currentSettings->sunScatterR;
		currentValues.data6.y = currentSettings->sunScatterG;
		currentValues.data6.z = currentSettings->sunScatterB;

		previousValues.data1.x = std::lerp(previousSettings->accumSunriseR, previousSettings->accumMiddayR, ShaderConst.SunAmount.y);
		previousValues.data1.y = std::lerp(previousSettings->accumSunriseG, previousSettings->accumMiddayG, ShaderConst.SunAmount.y);
		previousValues.data1.z = std::lerp(previousSettings->accumSunriseB, previousSettings->accumMiddayB, ShaderConst.SunAmount.y);
		previousValues.data2.x = std::lerp(previousSettings->baseSunriseR, previousSettings->baseMiddayR, ShaderConst.SunAmount.y);
		previousValues.data2.y = std::lerp(previousSettings->baseSunriseG, previousSettings->baseMiddayG, ShaderConst.SunAmount.y);
		previousValues.data2.z = std::lerp(previousSettings->baseSunriseB, previousSettings->baseMiddayB, ShaderConst.SunAmount.y);
		previousValues.data6.x = previousSettings->sunScatterR;
		previousValues.data6.y = previousSettings->sunScatterG;
		previousValues.data6.z = previousSettings->sunScatterB;
	}
	else {
		//midday -> sunset
		currentValues.data1.x = std::lerp(currentSettings->accumMiddayR, currentSettings->accumSunsetR, ShaderConst.SunAmount.z);
		currentValues.data1.y = std::lerp(currentSettings->accumMiddayG, currentSettings->accumSunsetG, ShaderConst.SunAmount.z);
		currentValues.data1.z = std::lerp(currentSettings->accumMiddayB, currentSettings->accumSunsetB, ShaderConst.SunAmount.z);
		currentValues.data2.x = std::lerp(currentSettings->baseMiddayR, currentSettings->baseSunsetR, ShaderConst.SunAmount.z);
		currentValues.data2.y = std::lerp(currentSettings->baseMiddayG, currentSettings->baseSunsetG, ShaderConst.SunAmount.z);
		currentValues.data2.z = std::lerp(currentSettings->baseMiddayB, currentSettings->baseSunsetB, ShaderConst.SunAmount.z);
		currentValues.data6.x = currentSettings->sunScatterR;
		currentValues.data6.y = currentSettings->sunScatterG;
		currentValues.data6.z = currentSettings->sunScatterB;

		previousValues.data1.x = std::lerp(previousSettings->accumMiddayR, previousSettings->accumSunsetR, ShaderConst.SunAmount.z);
		previousValues.data1.y = std::lerp(previousSettings->accumMiddayG, previousSettings->accumSunsetG, ShaderConst.SunAmount.z);
		previousValues.data1.z = std::lerp(previousSettings->accumMiddayB, previousSettings->accumSunsetB, ShaderConst.SunAmount.z);
		previousValues.data2.x = std::lerp(previousSettings->baseMiddayR, previousSettings->baseSunsetR, ShaderConst.SunAmount.z);
		previousValues.data2.y = std::lerp(previousSettings->baseMiddayG, previousSettings->baseSunsetG, ShaderConst.SunAmount.z);
		previousValues.data2.z = std::lerp(previousSettings->baseMiddayB, previousSettings->baseSunsetB, ShaderConst.SunAmount.z);
		previousValues.data6.x = previousSettings->sunScatterR;
		previousValues.data6.y = previousSettings->sunScatterG;
		previousValues.data6.z = previousSettings->sunScatterB;
	}

	if (dayPercent < 1.0f) {
		float phaseModifier = 1;
		if (ShaderConst.MoonPhaseCoeff == 0.0f) {
			phaseModifier = 0;
		}
		currentValues.data1.x = std::lerp(currentSettings->accumNightR * phaseModifier, currentValues.data1.x, dayPercent);
		currentValues.data1.y = std::lerp(currentSettings->accumNightG * phaseModifier, currentValues.data1.y, dayPercent);
		currentValues.data1.z = std::lerp(currentSettings->accumNightB * phaseModifier, currentValues.data1.z, dayPercent);
		currentValues.data2.x = std::lerp(currentSettings->baseNightR, currentValues.data2.x, dayPercent);
		currentValues.data2.y = std::lerp(currentSettings->baseNightG, currentValues.data2.y, dayPercent);
		currentValues.data2.z = std::lerp(currentSettings->baseNightB, currentValues.data2.z, dayPercent);
		currentValues.data6.x = std::lerp(0.3f * phaseModifier, currentSettings->sunScatterR, dayPercent);
		currentValues.data6.y = std::lerp(0.3f * phaseModifier, currentSettings->sunScatterG, dayPercent);
		currentValues.data6.z = std::lerp(0.3f * phaseModifier, currentSettings->sunScatterB, dayPercent);

		previousValues.data1.x = std::lerp(previousSettings->accumNightR * phaseModifier, previousValues.data1.x, dayPercent);
		previousValues.data1.y = std::lerp(previousSettings->accumNightG * phaseModifier, previousValues.data1.y, dayPercent);
		previousValues.data1.z = std::lerp(previousSettings->accumNightB * phaseModifier, previousValues.data1.z, dayPercent);
		previousValues.data2.x = std::lerp(previousSettings->baseNightR, previousValues.data2.x, dayPercent);
		previousValues.data2.y = std::lerp(previousSettings->baseNightG, previousValues.data2.y, dayPercent);
		previousValues.data2.z = std::lerp(previousSettings->baseNightB, previousValues.data2.z, dayPercent);
		previousValues.data6.x = std::lerp(0.3f * phaseModifier, previousSettings->sunScatterR, dayPercent);
		previousValues.data6.y = std::lerp(0.3f * phaseModifier, previousSettings->sunScatterG, dayPercent);
		previousValues.data6.z = std::lerp(0.3f * phaseModifier, previousSettings->sunScatterB, dayPercent);
	}

	ShaderConst.VolumetricLight.data1.x = std::lerp(previousValues.data1.x * TheShaderManager->previousModifier, currentValues.data1.x * TheShaderManager->currentModifier, weatherPercent);
	ShaderConst.VolumetricLight.data1.y = std::lerp(previousValues.data1.y * TheShaderManager->previousModifier, currentValues.data1.y * TheShaderManager->currentModifier, weatherPercent);
	ShaderConst.VolumetricLight.data1.z = std::lerp(previousValues.data1.z * TheShaderManager->previousModifier, currentValues.data1.z * TheShaderManager->currentModifier, weatherPercent);
	ShaderConst.VolumetricLight.data1.w = std::lerp(TheShaderManager->previousAccumDistance, TheShaderManager->currentAccumDistance, weatherPercent);

	ShaderConst.VolumetricLight.data2.x = std::lerp(previousValues.data2.x * TheShaderManager->previousModifier, currentValues.data2.x * TheShaderManager->currentModifier, weatherPercent);
	ShaderConst.VolumetricLight.data2.y = std::lerp(previousValues.data2.y * TheShaderManager->previousModifier, currentValues.data2.y * TheShaderManager->currentModifier, weatherPercent);
	ShaderConst.VolumetricLight.data2.z = std::lerp(previousValues.data2.z * TheShaderManager->previousModifier, currentValues.data2.z * TheShaderManager->currentModifier, weatherPercent);
	ShaderConst.VolumetricLight.data2.w = std::lerp(previousSettings->baseDistance, currentSettings->baseDistance, weatherPercent);

	ShaderConst.VolumetricLight.data3.y = std::lerp(previousSettings->accumCutOff, currentSettings->accumCutOff, weatherPercent);
	ShaderConst.VolumetricLight.data3.z = std::lerp(previousSettings->blurDistance * (1.0f/TheShaderManager->previousModifier), currentSettings->blurDistance * (1.0f/TheShaderManager->currentModifier), weatherPercent);
	ShaderConst.VolumetricLight.data3.w = std::lerp(TheShaderManager->previousFogHeight, TheShaderManager->currentFogHeight, weatherPercent);

	ShaderConst.VolumetricLight.data4.y = std::lerp(previousSettings->animatedFogToggle, currentSettings->animatedFogToggle, weatherPercent);
	ShaderConst.VolumetricLight.data4.z = TheRenderManager->width;
	ShaderConst.VolumetricLight.data4.w = TheRenderManager->height;

	ShaderConst.VolumetricLight.data5.w = std::lerp(previousSettings->fogPower, currentSettings->fogPower, weatherPercent);

	ShaderConst.VolumetricLight.data6.x = std::lerp(previousValues.data6.x, currentValues.data6.x, weatherPercent);
	ShaderConst.VolumetricLight.data6.y = std::lerp(previousValues.data6.y, currentValues.data6.y, weatherPercent);
	ShaderConst.VolumetricLight.data6.z = std::lerp(previousValues.data6.z, currentValues.data6.z, weatherPercent);
	ShaderConst.VolumetricLight.data6.w = dayPercent;

	if (weatherPercent > 0.5f) {
		ShaderConst.VolumetricLight.data4.y = std::lerp(0.0f, currentSettings->animatedFogToggle * 2, weatherPercent - .5);
		ShaderConst.VolumetricLight.data5.x = TheShaderManager->currentWind.x;
		ShaderConst.VolumetricLight.data5.y = TheShaderManager->currentWind.y;
		ShaderConst.VolumetricLight.data5.z = TheShaderManager->currentWind.z;
	} else {
		ShaderConst.VolumetricLight.data4.y = std::lerp(previousSettings->animatedFogToggle, 0.0f, weatherPercent * 2);
	}
}

void ShaderManager::UpdateSpecular(ShaderConstants& ShaderConst) {
	ShaderConst.Specular.SpecularData.x = TheSettingManager->SettingsSpecular.SpecularPower;
	ShaderConst.Specular.SpecularData.y = TheSettingManager->SettingsSpecular.FresnelPowerActors;
	ShaderConst.Specular.SpecularData.z = TheSettingManager->SettingsSpecular.FresnelPowerObjects;
}

void ShaderManager::UpdateConstants() {

	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_UpdateConstants);

	bool IsThirdPersonView;
	Sky* WorldSky = Tes->sky;
	NiNode* SunRoot = WorldSky->sun->RootNode;
	Sun* Sun = WorldSky->sun;
	Moon* Masser = WorldSky->masserMoon;
	Moon* Secunda = WorldSky->secundaMoon;
	TESClimate* currentClimate = WorldSky->firstClimate;
	TESWeather* currentWeather = WorldSky->firstWeather;
	TESWeather* previousWeather = WorldSky->secondWeather;
	TESObjectCELL* currentCell = Player->parentCell;
	TESWorldSpace* currentWorldSpace = Player->GetWorldSpace();
	bool isExteriorLike = Player->IsExteriorLike();
	TESRegion* currentRegion = Player->GetRegion();
	float weatherPercent = WorldSky->weatherPercent;
	float dayPercent = 0;
	float lastGameTime = ShaderConst.GameTime.y;
	TheSettingManager->SettingsShadows.Exteriors.UseIntervalUpdate = UseIntervalUpdate;

	if (currentCell != previousCell) {
		LoadEffectSettings();
		previousCell = currentCell;
	}

	if (CurrentBlend != previousBlend) {
		LoadEffectSettings();
		previousBlend = CurrentBlend;
	}

	IsThirdPersonView = Player->IsThirdPersonView(TheSettingManager->SettingsMain.CameraMode.Enabled, TheRenderManager->FirstPersonView);
	TheRenderManager->GetSceneCameraData();

	//Is fully init'd after two frame passes due to time calculations with sundir
	if (!isFullyInitialized && isExteriorLike) {
		if (InitFrameCount < InitFrameTarget) {
			InitFrameCount++;
		}
		else {
			TheShadowManager->ResetIntervals();
			isFullyInitialized = true;
		}
	}

	UpdateGameTime(ShaderConst);

	if (currentCell) {
		UpdateCelestialDirections(ShaderConst, SunRoot, Masser, Secunda, currentClimate, lastGameTime);
		UpdateMoonPhaseCoeff(ShaderConst, currentClimate, GameDay);

		if (isExteriorLike)
			dayPercent = UpdateExteriorLighting(ShaderConst, currentWeather, previousWeather, weatherPercent);
		else
			UpdateInteriorLighting(ShaderConst, currentCell, InteriorLighting, isFullyInitialized, InitFrameCount);

		if (TheSettingManager->SettingsMain.Shaders.Water || TheSettingManager->SettingsMain.Effects.Underwater)
			UpdateWater(ShaderConst, currentCell, sws);
		if (TheSettingManager->SettingsMain.Effects.SnowAccumulation)
			UpdateSnowAccumulation(ShaderConst, currentWeather, previousWeather);
		if (TheSettingManager->SettingsMain.Effects.WetWorld)
			UpdateWetWorld(ShaderConst, currentWeather, previousWeather, weatherPercent);
		if (TheSettingManager->SettingsMain.Effects.Precipitations)
			UpdatePrecipitation(ShaderConst, currentWeather, previousWeather, weatherPercent);
		if (TheSettingManager->SettingsMain.Shaders.Grass)
			UpdateGrass(ShaderConst, GrassCollisionActors, GrassCollisionActorCount);
	}

	if (TheSettingManager->SettingsMain.Shaders.POM)     UpdatePOM(ShaderConst);
	if (TheSettingManager->SettingsMain.Shaders.Terrain) UpdateTerrain(ShaderConst);
	if (TheSettingManager->SettingsMain.Shaders.Skin)    UpdateSkin(ShaderConst);
	if (TheSettingManager->SettingsMain.Effects.GodRays)     UpdateGodRays(ShaderConst);
	if (TheSettingManager->SettingsMain.Effects.KhajiitRays) UpdateKhajiitRays(ShaderConst);
	if (TheSettingManager->SettingsMain.Effects.AmbientOcclusion) UpdateAmbientOcclusion(ShaderConst, sas);
	if (TheSettingManager->SettingsMain.Effects.Bloom)    UpdateBloom(ShaderConst, sbs);
	if (TheSettingManager->SettingsMain.Effects.Coloring) UpdateColoring(ShaderConst, scs);
	if (TheSettingManager->SettingsMain.Effects.DepthOfField) UpdateDepthOfField(ShaderConst, IsThirdPersonView);
	if (TheSettingManager->SettingsMain.Effects.Cinema)   UpdateCinema(ShaderConst);
	if (TheSettingManager->SettingsMain.Effects.MotionBlur) UpdateMotionBlur(ShaderConst, IsThirdPersonView);
	if (TheSettingManager->SettingsMain.Effects.Sharpening) UpdateSharpening(ShaderConst);
	if (TheSettingManager->SettingsMain.Effects.VolumetricFog) UpdateVolumetricFog(ShaderConst, weatherPercent);

	UpdateTAA(ShaderConst, jitterIndex, jitterPattern);

	if (TheSettingManager->SettingsMain.Effects.VolumetricLight)
		UpdateVolumetricLight(ShaderConst, currentWeather, previousWeather, weatherPercent, dayPercent);

	UpdateSpecular(ShaderConst);
}


void ShaderManager::SetVolumetricLightModifiers(SettingsVolumetricLightStruct* currentSettings) {
	srand(TimeGlobals::GetGameTime());
	previousModifier = currentModifier;
	previousWind.x = currentWind.x;
	previousWind.y = currentWind.y;
	previousWind.z = currentWind.z;

	currentModifier = (rand() % (int)currentSettings->randomizer + 1) / currentSettings->randomizer;
	currentWind.x = ((rand() % 10 + 1) / 10.0f) * ((rand() % 2) + 1) - 1;
	currentWind.y = ((rand() % 2) + 1) == 1 ? -1 : 1;
	currentWind.z = ((rand() % 10 + 1) / 10.0f);

	previousFogHeight = currentFogHeight;
	int minHeight = (int)currentSettings->accumHeightMin;
	currentFogHeight = (rand() + rand() + rand()) % ((int)currentSettings->accumHeightMax - minHeight) + minHeight;

	previousAccumDistance = currentAccumDistance;
	int minDistance = (int)currentSettings->accumDistanceMin;
	currentAccumDistance = (rand() * ((rand() % 10 + 1))) % ((int)currentSettings->accumDistanceMax - minDistance) + minDistance;

	modifiersSet = true;
}

void ShaderManager::BeginScene() {

	// Fires once per SCENE, not once per frame: the off-screen renders that follow
	// the main pass (the water reflection map among them) each get their own.
	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_BeginScene);
	FrameProfiler::Count(FrameProfiler::Cnt_Scenes);

	if (MenuManager->IsActive(Menu::MenuType::kMenuType_BigFour)) {
		TheShaderManager->ShaderConst.Jitter.x = 0.0f;
		TheShaderManager->ShaderConst.Jitter.y = 0.0f;
	}

	jitterSet = false;

	if (ShaderConst.OverrideVanillaDirectionalLight) {
		Tes->niDirectionalLight->m_direction.x = TheShaderManager->ShaderConst.DirectionalLight.x;
		Tes->niDirectionalLight->m_direction.y = TheShaderManager->ShaderConst.DirectionalLight.y;
		Tes->niDirectionalLight->m_direction.z = TheShaderManager->ShaderConst.DirectionalLight.z;
	}

	RenderedBufferFilled = false;
	DepthBufferFilled = false;
	PreWaterDepthBufferFilled = false;
}

void ShaderManager::CreateShader(const char* Name) {

#if defined(NEWVEGAS)
	BSShader* (__cdecl * GetShaderDefinition)(UInt32) = (BSShader* (__cdecl *)(UInt32))0x00B55560;
	if (!strcmp(Name, "Water")) {
		WaterShader* WS = (WaterShader*)GetShaderDefinition(17);
		for each (NiD3DVertexShader* VS in WS->Vertex) LoadShader(VS);
		for each (NiD3DPixelShader* PS in WS->Pixel) LoadShader(PS);
		LoadShader(WaterHeightMapVertexShader);
		LoadShader(WaterHeightMapPixelShader);
	}
#elif defined(OBLIVION)
	NiD3DVertexShaderEx** PrecipitationVertexShaders = (NiD3DVertexShaderEx**)0x00B466E0;
	NiD3DPixelShaderEx** PrecipitationPixelShaders = (NiD3DPixelShaderEx**)0x00B46708;
	NiD3DVertexShaderEx** ShadowLightVertexShaders = (NiD3DVertexShaderEx**)0x00B4528C;
	NiD3DPixelShaderEx** ShadowLightPixelShaders = (NiD3DPixelShaderEx**)0x00B45088;
	NiD3DPixelShaderEx** SM3PixelShaders = (NiD3DPixelShaderEx**)0x00B46ED8;
	NiD3DPixelShaderEx** SM3LLPixelShaders = (NiD3DPixelShaderEx**)0x00B46C20;
	NiD3DVertexShaderEx** SM3VertexShaders = (NiD3DVertexShaderEx**)0x00B47288;

	if (!strcmp(Name, "Water")) {
		WaterShader* WS = (WaterShader*)GetShaderDefinition(17)->Shader;
		for each (NiD3DVertexShader* VS in WS->Vertex) LoadShader(VS);
		for each (NiD3DPixelShader* PS in WS->Pixel) LoadShader(PS);
		WaterShaderHeightMap* WSHM = (WaterShaderHeightMap*)GetShaderDefinition(19)->Shader;
		LoadShader(WSHM->Vertex);
		for each (NiD3DPixelShader* PS in WSHM->Pixel) LoadShader(PS);
		WaterShaderDisplacement* WSD = (WaterShaderDisplacement*)GetShaderDefinition(20)->Shader;
		for each (NiD3DVertexShader* VS in WSD->Vertex) LoadShader(VS);
		for each (NiD3DPixelShader* PS in WSD->Pixel) LoadShader(PS);
	}
	else if (!strcmp(Name, "Grass")) {
		TallGrassShader* TGS = (TallGrassShader*)GetShaderDefinition(2)->Shader;
		for each (NiD3DVertexShader* VS in TGS->Vertex2) LoadShader(VS);
		for each (NiD3DPixelShader* PS in TGS->Pixel2) LoadShader(PS);
	}
	else if (!strcmp(Name, "Precipitations")) {
		for (int i = 0; i < 4; i++) LoadShader(PrecipitationVertexShaders[i]);
		for (int i = 0; i < 2; i++) LoadShader(PrecipitationPixelShaders[i]);
	}
	else if (!strcmp(Name, "POM")) {
		ParallaxShader* PRS = (ParallaxShader*)GetShaderDefinition(15)->Shader;
		for each (NiD3DVertexShader* VS in PRS->Vertex) LoadShader(VS);
		for each (NiD3DPixelShader* PS in PRS->Pixel) LoadShader(PS);
	}
	else if (!strcmp(Name, "Skin")) {
		SkinShader* SS = (SkinShader*)GetShaderDefinition(14)->Shader;
		for each (NiD3DVertexShader * VS in SS->Vertex) LoadShader(VS);
		for each (NiD3DPixelShader * PS in SS->Pixel) LoadShader(PS);
	}
	else if (!strcmp(Name, "Terrain")) {
		for (int i = 0; i < 130; i++) {
			NiD3DVertexShaderEx* VS = ShadowLightVertexShaders[i];
			if (VS && strstr(TerrainShaders, VS->ShaderName)) {
				LoadShader(VS);
			}
		}
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && strstr(TerrainShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}
	}
	else if (!strcmp(Name, "Blood")) {
		GeometryDecalShader* GDS = (GeometryDecalShader*)GetShaderDefinition(16)->Shader;
		for each (NiD3DVertexShader* VS in GDS->Vertex) LoadShader(VS);
		for each (NiD3DPixelShader* PS in GDS->Pixel) LoadShader(PS);
	}
	else if (!strcmp(Name, "InteriorShadows")) {
		for (int i = 0; i < 130; i++) {
			NiD3DVertexShaderEx* VS = ShadowLightVertexShaders[i];
			if (VS && strstr(InteriorShadowShaders, VS->ShaderName)) {
				LoadShader(VS);
			}
		}
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && strstr(InteriorShadowShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}

		for (int i = 0; i < 39; i++) {
			NiD3DPixelShaderEx* PS = SM3PixelShaders[i];
			if (PS && strstr(InteriorShadowShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}

		for (int i = 0; i < 20; i++) {
			NiD3DPixelShaderEx* PS = SM3LLPixelShaders[i];
			if (PS && strstr(InteriorShadowShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}

		for (int i = 0; i < 32; i++) {
			NiD3DVertexShaderEx* VS = SM3VertexShaders[i];
			if (VS && strstr(InteriorShadowShaders, VS->ShaderName)) {
				LoadShader(VS);
			}
		}

		ParallaxShader* PRS = (ParallaxShader*)GetShaderDefinition(15)->Shader;
		for each (NiD3DPixelShaderEx * PS in PRS->Pixel) {
			if (PS && strstr(InteriorShadowShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}
	}
	else if (!strcmp(Name, "ExteriorExtraShaders")) {
		for (int i = 0; i < 20; i++) {
			NiD3DPixelShaderEx* PS = SM3LLPixelShaders[i];
			if (PS && strstr(InteriorShadowShaders, PS->ShaderName)) {
				LoadShader(PS, "Exterior");
			}
		}
		for (int i = 0; i < 32; i++) {
			NiD3DVertexShaderEx* VS = SM3VertexShaders[i];
			if (VS && strstr(InteriorShadowShaders, VS->ShaderName)) {
				LoadShader(VS, "Exterior");
			}
		}
	}
	else if (!strcmp(Name, "ExteriorPom")) {
		ParallaxShader* PRS = (ParallaxShader*)GetShaderDefinition(15)->Shader;
		for each (NiD3DPixelShaderEx * PS in PRS->Pixel) {
			if (PS && strstr(ExteriorPom, PS->ShaderName)) {
				LoadShader(PS, "Exterior");
			}
		}
	}
	else if (!strcmp(Name, "ExteriorDialogActive")) {
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && strstr(ExteriorDialogShaders, PS->ShaderName)) {
				LoadShader(PS,"Dialog");
			}
		}

		SkinShader* SS = (SkinShader*)GetShaderDefinition(14)->Shader;
		for each (NiD3DPixelShaderEx * PS in SS->Pixel) {
			if (PS && strstr(ExteriorDialogShaders, PS->ShaderName)) {
				LoadShader(PS, "Dialog");
			}
		}
	}
	else if (!strcmp(Name, "ExteriorDialogInactive")) {
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && strstr(ExteriorDialogShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}

		SkinShader* SS = (SkinShader*)GetShaderDefinition(14)->Shader;
		for each (NiD3DPixelShaderEx * PS in SS->Pixel) {
			if (PS && strstr(ExteriorDialogShaders, PS->ShaderName)) {
				LoadShader(PS);
			}
		}
	}
#elif defined(SKYRIM)
	if (!strcmp(Name, "Water")) {
		for each (NiD3DVertexShader* VS in WaterVertexShaders) LoadShader(VS);
		for each (NiD3DPixelShader* PS in WaterPixelShaders) LoadShader(PS);
	}
#endif

}

void ShaderManager::LoadShader(NiD3DVertexShader* Shader, const char* DirPostFix) {
	
	ShaderRecord* ShaderProg = new ShaderRecord();
	NiD3DVertexShaderEx* VertexShader = (NiD3DVertexShaderEx*)Shader;

	if (ShaderProg->LoadShader(VertexShader->ShaderName, DirPostFix)) {
		VertexShader->ShaderProg = ShaderProg;
		VertexShader->ShaderHandleBackup = VertexShader->ShaderHandle;
		TheRenderManager->device->CreateVertexShader((const DWORD*)ShaderProg->Function, &VertexShader->ShaderHandle);
	}
	else {
		delete ShaderProg;
	}

}

void ShaderManager::LoadShader(NiD3DPixelShader* Shader, const char* DirPostFix) {

	ShaderRecord* ShaderProg = new ShaderRecord();
	NiD3DPixelShaderEx* PixelShader = (NiD3DPixelShaderEx*)Shader;

	if (ShaderProg->LoadShader(PixelShader->ShaderName, DirPostFix)) {
		PixelShader->ShaderProg = ShaderProg;
		PixelShader->ShaderHandleBackup = PixelShader->ShaderHandle;
		TheRenderManager->device->CreatePixelShader((const DWORD*)ShaderProg->Function, &PixelShader->ShaderHandle);
	}
	else {
		delete ShaderProg;
	}

}

void ShaderManager::DisposeShader(const char* Name) {

#if defined(NEWVEGAS)
	BSShader* (__cdecl * GetShader)(UInt32) = (BSShader* (__cdecl *)(UInt32))0x00B55560;
	if (!strcmp(Name, "Water")) {
		WaterShader* WS = (WaterShader*)GetShader(17);
		for each (NiD3DVertexShaderEx* VS in WS->Vertex) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in WS->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
		NiD3DVertexShaderEx* VS = (NiD3DVertexShaderEx*)WaterHeightMapVertexShader;
		if (VS->ShaderProg) {
			VS->ShaderHandle = VS->ShaderHandleBackup;
			delete VS->ShaderProg; VS->ShaderProg = NULL;
		}
		NiD3DPixelShaderEx* PS = (NiD3DPixelShaderEx*)WaterHeightMapPixelShader;
		if (PS->ShaderProg) {
			PS->ShaderHandle = PS->ShaderHandleBackup;
			delete PS->ShaderProg; PS->ShaderProg = NULL;
		}
	}
#elif defined(OBLIVION)
	ShaderDefinition* (__cdecl * GetShaderDefinition)(UInt32) = (ShaderDefinition* (__cdecl *)(UInt32))0x007B4290;
	NiD3DVertexShaderEx** ShadowLightVertexShaders = (NiD3DVertexShaderEx**)0x00B4528C;
	NiD3DPixelShaderEx** ShadowLightPixelShaders = (NiD3DPixelShaderEx**)0x00B45088;
	NiD3DPixelShaderEx** SM3PixelShaders = (NiD3DPixelShaderEx**)0x00B46ED8;//SM3 psos + 39
	NiD3DPixelShaderEx** SM3LLPixelShaders = (NiD3DPixelShaderEx**)0x00B46C20; //SM3LL psos + 20
	NiD3DVertexShaderEx** SM3VertexShaders = (NiD3DVertexShaderEx**)0x00B47288;//SM3* vsos + 32

	if (!strcmp(Name, "Water")) {
		WaterShader* WS = (WaterShader*)GetShaderDefinition(17)->Shader;
		for each (NiD3DVertexShaderEx* VS in WS->Vertex) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in WS->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
		WaterShaderHeightMap* WSHM = (WaterShaderHeightMap*)GetShaderDefinition(19)->Shader;
		NiD3DVertexShaderEx* VS = (NiD3DVertexShaderEx*)WSHM->Vertex;
		if (VS->ShaderProg) {
			VS->ShaderHandle = VS->ShaderHandleBackup;
			delete VS->ShaderProg; VS->ShaderProg = NULL;
		}
		for each (NiD3DPixelShaderEx* PS in WSHM->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
		WaterShaderDisplacement* WSD = (WaterShaderDisplacement*)GetShaderDefinition(20)->Shader;
		for each (NiD3DVertexShaderEx* VS in WSD->Vertex) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in WSD->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "Grass")) {
		TallGrassShader* TGS = (TallGrassShader*)GetShaderDefinition(2)->Shader;
		for each (NiD3DVertexShaderEx* VS in TGS->Vertex2) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in TGS->Pixel2) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "POM")) {
		ParallaxShader* PRS = (ParallaxShader*)GetShaderDefinition(15)->Shader;
		for each (NiD3DVertexShaderEx* VS in PRS->Vertex) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in PRS->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "Skin")) {
		SkinShader* SS = (SkinShader*)GetShaderDefinition(14)->Shader;
		for each (NiD3DVertexShaderEx* VS in SS->Vertex) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in SS->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "Terrain")) {
		for (int i = 0; i < 130; i++) {
			NiD3DVertexShaderEx* VS = ShadowLightVertexShaders[i];
			if (VS && VS->ShaderProg && strstr(TerrainShaders, VS->ShaderName)) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && PS->ShaderProg && strstr(TerrainShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "Blood")) {
		GeometryDecalShader* GDS = (GeometryDecalShader*)GetShaderDefinition(16)->Shader;
		for each (NiD3DVertexShaderEx* VS in GDS->Vertex) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in GDS->Pixel) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "InteriorShadows")) {
		for (int i = 0; i < 130; i++) {
			NiD3DVertexShaderEx* VS = ShadowLightVertexShaders[i];
			if (VS && VS->ShaderProg && strstr(InteriorShadowShaders, VS->ShaderName)) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && PS->ShaderProg && strstr(InteriorShadowShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}

		for (int i = 0; i < 39; i++) {
			NiD3DPixelShaderEx* PS = SM3PixelShaders[i];
			if (PS && PS->ShaderProg && strstr(InteriorShadowShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}

		for (int i = 0; i < 20; i++) {
			NiD3DPixelShaderEx* PS = SM3LLPixelShaders[i];
			if (PS && PS->ShaderProg && strstr(InteriorShadowShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
		for (int i = 0; i < 32; i++) {
			NiD3DVertexShaderEx* VS = SM3VertexShaders[i];
			if (VS && VS->ShaderProg && strstr(InteriorShadowShaders, VS->ShaderName)) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}

		ParallaxShader* PRS = (ParallaxShader*)GetShaderDefinition(15)->Shader;
		for each (NiD3DPixelShaderEx * PS in PRS->Pixel) {
			if (PS && PS->ShaderProg && strstr(InteriorShadowShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
	else if (!strcmp(Name, "ExteriorDialog")) {
		for (int i = 0; i < 130; i++) {
			NiD3DPixelShaderEx* PS = ShadowLightPixelShaders[i];
			if (PS && PS->ShaderProg && strstr(ExteriorDialogShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}

		SkinShader* SS = (SkinShader*)GetShaderDefinition(14)->Shader;
		for each (NiD3DPixelShaderEx * PS in SS->Pixel) {
			if (PS && PS->ShaderProg && strstr(ExteriorDialogShaders, PS->ShaderName)) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
#elif defined(SKYRIM)
	if (!strcmp(Name, "Water")) {
		for each (NiD3DVertexShaderEx* VS in WaterVertexShaders) {
			if (VS->ShaderProg) {
				VS->ShaderHandle = VS->ShaderHandleBackup;
				delete VS->ShaderProg; VS->ShaderProg = NULL;
			}
		}
		for each (NiD3DPixelShaderEx* PS in WaterPixelShaders) {
			if (PS->ShaderProg) {
				PS->ShaderHandle = PS->ShaderHandleBackup;
				delete PS->ShaderProg; PS->ShaderProg = NULL;
			}
		}
	}
#endif

}

void ShaderManager::CreateEffect(EffectRecordType EffectType) {

	char Filename[MAX_PATH];

	strcpy(Filename, EffectsPath);
	switch (EffectType) {
		case EffectRecordType_Underwater:
			strcat(Filename, "Water\\Underwater.fx");
			UnderwaterEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.Underwater = LoadEffect(UnderwaterEffect, Filename, NULL);
			break;
		case EffectRecordType_WaterLens:
			strcat(Filename, "Water\\WaterLens.fx");
			WaterLensEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.WaterLens = LoadEffect(WaterLensEffect, Filename, NULL);
			break;
		case EffectRecordType_GodRays:
			strcat(Filename, "GodRays\\GodRays.fx");
			GodRaysEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.GodRays = LoadEffect(GodRaysEffect, Filename, NULL);
			break;
		case EffectRecordType_KhajiitRays:
			char mFilename[MAX_PATH];
			strcpy(mFilename, EffectsPath);
			strcat(mFilename, "KhajiitRays\\MasserRays.fx");
			char sFilename[MAX_PATH];
			strcpy(sFilename, EffectsPath);
			strcat(sFilename, "KhajiitRays\\SecundaRays.fx");
			MasserRaysEffect = new EffectRecord();
			SecundaRaysEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.KhajiitRays = LoadEffect(MasserRaysEffect, mFilename, NULL) && LoadEffect(SecundaRaysEffect, sFilename, NULL);
			break;
		case EffectRecordType_DepthOfField:
			strcat(Filename, "DepthOfField\\DepthOfField.fx");
			DepthOfFieldEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.DepthOfField = LoadEffect(DepthOfFieldEffect, Filename, NULL);
			break;
		case EffectRecordType_AmbientOcclusion:
			strcat(Filename, "AmbientOcclusion\\AmbientOcclusion.fx");
			AmbientOcclusionEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.AmbientOcclusion = LoadEffect(AmbientOcclusionEffect, Filename, NULL);
			break;
		case EffectRecordType_Coloring:
			strcat(Filename, "Coloring\\Coloring.fx");
			ColoringEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.Coloring = LoadEffect(ColoringEffect, Filename, NULL);
			break;
		case EffectRecordType_Cinema:
			strcat(Filename, "Cinema\\Cinema.fx");
			CinemaEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.Cinema = LoadEffect(CinemaEffect, Filename, NULL);
			break;
		case EffectRecordType_Bloom:
			strcat(Filename, "Bloom\\Bloom.fx");
			BloomEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.Bloom = LoadEffect(BloomEffect, Filename, NULL);
			break;
		case EffectRecordType_SnowAccumulation:
			strcat(Filename, "Precipitations\\SnowAccumulation.fx");
			SnowAccumulationEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.SnowAccumulation = LoadEffect(SnowAccumulationEffect, Filename, NULL);
			break;
		case EffectRecordType_SMAA:
			strcat(Filename, "SMAA\\SMAA.fx");
			SMAAEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.SMAA = LoadEffect(SMAAEffect, Filename, NULL);
			break;
		case EffectRecordType_TAA:
			strcat(Filename, "TAA\\TAA.fx");
			TAAEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.TAA = LoadEffect(TAAEffect, Filename, NULL);
			break;
		case EffectRecordType_MotionBlur:
			strcat(Filename, "MotionBlur\\MotionBlur.fx");
			MotionBlurEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.MotionBlur = LoadEffect(MotionBlurEffect, Filename, NULL);
			break;
		case EffectRecordType_WetWorld:
			strcat(Filename, "Precipitations\\WetWorld.fx");
			WetWorldEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.WetWorld = LoadEffect(WetWorldEffect, Filename, NULL);
			break;
		case EffectRecordType_Sharpening:
			strcat(Filename, "Sharpening\\Sharpening.fx");
			SharpeningEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.Sharpening = LoadEffect(SharpeningEffect, Filename, NULL);
			break;
		case EffectRecordType_VolumetricFog:
			strcat(Filename, "Fog\\VolumetricFog.fx");
			VolumetricFogEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.VolumetricFog = LoadEffect(VolumetricFogEffect, Filename, NULL);
			break;
		case EffectRecordType_VolumetricLight:
			strcat(Filename, "VolumetricLight\\VolumetricLight.fx");
			VolumetricLightEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.VolumetricLight = LoadEffect(VolumetricLightEffect, Filename, NULL);
			break;
		case EffectRecordType_Precipitations:
			strcat(Filename, "Precipitations\\Rain.fx");
			RainEffect = new EffectRecord();
			TheSettingManager->SettingsMain.Effects.Precipitations = LoadEffect(RainEffect, Filename, NULL);
			if (TheSettingManager->SettingsMain.Effects.Precipitations) {
				strcpy(Filename, EffectsPath);
				strcat(Filename, "Precipitations\\Snow.fx");
				SnowEffect = new EffectRecord();
				TheSettingManager->SettingsMain.Effects.Precipitations = LoadEffect(SnowEffect, Filename, NULL);
			}
			break;
		case EffectRecordType_ShadowsExteriors:
			strcat(Filename, "Shadows\\ShadowsExteriors.fx");
			ShadowsExteriorsEffect = new EffectRecord();
			TheSettingManager->SettingsShadows.Exteriors.UsePostProcessing = LoadEffect(ShadowsExteriorsEffect, Filename, NULL);
			break;
		case EffectRecordType_ShadowsPoint:
			strcat(Filename, "Shadows\\ShadowsPoint.fx");
			ShadowsPointEffect = new EffectRecord();
			TheSettingManager->SettingsShadows.Point.UsePostProcessing = LoadEffect(ShadowsPointEffect, Filename, NULL);
			break;
		case EffectRecordType_Extra:
			WIN32_FIND_DATAA File;
			HANDLE H;
			char* cFileName = NULL;
			EffectRecord* ExtraEffect = NULL;

			if (TheSettingManager->SettingsMain.Develop.CompileEffects)
				strcat(Filename, "ExtraEffects\\*.hlsl");
			else
				strcat(Filename, "ExtraEffects\\*.fx");
			H = FindFirstFileA((LPCSTR)Filename, &File);
			if (H != INVALID_HANDLE_VALUE) {
				cFileName = (char*)File.cFileName;
				if (TheSettingManager->SettingsMain.Develop.CompileEffects) File.cFileName[strlen(cFileName) - 5] = NULL;
				strcpy(Filename, EffectsPath);
				strcat(Filename, "ExtraEffects\\");
				strcat(Filename, cFileName);
				ExtraEffect = new EffectRecord();
				LoadEffect(ExtraEffect, Filename, cFileName);
				while (FindNextFileA(H, &File)) {
					cFileName = (char*)File.cFileName;
					if (TheSettingManager->SettingsMain.Develop.CompileEffects) File.cFileName[strlen(cFileName) - 5] = NULL;
					strcpy(Filename, EffectsPath);
					strcat(Filename, "ExtraEffects\\");
					strcat(Filename, cFileName);
					ExtraEffect = new EffectRecord();
					LoadEffect(ExtraEffect, Filename, cFileName);
				}
				FindClose(H);
			}
			break;
	}

}

bool ShaderManager::LoadEffect(EffectRecord* TheEffect, char* Filename, char* CustomEffectName) {

	bool IsLoaded = TheEffect->LoadEffect(Filename);

	if (IsLoaded) {
		if (CustomEffectName) {
			std::string Name = std::string(CustomEffectName).substr(0, strlen(CustomEffectName) - 3);
			TheEffect->ProfileName = Name;
			ExtraEffects[Name] = TheEffect;
		}
		else {
			const char* base = strrchr(Filename, '\\');
			TheEffect->ProfileName = base ? base + 1 : Filename; // file basename, e.g. "GodRays"
		}
	}
	else
		DisposeEffect(TheEffect);
	return IsLoaded;

}

void ShaderManager::DisposeEffect(EffectRecord* TheEffect) {

	if (TheEffect == AmbientOcclusionEffect) AmbientOcclusionEffect = NULL;
	else if (TheEffect == BloomEffect) BloomEffect = NULL;
	else if (TheEffect == CinemaEffect) CinemaEffect = NULL;
	else if (TheEffect == ColoringEffect) ColoringEffect = NULL;
	else if (TheEffect == DepthOfFieldEffect) DepthOfFieldEffect = NULL;
	else if (TheEffect == GodRaysEffect) GodRaysEffect = NULL;
	else if (TheEffect == MasserRaysEffect) MasserRaysEffect = NULL;
	else if (TheEffect == SecundaRaysEffect) SecundaRaysEffect = NULL;
	else if (TheEffect == MotionBlurEffect) MotionBlurEffect = NULL;
	else if (TheEffect == SMAAEffect) SMAAEffect = NULL;
	else if (TheEffect == TAAEffect) TAAEffect = NULL;
	else if (TheEffect == SnowAccumulationEffect) SnowAccumulationEffect = NULL;
	else if (TheEffect == UnderwaterEffect) UnderwaterEffect = NULL;
	else if (TheEffect == WaterLensEffect) WaterLensEffect = NULL;
	else if (TheEffect == WetWorldEffect) WetWorldEffect = NULL;
	else if (TheEffect == SharpeningEffect) SharpeningEffect = NULL;
	else if (TheEffect == VolumetricFogEffect) VolumetricFogEffect = NULL;
	else if (TheEffect == VolumetricLightEffect) VolumetricLightEffect = NULL;
	else if (TheEffect == RainEffect) RainEffect = NULL;
	else if (TheEffect == SnowEffect) SnowEffect = NULL;
	else if (TheEffect == ShadowsExteriorsEffect) ShadowsExteriorsEffect = NULL;
	else if (TheEffect == ShadowsPointEffect) ShadowsPointEffect = NULL;

	if (TheEffect) delete TheEffect;

}

void ShaderManager::ProfileBlitToSource(IDirect3DSurface9* RenderTarget) {
	EffCountBlit();
	TheRenderManager->device->StretchRect(RenderTarget, NULL, SourceSurface, NULL, D3DTEXF_NONE);
}

void ShaderManager::RenderEffects(IDirect3DSurface9* RenderTarget) {
	SettingsMainStruct::EffectsStruct* Effects = &TheSettingManager->SettingsMain.Effects;
	IDirect3DDevice9* Device = TheRenderManager->device;
	bool isExteriorLike = Player->IsExteriorLike();
	D3DXVECTOR4* SunDir = &TheShaderManager->ShaderConst.SunDir;

	EffectProfileChainBegin(Device);

	// Opt-in buffer rotation (Develop.EffectChainPingPong). Decided per chain rather than latched at
	// startup so the INI can be reloaded mid-session and the next chain simply picks the other path;
	// with it off, every call site below behaves exactly as it did before this existed.
	if (TheSettingManager->SettingsMain.Develop.EffectChainPingPong && RenderedTexture && PingTexture && EffectTexture)
		ChainBegin();

	if (Effects->WetWorld && isExteriorLike && ShaderConst.WetWorld.Data.x > 0.0f) {
		RunEffect(WetWorldEffect, Device, RenderTarget, true, false);
	}
	else if (Effects->SnowAccumulation && isExteriorLike && ShaderConst.SnowAccumulation.Params.w > 0.0f) {
		RunEffect(SnowAccumulationEffect, Device, RenderTarget, true, false);
	}
	// Shadows are not part of this post chain — both the exterior sun apply and the point-light
	// apply run MID-SCENE via RenderShadowsMidScene() (before the first near-water draw) so water
	// and the Underwater effect composite over the shadows instead of being painted over by them.
	if (Effects->Bloom) {
		RunEffect(BloomEffect, Device, RenderTarget, true, false);
	}
	if (Effects->Underwater && ShaderConst.HasWater && TheRenderManager->CameraPosition.z < ShaderConst.Water.waterSettings.x + 3.0f) { //  + 20.0f araf Bad offset with Enhanced Camera
		if (TheRenderManager->CameraPosition.z < ShaderConst.Water.waterSettings.x) {
			if (ShaderConst.WaterLens.Percent > -2.0f) ShaderConst.WaterLens.Percent = ShaderConst.WaterLens.Percent - 1.0f;
		}
		RunEffect(UnderwaterEffect, Device, RenderTarget, false, false);
	}
	else {
		if (ShaderConst.WaterLens.Percent <= -2.0f)
			ShaderConst.WaterLens.Percent = 1.0f;
		else if (ShaderConst.WaterLens.Percent <= -1.0f)
			ShaderConst.WaterLens.Percent = 0.0f;

		if (Effects->Precipitations && isExteriorLike) {
			if (ShaderConst.Precipitations.RainData.x > 0.0f) {
				RunEffect(RainEffect, Device, RenderTarget, false, false);
			}
			if (ShaderConst.Precipitations.SnowData.x > 0.0f) {
				RunEffect(SnowEffect, Device, RenderTarget, false, false);
			}
		}
		if (Effects->AmbientOcclusion && ShaderConst.AmbientOcclusion.Enabled) {
			RunEffect(AmbientOcclusionEffect, Device, RenderTarget, true, false);
		}
		if (Effects->GodRays && isExteriorLike && (ShaderConst.SunAmount.x >= 0.4 || ShaderConst.SunAmount.y > 0 || ShaderConst.SunAmount.z >= 0.3)) {
			RunEffect(GodRaysEffect, Device, RenderTarget, true, false);
		}
		else if (ShaderConst.MoonsExist && Effects->KhajiitRays && isExteriorLike && (ShaderConst.SunAmount.x < 0.4 || ShaderConst.SunAmount.z < 0.3)) {
			RunEffect(SecundaRaysEffect, Device, RenderTarget, true, false);
			RunEffect(MasserRaysEffect, Device, RenderTarget, true, false);
		}
		if (Effects->VolumetricFog && isExteriorLike && ShaderConst.VolumetricFog.Data.w) {
			// VolumetricFog samples only TESR_RenderedBuffer/TESR_DepthBuffer, never
			// TESR_SourceBuffer -- so the scene->SourceSurface blit was wasted work.
			RunEffect(VolumetricFogEffect, Device, RenderTarget, false, false);
		}
	}
	if (Effects->VolumetricLight && isExteriorLike) {
		RunEffect(VolumetricLightEffect, Device, RenderTarget, true, false);
	}
	if (Effects->SMAA) {
		if (ChainActive) {
			// The rotation gives SMAA exactly what RenderSurfaceSMAA plus the per-pass copies were
			// emulating: every pass clears and writes its own scratch buffer, and the next pass reads
			// that buffer directly. The dedicated surface and all five of SMAA's blits fall away.
			// Its TESR_RenderedBuffer is at s1, not s0 - RenderChained binds by RBRegister, which is
			// the whole reason that lookup exists.
			RunEffect(SMAAEffect, Device, RenderTarget, true, true);
		}
		else {
			ProfileBlitToSource(RenderTarget);
			Device->SetRenderTarget(0, RenderSurfaceSMAA);
			SMAAEffect->SetCT();
			SMAAEffect->Render(Device, RenderSurfaceSMAA, RenderedSurface, true);
			Device->StretchRect(RenderSurfaceSMAA, NULL, RenderTarget, NULL, D3DTEXF_NONE);
			EffCountBlit();
			Device->SetRenderTarget(0, RenderTarget);
		}
	}
	if (Effects->TAA) {
		RunEffect(TAAEffect, Device, RenderTarget, true, false);
		// TAA's history buffer. Under the rotation the finished frame sits in whichever scratch the
		// last pass wrote, not necessarily RenderedSurface, so take it from the live buffer.
		Device->StretchRect(ChainActive ? ChainSurf[ChainCur] : RenderedSurface, NULL, TAASurface, NULL, D3DTEXF_NONE);
		EffCountBlit();
	}
	if (Effects->DepthOfField && ShaderConst.DepthOfField.Enabled) {
		RunEffect(DepthOfFieldEffect, Device, RenderTarget, true, false);
	}
	if (Effects->WaterLens && ShaderConst.WaterLens.Percent > 0.0f) {
		RunEffect(WaterLensEffect, Device, RenderTarget, false, false);
	}
	if (Effects->MotionBlur && (ShaderConst.MotionBlur.Data.x || ShaderConst.MotionBlur.Data.y)) {
		RunEffect(MotionBlurEffect, Device, RenderTarget, false, false);
	}
	if (Effects->Sharpening) {
		RunEffect(SharpeningEffect, Device, RenderTarget, false, false);
	}
	if (Effects->Coloring) {
		RunEffect(ColoringEffect, Device, RenderTarget, false, false);
	}
	if (Effects->Extra) {
		for (ExtraEffectsList::iterator iter = ExtraEffects.begin(); iter != ExtraEffects.end(); ++iter) {
			if (iter->second->Enabled) {
				// Every built-in effect above has its blit matched to a real TESR_SourceBuffer
				// declaration (see the VolumetricFog note for the last one that did not). Extras were
				// the one path still paying a full-screen FP16 copy unconditionally.
				RunEffect(iter->second, Device, RenderTarget, iter->second->HasSB, false);
			}
		}
	}
	if (Effects->Cinema && (ShaderConst.Cinema.Data.x != 0.0f || ShaderConst.Cinema.Data.y != 0.0f)) {
		RunEffect(CinemaEffect, Device, RenderTarget, false, false);
	}

	// Materialise the rotation before the profiler closes, so its one remaining copy is counted in
	// the chain's numbers rather than disappearing between them.
	if (ChainActive) ChainEnd(RenderTarget);

	EffectProfileChainEnd(); // exclude the (rare) screenshot save below from timing

	if (TheKeyboardManager->OnKeyDown(TheSettingManager->SettingsMain.Main.ScreenshotKey)) {
		char Filename[MAX_PATH];
		char Name[80];
		time_t CurrentTime = time(NULL);

		strcpy(Filename, TheSettingManager->SettingsMain.Main.ScreenshotPath);
		strcat(Filename, ScreenshotFilenamePrefix);
		strftime(Name, 80, "%Y%m%d %H.%M.%S", localtime(&CurrentTime));
		strcat(Filename, Name);
		if (TheSettingManager->SettingsMain.Main.ScreenshotType == 0)
			strcat(Filename, ".bmp");
		else
			strcat(Filename, ".jpg");
		if (GetFileAttributesA(TheSettingManager->SettingsMain.Main.ScreenshotPath) == INVALID_FILE_ATTRIBUTES) CreateDirectoryA(TheSettingManager->SettingsMain.Main.ScreenshotPath, NULL);
		D3DXSaveSurfaceToFileA(Filename, (D3DXIMAGE_FILEFORMAT)TheSettingManager->SettingsMain.Main.ScreenshotType, RenderTarget, NULL, NULL);
		MenuManager->ShowMessage("Screenshot taken!");
	}
}

void ShaderManager::RenderEffectsPreHdr(IDirect3DSurface9* RenderTargetParam) {
	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_EffectsPreHdr);
	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	int rs1 = RenderState->GetRenderState(D3DRS_ZENABLE); //1
	int rs2 = RenderState->GetRenderState(D3DRS_ALPHAREF); //1
	int rs3 = RenderState->GetRenderState(D3DRS_ALPHABLENDENABLE); //1
	int rs4 = RenderState->GetRenderState(D3DRS_COLORWRITEENABLE); //1
	RenderState->SetRenderState(D3DRS_ZENABLE, FALSE, 0); //1
	RenderState->SetRenderState(D3DRS_ALPHAREF, FALSE, 0); //10
	RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE, 0);//1
	RenderState->SetRenderState(D3DRS_COLORWRITEENABLE, 15, 0); //0
	TheRenderManager->SetupSceneCamera();
	Device->SetStreamSource(0, EffectVertex, 0, sizeof(EffectQuad));
	Device->SetFVF(EFFECTQUADFORMAT);
    Device->StretchRect(RenderTargetParam, NULL, EffectSurface, NULL, D3DTEXF_NONE); 
	Device->StretchRect(RenderTargetParam, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	Device->SetRenderTarget(0,EffectSurface);
	IDirect3DSurface9* RenderTarget = EffectSurface;
	RenderEffects(RenderTarget);
	TheShaderManager->PrevWorldViewProjMatrix = TheRenderManager->WorldViewProjMatrix;
	Device->SetRenderTarget(0, RenderTargetParam);
	RenderState->SetRenderState(D3DRS_ZENABLE, rs1, 0); //1
	RenderState->SetRenderState(D3DRS_ALPHAREF, rs2, 0); //10
	RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, rs3, 0);//1
	RenderState->SetRenderState(D3DRS_COLORWRITEENABLE, rs4, 0); //0
}

void ShaderManager::RenderEffectsPostHdr(IDirect3DSurface9* RenderTargetParam) {
	FrameProfiler::Scope ProfileScope(FrameProfiler::Buck_EffectsPostHdr);
	IDirect3DDevice9* Device = TheRenderManager->device;
	TheRenderManager->SetupSceneCamera();
	Device->SetStreamSource(0, EffectVertex, 0, sizeof(EffectQuad));
	Device->SetFVF(EFFECTQUADFORMAT);
	Device->StretchRect(RenderTargetParam, NULL, RenderedSurface, NULL, D3DTEXF_NONE);
	RenderEffects(RenderTargetParam);
	TheShaderManager->PrevWorldViewProjMatrix = TheRenderManager->WorldViewProjMatrix;
}

// Snapshots the whole device state into CachedStateBlock, so a mid-scene pass can restore it exactly.
// Replaces a per-call CreateStateBlock(D3DSBT_ALL): that allocates a block AND captures, and only the
// capture is wanted after the first time. The state SET a block records is fixed when it is created,
// and D3DSBT_ALL is every state there is, so the one block serves all three callers regardless of
// which of them built it. False means no block is available and the caller must bail out rather than
// run a pass it cannot undo.
//
// The three callers are sequential top-level calls from the render hook and never nest, so there is
// never more than one live capture - if that ever changes, this has to go back to per-call blocks.
bool ShaderManager::CaptureDeviceState() {

	IDirect3DDevice9* Device = TheRenderManager->device;
	if (!Device) return false;

	// Every other D3D resource this manager caches assumes a device that outlives the plugin, and the
	// game never resets one. This costs a compare to not silently depend on that.
	if (CachedStateBlock && CachedStateBlockDevice != Device) {
		CachedStateBlock->Release();
		CachedStateBlock = NULL;
		CachedStateBlockDevice = NULL;
	}
	if (!CachedStateBlock) {
		if (FAILED(Device->CreateStateBlock(D3DSBT_ALL, &CachedStateBlock))) {
			CachedStateBlock = NULL;
			return false;
		}
		CachedStateBlockDevice = Device;
		return true; // CreateStateBlock captures the current state as it builds the block
	}
	return SUCCEEDED(CachedStateBlock->Capture());

}

// Shadow apply, run MID-SCENE: invoked from the render hook right before the first near-water
// surface draw of the main pass (grass and LOD water are already drawn), or at the end of the
// WorldSceneGraph render when no near water binds this frame. Rendering the darkening quad before
// the water surface lets water (and the Underwater post-effect) composite OVER the shadows, so
// shadows are never painted onto the water surface or over the underwater look.
//
// Two effects run here: the exterior sun shadows (worldspace only), then the point-light cube
// shadows (interiors AND exteriors). Order matters and costs nothing extra — a single-pass
// EffectRecord::Render ends by blitting the render target back into RenderedSurface, so the point
// effect reads the already-sun-shadowed image through TESR_RenderedBuffer with no second blit.
//
// The engine is mid-accumulation here, so all device changes go through raw Device calls (NOT
// NiDX9RenderState, whose cache must keep matching the device) and are bracketed by a full state
// block, restoring the exact device state the engine's state caches believe is current.
void ShaderManager::RenderShadowsMidScene() {

	bool DoSun = TheSettingManager->SettingsShadows.Exteriors.UsePostProcessing && ShadowsExteriorsEffect && Player->IsExteriorLike();
	bool DoPoint = TheSettingManager->SettingsShadows.Point.UsePostProcessing && ShadowsPointEffect
		&& TheShadowManager && TheShadowManager->PointSlotsShaded > 0;
	if (!DoSun && !DoPoint) return;

	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* SceneRT = NULL;

	if (!RenderedSurface || !EffectVertex) return;
	if (FAILED(Device->GetRenderTarget(0, &SceneRT)) || !SceneRT) return;
	if (!CaptureDeviceState()) { SceneRT->Release(); return; }

	TheRenderManager->SetupSceneCamera(); // CPU-side matrices only; refreshes WorldViewProj/InvViewProj for SetCT
	Device->SetRenderState(D3DRS_ZENABLE, FALSE);
	Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
	Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	Device->SetStreamSource(0, EffectVertex, 0, sizeof(EffectQuad));
	Device->SetFVF(EFFECTQUADFORMAT);
	// Seed TESR_RenderedBuffer for whichever effect runs first; each Render() re-blits for the next.
	Device->StretchRect(SceneRT, NULL, RenderedSurface, NULL, D3DTEXF_NONE); // scene color -> TESR_RenderedBuffer
	if (DoSun) {
		ShadowsExteriorsEffect->SetCT();
		ShadowsExteriorsEffect->Render(Device, SceneRT, RenderedSurface, false);
	}
	if (DoPoint) {
		ShadowsPointEffect->SetCT();
		ShadowsPointEffect->Render(Device, SceneRT, RenderedSurface, false);
	}

	CachedStateBlock->Apply();
	SceneRT->Release();

}

// Builds the near-shell flatten resources on first use, so a disabled or inactive shell pays
// nothing: a full-screen INTZ surface plus two small shaders. Any failure latches ShellFlattenFailed
// and the flatten silently stays off - the frame then looks exactly as it did before this pass
// existed, which is a visible artifact but never a broken render.
//
// Everything except the target surface is shared between the two flatten targets (TESR_DepthBuffer
// and TESR_DepthBufferPreWater): same mask, same shaders, same threshold. Target/TargetSurface name
// the texture being rewritten and the caller's cache slot for its level-0 surface.
//
// ShellFlattenFailed is shared too, ON PURPOSE and with consequences worth stating: a failure while
// preparing the post-shell flatten permanently disables the pre-water clamp as well, and vice versa,
// even though losing the first only reintroduces the Ambient Occlusion band at M while losing the
// second only worsens the submerged over-tint. The coupling is sound rather than merely convenient,
// because there is no failure here that could be specific to one target. Every shared resource -
// coverage mask, both shaders - fails for both by definition. The two target-specific steps,
// GetSurfaceLevel(0) and binding the result as a depth-stencil, are asked of two textures created in
// the same two lines of RenderManager::Initialize with identical format (INTZ), size and usage, so a
// driver that refuses one refuses the other. A per-target latch would buy a distinction the hardware
// does not make, and would risk retrying a doomed operation once per frame forever.
//
// Builds ShellMaskTexture, the shell's coverage mask. Shared by the depth flatten and the masked
// colour capture, which want the identical mask at the identical instant. False = creation failed.
bool ShaderManager::CreateShellMask() {

	if (ShellMaskTexture) return true;

	// Format and size must match the depth textures it is resolved from: ResolveDepthInto's RESZ path
	// requires an exact match, and its NvAPI path requires the resource to be registered - done here
	// because that function's one-shot block has already run by the time this texture exists.
	if (FAILED(TheRenderManager->device->CreateTexture(TheRenderManager->width, TheRenderManager->height, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I','N','T','Z'), D3DPOOL_DEFAULT, &ShellMaskTexture, NULL))) {
		Logger::Log("ERROR: Cannot create the near shell coverage buffer. The near shell depth flatten is disabled.");
		return false;
	}
	if (!TheRenderManager->RESZ) NvAPI_D3D9_RegisterResource(ShellMaskTexture);
	return true;

}

// Builds ShellFlatten.vso, the full-screen quad shared by the flatten and the masked colour capture.
// False = the shader is unavailable; enable CompileShaders once to build it.
bool ShaderManager::CreateShellQuadVertexShader() {

	if (ShellFlattenVertexShader) return true;

	// The record exists only to carry the bytecode into CreateVertexShader, so drop it again on
	// failure rather than leaving it hanging off the member - same shape as ShaderManager::LoadShader.
	ShellFlattenVertex = new ShaderRecord();
	if (ShellFlattenVertex->LoadShader("ShellFlatten.vso")) TheRenderManager->device->CreateVertexShader((const DWORD*)ShellFlattenVertex->Function, &ShellFlattenVertexShader);
	if (!ShellFlattenVertexShader) {
		delete ShellFlattenVertex;
		ShellFlattenVertex = NULL;
		return false;
	}
	return true;

}

bool ShaderManager::CreateShellFlatten(IDirect3DTexture9* Target, IDirect3DSurface9** TargetSurface) {

	if (ShellFlattenFailed) return false;
	if (!Target || !TargetSurface) return false;
	if (ShellFlattenPixelShader && ShellFlattenVertexShader && ShellMaskTexture && *TargetSurface) return true;

	IDirect3DDevice9* Device = TheRenderManager->device;

	if (!CreateShellMask()) {
		ShellFlattenFailed = true;
		return false;
	}

	// The flatten writes into the target texture by binding it as the depth-stencil target. Both
	// targets are INTZ textures created with D3DUSAGE_DEPTHSTENCIL at the backbuffer size in the same
	// two lines of RenderManager::Initialize, so this is what they are for and they are
	// interchangeable here.
	if (!*TargetSurface && FAILED(Target->GetSurfaceLevel(0, TargetSurface))) {
		Logger::Log("ERROR: Cannot address the depth buffer surface. The near shell depth flatten is disabled.");
		ShellFlattenFailed = true;
		return false;
	}

	CreateShellQuadVertexShader();
	if (!ShellFlattenPixelShader) {
		ShellFlattenPixel = new ShaderRecord();
		if (ShellFlattenPixel->LoadShader("ShellFlatten.pso")) Device->CreatePixelShader((const DWORD*)ShellFlattenPixel->Function, &ShellFlattenPixelShader);
		if (!ShellFlattenPixelShader) {
			delete ShellFlattenPixel;
			ShellFlattenPixel = NULL;
		}
	}
	if (!ShellFlattenVertexShader || !ShellFlattenPixelShader) {
		Logger::Log("ERROR: ShellFlatten shaders not loaded. The near shell depth flatten is disabled - enable CompileShaders once to build them.");
		ShellFlattenFailed = true;
		return false;
	}
	return true;

}

// Builds what the masked TESR_RenderedBuffer capture needs. False = the caller falls back to a blind
// blit, which still fixes the submerged arms and only leaves the boundary strip.
//
// A missing ShellCopy.pso deliberately does NOT latch ShellFlattenFailed: unlike the flatten, whose
// only alternative is to do nothing, this pass has a real fallback. It still refuses to retry once
// that latch is set, because then the shared mask or vertex shader is gone.
bool ShaderManager::CreateShellCopy() {

	if (ShellFlattenFailed) return false;
	if (ShellCopyPixelShader && ShellFlattenVertexShader && ShellMaskTexture) return true;

	if (!CreateShellMask()) {
		ShellFlattenFailed = true;
		return false;
	}
	if (!CreateShellQuadVertexShader()) {
		Logger::Log("ERROR: ShellFlatten.vso not loaded. The near shell depth flatten is disabled - enable CompileShaders once to build it.");
		ShellFlattenFailed = true;
		return false;
	}
	if (!ShellCopyPixelShader) {
		ShellCopyPixel = new ShaderRecord();
		if (ShellCopyPixel->LoadShader("ShellCopy.pso")) TheRenderManager->device->CreatePixelShader((const DWORD*)ShellCopyPixel->Function, &ShellCopyPixelShader);
		if (!ShellCopyPixelShader) {
			delete ShellCopyPixel;
			ShellCopyPixel = NULL;
			Logger::Log("WARNING: ShellCopy.pso not loaded. The near shell falls back to a blind rendered-buffer capture - enable CompileShaders once to build it.");
			return false;
		}
	}
	return true;

}

// Near shell: rewrite TESR_DepthBuffer to "exactly at M" everywhere the shell drew.
//
// TESR_DepthBuffer only ever holds the FAR pass, encoded (M, F). Shell geometry cannot be
// represented in that encoding at all - anything nearer than M maps behind its near plane - so at
// every pixel the shell covered, the buffer instead holds whatever the far pass drew BEHIND it: the
// lake bottom under near water, the terrain beyond a near bank. Along any surface crossing M that
// leaves a hard cliff at exactly z = M which does not exist in the scene, and the sixteen effects
// that sample the buffer read it as a real edge - Ambient Occlusion paints it as a dark band with a
// sharp cutoff at the boundary.
//
// The effects do not need true shell depth; they only need the cliff gone. Depth 0 is the nearest
// value the (M, F) encoding can express and decodes to exactly z = M, so writing it over the shell's
// coverage turns the cliff into a flat surface at the boundary distance. The depth field is then
// continuous where a surface crosses M (the far pass has M + epsilon on its side of the line) and
// every consumer quietly no-ops across the near band instead of answering from the wrong geometry.
//
// The coverage mask is free: the shell renders on a buffer cleared to ShellClearDepth, so a resolve
// taken now IS the mask - below the clear value means the shell wrote there. That is why no stencil
// is involved. Stencil would have to be armed across the engine's own per-draw render state, which
// is exactly how the original sky colour-write suppression failed, and the engine's stencil property
// application would clobber it mid-pass with no way to tell.
//
// Called from RenderHook::TrackRenderObject immediately after the shell's RenderObject; see the
// comment there for why the position in the frame is load bearing.
void ShaderManager::FlattenShellDepth() {

	FlattenShellDepthInto(TheRenderManager->DepthTexture, &ShellFlattenDepthSurface, true);

}

// Near shell: refresh TESR_RenderedBuffer over the shell's coverage AND NOWHERE ELSE, so shell water
// refracts the far pass's PRE-water frame everywhere else rather than its already-shaded water. A
// blind blit of the live target instead leaves a strip of doubly-extinguished water along the
// boundary. Called at the shell's first near-water draw - the last moment its depth-stencil holds all
// of its geometry and none of its water, which is also what the pre-water clamp needs. See the
// near-shell design doc, "A fresh TESR_RenderedBuffer capture" and "A fifth defect".
//
// Returns true when ShellMaskTexture holds this instant's coverage resolve, which the clamp then
// reuses. False means the caller must fall back to the blind blit and let the clamp resolve itself.
bool ShaderManager::CaptureShellRenderedBuffer() {

	if (!RenderManager::ShellActive) return false;
	if (!EffectVertex || !EffectSurface || !EffectTexture || !RenderedSurface) return false;
	if (!TheRenderManager->currentRTGroup) return false;
	if (!CreateShellCopy()) return false;

	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* SceneRT = TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface;
	IDirect3DSurface9* PrevRenderTarget = NULL;
	IDirect3DSurface9* PrevDepthSurface = NULL;

	if (!SceneRT) return false;

	// The scene target can be multisampled under HDR and so cannot be sampled directly. Resolve it into
	// EffectTexture, free scratch mid-scene that the effect chain overwrites wholesale later.
	if (FAILED(Device->StretchRect(SceneRT, NULL, EffectSurface, NULL, D3DTEXF_NONE))) return false;

	// Snapshot the shell's coverage. Must precede the target switch below, which takes the
	// depth-stencil surface this reads out from under the device.
	TheRenderManager->ResolveDepthInto(ShellMaskTexture);

	if (FAILED(Device->GetRenderTarget(0, &PrevRenderTarget)) || !PrevRenderTarget) return false;
	if (FAILED(Device->GetDepthStencilSurface(&PrevDepthSurface))) { PrevRenderTarget->Release(); return false; }
	if (!CaptureDeviceState()) {
		PrevRenderTarget->Release();
		if (PrevDepthSurface) PrevDepthSurface->Release();
		return false;
	}

	// LOAD BEARING: RenderedTexture backs the surface about to become the render target, and the water
	// draws already down this pass had it bound as TESR_RenderedBuffer. Clear every stage
	// unconditionally rather than by identity; the state block puts them all back.
	for (DWORD i = 0; i < 16; i++) Device->SetTexture(i, NULL);

	// Unbind depth rather than leave it bound with Z off: the shell's is multisampled whenever the
	// scene target is, and D3D9 will not pair that with the non-multisampled RenderedSurface.
	if (FAILED(Device->SetRenderTarget(0, RenderedSurface))) {
		Device->SetRenderTarget(0, PrevRenderTarget);
		CachedStateBlock->Apply();
		PrevRenderTarget->Release();
		if (PrevDepthSurface) PrevDepthSurface->Release();
		return false;
	}
	Device->SetDepthStencilSurface(NULL);

	Device->SetVertexShader(ShellFlattenVertexShader);
	Device->SetPixelShader(ShellCopyPixelShader);
	Device->SetTexture(0, ShellMaskTexture);
	Device->SetTexture(1, EffectTexture);
	for (DWORD s = 0; s < 2; s++) {
		Device->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		Device->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		Device->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		Device->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		Device->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		// This pass writes colour, so it must be bit-exact: an sRGB conversion on either end would tint
		// the shell's pixels relative to the far pass's. Neither flag is ours by default - this one is
		// per-shader from the texture INI, the write one below is the engine's. See the design doc.
		Device->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
	}
	Device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	Device->SetRenderState(D3DRS_ZENABLE, FALSE);
	Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	Device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
	Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	D3DXVECTOR4 CopyData(RenderManager::ShellCoveredMax, 0.0f, 0.0f, 0.0f);
	Device->SetPixelShaderConstantF(0, (const float*)&CopyData, 1);
	Device->SetStreamSource(0, EffectVertex, 0, sizeof(EffectQuad));
	Device->SetFVF(EFFECTQUADFORMAT);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

	// Order matters: SetRenderTarget resets the viewport, so the state block - which restores the
	// engine's - has to be applied after the targets are back.
	Device->SetTexture(0, NULL);
	Device->SetTexture(1, NULL);
	Device->SetRenderTarget(0, PrevRenderTarget);
	Device->SetDepthStencilSurface(PrevDepthSurface);
	CachedStateBlock->Apply();
	PrevRenderTarget->Release();
	if (PrevDepthSurface) PrevDepthSurface->Release();
	return true;

}

// Near shell: the same clamp, applied to TESR_DepthBufferPreWater instead, and DURING the shell.
//
// TESR_DepthBufferPreWater is the depth term shell water shades with (RenderHook's per-draw sampler
// swap binds it in place of TESR_DepthBuffer for shell WATER draws). It is resolved during the FAR
// pass, which clipped away everything nearer than M, so at any pixel covered only by shell geometry
// it holds whatever the far pass drew behind it. Treading water that is the lake bottom two hundred
// units down, and WATER007's readDepth -> refract_uw_pos -> extinction/volume-colour chain then
// shades the player's own submerged arms as if they were lying on it.
//
// The exact fix is out of reach: readDepth's decode range IS the range published in
// TESR_DepthProjectionTransform, so re-capturing the shell's (n, M) depth would need c29 to be
// (n, M) too, which caps open water's bottom at M and re-breaks the deep-water shading. Clamp
// instead. Depth 0 decodes to exactly z = M, the nearest distance the (M, F) encoding can express,
// so shell-covered pixels shade as ~M units of water rather than ~200 - most of the error gone for
// one extra resolve and one extra full-screen quad, and no change to the encoding anything else
// reads.
//
// Ordering, which is the whole difficulty: this must run BEFORE the shell's water draws, not after
// the shell like the flatten above, because those draws sample the texture while the shell is still
// running. RenderHook fires it at the shell's FIRST near-water draw - after the shell's opaque,
// EQUAL-depth detail and grass draws (so the mask holds the arms) and before any water (so the mask
// holds no water surface, and the clamp lands before the first read).
//
// Nothing downstream sees a clamped value it should not: the only other consumer of this texture is
// the sun/point shadow apply in RenderShadowsMidScene, which runs at PassFar - either at the far
// pass's first near-water draw or, failing that, at the end of the far pass - and both of those, and
// the pre-water resolve they are paired with, are complete before the shell starts.
//
// MaskResolved says CaptureShellRenderedBuffer already resolved ShellMaskTexture at this exact
// instant, so it is not paid for twice. True only when that capture just ran and reported success.
void ShaderManager::FlattenShellPreWaterDepth(bool MaskResolved) {

	FlattenShellDepthInto(TheRenderManager->DepthTexturePreWater, &ShellFlattenPreWaterSurface, !MaskResolved);

}

void ShaderManager::FlattenShellDepthInto(IDirect3DTexture9* Target, IDirect3DSurface9** TargetSurface, bool ResolveMask) {

	if (!RenderManager::ShellActive) return;
	// The shell submitted nothing that could write depth this frame - no coverage to flatten, so no
	// reason to pay for the resolve, the state block or the quad. ShellDraws deliberately excludes the
	// SKY* draws that the shell's sub-1.0 clear rejects, or the sky would hold this gate open in every
	// exterior; it is still only an upper bound on coverage (a counted draw may be depth-rejected or
	// have ZWRITE off), so this is a cheap degenerate-case guard rather than an exact one. When it does
	// let a coverage-free frame through the result is still correct: ShellFlatten's clip() discards
	// every pixel.
	if (!RenderManager::ShellDraws) return;
	if (!EffectVertex || !EffectSurface || !Target) return;
	if (!CreateShellFlatten(Target, TargetSurface)) return;

	IDirect3DDevice9* Device = TheRenderManager->device;
	IDirect3DSurface9* PrevRenderTarget = NULL;
	IDirect3DSurface9* PrevDepthSurface = NULL;

	// Snapshot the shell's depth buffer. Must precede the target switch below, which takes the
	// depth-stencil surface this reads out from under the device. Skipped only when the masked
	// rendered-buffer capture just took the identical resolve (see FlattenShellPreWaterDepth).
	if (ResolveMask) TheRenderManager->ResolveDepthInto(ShellMaskTexture);

	if (FAILED(Device->GetRenderTarget(0, &PrevRenderTarget)) || !PrevRenderTarget) return;
	if (FAILED(Device->GetDepthStencilSurface(&PrevDepthSurface))) { PrevRenderTarget->Release(); return; }
	if (!CaptureDeviceState()) {
		PrevRenderTarget->Release();
		if (PrevDepthSurface) PrevDepthSurface->Release();
		return;
	}

	// The hazard, named at the one place it exists: a texture cannot be a sampler source and the
	// depth-stencil target at the same time, and Target is normally BOTH bound at this instant. For
	// the pre-water clamp that is not a corner case but the standard path - RenderHook's per-draw swap
	// binds TESR_DepthBufferPreWater into the sampler TESR_DepthBuffer occupies for every shell WATER
	// draw, and the shell's LOD water (WATER012+) draws run through that swap BEFORE this clamp fires
	// at the first NEAR water draw. Other scene textures can alias the target too, since the water
	// shaders read TESR_DepthBuffer in both passes. So clear every stage, unconditionally; the state
	// block puts them all back.
	//
	// LOAD BEARING, not tidiness: this is the only thing that unbinds Target. Anyone tempted to narrow
	// it to "the stages that actually matter" has to keep unbinding Target by identity, or the flatten
	// silently samples and writes the same surface.
	for (DWORD i = 0; i < 16; i++) Device->SetTexture(i, NULL);

	// EffectSurface only stands in as a colour target of the right size and (unlike the scene target
	// under HDR + MSAA) without multisampling, which the non-multisampled depth texture could not be
	// paired with. Colour writes are off, so its contents are untouched; it is overwritten wholesale
	// by the effect chain later anyway.
	// Checked, not assumed: if the depth texture cannot be paired with this colour target on this
	// hardware, the draw below would land in the depth-stencil surface still bound - the shell's own
	// - and stamp zeroes through it. Bail out instead, once and for all.
	if (FAILED(Device->SetRenderTarget(0, EffectSurface)) || FAILED(Device->SetDepthStencilSurface(*TargetSurface))) {
		Logger::Log("ERROR: Cannot bind the depth buffer as a render surface. The near shell depth flatten is disabled.");
		ShellFlattenFailed = true;
		Device->SetRenderTarget(0, PrevRenderTarget);
		Device->SetDepthStencilSurface(PrevDepthSurface);
		CachedStateBlock->Apply();
		PrevRenderTarget->Release();
		if (PrevDepthSurface) PrevDepthSurface->Release();
		return;
	}
	Device->SetVertexShader(ShellFlattenVertexShader);
	Device->SetPixelShader(ShellFlattenPixelShader);
	Device->SetTexture(0, ShellMaskTexture);
	Device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	Device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	Device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	Device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	Device->SetRenderState(D3DRS_ZENABLE, TRUE);
	Device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	Device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);	// the pixel shader's clip() is the only test
	Device->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
	Device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	Device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	Device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	Device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	D3DXVECTOR4 FlattenData(RenderManager::ShellCoveredMax, 0.0f, 0.0f, 0.0f);
	Device->SetPixelShaderConstantF(0, (const float*)&FlattenData, 1);
	Device->SetStreamSource(0, EffectVertex, 0, sizeof(EffectQuad));
	Device->SetFVF(EFFECTQUADFORMAT);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

	// Order matters: SetRenderTarget resets the viewport, so the state block - which restores the
	// engine's - has to be applied after the targets are back.
	Device->SetTexture(0, NULL);
	Device->SetRenderTarget(0, PrevRenderTarget);
	Device->SetDepthStencilSurface(PrevDepthSurface);
	CachedStateBlock->Apply();
	PrevRenderTarget->Release();
	if (PrevDepthSurface) PrevDepthSurface->Release();

}

void ShaderManager::LoadEffectSettings() {
	TESObjectCELL* currentCell = Player->parentCell;
	TESWorldSpace* currentWorldSpace = Player->GetWorldSpace();
	bool isExteriorLike = Player->IsExteriorLike();
		//Color
		if (!(scs = TheSettingManager->GetSettingsColoring(currentCell->GetEditorName())))
			if (currentWorldSpace)
				scs = TheSettingManager->GetSettingsColoring(currentWorldSpace->GetEditorName());

		if (!scs) scs = TheSettingManager->GetSettingsColoring("Default");

		//Water
		if (CurrentBlend == 0.25f)
			sws = TheSettingManager->GetSettingsWater("Blood");
		else if (CurrentBlend == 0.50f)
			sws = TheSettingManager->GetSettingsWater("Lava");
		else
			if (!(sws = TheSettingManager->GetSettingsWater(currentCell->GetEditorName())))
				if (currentWorldSpace) sws = TheSettingManager->GetSettingsWater(currentWorldSpace->GetEditorName());

		if (!sws) sws = TheSettingManager->GetSettingsWater("Default");


		//Bloom
		if (!(sbs = TheSettingManager->GetSettingsBloom(currentCell->GetEditorName())))
			if (currentWorldSpace)
				sbs = TheSettingManager->GetSettingsBloom(currentWorldSpace->GetEditorName());

		if (!sbs) sbs = TheSettingManager->GetSettingsBloom("Default");


		//Ambient Occlusion
		if (isExteriorLike)
			sas = TheSettingManager->GetSettingsAmbientOcclusion("Exterior");
		else
			sas = TheSettingManager->GetSettingsAmbientOcclusion("Interior");


		//Interior Lighting
		if (isExteriorLike) {
			//do nothing
		}
		else {
			if (ShaderConst.InteriorLighting.find(currentCell->GetEditorName()) == ShaderConst.InteriorLighting.end()) {
				TESObjectCELL::LightingData* LightData = currentCell->lighting;
				ShaderConstants::SimpleLightingStruct sls;
				sls.r = LightData->ambient.r;
				sls.g = LightData->ambient.g;
				sls.b = LightData->ambient.b;
				sls.a = LightData->ambient.a;
				ShaderConst.InteriorLighting.emplace(currentCell->GetEditorName(), sls);
			}

			InteriorLighting.r = ShaderConst.InteriorLighting[currentCell->GetEditorName()].r;
			InteriorLighting.g = ShaderConst.InteriorLighting[currentCell->GetEditorName()].g;
			InteriorLighting.b = ShaderConst.InteriorLighting[currentCell->GetEditorName()].b;
		}

}
void ShaderManager::SwitchShaderStatus(const char* Name) {
	
	SettingsMainStruct::EffectsStruct* Effects = &TheSettingManager->SettingsMain.Effects;
	SettingsMainStruct::ShadersStruct* Shaders = &TheSettingManager->SettingsMain.Shaders;

	bool Value = false;

	LoadEffectSettings();

	if (!strcmp(Name, "AmbientOcclusion")) {
		Value = !Effects->AmbientOcclusion;
		Effects->AmbientOcclusion = Value;
		DisposeEffect(AmbientOcclusionEffect);
		if (Value) {
			CreateEffect(EffectRecordType_AmbientOcclusion);
		}
	}
	else if (!strcmp(Name, "Blood")) {
		Value = !Shaders->Blood;
		Shaders->Blood = Value;
		DisposeShader(Name);
		if (Value) CreateShader(Name);
	}
	else if (!strcmp(Name, "Bloom")) {
		Value = !Effects->Bloom;
		Effects->Bloom = Value;
		DisposeEffect(BloomEffect);
		if (Value) {
			CreateEffect(EffectRecordType_Bloom);
		}
	}
	else if (!strcmp(Name, "Cinema")) {
		Value = !Effects->Cinema;
		Effects->Cinema = Value;
		DisposeEffect(CinemaEffect);
		if (Value) CreateEffect(EffectRecordType_Cinema);
	}
	else if (!strcmp(Name, "Coloring")) {
		Value = !Effects->Coloring;
		Effects->Coloring = Value;
		DisposeEffect(ColoringEffect);
		if (Value) {
			CreateEffect(EffectRecordType_Coloring);
		}
	}
	else if (!strcmp(Name, "DepthOfField")) {
		Value = !Effects->DepthOfField;
		Effects->DepthOfField = Value;
		DisposeEffect(DepthOfFieldEffect);
		if (Value) CreateEffect(EffectRecordType_DepthOfField);
	}
	else if (!strcmp(Name, "Grass")) {
		Value = !Shaders->Grass;
		Shaders->Grass = Value;
		DisposeShader(Name);
		if (Value) CreateShader(Name);
	}
	else if (!strcmp(Name, "GodRays")) {
		Value = !Effects->GodRays;
		Effects->GodRays = Value;
		DisposeEffect(GodRaysEffect);
		if (Value) {
			CreateEffect(EffectRecordType_GodRays);
		}
	}
	else if (!strcmp(Name, "KhajiitRays")) {
		Value = Effects->KhajiitRays = !Effects->KhajiitRays;
		DisposeEffect(MasserRaysEffect);
		DisposeEffect(SecundaRaysEffect);
		if (Value) {
			CreateEffect(EffectRecordType_KhajiitRays);
		}
	}
	else if (!strcmp(Name, "MotionBlur")) {
		Value = !Effects->MotionBlur;
		Effects->MotionBlur = Value;
		DisposeEffect(MotionBlurEffect);
		if (Value) CreateEffect(EffectRecordType_MotionBlur);
	}
	else if (!strcmp(Name, "POM")) {
		Value = !Shaders->POM;
		Shaders->POM = Value;
		DisposeShader(Name);
		if (Value) {
			CreateShader(Name);
		}
	}
	else if (!strcmp(Name, "Precipitations")) {
		Value = !Effects->Precipitations;
		Effects->Precipitations = Value;
		DisposeEffect(RainEffect);
		DisposeEffect(SnowEffect);
		if (Value) CreateEffect(EffectRecordType_Precipitations);
	}
	else if (!strcmp(Name, "Skin")) {
		Value = !Shaders->Skin;
		Shaders->Skin = Value;
		DisposeShader(Name);
		if (Value) CreateShader(Name);
	}
	else if (!strcmp(Name, "SkinVanilla")) {
		DisposeShader("Skin");
		CreateShader("Skin");
	}
	else if (!strcmp(Name, "SMAA")) {
		Value = !Effects->SMAA;
		Effects->SMAA = Value;
		DisposeEffect(SMAAEffect);
		if (Value) CreateEffect(EffectRecordType_SMAA);
	}
	else if (!strcmp(Name, "TAA")) {
		Value = !Effects->TAA;
		Effects->TAA = Value;
		DisposeEffect(TAAEffect);
		if (Value) CreateEffect(EffectRecordType_TAA);
	}
	else if (!strcmp(Name, "SnowAccumulation")) {
		Value = !Effects->SnowAccumulation;
		Effects->SnowAccumulation = Value;
		DisposeEffect(SnowAccumulationEffect);
		if (Value) CreateEffect(EffectRecordType_SnowAccumulation);
	}
	else if (!strcmp(Name, "Terrain")) {
		Value = !Shaders->Terrain;
		Shaders->Terrain = Value;
		DisposeShader(Name);
		if (Value) CreateShader(Name);
	}
	else if (!strcmp(Name, "Underwater")) {
		Value = !Effects->Underwater;
		Effects->Underwater = Value;
		DisposeEffect(UnderwaterEffect);
		if (Value) CreateEffect(EffectRecordType_Underwater);
	}
	else if (!strcmp(Name, "Water")) {
		Value = !Shaders->Water;
		Shaders->Water = Value;
		DisposeShader(Name);
		if (Value) CreateShader(Name);
	}
	else if (!strcmp(Name, "WaterLens")) {
		Value = !Effects->WaterLens;
		Effects->WaterLens = Value;
		DisposeEffect(WaterLensEffect);
		if (Value) CreateEffect(EffectRecordType_WaterLens);
	}
	else if (!strcmp(Name, "WetWorld")) {
		Value = !Effects->WetWorld;
		Effects->WetWorld = Value;
		DisposeEffect(WetWorldEffect);
		if (Value) CreateEffect(EffectRecordType_WetWorld);
	}
	else if (!strcmp(Name, "Sharpening")) {
		Value = !Effects->Sharpening;
		Effects->Sharpening = Value;
		DisposeEffect(SharpeningEffect);
		if (Value) CreateEffect(EffectRecordType_Sharpening);
	}
	else if (!strcmp(Name, "VolumetricFog")) {
		Value = !Effects->VolumetricFog;
		Effects->VolumetricFog = Value;
		DisposeEffect(VolumetricFogEffect);
		if (Value) CreateEffect(EffectRecordType_VolumetricFog);
	}
	else if (!strcmp(Name, "VolumetricLight")) {
		Value = !Effects->VolumetricLight;
		Effects->VolumetricLight = Value;
		DisposeEffect(VolumetricLightEffect);
		if (Value) CreateEffect(EffectRecordType_VolumetricLight);
	}
	else if (!strcmp(Name, "ShadowsExteriors")) {
		DisposeEffect(ShadowsExteriorsEffect);
		if (TheSettingManager->SettingsShadows.Exteriors.UsePostProcessing) CreateEffect(EffectRecordType_ShadowsExteriors);
	}
	else if (!strcmp(Name, "ShadowsPoint")) {
		DisposeEffect(ShadowsPointEffect);
		if (TheSettingManager->SettingsShadows.Point.UsePostProcessing) CreateEffect(EffectRecordType_ShadowsPoint);
	}

}

void ShaderManager::SetCustomConstant(const char* Name, D3DXVECTOR4 Value) {
	
	CustomConstants::iterator v = CustomConst.find(std::string(Name));
	if (v != CustomConst.end()) v->second = Value;

}

void ShaderManager::SetExtraEffectEnabled(const char* Name, bool Value) {

	ExtraEffectsList::iterator v = ExtraEffects.find(std::string(Name));
	if (v != ExtraEffects.end()) v->second->Enabled = Value;

}

void ShaderManager::SetPhaseLumCoeff(int phaseLength, int phaseDay) {
	if (phaseDay < phaseLength * 1) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumFull;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumFull;
		return;
	}
	if (phaseDay < phaseLength * 2) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumTQtr;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumTQtr;
		return;
	}
	if (phaseDay < phaseLength * 3) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumHalf;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumHalf;
		return;
	}
	if (phaseDay < phaseLength * 4) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumQtr;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumQtr;
		return;
	}
	if (phaseDay < phaseLength * 5) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumNew;
		ShaderConst.RaysPhaseCoeff.x = 0.0f;
		return;
	}
	if (phaseDay < phaseLength * 6) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumQtr;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumQtr;
		return;
	}
	if (phaseDay < phaseLength * 7) {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumHalf;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumHalf;
		return;
	}
	else {
		ShaderConst.MoonPhaseCoeff = TheSettingManager->SettingsMain.Main.MoonPhaseLumTQtr;
		ShaderConst.RaysPhaseCoeff.x = TheSettingManager->SettingsKhajiitRays.phaseLumTQtr;
		return;
	}
}