---
name: beginscene-pre-init-window
description: "ShaderManager::BeginScene fires on the main menu before Player/Tes exist, so any per-frame hook must null-guard those globals"
metadata: 
  node_type: memory
  type: project
  originSessionId: 5f87d0d8-288c-4806-b5bb-c551a26b996a
  modified: 2026-08-15T18:27:12.094Z
---

`ShaderManager::BeginScene` (called from `RenderHook::TrackBeginScene`) is a per-frame D3D render
hook with **no notion of whether a game is loaded**. It fires during the main menu and the intro,
before the engine has constructed `PlayerCharacter` and `TES`. Both are plain globals initialised to
`NULL` in `Game.cpp:3` and only assigned by `GameInitialization::TrackNewPlayerCharacter` /
`TrackNewTES`.

Any code added to `BeginScene` (or to any other per-frame hook) that touches `Player` or `Tes` must
guard first. The established idiom in this codebase is at `ShadowManager.cpp:1470`
(`if (!Player || !Player->parentCell) return false;`) and `ShaderManager.cpp:1890`
(`if (Player && Player->parentCell)`).

Symptom when missed: access violation reading `0x00000040` on the first frame of the main menu —
that is `parentCell` at offset 0x40 off a null `this` inside `PlayerCharacter::GetWorldSpace()`
(`Game.h:6804`).
