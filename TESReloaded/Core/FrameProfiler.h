#pragma once

// --- Whole-frame plugin profiler (Develop.ProfileFrame) -----------------------
// ProfileShadows and ProfileEffects each scope ONE subsystem in isolation, so
// neither can answer "the frame got slower and it is not those two". This
// profiler attributes a frame across ALL top-level plugin per-frame entry points
// in a SINGLE view, plus a residual ("Other") that captures the game's own
// rendering and any unhooked plugin work.
//
// It measures BOTH clocks, and the distinction matters: a QPC wall-clock around
// a D3D9 call chain measures CPU *submission* cost only. With a driver-queued
// frame, GPU cost surfaces as a stall wherever the queue happens to back up
// (typically Present), NOT at the expensive draw - so CPU buckets alone will
// read flat for a GPU-side regression and dump everything into "Other". The
// FrameTotal GPU timestamp pair is what separates those two cases.
//
// Averages hide stutters, so every bucket also carries the worst single frame in
// the window. A regression that shows up in max but not mean is a spike (a cache
// miss cascade, a rebake), not a uniform slowdown.
namespace FrameProfiler {

	// Buck_FrameTotal is the envelope (RenderHook::TrackRender). The TOP-LEVEL
	// buckets are mutually disjoint and are subtracted from the envelope to form
	// the "Other" residual. The DETAIL buckets are NESTED inside a top-level
	// bucket and are reported as a breakdown only - summing them into the residual
	// would double-count. A NEGATIVE "Other" in the report means two top-level
	// buckets overlapped and the disjointness assumption has been broken.
	enum Bucket {
		Buck_FrameTotal = 0,      // whole RenderHook::TrackRender (plugin per-frame envelope)

		// --- top-level, disjoint ---
		Buck_UpdateShaderStates,  // ShaderManager::UpdateShaderStates
		Buck_SetSceneGraph,       // RenderManager::SetSceneGraph
		Buck_UpdateConstants,     // ShaderManager::UpdateConstants
		Buck_ShadowMaps,          // ShadowManager::RenderShadowMaps (CPU draw submission)
		Buck_EffectsPreHdr,       // ShaderManager::RenderEffectsPreHdr
		Buck_EffectsPostHdr,      // ShaderManager::RenderEffectsPostHdr
		Buck_BeginScene,          // ShaderManager::BeginScene, summed over ALL scenes in the frame
		Buck_ShaderHook,          // RenderHook::TrackSetupShaderPrograms, summed over all draws

		// --- detail, nested inside Buck_ShaderHook ---
		Buck_HookEngine,          // the nested engine SetupShaderPrograms call (game cost, not ours)
		Buck_HookSetCT,           // ShaderRecord::SetCT + SetPerGeomCT (constant/texture upload loops)
		Buck_HookResolve,         // depth resolve + RenderedBuffer blit fired from inside SetCT
		Buck_HookMidScene,        // pre-water depth resolve + RenderShadowsMidScene (sun shadow apply)
		Buck_HookShellWater,      // near-shell water prep (capture + pre-water depth flatten)

		Buck_COUNT
	};
	const int TopLevelBegin = Buck_UpdateShaderStates;
	const int TopLevelEnd   = Buck_HookEngine;   // one past the last disjoint bucket

	// Structural counts. Milliseconds alone cannot distinguish "each call got
	// slower" from "the same work now runs on three times the draws".
	enum Counter {
		Cnt_Draws,          // TrackSetupShaderPrograms invocations
		Cnt_SetCT,          // ShaderRecord::SetCT invocations
		Cnt_SetPerGeomCT,   // ShaderRecord::SetPerGeomCT invocations
		Cnt_DepthResolves,  // depth resolves triggered from SetCT
		Cnt_RenderedBlits,  // full-surface StretchRect blits triggered from SetCT
		Cnt_Scenes,         // ShaderManager::BeginScene invocations (main pass + off-screen renders)
		Cnt_COUNT
	};

	// Armed by FrameBegin for the duration of one TrackRender. Exposed so the RAII
	// scope and the counters inline their disabled path down to one predictable
	// branch - they sit on per-draw code paths that run thousands of times a frame.
	extern bool Active;
	extern unsigned int Counters[Cnt_COUNT];

	void FrameBegin();                          // top of TrackRender: re-reads the INI flag, arms timing
	void FrameEnd();                            // end of TrackRender: accumulates, logs every report window
	void Accumulate(int bucket, LONGLONG start); // out-of-line: only ever called while Active

	inline LONGLONG QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
	inline void Count(int counter, unsigned int n = 1) { if (Active) Counters[counter] += n; }

	// Issues NO QueryPerformanceCounter calls when disabled, so it is free in
	// normal builds. B < 0 marks a scope that was constructed while disarmed.
	struct Scope {
		int      B;
		LONGLONG Start;
		Scope(int bucket) {
			if (Active) { B = bucket; Start = QpcNow(); }
			else        { B = -1;     Start = 0; }
		}
		~Scope() { if (B >= 0) Accumulate(B, Start); }
		// Close early, e.g. to end the frame envelope before the report that reads it runs.
		void Close() { if (B >= 0) { Accumulate(B, Start); B = -1; } }
	};
}
