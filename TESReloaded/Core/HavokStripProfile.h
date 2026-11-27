#pragma once

// --- hkNiTriStripsShape triangle lookup profile (Develop.ProfileFrame) -----------------------------
// hkNiTriStripsShape's child-shape lookup (vtable 0xA9D010 slot 10, 0x915D00) finds a shape key's
// triangle by walking the strip-length array from the first strip, so each call costs O(strips in
// that NiTriStripsData). This profile counts the calls, the strips each one walked, the strip data
// they walked and the engine call sites.
//
// A bare hkNiTriStripsShape has no BV tree, so every Havok query against it sweeps all of its keys
// (getFirstKey, slot 8, then getNextKey, slot 9, until -1). The profile also times each full sweep,
// per shape and per engine sweep site, net of its own recording, and names the loaded references
// whose collision graph reaches the top shapes (a heuristic pointer search that also matches unrelated
// references, cached per shape pointer for the session).
//
// The hooks are vtable patches installed the first time the frame profiler arms and left in place;
// while the profiler is off they only forward. Recording re-walks the strips, so the update phase is
// slower while profiling. Only main-thread calls are recorded; others are counted and dropped.
namespace HavokStripProfile {

	/// Turns recording on or off; the first call with Enabled set installs the hooks. Main thread only.
	void SetEnabled(bool Enabled);

	/// Logs the calls and sweeps recorded since the last report, per frame over Frames, and clears them.
	void Report(int Frames);
}
