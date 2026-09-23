#pragma once

/// Bends small SpeedTree trees and shrubs away from nearby actors, complementing grass collision.
/// Installs the per-tree hooks: SpeedTreeBranchShader vtable slot 13 (SetupTransformations) and the
/// SpeedTreeLeafShader UpdatePipeline body. Tree draws are batched, so SetupShaderPrograms only
/// sees each batch's first tree. Oblivion only; needs the grass shaders for its collision sources.
void CreateTreeCollisionHook();

/// Uploads the collision constants (vertex c240-c243) for the tree geometry about to draw. Always
/// uploads: a tree that shouldn't bend gets zeros, since constants persist from the last tree.
/// Active is false for callers that must not bend this draw (cached shadow bakes).
void SetTreeCollisionConstants(NiGeometry* Geometry, const NiTransform* WorldTransform, bool Active);
