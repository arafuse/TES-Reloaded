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

		// --- detail, nested inside Buck_HookMidScene ---
		Buck_MidResolve,          // ResolvePreWaterDepthBuffer
		Buck_MidSetup,            // state capture, camera refresh, render state, seed blit
		Buck_MidSun,              // sun shadow apply (SetCT + Render)
		Buck_MidProbe,            // RenderSunShadowProbe, including its readback
		Buck_MidProbeRead,        // the probe's event-query poll + LockRect readback (nested in Buck_MidProbe)
		Buck_MidPoint,            // point-light shadow apply (SetCT + Render)
		Buck_MidRestore,          // CachedStateBlock->Apply

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
		Cnt_GrassBatches,   // main-pass grass batches (TallGrassShader instance staging calls)
		Cnt_GrassInstances, // instances in those batches (batch used count)
		Cnt_GrassTris,      // instances x the grass type's triangles per instance
		// ProfileFrame = 2 only: main-pass grass instances by distance from the camera.
		Cnt_GrassNear1k, Cnt_Grass1to3k, Cnt_Grass3to6k, Cnt_Grass6to9k, Cnt_GrassBeyond9k,
		Cnt_TreeItemsNear,  // tree geometry redrawn into the near map by the dynamic-tree composite
		Cnt_TreeItemsFar,   // the same for the far map
		Cnt_ActorItems,     // casters drawn into the actor (skin) overlay map
		Cnt_COUNT
	};

	// GPU timestamp ranges. Each is timed at most once per frame (a second Begin in
	// the same frame is ignored) and is reported as an average over the frames that
	// issued it. Nesting is noted per bucket; nested ranges are not disjoint.
	enum GpuBucket {
		Gpu_ShadowMaps,     // ShadowManager::RenderShadowMaps, our part only
		Gpu_ShadowExt,      // nested in Gpu_ShadowMaps: exterior sun maps
		Gpu_ShadowPoint,    // nested in Gpu_ShadowMaps: point-light cube maps
		Gpu_MainPass,       // the main WorldSceneGraph RenderObject (the far pass when the shell is on)
		Gpu_MidSun,         // nested in Gpu_MainPass: sun shadow apply quad
		Gpu_MidPoint,       // nested in Gpu_MainPass: point shadow apply quad
		Gpu_Shell,          // the near-shell RenderObject
		Gpu_FirstPerson,    // the first-person RenderObject
		Gpu_TreesNear,      // nested in Gpu_ShadowExt: near-map dynamic-tree composite (copy + tree draws)
		Gpu_TreesFar,       // nested in Gpu_ShadowExt: far-map dynamic-tree composite
		Gpu_ActorOverlay,   // nested in Gpu_ShadowExt: actor (skin) overlay map
		Gpu_COUNT
	};

	// Armed by FrameBegin for the duration of one TrackRender. Exposed so the RAII
	// scope and the counters inline their disabled path down to one predictable
	// branch - they sit on per-draw code paths that run thousands of times a frame.
	extern bool Active;
	extern unsigned int Counters[Cnt_COUNT];

	void FrameBegin();                          // top of TrackRender: re-reads the INI flag, arms timing
	void FrameEnd();                            // end of TrackRender: accumulates, logs every report window
	void Accumulate(int bucket, LONGLONG start); // out-of-line: only ever called while Active
	void GpuBegin(int bucket);                  // issues the range's start timestamp; no-op when disarmed
	void GpuEnd(int bucket);                    // issues the range's end timestamp; no-op unless GpuBegin issued

	// Per-shader GPU breakdown of Gpu_MainPass (ProfileFrame = 2). Segment flags:
	enum ShaderSegmentFlags {
		Seg_Actor = 1, Seg_Tree = 2, Seg_Grass = 4, Seg_NearWater = 8, Seg_OurPS = 16, Seg_OurVS = 32,
		Seg_MidScene = 64, // the mid-scene sun/point shadow apply, not a draw
		// One grass batch: never merged with the previous segment, so each batch is timed on its own.
		// The last one before a segment without Seg_Grass is relabelled to the SetGrassTailKey key.
		Seg_GrassBatch = 128,
	};
	/// Key and name the last grass batch's segment is relabelled to: it also holds whatever the GPU
	/// runs between that batch's draw and the next hooked draw.
	void SetGrassTailKey(const void* Key, const char* Name);
	extern bool ShaderDetail;                   // armed by FrameBegin when ProfileFrame >= 2

	/// Called before each main-pass draw (and before the mid-scene apply, with Key = NULL and
	/// Seg_MidScene). A key change closes the running segment with a timestamp and opens a new one;
	/// the segments only run while Gpu_MainPass is open, and its GpuEnd closes the last one.
	void GpuShaderSegment(const void* Key, const char* PixelName, const char* VertexName, unsigned int Flags);

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

	/// RAII GPU timestamp range; costs one branch when the profiler is disarmed.
	struct GpuScope {
		int B;
		GpuScope(int bucket) : B(Active ? bucket : -1) { if (B >= 0) GpuBegin(B); }
		~GpuScope() { if (B >= 0) GpuEnd(B); }
	};
}
