#pragma once

class NiD3DVertexShaderEx;
class NiD3DPixelShaderEx;

// --- Main-pass draw census (Develop.ProfileFrame + ProfileDrawCensus) ----------
// Answers "would batching the engine's static draws pay off?" before any batching is built. Every
// hooked SetupShaderPrograms call in the main scene (the far pass when the near shell is on) and in
// the shell pass is classified by where its geometry hangs in the scene graph, and the report shows
// per class how far instancing or same-material merging could collapse the draws, plus the CPU time
// of the nested engine call. That time excludes the engine's accumulator and sort work before the
// hook, so it is a floor for the per-draw engine cost, not the whole of it. The water reflection
// pass gets the same table, and main-pass draws are also binned by their bound's on-screen size.
namespace DrawStats {

	enum Pass { Pass_Main, Pass_Shell, Pass_Reflection, Pass_COUNT };

	enum DrawClass {
		Class_Statics,     // loaded-grid geometry that is not an actor, land or a tree
		Class_DistantLOD,  // DistantRefLOD, except LOD Trees
		Class_LODTrees,    // DistantRefLOD's "LOD Trees" merged meshes
		Class_Land,        // loaded-grid "Block (X, Y)" terrain
		Class_LandLOD,     // Tes->LODRoot (the node named LandLOD)
		Class_Trees,       // SpeedTree branch/leaf batches (one hook call per batch)
		Class_Grass,       // grass batches (one hook call per batch)
		Class_Actors,      // skinned, or under a Bip01 skeleton node
		Class_Water,       // WATER* pixel shaders and LODWaterRoot
		Class_Other,       // sky, weather, magic projectiles, anything unplaced
		Class_COUNT
	};

	/// Classifies one draw and records its keys. Returns the draw's pass/class slot, or -1 when the
	/// profiler is disarmed or the draw is outside the main and shell passes.
	int Record(void* Shader, NiGeometry* Geometry, NiSkinInstance* SkinInstance, NiPropertyState* PropertyState,
		NiD3DPass* ShaderPass, NiD3DVertexShaderEx* VertexShader, NiD3DPixelShaderEx* PixelShader);

	/// Adds the nested engine call's QPC ticks to a slot returned by Record.
	void AddEngineTicks(int Slot, LONGLONG Ticks);

	/// Folds this frame's distinct counts into the report window; once per armed frame.
	void FrameEnd();

	/// Logs the window's per-class tables and clears the window.
	void Report(int Frames, LONGLONG QpcFreq);
}
