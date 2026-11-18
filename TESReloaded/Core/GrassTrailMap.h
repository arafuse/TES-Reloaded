#pragma once

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

/// CPU-side grass trail map: a toroidal 512x512 grid of 8-unit cells that remembers where actors
/// pressed the grass. Each layer is a RGBA16 image the renderer uploads as a texture, and the
/// spring recovery is evaluated per cell from its press time, so nothing is rewritten while the
/// grass springs back. Free of D3D so it can be tested standalone.
namespace GrassTrailMap {

/// Cells per side of the square window.
const int   kSize = 512;
/// World units per cell.
const float kCellSize = 8.0f;
/// Seconds after which the stored press time wraps.
const float kTimePeriod = 256.0f;
/// Per-axis distance from the window centre at which the window recentres on the player.
const float kRecentreDistance = 512.0f;
/// Rows swept for expired cells per layer per BeginFrame.
const int   kSweepRowsPerFrame = 8;
/// Ground pull vectors are stored divided by this.
const float kPullScale = 32.0f;
/// Lower clamp of the recovery time in seconds.
const float kMinRecovery = 0.05f;
/// Upper clamp of the recovery time in seconds.
const float kMaxRecovery = kTimePeriod * 0.5f;

/// Trail layers: tall grass bending and ground cover pressing.
enum LayerId { Layer_Tall, Layer_Ground, Layer_Count };

/// Spring recovery weight at Age seconds after a press: 1 at Age 0, 0 at Age >= Recovery (or negative Age).
float SpringWeight(float Age, float Recovery, float Springiness);
/// Angular frequency of the spring for a springiness in [0, 1].
float SpringOmega(float Springiness);

/// An actor stamping the trail: world position and the radius it affects each layer to.
struct Source { float X, Y, TallRadius, GroundRadius; };
/// Half-open rectangle of texture cells.
struct Rect { int X0, Y0, X1, Y1; };
/// World-space axis-aligned bounds.
struct WorldBounds { float MinX, MinY, MaxX, MaxY; };
/// Decoded cell: signed vector in [-1, 1], peak and press time in [0, 1].
struct CellValue { float VecX, VecY, Peak, TimeN; };

/// The two-layer trail map.
class Map {
public:
	/// Creates the map in the Reset state.
	Map();
	/// Clears both layers, forgets the window and marks everything dirty.
	void Reset();
	/// Advances the clock, recentres the window on the player and sweeps expired rows.
	void BeginFrame(float PlayerX, float PlayerY, double Now, float Recovery, float Springiness);
	/// Stamps a source into both layers.
	void Stamp(const Source& S);
	/// Appends merged dirty bands (texture cells) of a layer, then clears that layer's dirty state.
	void CollectDirty(LayerId Layer, std::vector<Rect>& Out);
	/// Texel data of a layer, kSize*kSize*4 values, row-major, memory order R,G,B,A.
	const uint16_t* Data(LayerId Layer) const;
	/// World bounds of live peaks (padded by the bilinear footprint), intersected with the window; false when none.
	bool LiveBounds(LayerId Layer, WorldBounds& Out) const;
	/// World X of the window corner.
	float OriginX() const;
	/// World Y of the window corner.
	float OriginY() const;
	/// Quantised current time fraction in [0, 1], the value the shader receives.
	float NowN() const;
	/// Clamped recovery time in seconds.
	float Recovery() const;
	/// Springiness in [0, 1].
	float Springiness() const;
	/// Decodes the cell containing a world position; zeros outside the window.
	CellValue Decode(LayerId Layer, float WorldX, float WorldY) const;
	/// Peak * spring at a world position; the ground layer is clamped at 0; zero outside the window.
	float CurrentWeight(LayerId Layer, float WorldX, float WorldY) const;

private:
	struct Layer {
		std::vector<uint16_t> Cells;
		std::vector<char> Live;
		std::vector<int> LiveMin, LiveMax;
		std::vector<int> DirtyMin, DirtyMax;
	};

	void ClearLayerAll(Layer& L);
	void ClearColumns(int CellStart, int Count);
	void ClearRows(int CellStart, int Count);
	void ClipLiveSpans(int MinCell, int MaxCell);
	void MarkDirty(Layer& L, int Row, int Col);
	void SweepLayer(Layer& L);
	float AgeOf(uint16_t TimeValue) const;
	float Weight(LayerId Id, uint16_t Peak, uint16_t TimeValue) const;
	void StampLayer(LayerId Id, const Source& S, float Radius);
	bool InWindow(int CellX, int CellY) const;

	Layer Layers[Layer_Count];
	bool HasWindow;
	int OriginCellX, OriginCellY;
	int SweepCursor;
	float NowNValue;
	float RecoveryValue;
	float SpringinessValue;
};

}
