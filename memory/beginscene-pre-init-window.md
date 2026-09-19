---
name: beginscene-pre-init-window
description: "ShaderManager::BeginScene fires on the main menu before Player/Tes exist, so any per-frame hook must null-guard those globals"
metadata:
  type: project
---

`ShaderManager::BeginScene` (called from `RenderHook::TrackBeginScene`) is a per-frame D3D render
hook with **no notion of whether a game is loaded**. It fires during the main menu and the intro,
before the engine has constructed `PlayerCharacter` and `TES`. Both are plain globals initialised to
`NULL` at the top of `Game.cpp` and only assigned by `GameInitialization::TrackNewPlayerCharacter` /
`TrackNewTES`.

**How to apply:** any code added to `BeginScene` (or any other per-frame hook) that touches `Player`
or `Tes` must guard first. The established idiom is `if (!Player || !Player->parentCell) return false;`
(ShadowManager.cpp).

Symptom when missed: access violation reading `0x00000040` on the first frame of the main menu —
`parentCell` at offset 0x40 off a null `this` inside `PlayerCharacter::GetWorldSpace()`.
