---
name: framerate-elapsedtime-is-dead
description: FrameRateManager::Time and ElapsedTime are never assigned - always 0; use GetPerformance() for delta time
metadata:
  type: project
---

`FrameRateManager::ElapsedTime` and `::Time` are initialised to 0.0 in the constructor
and **never assigned anywhere else in the repo**. Several callers still read them
(Dodge.cpp, EquipmentManager.cpp, ScriptManager.cpp, and GameMenuManager.cpp:456 does
`1.0 / ElapsedTime`), so those paths are silently broken or dead.

**Why:** it looks like the obvious delta-time source and is not one.

**How to apply:** for per-frame timing, follow the ShadowManager idiom instead - take
`TheFrameRateManager->GetPerformance()` (milliseconds since startup) and diff it
against a stored `LastMs`, clamping the result for hitched/paused frames. See
`ShadowManager::PointFadeLastMs` / `StaticFadeLastMs` and the grass stamp aging in
`ShaderManager.cpp`.
