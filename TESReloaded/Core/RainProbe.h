#pragma once

/// Rain exposure for scripts: while rain falls, a GPU probe tests the precipitation occlusion map at
/// points on the player's body, each pushed out along the wind-slanted fall line to the body's
/// surface, so the answer matches the rain that is drawn. Oblivion only.

/// Arms or disarms the probe for this frame and publishes 0 whenever rain can't reach the player (no
/// rain, interior, Precipitations off). Main thread, from ShaderManager::UpdateConstants.
void UpdateRainProbe();

/// Publishes the result of any finished probe, then renders this frame's probe. Main thread, from
/// ShaderManager::RenderEffects straight after the rain effect, with the effect quad bound. Restores
/// render target 0 and the depth-stencil itself before returning.
void RenderRainProbe(IDirect3DDevice9* Device);

/// True if falling rain reached any probe point on the player at the last readback, which trails
/// rendering by a frame or two.
bool IsRainHittingPlayer();
