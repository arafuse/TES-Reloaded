#pragma once

void CreateRenderHook();

/// Model-space size of a grass batch's clump model.
struct GrassModelMetrics {
	float Height;		///< Highest vertex z, or the bound top without a CPU copy; FLT_MAX without geometry data
	float HalfWidth;	///< Largest horizontal vertex distance from the model origin, or the bound radius
};

/// Returns the cached size of a grass batch geometry's model. Main thread only.
GrassModelMetrics GetGrassModelMetrics(NiGeometry* Geometry);