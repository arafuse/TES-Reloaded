---
name: framerate-elapsedtime-is-dead
description: FrameRateManager::Time and ElapsedTime are never assigned - always 0; use GetPerformance() for delta time
metadata:
  node_type: memory
  type: project
  originSessionId: bf3a7c40-4d00-4883-9fd5-825f5a31cf09
  modified: 2026-11-21T20:41:49.340Z
---

`FrameRateManager::ElapsedTime` and `::Time` are initialised to 0.0 in the constructor
and **never assigned anywhere else in the repo**. Several callers still read them
(Dodge.cpp, EquipmentManager.cpp, ScriptManager.cpp), so those paths are silently
broken or dead. The FPS overlay in GameMenuManager.cpp used to do `1.0 / ElapsedTime`
(+inf cast to int = -2^31) and now averages GetPerformance() over 10 frames.

**Why:** it looks like the obvious delta-time source and is not one.

**How to apply:** for per-frame timing, follow the ShadowManager idiom instead - take
`TheFrameRateManager->GetPerformance()` (milliseconds since startup) and diff it
against a stored `LastMs`, clamping the result for hitched/paused frames. See
`ShadowManager::PointFadeLastMs` / `StaticFadeLastMs` and the grass stamp aging in
`ShaderManager.cpp`.
