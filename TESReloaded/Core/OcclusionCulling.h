#pragma once

/// Occlusion culling for the main scene's far pass. Each frame, every cullable reference in view is
/// tested with one occlusion query: its bound, as a screen rect at its nearest depth, against the
/// finished far-pass depth. The next frame's far pass skips the references that came back with no
/// pixels by setting AppCulled on their roots for that pass only. Hidden objects that come back into
/// view are drawn one frame late. Oblivion only; inert while [Main] OcclusionCulling is 0.
/// Actors (characters and creatures) follow [Main] OcclusionCullActors: 0 leaves them out, 1 queries
/// and counts them without culling, 2 (default) culls them; their query bound grows by
/// OcclusionActorMargin because they move during the frame of latency.
/// All functions run on the main thread, from RenderHook's TrackRenderObject.

/// Before the far pass: reads last frame's queries, walks the loaded cells for candidates and sets
/// AppCulled on those that were hidden.
void BeginOcclusionCull();

/// Immediately after the far pass: clears exactly the AppCulled flags BeginOcclusionCull set.
void EndOcclusionCull();

/// After the far-pass depth resolve and before the near shell's depth clear: queries every candidate's
/// bound against the far-pass depth still in the depth buffer.
void IssueOcclusionQueries();

/// Drops every pending result and the hidden set, so nothing measured before a load or new game is
/// used. From the co-save load and new-game callbacks.
void ResetOcclusionCull();

/// True between a BeginOcclusionCull that set flags and its EndOcclusionCull.
bool OcclusionCullFlagsHeld();
