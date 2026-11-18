#pragma once

/// Bends small SpeedTree trees and shrubs away from nearby actors, complementing grass collision.
/// Installs the per-tree hooks: SpeedTreeBranchShader vtable slot 13 (SetupTransformations) and the
/// SpeedTreeLeafShader UpdatePipeline body. Tree draws are batched, so SetupShaderPrograms only
/// sees each batch's first tree. Oblivion only. The bend comes from the shrub layer of the grass trail
/// map, sampled per vertex; without grass shaders or vertex texture fetch it is off and trees upload zeros.
void CreateTreeCollisionHook();

/// Uploads the collision constants (vertex c240-c243: the tree's model-to-world transform and the
/// bend strengths) for the tree geometry about to draw. Always uploads: a tree that shouldn't bend
/// gets zeros, since constants persist from the last tree.
/// Active is false for callers that must not bend this draw (cached shadow bakes).
void SetTreeCollisionConstants(NiGeometry* Geometry, const NiTransform* WorldTransform, bool Active);

/// True if live shrub trail would bend this tree geometry right now, i.e. SetTreeCollisionConstants
/// with Active = true would upload a set live flag. Batched tree draws cannot bend per tree,
/// so such trees draw on their own.
bool TreeCollisionReaches(NiGeometry* Geometry, const NiTransform* WorldTransform);
