#pragma once

/// Engine side of the grass trail: owns the two trail textures, feeds actor stamps into
/// GrassTrailMap, uploads only the dirty bands and exposes the shader constants.
namespace GrassTrail {

/// One actor pressing grass this frame.
struct Stamper { UInt32 RefId; float X, Y, RadiusScale; };

/// Per-frame constants: Window = (originX, originY, 4096, 1/4096); Time = (nowN, period, 1/recovery, omega).
struct Constants { D3DXVECTOR4 Window, Time; };

/// Creates the trail textures; must run before any shader record loads its samplers.
void Create(IDirect3DDevice9* Device);

/// Advances the trail clock, stamps the actors, uploads dirty bands and fills Out.
void Update(const Stamper* Stampers, int Count, float PlayerX, float PlayerY, const void* Space, Constants& Out);

/// True when the tall-grass bending layer is usable (textures exist and vertex texture fetch is supported).
bool TallEnabled();

/// True when the groundcover pressing layer is usable.
bool GroundEnabled();

/// True when live tall-grass trail overlaps the world-space box.
bool TallReaches(float MinX, float MinY, float MaxX, float MaxY);

/// True when live groundcover trail overlaps the world-space box.
bool GroundReaches(float MinX, float MinY, float MaxX, float MaxY);

/// The tall-grass trail texture, or NULL.
IDirect3DTexture9* TallTexture();

/// The groundcover trail texture, or NULL.
IDirect3DTexture9* GroundTexture();

}
