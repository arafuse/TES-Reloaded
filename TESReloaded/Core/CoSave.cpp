#include "CoSave.h"
#include "Wind.h"

static const UInt32 kCoSaveMinObseVersion	= 15; // First OBSE with GetPluginHandle
static const UInt32 kWindRecordType			= 'WIND';
static const UInt32 kWindRecordVersion		= 1;

static SerializationInterface* CoSave = NULL;

static void SaveCallback(void* Reserved) {

	WindState Wind;
	if (GetWindState(Wind))
		CoSave->WriteRecord(kWindRecordType, kWindRecordVersion, &Wind, sizeof(Wind));

}

static bool LoadWindRecord(UInt32 Version, UInt32 Length) {

	WindState Wind;
	if (Version != kWindRecordVersion || Length != sizeof(Wind)) return false;
	if (CoSave->ReadRecordData(&Wind, sizeof(Wind)) != sizeof(Wind)) return false;
	if (!CoSave->ResolveRefID(Wind.WeatherRefID, &Wind.WeatherRefID)) Wind.WeatherRefID = 0;
	SetWindState(&Wind);
	return true;

}

static void LoadCallback(void* Reserved) {

	UInt32 Type, Version, Length;

	SetWindState(NULL);
	while (CoSave->GetNextRecordInfo(&Type, &Version, &Length)) {
		if (Type == kWindRecordType && !LoadWindRecord(Version, Length))
			Logger::Log("CoSave: skipped an unreadable wind record (version %u, %u bytes); the wind starts fresh.", Version, Length);
	}

}

static void NewGameCallback(void* Reserved) {

	SetWindState(NULL);

}

void CreateCoSave(const PluginInterface* Interface) {

	if (Interface->Version < kCoSaveMinObseVersion) {
		Logger::Log("CoSave: OBSE %u is older than %u; the wind is not saved with the game.", Interface->Version, kCoSaveMinObseVersion);
		return;
	}
	CoSave = (SerializationInterface*)Interface->QueryInterface(PluginInterface::kInterface_Serialization);
	if (!CoSave || CoSave->Version < SerializationInterface::kVersion) {
		Logger::Log("CoSave: OBSE serialization interface unavailable; the wind is not saved with the game.");
		CoSave = NULL;
		return;
	}

	PluginHandle Handle = Interface->GetPluginHandle();
	CoSave->SetSaveCallback(Handle, SaveCallback);
	CoSave->SetLoadCallback(Handle, LoadCallback);
	CoSave->SetNewGameCallback(Handle, NewGameCallback);

}
