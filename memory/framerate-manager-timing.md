---
name: framerate-manager-timing
description: FrameRateManager::ElapsedTime is per-frame seconds (clamped 0.25) updated in TrackRender; Time is still never assigned; when to use GetPerformance() + LastMs instead
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

History: the original fork set it from `ShaderManager::UpdateConstants`; upstream
commit 627250a deleted that update and left the readers, so from 2021 until
2026-11-21 it was always 0. While it was 0 the FPS overlay showed -2^31, the Purger
timer never fired, the double-tap window never expired, holding block with a left
weapon never blocked, and swimming equipment never swapped back.

`FrameRateManager::Time` is **still never assigned** (always 0) and feeds the
`Tick.x` / `GameTime.z` shader constants. No shader reads those components.

**Why:** ElapsedTime looks like the obvious delta-time source and once wasn't one.
The clamp and once-per-frame semantics matter for any new reader.

**How to apply:** use ElapsedTime only from code that runs exactly once per rendered
frame. For anything called more or less often, or that needs unclamped wall time,
follow the ShadowManager idiom: diff `TheFrameRateManager->GetPerformance()`
(milliseconds since startup) against a stored `LastMs` and clamp it yourself (see
`ShadowManager::PointFadeLastMs`, `Wind.cpp`, `WetActors.cpp`). The FPS overlay in
GameMenuManager.cpp averages GetPerformance() over 10 frames.
