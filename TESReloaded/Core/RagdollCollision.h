#pragma once

/// Makes walking actors (character controllers) collide with ragdoll bones, so they push
/// ragdolled actors aside instead of walking through them. Oblivion only.
void CreateRagdollCollisionHook();
