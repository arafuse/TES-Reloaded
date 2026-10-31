// Grass wind sway, shared by every GRASS vertex shader override.
//
// The engine swings blades along WindData.xy, which the plugin points downwind (see Wind.h) in
// place of the stock north axis. The bias keeps the swing mostly on the downwind side, so the
// grass leans with the wind instead of only rocking about upright.

#define GrassWindSwing 0.6
#define GrassWindLean  0.5

// Phase : wrapped sway phase in radians
// Returns the sway weight to scale by WindData.z * WindData.xy.
float GrassWindSway(float Phase) {
    return sin(Phase) * GrassWindSwing + GrassWindLean;
}
