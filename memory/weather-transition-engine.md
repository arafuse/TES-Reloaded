---
name: weather-transition-engine
description: "RE'd Oblivion weather transition - percent is GAME-HOURS based (fWeatherTransMin/Max × transDelta/255), per-frame seam at 0x543004, and every engine path that snaps weatherPercent to 1.0"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 57c88b05-51ab-4cd6-a9ce-28f6e179103c
  modified: 2026-09-12T23:41:03.777Z
---

Disassembled 2026-09-12 (Oblivion.exe + community PDB, see [[oblivion-pdb-symbols]]). Oblivion `Sky` layout is the OBLIVION block in Game.h ([[gameh-multigame-blocks]]): first=+0x10, second(prev)=+0x14, next=+0x18, override=+0x1C, gameHour=+0xD0, transStartHour=+0xD4, weatherPercent=+0xD8, accelBasePct=+0xF4, flags=+0xFC. `TESWeather::transDelta` = byte +0x4B.

**Per-frame:** `Sky::Update` 0x542F20 writes hour to +0xD0, then `call 0x5422F0` at **0x543004** (transition), then 0x5418F0 / 0x541DD0 / 0x53FF90 / 0x542590 and the atmosphere/stars/sun/clouds/moon vtable updates — all read first/second/percent AFTER 0x5422F0 returns, so a post-fixup on that call site reaches every engine consumer and OR's `UpdateConstants`. 0x540850 (HDR param blend) is called *inside* 0x5422F0.

**Percent formula (0x5424B2):** `pct = (hour + (start>hour ? 24 : 0) - start) / (fWeatherTransMin + (fWeatherTransMax - fWeatherTransMin) * transDelta/255)`. Settings at 0xB36630 (Min, def 0.01), 0xB36628 (Max, def 0.25), 0xB36638 (fWeatherTransAccel, def 4) — units are **game hours**, so real duration scales with timescale (ts 30: 1.2 s at transDelta 0, 30 s at 255). pct>1 → clamps to 1.0, clears second (+0x14) and flag 8.

**New weather is only accepted when second==NULL** (0x542430) unless interrupt flag 0x10.

**Snap paths (pct := 1.0 with second := NULL, i.e. instant):**
- `ForceWeather` 0x542260 (Cmd_ForceWeather 0x500D90; SetWeather cmd 0x507AE0 when no current weather; OR's GameMenuManager).
- Interrupt flag 0x10 set by 0x53FBB0, called only from sub_66F420 around a player MoveTo (teleport/load) → 0x54244E nulls second.
- Game-hour jump (wait/sleep/fast travel/set GameHour) → pct >1 in one frame.
- Low transDelta in plugin defs → 0.01 game-hour transitions.
- Accel: SetWeather (0x507AE0) calls 0x53FB60 → flag 8 → remaining transition runs (1+Accel)=5× faster (continuous, not a snap).
- Start hour +0xD4 is NOT reset on weather change when sub_45A500(SaveLoad_CurrentSavegame) is true (load in progress).

OR's WeatherMode (INI TransDelta override) is **disabled** in Adam's install, so plugin weather defs govern transDelta.

**Save load:** `SaveLoad_LoadGame` → sub_5437C0 sets `(*0xB33B00)+0x18 |= 0x400` around its Sky::Update call — a reliable "snap, don't blend" signal. sub_65F770 also calls Sky::Update directly (likely wait/sleep time advance).

Implemented as `WeatherSmoothing.cpp` (INI `Main.WeatherMinTransitionTime`, default 1.0 s): rel-call at 0x543004, rate-limits percent and restores secondWeather, re-runs 0x540850, logs `[WeatherSmoothing] <cause>` lines. Volumetric fog `Data.w` became a 0–1 weight lerped by percent (also scales `Data.z` and the shadow precip-darkness blend).

OR-side discontinuities that engine smoothing will NOT fix: `UpdateVolumetricFog` flips Data.w 1→0 on the exact frame pct==1.0 when fog far > Fog.ini MaxDistance (100000); `UpdateExteriorLighting` blends from its own `pWeather` (updated only when pct==1.0), not the engine's secondWeather.
