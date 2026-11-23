#pragma once

// --- hkNiTriStripsShape triangle lookup profile (Develop.ProfileFrame) -----------------------------
// hkNiTriStripsShape's child-shape lookup (vtable 0xA9D010 slot 10, 0x915D00) finds a shape key's
// triangle by walking the strip-length array from the first strip, so each call costs O(strips in
// that NiTriStripsData). The sampler puts ~1 ms/frame of update time in that walk in the dense town.
// This profile counts the calls, the strips each one walked, the strip data they walked and the
// engine call sites, to show whether a cached lookup would remove that cost.
//
// The hook is a vtable patch installed the first time the frame profiler arms and left in place;
// while the profiler is off it only forwards. Recording re-walks the strips, so the update phase is
// slower while profiling. Only main-thread calls are recorded; others are counted and dropped.
namespace HavokStripProfile {

	/// Turns recording on or off; the first call with Enabled set installs the hook. Main thread only.
	void SetEnabled(bool Enabled);

	/// Logs the calls recorded since the last report, per frame over Frames, and clears them.
	void Report(int Frames);
}
