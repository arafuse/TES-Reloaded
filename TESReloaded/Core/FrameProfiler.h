#pragma once

// --- Whole-frame plugin CPU profiler (Develop.ProfileFrame) --------------------
// ProfileShadows and ProfileEffects each scope ONE subsystem in isolation. This
// profiler attributes wall-clock CPU across ALL top-level plugin per-frame entry
// points in a SINGLE view, so the relative cost of every plugin CPU consumer is
// visible at once, plus a residual ("Other") = FrameTotal - sum(plugin buckets)
// that captures the game's own rendering + any unhooked plugin work. The point is
// to answer one question: is there a plugin CPU hot spot OUTSIDE shadows/effects
// big enough (> ~20% of frame) to be worth moving off the render thread?
//
// All buckets except FrameTotal execute INSIDE the FrameTotal scope (TrackRender's
// envelope, which wraps the nested engine Render() where shadows and image-space
// effects run via their own hooks), so the subtraction is well defined.
//
// The RAII Scope issues NO QueryPerformanceCounter calls when disabled, so it is
// free in normal builds. Definitions live in RenderHook.cpp (an always-compiled
// TU); cross-file call sites just include this header.
namespace FrameProfiler {
	enum Bucket {
		Buck_FrameTotal,          // whole RenderHook::TrackRender (plugin per-frame envelope)
		Buck_UpdateShaderStates,  // ShaderManager::UpdateShaderStates
		Buck_SetSceneGraph,       // RenderManager::SetSceneGraph
		Buck_UpdateConstants,     // ShaderManager::UpdateConstants
		Buck_ShadowMaps,          // ShadowManager::RenderShadowMaps (CPU draw submission)
		Buck_Effects,             // ShaderManager::RenderEffectsPre/PostHdr (CPU side of post-FX)
		Buck_SetupShaderPrograms, // per-geometry shader hook, summed over all draws this frame
		Buck_COUNT
	};

	bool Enabled();               // true once FrameBegin has armed the current frame
	void FrameBegin();            // top of TrackRender: re-reads the INI flag, arms timing
	void FrameEnd();              // end of TrackRender: averages + logs every ReportFrames

	struct Scope {
		int      B;
		LONGLONG Start;
		Scope(int bucket);
		~Scope();
	};
}
