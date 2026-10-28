#include "Wind.h"

static const UInt32	kGrassWindAxisCall		= 0x007E6807; // TallGrassShader slot 12: D3DXVec3TransformNormal of world (0,1,0) into patch space
static const float	kWindTwoPi				= 2.0f * D3DX_PI;
static const float	kWindMaxTurn			= 0.5f * D3DX_PI;
static const float	kWindTurnSeconds		= 20.0f;
static const float	kWindDriftRadians		= 0.25f;
static const float	kWindDriftPeriodHours	= 7.0f;
static const double	kWindMaxStepMs			= 100.0;

static bool			WindInitialized	= false;
static TESWeather*	WindWeather		= NULL;
static float		WindHeading		= 0.0f;
static float		WindTarget		= 0.0f;
static double		WindLastMs		= -1.0;

static UInt32 HashWind(UInt32 Value) {

	Value ^= Value >> 16;
	Value *= 0x7FEB352D;
	Value ^= Value >> 15;
	Value *= 0x846CA68B;
	Value ^= Value >> 16;
	return Value;

}

static float HashUnit(UInt32 Seed) {

	return (HashWind(Seed) >> 8) / 16777215.0f;

}

// Replaces the engine's hardcoded north axis with the wind direction before it is taken into patch space.
static D3DXVECTOR3* __stdcall TransformGrassWindAxis(D3DXVECTOR3* Out, const D3DXVECTOR3* Axis, const D3DXMATRIX* WorldToPatch) {

	if (!WindInitialized) return D3DXVec3TransformNormal(Out, Axis, WorldToPatch);
	const D3DXVECTOR4& Wind = TheShaderManager->ShaderConst.WindData;
	D3DXVECTOR3 WindAxis(Wind.x, Wind.y, 0.0f);
	return D3DXVec3TransformNormal(Out, &WindAxis, WorldToPatch);

}

void CreateWindHook() {

	WriteRelCall(kGrassWindAxisCall, (UInt32)TransformGrassWindAxis);

}

void UpdateWind(ShaderConstants& ShaderConst, TESWeather* CurrentWeather) {

	double NowMs = TheFrameRateManager->GetPerformance();
	float GameHours = TimeGlobals::GetGameDaysPassed() * 24.0f + ShaderConst.GameTime.y;

	if (CurrentWeather && CurrentWeather != WindWeather) {
		float Roll = HashUnit(CurrentWeather->refID ^ (UInt32)(GameHours * 60.0f));
		if (WindInitialized)
			WindTarget = WindHeading + (Roll * 2.0f - 1.0f) * kWindMaxTurn;
		else
			WindHeading = WindTarget = Roll * kWindTwoPi;
		WindInitialized = true;
		WindWeather = CurrentWeather;
	}

	double StepMs = WindLastMs < 0.0 ? 0.0 : NowMs - WindLastMs;
	if (StepMs > kWindMaxStepMs) StepMs = kWindMaxStepMs;
	float StepSeconds = (float)(StepMs / 1000.0);
	WindLastMs = NowMs;
	WindHeading += (WindTarget - WindHeading) * (1.0f - expf(-StepSeconds / kWindTurnSeconds));

	float Wrap = floorf(WindHeading / kWindTwoPi) * kWindTwoPi;
	WindHeading -= Wrap;
	WindTarget -= Wrap;

	float Angle = WindHeading + kWindDriftRadians * sinf(GameHours * kWindTwoPi / kWindDriftPeriodHours);
	ShaderConst.WindData.x = cosf(Angle);
	ShaderConst.WindData.y = sinf(Angle);
	ShaderConst.WindData.z = ShaderConst.currentwindSpeed;
	ShaderConst.WindData.w = 0.0f;

}

float GetWindHeading() {

	if (!WindInitialized) return 0.0f;
	const D3DXVECTOR4& Wind = TheShaderManager->ShaderConst.WindData;
	float Heading = D3DXToDegree(atan2f(Wind.x, Wind.y));
	return Heading < 0.0f ? Heading + 360.0f : Heading;

}
