#include "LODFadeManager.h"

LODFadeManager::LODFadeManager() {

	TheLODFadeManager = this;
	Fades.clear();
	LiveCount = 0;
	CurrentTime = 0.0f;
	DitherSeed = 0.0f;

}

LODFadeManager::FadeRecord* LODFadeManager::AddFade(NiAVObject* Root, UInt8 Direction) {

	if (!Root) return NULL;
	if (Fades.size() >= TheSettingManager->SettingsMain.LODFade.MaxFades) return NULL;

	FadeRecord Record;
	Record.Root = Root;
	Record.Direction = Direction;
	Record.StartTime = CurrentTime;
	Record.Pinned = false;
	Fades.push_back(Record);
	LiveCount = Fades.size();

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] start root=%08X dir=%d live=%d", (UInt32)Root, Direction, LiveCount);

	return &Fades.back();

}

float LODFadeManager::GetAlpha(FadeRecord* Record) {

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	if (FadeTime <= 0.0f) return 1.0f;

	float t = (CurrentTime - Record->StartTime) / FadeTime;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return Record->Direction == FadeDir_In ? t : 1.0f - t;

}

void LODFadeManager::Retire(UInt32 Index) {

	if (TheSettingManager->SettingsMain.Develop.LogLODFade)
		Logger::Log("[LODFade] retire root=%08X", (UInt32)Fades[Index].Root);

	Fades.erase(Fades.begin() + Index);

}

void LODFadeManager::Update() {

	CurrentTime = (float)GetTickCount() * 0.001f;
	DitherSeed = (float)(GetTickCount() & 0xFFFF);

	float FadeTime = TheSettingManager->SettingsMain.LODFade.FadeTime;
	for (SInt32 i = (SInt32)Fades.size() - 1; i >= 0; i--) {
		if (CurrentTime - Fades[i].StartTime >= FadeTime) Retire(i);
	}
	LiveCount = Fades.size();

}
