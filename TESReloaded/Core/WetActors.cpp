#include "WetActors.h"
#include "RainProbe.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

static const double	kScanIntervalMs		= 200.0;
static const double	kLogIntervalMs		= 1000.0;
static const float	kMaxStepSeconds		= 0.25f;
static const double	kSkipThresholdHours	= 1.0 / 60.0;	// a frame more than a game minute ahead of the clock
static const UInt32	kMovementSwim		= 0x800;
static const int	kMaxParentHops		= 32;
static TimeGlobals* const kTimeGlobals	= (TimeGlobals*)0x00B332E0;

/// One actor's wetness, the update frame that last advanced it, and whether it was then in an interior.
struct WetState {
	float	Wetness;
	UInt32	Frame;
	bool	Indoors;
};

/// One loaded NPC as of the last scan. Values only: the ref is not dereferenced after the scan, and
/// Root is only compared, never dereferenced.
struct ScannedActor {
	UInt32		RefID;
	NiNode*		Root;
	bool		Outdoors;
	bool		Indoors;
	bool		Swimming;
	bool		Probed;
	NiPoint3	Position;
	float		Scale;
	float		DistanceSq;
};

static std::unordered_map<UInt32, WetState>		Wetness;
static std::unordered_map<NiAVObject*, float>	DrawRoots;
static std::vector<ScannedActor>				Scanned;
static std::vector<RainProbeActor>				ProbeActors;
static UInt32	Frame			= 0;
static double	LastMs			= -1.0;
static double	LastScanMs		= -1.0;
static TESObjectCELL*	LastScanCell	= NULL;	// compared by pointer only, never dereferenced
static SInt32	LastGridX		= 0;
static SInt32	LastGridY		= 0;
static double	LastGameHours	= -1.0;	// < 0: resync on the next update
static double	LastLogMs		= -1.0;
static float	LogSkippedHours	= 0.0f;

static bool IsExteriorLikeCell(TESObjectCELL* Cell) {

	return Cell && (Cell->worldSpace || (Cell->flags0 & TESObjectCELL::kFlags0_BehaveLikeExterior));

}

static bool IsInteriorCell(TESObjectCELL* Cell) {

	return Cell && !Cell->worldSpace;

}

static float DrySeconds(bool Indoors, const SettingsPrecipitationsStruct::WetActorsStruct& Settings) {

	return max(Indoors ? Settings.InteriorDryTime : Settings.DryTime, 0.01f);

}

static bool IsSwimming(Actor* Act) {

	return Act->process && (Act->process->GetMovementFlags() & kMovementSwim);

}

static double GameHoursNow() {

	return floor((double)kTimeGlobals->GameDaysPassed->data) * 24.0 + (double)kTimeGlobals->GameHour->data;

}

static void ScanCell(TESObjectCELL* Cell) {

	if (!Cell) return;
	for (TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First; Entry; Entry = Entry->next) {
		TESObjectREFR* Ref = Entry->item;
		if (!Ref || Ref == Player) continue;
		// A plain REFR can have an actor base (e.g. a leveled creature), but only ACHR/ACRE refs are Actor objects.
		if (Ref->formType != TESForm::FormType::kFormType_ACHR && Ref->formType != TESForm::FormType::kFormType_ACRE) continue;
		NiNode* Root = Ref->GetNode();
		if (!Root) continue;
		ScannedActor Item;
		Item.RefID = Ref->refID;
		Item.Root = Root;
		Item.Outdoors = IsExteriorLikeCell(Ref->parentCell);
		Item.Indoors = IsInteriorCell(Ref->parentCell);
		Item.Swimming = IsSwimming((Actor*)Ref);
		Item.Probed = false;
		Item.Position = Ref->pos;
		Item.Scale = Ref->scale;
		float dx = Ref->pos.x - Player->pos.x;
		float dy = Ref->pos.y - Player->pos.y;
		float dz = Ref->pos.z - Player->pos.z;
		Item.DistanceSq = dx * dx + dy * dy + dz * dz;
		Scanned.push_back(Item);
	}

}

// Rebuilds the NPC list and picks the nearest outdoor NPCs for the probe.
static void ScanActors(const SettingsPrecipitationsStruct::WetActorsStruct& Settings) {

	Scanned.clear();
	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++)
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++)
				ScanCell(Tes->gridCellArray->GetCell(x, y));
	}
	else {
		ScanCell(Player->parentCell);
	}

	ProbeActors.clear();
	int Max = min(max(Settings.MaxProbedNPCs, 0), RainProbeMaxActors);
	if (Settings.NPCExposure != 1 || Max == 0) return;
	float OrthoRadius = TheSettingManager->SettingsShadows.Exteriors.ShadowMapRadius[ShadowManager::ShadowMapTypeEnum::MapOrtho];
	float Radius = min(Settings.ProbeRadius, OrthoRadius);
	std::vector<ScannedActor*> Near;
	for (ScannedActor& Item : Scanned)
		if (Item.Outdoors && Item.DistanceSq <= Radius * Radius) Near.push_back(&Item);
	std::sort(Near.begin(), Near.end(), [](const ScannedActor* A, const ScannedActor* B) { return A->DistanceSq < B->DistanceSq; });
	if ((int)Near.size() > Max) Near.resize(Max);
	for (ScannedActor* Item : Near) {
		Item->Probed = true;
		ProbeActors.push_back({ Item->RefID, Item->Position, Item->Scale });
	}

}

// Advances one actor this frame. Only actors that are wet, exposed or swimming get an entry.
static void Advance(UInt32 RefID, bool Indoors, bool Swimming, bool Exposed, float ActiveSeconds, float Intensity, const SettingsPrecipitationsStruct::WetActorsStruct& Settings) {

	std::unordered_map<UInt32, WetState>::iterator It = Wetness.find(RefID);
	if (It == Wetness.end()) {
		if (!Swimming && !(Exposed && Intensity > 0.0f)) return;
		It = Wetness.insert({ RefID, { 0.0f, Frame, Indoors } }).first;
	}
	float& Wet = It->second.Wetness;
	if (Swimming)
		Wet = 1.0f;
	else if (Exposed)
		Wet += ActiveSeconds / max(Settings.SoakTime, 0.01f) * Intensity;
	else
		Wet -= ActiveSeconds / DrySeconds(Indoors, Settings);
	Wet = min(max(Wet, 0.0f), 1.0f);
	It->second.Frame = Frame;
	It->second.Indoors = Indoors;

}

static void AddDrawRoot(NiAVObject* Root, UInt32 RefID) {

	std::unordered_map<UInt32, WetState>::const_iterator It = Wetness.find(RefID);
	if (Root && It != Wetness.end() && It->second.Wetness > 0.0f) DrawRoots[Root] = It->second.Wetness;

}

static void LogWetActors(double NowMs, bool Raining, float Intensity, bool PlayerIndoors, bool PlayerExposed, bool PlayerSwimming, float ActiveSeconds, float SkippedHours) {

	LogSkippedHours += SkippedHours;
	if (!TheSettingManager->SettingsMain.Develop.LogWetActors) return;
	if (LastLogMs >= 0.0 && NowMs - LastLogMs < kLogIntervalMs) return;
	LastLogMs = NowMs;

	std::unordered_map<UInt32, WetState>::const_iterator PlayerIt = Wetness.find(Player->refID);
	int Probed = 0;
	const ScannedActor* Nearest = NULL;
	for (const ScannedActor& Item : Scanned) {
		Probed += Item.Probed ? 1 : 0;
		if (!Nearest || Item.DistanceSq < Nearest->DistanceSq) Nearest = &Item;
	}
	Logger::Log("[WetActors] player wet=%.3f indoors=%d exposed=%d swim=%d | rain=%d intensity=%.2f probe=%d | step=%.3fs skipped=%.3fh | tracked=%d scanned=%d probed=%d roots=%d",
		PlayerIt != Wetness.end() ? PlayerIt->second.Wetness : 0.0f, PlayerIndoors, PlayerExposed, PlayerSwimming,
		Raining, Intensity, IsRainProbeWorking(), ActiveSeconds, LogSkippedHours,
		(int)Wetness.size(), (int)Scanned.size(), Probed, (int)DrawRoots.size());
	if (Nearest) {
		std::unordered_map<UInt32, WetState>::const_iterator It = Wetness.find(Nearest->RefID);
		bool Answer = false;
		bool HasAnswer = GetRainProbeActorExposure(Nearest->RefID, &Answer);
		Logger::Log("[WetActors] nearest npc %08X dist=%.0f wet=%.3f outdoors=%d swim=%d probed=%d probeAnswer=%s",
			(unsigned)Nearest->RefID, sqrtf(Nearest->DistanceSq), It != Wetness.end() ? It->second.Wetness : 0.0f,
			Nearest->Outdoors, Nearest->Swimming, Nearest->Probed, HasAnswer ? (Answer ? "open" : "sheltered") : "none");
	}
	LogSkippedHours = 0.0f;

}

void UpdateWetActors() {

	const SettingsPrecipitationsStruct::WetActorsStruct& Settings = TheSettingManager->SettingsPrecipitations.WetActors;
	if (!Settings.Enabled || !Player || !Tes) {
		Wetness.clear();
		DrawRoots.clear();
		Scanned.clear();
		ProbeActors.clear();
		SetRainProbeActors(NULL, 0);
		LastMs = LastScanMs = LastGameHours = -1.0;
		LastScanCell = NULL;
		return;
	}
	Frame++;

	double NowMs = TheFrameRateManager->GetPerformance();
	float RealSeconds = LastMs < 0.0 ? 0.0f : (float)min((NowMs - LastMs) / 1000.0, (double)kMaxStepSeconds);
	LastMs = NowMs;

	// The game clock separates play (advancing at TimeScale), pause (stopped) and skips (jumping ahead).
	double Hours = GameHoursNow();
	float ActiveSeconds = 0.0f;
	float SkippedHours = 0.0f;
	if (LastGameHours >= 0.0) {
		double Delta = Hours - LastGameHours;
		bool Rollover = fabs(fabs(Delta) - 24.0) < kSkipThresholdHours;
		if (Delta > 0.0 && !Rollover) {
			double Expected = RealSeconds * kTimeGlobals->TimeScale->data / 3600.0;
			double Skipped = Delta - Expected;
			if (Skipped > kSkipThresholdHours)
				SkippedHours = (float)Skipped;
			else
				ActiveSeconds = RealSeconds;
		}
	}
	LastGameHours = Hours;

	// A grid shift unloads cells, so rescan at once to drop their actors' roots before the addresses are reused.
	SInt32 GridX = Tes->gridCellArray ? Tes->gridCellArray->worldX : 0;
	SInt32 GridY = Tes->gridCellArray ? Tes->gridCellArray->worldY : 0;
	bool GridShifted = GridX != LastGridX || GridY != LastGridY;
	if (LastScanMs < 0.0 || NowMs - LastScanMs >= kScanIntervalMs || Player->parentCell != LastScanCell || GridShifted) {
		LastScanMs = NowMs;
		LastScanCell = Player->parentCell;
		LastGridX = GridX;
		LastGridY = GridY;
		ScanActors(Settings);
	}
	SetRainProbeActors(ProbeActors.data(), (int)ProbeActors.size());

	bool Raining = IsRainFalling();
	float Intensity = Raining ? min(max(TheShaderManager->ShaderConst.Precipitations.RainData.x, 0.0f), 1.0f) : 0.0f;
	bool ProbeWorking = IsRainProbeWorking();

	bool PlayerIndoors = IsInteriorCell(Player->parentCell);
	bool PlayerSwimming = IsSwimming((Actor*)Player);
	bool PlayerExposed = Raining && (ProbeWorking ? IsRainReachingPlayer() : Player->IsExteriorLike());
	Advance(Player->refID, PlayerIndoors, PlayerSwimming, PlayerExposed, ActiveSeconds, Intensity, Settings);

	for (const ScannedActor& Item : Scanned) {
		bool Exposed = false;
		if (Raining && Item.Outdoors) {
			bool Answer;
			if (Item.Probed && ProbeWorking && GetRainProbeActorExposure(Item.RefID, &Answer))
				Exposed = Answer;
			else
				Exposed = true;
		}
		Advance(Item.RefID, Item.Indoors, Item.Swimming, Exposed, ActiveSeconds, Intensity, Settings);
	}

	// Actors not advanced this frame (unloaded, or between scans out of range) keep drying where they were last seen.
	float SkipLoss = SkippedHours * Settings.DryPerSkippedHour;
	for (std::unordered_map<UInt32, WetState>::iterator It = Wetness.begin(); It != Wetness.end();) {
		if (It->second.Frame != Frame) It->second.Wetness -= ActiveSeconds / DrySeconds(It->second.Indoors, Settings);
		It->second.Wetness -= SkipLoss;
		if (It->second.Wetness <= 0.0f)
			It = Wetness.erase(It);
		else
			++It;
	}

	DrawRoots.clear();
	AddDrawRoot(Player->GetNode(), Player->refID);
	AddDrawRoot(Player->firstPersonNiNode, Player->refID);
	for (const ScannedActor& Item : Scanned) AddDrawRoot(Item.Root, Item.RefID);

	LogWetActors(NowMs, Raining, Intensity, PlayerIndoors, PlayerExposed, PlayerSwimming, ActiveSeconds, SkippedHours);

}

float GetDrawWetness(NiAVObject* Geometry) {

	if (DrawRoots.empty()) return 0.0f;
	NiAVObject* Object = Geometry;
	for (int Hop = 0; Object && Hop < kMaxParentHops; Hop++, Object = Object->m_parent) {
		std::unordered_map<NiAVObject*, float>::const_iterator It = DrawRoots.find(Object);
		if (It != DrawRoots.end()) return It->second;
	}
	return 0.0f;

}

void GetWetActorRecords(std::vector<WetActorRecord>& Records) {

	for (const std::pair<const UInt32, WetState>& Entry : Wetness)
		if (Entry.second.Wetness > 0.0f) Records.push_back({ Entry.first, Entry.second.Wetness });

}

void SetWetActorRecords(const WetActorRecord* Records, UInt32 Count) {

	Wetness.clear();
	DrawRoots.clear();
	Scanned.clear();
	ProbeActors.clear();
	LastScanCell = NULL;
	for (UInt32 i = 0; Records && i < Count; i++)
		if (Records[i].Wetness > 0.0f) Wetness[Records[i].RefID] = { min(Records[i].Wetness, 1.0f), 0, false };
	LastMs = LastScanMs = LastGameHours = -1.0;

}
