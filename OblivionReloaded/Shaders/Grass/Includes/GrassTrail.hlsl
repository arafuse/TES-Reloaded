// Grass trail map helpers, shared by the grass vertex shaders and the groundcover pixel shaders.
//
// The CPU keeps a toroidal 512x512 map of 8-unit cells holding each cell's peak deformation and
// the time it was pressed; the shader derives the recovery from the age. The includer declares
// TESR_GrassTrailWindow (origin XY, size, 1/size) and TESR_GrassTrailTime (nowN, period,
// 1/recovery, omega) and the trail sampler.

// Scale of the ground trail's signed pull channels; must match GrassTrailMap::kPullScale.
static const float GrassTrailPullScale = 32.0;

// The stored time wraps every period: a bilinear fetch straddling a cell stamped just before the wrap and
// one just after blends the time to ~0.5 and reads as expired, a one-cell gap once per period.

// TimeN : a cell's stored press time, as a fraction of the time period
// Returns the recovery weight: 1 when freshly pressed, dipping negative past upright for a spring
// overshoot, and 0 once the recovery time has elapsed.
float GrassTrailSpring(float TimeN) {
    float age = frac(TESR_GrassTrailTime.x - TimeN) * TESR_GrassTrailTime.y;
    float t = age * TESR_GrassTrailTime.z;
    return t < 1.0 ? (1.0 - t) * cos(TESR_GrassTrailTime.w * t) : 0.0;
}

// TimeN : a cell's stored press time, as a fraction of the time period
// Returns the groundcover recovery weight: the springiness-0 curve, never negative, 0 once recovered.
float GrassTrailGroundSpring(float TimeN) {
    float t = frac(TESR_GrassTrailTime.x - TimeN) * TESR_GrassTrailTime.y * TESR_GrassTrailTime.z;
    return t < 1.0 ? (1.0 - t) * cos(1.5707963 * t) : 0.0;
}

// WorldXY : world position being shaded
// Returns 0 at the trail window's edge rising to 1 over the last 64 units, hiding the seam.
float GrassTrailEdgeFade(float2 WorldXY) {
    float2 local = WorldXY - TESR_GrassTrailWindow.xy;
    float edge = min(min(local.x, local.y), TESR_GrassTrailWindow.z - max(local.x, local.y));
    return saturate(edge / 64.0);
}

// WorldXY : world position being shaded
// Returns the tex2Dlod coordinate of that position in the trail map.
float4 GrassTrailUV(float2 WorldXY) {
    return float4(WorldXY * TESR_GrassTrailWindow.w, 0, 0);
}
