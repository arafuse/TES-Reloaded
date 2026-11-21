#pragma once

class FrameRateManager { // Never disposed
public:
	FrameRateManager();

	double			GetPerformance();

	/// Advances ElapsedTime; call exactly once per rendered frame.
	void			UpdateFrameTime();

	static constexpr double kMaxElapsedTime = 0.25;	///< ElapsedTime clamp, so load screens and hitches do not jump timers

	LONGLONG		PerformanceFrequency;
	LONGLONG		PerformanceCounterStart;
	double			Time;
	double			LastTime;		///< GetPerformance() in seconds at the last UpdateFrameTime(); < 0 before the first frame
	double			ElapsedTime;	///< Seconds between the last two rendered frames, clamped to kMaxElapsedTime
	double			LastPerformance;
};