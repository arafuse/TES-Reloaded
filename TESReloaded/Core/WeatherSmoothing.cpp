#include "WeatherSmoothing.h"
#include "WeatherMode.h"

static const UInt32 kUpdateTransitionCall	= 0x00543004; // Call site inside Sky::Update (0x00542F20)
static const UInt32 kUpdateTransition		= 0x005422F0; // Picks the next weather and advances weatherPercent from game hours
static const UInt32 kUpdateHDRBlend			= 0x00540850; // Blends HDR parameters from the Sky weathers and weatherPercent
static const UInt32 kCurrentSavegame		= 0x00B33B00; // Pointer; flags at +0x18
static const float* kWeatherTransMax		= (float*)0x00B36628; // fWeatherTransMax (game hours)
static const float* kWeatherTransMin		= (float*)0x00B36630; // fWeatherTransMin (game hours)

static const UInt32 kSaveFlagLoadingSky		= 0x400; // Set by the save-load path around its Sky::Update call
static const UInt32 kSkyFlagAccelerated		= 0x08;
static const UInt32 kSkyFlagInterrupt		= 0x10;
static const double kMaxElapsedSeconds		= 0.1;	// A paused or hitching frame advances the blend by at most this
static const float	kTimeJumpHours			= 0.1f;	// Game-hour step in one frame treated as a wait/sleep/script jump

/// Transition as it is shown: blend from From to To by Percent. From is NULL when settled.
struct SmoothedTransition {
	TESWeather*	To;
	TESWeather*	From;
	float		Percent;
	float		LastHour;
	double		LastMs;
	bool		Valid;
	bool		Forced;		// The engine changed weather without blending from what was shown
	bool		Detached;	// The engine dropped From, so its percent no longer describes this blend
	bool		Logged;
};

static SmoothedTransition State = {};
static double MinTransitionSeconds = 1.0;
static LARGE_INTEGER CounterFrequency;

static double NowMs() {

	LARGE_INTEGER Counter;
	QueryPerformanceCounter(&Counter);
	return (double)Counter.QuadPart * 1000.0 / (double)CounterFrequency.QuadPart;

}

static const char* WeatherName(TESWeather* Weather) {

	return Weather ? ((TESWeatherEx*)Weather)->EditorName : "none";

}

static bool IsLoadingSky() {

	void* Savegame = *(void**)kCurrentSavegame;
	return Savegame && (*(UInt32*)((UInt32)Savegame + 0x18) & kSaveFlagLoadingSky);

}

static void LogIntervention(Sky* WorldSky, float EnginePercent, float HourDelta, double Elapsed) {

	const char* Cause = "short transition";
	if (State.Forced)
		Cause = "forced weather change";
	else if (HourDelta > kTimeJumpHours)
		Cause = "game time jump";
	else if (WorldSky->Flags0FC & kSkyFlagAccelerated)
		Cause = "accelerated transition";

	UInt8 TransDelta = State.To->transDelta;
	float DurationHours = *kWeatherTransMin + (*kWeatherTransMax - *kWeatherTransMin) * TransDelta / 255.0f;
	float HoursPerSecond = Elapsed > 0.0 ? HourDelta / (float)Elapsed : 0.0f;
	float DurationSeconds = HoursPerSecond > 0.0f ? DurationHours / HoursPerSecond : 0.0f;

	Logger::Log("[WeatherSmoothing] %s: %s (%08X) -> %s (%08X); engine percent %.3f, shown %.3f; transDelta %u = %.4f game h (~%.2f s); game hour step %.4f",
		Cause, WeatherName(State.From), State.From ? State.From->refID : 0, WeatherName(State.To), State.To->refID,
		EnginePercent, State.Percent, TransDelta, DurationHours, DurationSeconds, HourDelta);

}

/// Replaces the Sky::Update call to the transition update. Runs the engine update, then limits how
/// fast the shown transition may advance and writes it back before any sky/lighting consumer reads it.
static void __fastcall UpdateTransitionHook(Sky* WorldSky) {

	ThisCall(kUpdateTransition, WorldSky);

	double Now = NowMs();
	double Elapsed = min((Now - State.LastMs) / 1000.0, kMaxElapsedSeconds);
	State.LastMs = Now;
	float HourDelta = WorldSky->gameHour - State.LastHour;
	if (HourDelta < 0.0f) HourDelta += 24.0f;
	State.LastHour = WorldSky->gameHour;

	TESWeather* EngineTo = WorldSky->firstWeather;
	TESWeather* EngineFrom = WorldSky->secondWeather;
	float EnginePercent = WorldSky->weatherPercent;

	if (!State.Valid || !EngineTo || !State.To || (WorldSky->Flags0FC & kSkyFlagInterrupt) || IsLoadingSky()) {
		State.To = EngineTo;
		State.From = EngineFrom;
		State.Percent = EngineFrom ? EnginePercent : 1.0f;
		State.Valid = EngineTo != NULL;
		State.Detached = false;
		State.Logged = true;
		return;
	}

	if (EngineTo != State.To) {
		if (State.From && EngineTo == State.From) {
			// Reversing: blend(A, B, p) == blend(B, A, 1 - p), so nothing jumps.
			State.From = State.To;
			State.Percent = 1.0f - State.Percent;
		}
		else {
			// Two slots can't hold a three-way blend; continue from the dominant weather.
			TESWeather* Shown = (State.From && State.Percent < 0.5f) ? State.From : State.To;
			State.From = Shown;
			State.Percent = 0.0f;
		}
		State.To = EngineTo;
		State.Forced = EngineFrom != State.From;
		State.Detached = false;
		State.Logged = false;
	}

	if (!State.From) {
		State.From = EngineFrom;
		State.Percent = EngineFrom ? EnginePercent : 1.0f;
		State.Detached = false;
		return;
	}

	// The engine dropped the source weather: drive the shown blend to 1. Latched,
	// as restoring secondWeather makes the engine restart its transition from the
	// reset start hour, which would stall the blend near 0.
	if (EngineFrom != State.From) State.Detached = true;
	float Target = State.Detached ? 1.0f : EnginePercent;
	float Percent = min(Target, State.Percent + (float)(Elapsed / MinTransitionSeconds));
	State.Percent = max(Percent, 0.0f);
	if (Percent < Target - 0.001f && !State.Logged) {
		LogIntervention(WorldSky, EnginePercent, HourDelta, Elapsed);
		State.Logged = true;
	}
	if (State.Percent >= 1.0f) {
		State.From = NULL;
		State.Percent = 1.0f;
		State.Detached = false;
	}

	if (WorldSky->secondWeather != State.From || WorldSky->weatherPercent != State.Percent) {
		WorldSky->secondWeather = State.From;
		WorldSky->weatherPercent = State.Percent;
		ThisCall(kUpdateHDRBlend, WorldSky);
	}

}

void CreateWeatherSmoothingHook() {

	MinTransitionSeconds = TheSettingManager->SettingsMain.Main.WeatherMinTransitionTime;
	QueryPerformanceFrequency(&CounterFrequency);
	WriteRelCall(kUpdateTransitionCall, (UInt32)UpdateTransitionHook);

}
