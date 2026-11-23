---
name: framerate-manager-timing
description: FrameRateManager::ElapsedTime is per-frame seconds (clamped 0.25) updated in TrackRender; Time is never assigned; when to use GetPerformance() + LastMs instead
metadata:
  type: project
---

`FrameRateManager::ElapsedTime` is the seconds between the last two rendered frames,
clamped to `kMaxElapsedTime` (0.25 s) so load screens and hitches don't jump timers.
`UpdateFrameTime()` sets it once per frame at the top of the Oblivion
`RenderHook::TrackRender`. Readers: Dodge.cpp (double-tap window), EquipmentManager.cpp
(DualBlockDelay), ScriptManager.cpp (Purger timer, swimming-equipment 2 s grace),
which accumulate it per call and ASSUME one call per frame (the RunScripts hook is
per frame; the Dodge and ProcessControlAttack call rates are unverified).

`FrameRateManager::Time` is **never assigned** (always 0) and feeds the
`Tick.x` / `GameTime.z` shader constants. No shader reads those components.

**Why:** the clamp and once-per-frame semantics matter for any new reader.

**How to apply:** use ElapsedTime only from code that runs exactly once per rendered
frame. For anything called more or less often, or that needs unclamped wall time,
follow the ShadowManager idiom: diff `TheFrameRateManager->GetPerformance()`
(milliseconds since startup) against a stored `LastMs` and clamp it yourself (see
`ShadowManager::PointFadeLastMs`, `Wind.cpp`, `WetActors.cpp`). The FPS overlay in
GameMenuManager.cpp averages GetPerformance() over 10 frames.
