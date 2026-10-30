#include "CoSave.h"
#include "Wind.h"
#include "WetActors.h"
#include "RainProbe.h"

static const UInt32 kCoSaveMinObseVersion	= 15; // First OBSE with GetPluginHandle
static const UInt32 kWindRecordType			= 'WIND';
static const UInt32 kWindRecordVersion		= 1;
static const UInt32 kWetActorsRecordType	= 'WETA';
static const UInt32 kWetActorsRecordVersion	= 1;

static SerializationInterface* CoSave = NULL;

static void SaveCallback(void* Reserved) {

	WindState Wind;
	if (GetWindState(Wind))
		CoSave->WriteRecord(kWindRecordType, kWindRecordVersion, &Wind, sizeof(Wind));
	std::vector<WetActorRecord> Wet;
	GetWetActorRecords(Wet);
	if (!Wet.empty()) {
		UInt32 Count = (UInt32)Wet.size();
		std::vector<UInt8> Buffer(sizeof(Count) + Count * sizeof(WetActorRecord));
		memcpy(Buffer.data(), &Count, sizeof(Count));
		memcpy(Buffer.data() + sizeof(Count), Wet.data(), Count * sizeof(WetActorRecord));
		CoSave->WriteRecord(kWetActorsRecordType, kWetActorsRecordVersion, Buffer.data(), (UInt32)Buffer.size());
	}

}

static bool LoadWindRecord(UInt32 Version, UInt32 Length) {

	WindState Wind;
	if (Version != kWindRecordVersion || Length != sizeof(Wind)) return false;
	if (CoSave->ReadRecordData(&Wind, sizeof(Wind)) != sizeof(Wind)) return false;
	if (!CoSave->ResolveRefID(Wind.WeatherRefID, &Wind.WeatherRefID)) Wind.WeatherRefID = 0;
	SetWindState(&Wind);
	return true;

}

static bool LoadWetActorsRecord(UInt32 Version, UInt32 Length) {

	UInt32 Count;
	if (Version != kWetActorsRecordVersion || Length < sizeof(Count)) return false;
	if (CoSave->ReadRecordData(&Count, sizeof(Count)) != sizeof(Count)) return false;
	if ((UInt64)Length != sizeof(Count) + (UInt64)Count * sizeof(WetActorRecord)) return false;
	std::vector<WetActorRecord> Wet(Count);
	UInt32 Bytes = Count * sizeof(WetActorRecord);
	if (Count && CoSave->ReadRecordData(Wet.data(), Bytes) != Bytes) return false;
	std::vector<WetActorRecord> Resolved;
	for (const WetActorRecord& Record : Wet) {
		UInt32 RefID;
		if (CoSave->ResolveRefID(Record.RefID, &RefID)) Resolved.push_back({ RefID, Record.Wetness });
	}
	SetWetActorRecords(Resolved.data(), (UInt32)Resolved.size());
	return true;

}

static void LoadCallback(void* Reserved) {

	UInt32 Type, Version, Length;

	SetWindState(NULL);
	SetWetActorRecords(NULL, 0);
	ResetRainProbe();
	while (CoSave->GetNextRecordInfo(&Type, &Version, &Length)) {
		if (Type == kWindRecordType && !LoadWindRecord(Version, Length))
			Logger::Log("CoSave: skipped an unreadable wind record (version %u, %u bytes); the wind starts fresh.", Version, Length);
		if (Type == kWetActorsRecordType && !LoadWetActorsRecord(Version, Length))
			Logger::Log("CoSave: skipped an unreadable wet actors record (version %u, %u bytes); everyone starts dry.", Version, Length);
	}

}

static void NewGameCallback(void* Reserved) {

	SetWindState(NULL);
	SetWetActorRecords(NULL, 0);
	ResetRainProbe();

}

void CreateCoSave(const PluginInterface* Interface) {

	if (Interface->Version < kCoSaveMinObseVersion) {
		Logger::Log("CoSave: OBSE %u is older than %u; the wind and wet actors are not saved with the game.", Interface->Version, kCoSaveMinObseVersion);
		return;
	}
	CoSave = (SerializationInterface*)Interface->QueryInterface(PluginInterface::kInterface_Serialization);
	if (!CoSave || CoSave->Version < SerializationInterface::kVersion) {
		Logger::Log("CoSave: OBSE serialization interface unavailable; the wind and wet actors are not saved with the game.");
		CoSave = NULL;
		return;
	}

	PluginHandle Handle = Interface->GetPluginHandle();
	CoSave->SetSaveCallback(Handle, SaveCallback);
	CoSave->SetLoadCallback(Handle, LoadCallback);
	CoSave->SetNewGameCallback(Handle, NewGameCallback);

}
