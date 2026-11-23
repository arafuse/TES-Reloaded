#pragma once

// --- hkNiTriStripsShape strip lookup cache ([Main] HavokStripCache) ------------------------------
// hkNiTriStripsShape's triangle lookup (0x915D00) finds a shape key's strip by walking the strip
// lengths from strip 0 (0x915D60-0x915D9F). In the dense town one 143-strip mesh takes ~20k of
// those walks a frame (~1 ms). This module replaces the loop with an O(1) table lookup that returns
// exactly the engine's answer (HavokStripMath.h), built per NiTriStripsData on the main thread and
// dropped when the engine destroys that data. Design: docs/superpowers/specs/2026-11-21-havok-strip-cache-design.md.

/// Patches the loop and hooks ~NiTriStripsData. Logs and installs nothing if the patch site holds
/// unexpected bytes.
void CreateHavokStripCacheHook();

namespace HavokStripCache {

	/// Once per frame from RenderHook::TrackRender: adopts the render thread as the main thread on
	/// the first call, and logs the verify stats every 600 frames when Develop.HavokStripCacheVerify is set.
	void OnFrame();

	/// True once the patch and the destructor hook are in place.
	bool Installed();
}
