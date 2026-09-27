#include "SunShadowStealth.h"

static const UInt32	kSunTermCall					= 0x006561FD; // HighProcess_GetLightLevel's exterior sun term
static const UInt32	kShadowSceneLightContribution	= 0x007D31B0;

static volatile float PlayerSunLightScale = 1.0f;
static volatile float LastPlayerSunTerm = 0.0f;

static float CallShadowSceneLightContribution(void* Light, float X, float Y, float Z, UInt32 Exclude) {

	class T {}; union { UInt32 x; float(T::* m)(float, float, float, UInt32); } u = { kShadowSceneLightContribution };
	return ((T*)Light->*u.m)(X, Y, Z, Exclude);

}

// Replaces the sun-term call; thiscall with callee cleanup, so __fastcall fits once the stub puts the actor in edx.
static float __fastcall SunTermHook(void* SunLight, Actor* Owner, float X, float Y, float Z, UInt32 Exclude) {

	float Term = CallShadowSceneLightContribution(SunLight, X, Y, Z, Exclude);
	if (Owner != (Actor*)Player) return Term;
	LastPlayerSunTerm = Term;
	return Term * PlayerSunLightScale;

}

// edi holds the actor for the whole of HighProcess_GetLightLevel.
static __declspec(naked) void SunTermStub() {

	__asm {
		mov		edx, edi
		jmp		SunTermHook
	}

}

void CreateSunShadowStealthHook() {

	WriteRelCall(kSunTermCall, (UInt32)SunTermStub);

}
