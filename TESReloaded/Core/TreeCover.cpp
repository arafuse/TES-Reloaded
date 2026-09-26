#include "TreeCover.h"
#include "TreeCoverMath.h"

static const void*	VFTBSTreeNode			= (void*)0x00A65854;
static const float	kTreeCoverTorsoHeight	= 50.0f;
static const UInt32	kMovementSneak			= 0x400;
static const UInt32	kMovementSwim			= 0x800;
static const UInt32	kDetectionLevelCall		= 0x005F68DB; // the only call to Calc_DetectionLevel
static const UInt32	kCalcDetectionLevel		= 0x005463F0;
static const int	kDetectionArgLight		= 5;
static const int	kDetectionArgSneaking	= 9;
static const UInt32	kDetectionLosCall		= 0x005F6647; // the detection call to the actor LOS test
static const UInt32	kActorHasLineOfSight	= 0x005F2820;
static const float	kTreeCoverEyeHeight		= 110.0f;
static const int	kTreeCoverMaxShrubs		= 8;
static const int	kSnapshotReadTries		= 4;

/// The shrubs containing the sneaking player's torso this frame, for the line-of-sight hook.
struct TreeCoverSnapshot {
	int				Count;
	float			TorsoX, TorsoY, TorsoZ;
	TreeCoverInput	Shrubs[kTreeCoverMaxShrubs];
};

static volatile LONG		SnapshotSequence = 0;
static TreeCoverSnapshot	Snapshot = {};

static volatile float PlayerTreeCover = 0.0f;

// Seqlock: odd while writing. Detection may run on the threaded-AI thread.
static void PublishSnapshot(const TreeCoverSnapshot& Next) {

	InterlockedIncrement(&SnapshotSequence);
	Snapshot = Next;
	InterlockedIncrement(&SnapshotSequence);

}

static bool ReadSnapshot(TreeCoverSnapshot& Out) {

	for (int Try = 0; Try < kSnapshotReadTries; Try++) {
		LONG Before = SnapshotSequence;
		_ReadWriteBarrier();
		if (Before & 1) {
			YieldProcessor();
			continue;
		}
		Out = Snapshot;
		_ReadWriteBarrier();
		if (SnapshotSequence == Before) return true;
	}
	return false;

}

static NiNode* FindRefTreeNode(NiNode* Root) {

	if (*(void**)Root == VFTBSTreeNode) return Root;
	for (int i = 0; i < Root->m_children.end; i++) {
		NiAVObject* Child = Root->m_children.data[i];
		if (Child && *(void**)Child == VFTBSTreeNode) return (NiNode*)Child;
	}
	return NULL;

}

static void AccumulateTreeCover(TList<TESObjectREFR>::Entry* Entry, TreeCoverInput& In, float MaxBound, float& Exposure, TreeCoverSnapshot& Shrubs) {

	for (; Entry; Entry = Entry->next) {
		TESObjectREFR* Ref = Entry->item;
		if (!Ref || (Ref->flags & (TESObjectREFR::kFlags_Disabled | TESObjectREFR::kFlags_Deleted))) continue;
		if (!Ref->baseForm || Ref->baseForm->formType != TESForm::FormType::kFormType_Tree) continue;
		NiNode* Root = Ref->GetNode();
		NiNode* Tree = Root ? FindRefTreeNode(Root) : NULL;
		if (!Tree) continue;
		NiBound* Bound = Tree->GetWorldBound();
		if (Bound->Radius <= 0.0f || Bound->Radius > MaxBound) continue;
		float DX = In.QX - Bound->Center.x;
		float DY = In.QY - Bound->Center.y;
		if (DX * DX + DY * DY > Bound->Radius * Bound->Radius) continue;
		if (In.QZ < Ref->pos.z || In.QZ > Bound->Center.z + Bound->Radius) continue;
		In.BaseZ = Ref->pos.z;
		In.CX = Bound->Center.x;
		In.CY = Bound->Center.y;
		In.CZ = Bound->Center.z;
		In.R = Bound->Radius;
		float Cover = TreeCoverAt(In);
		Exposure *= 1.0f - Cover;
		if (Cover > 0.0f && Shrubs.Count < kTreeCoverMaxShrubs) Shrubs.Shrubs[Shrubs.Count++] = In;
	}

}

static float ScanTreeCover(TreeCoverSnapshot& Shrubs) {

	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	if (!Settings->TreeCover || !Player || !Player->process || !Player->parentCell) {
		return 0.0f;
	}
	UInt32 Movement = Player->process->GetMovementFlags();
	if (!(Movement & kMovementSneak) || (Movement & kMovementSwim)) {
		return 0.0f;
	}

	TreeCoverInput In = {};
	In.QX = Player->pos.x;
	In.QY = Player->pos.y;
	In.QZ = Player->pos.z + kTreeCoverTorsoHeight;
	bool Bending = Settings->TreeCollision && TheShaderManager->GrassCollisionSourceCount > 0;
	In.PushStrength = Bending ? Settings->TreeCollisionStrength : 0.0f;

	Shrubs.TorsoX = In.QX;
	Shrubs.TorsoY = In.QY;
	Shrubs.TorsoZ = In.QZ;

	float Exposure = 1.0f;
	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++) {
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
				TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y);
				if (Cell) AccumulateTreeCover(&Cell->objectList.First, In, Settings->TreeCollisionMaxBound, Exposure, Shrubs);
			}
		}
	}
	else {
		AccumulateTreeCover(&Player->parentCell->objectList.First, In, Settings->TreeCollisionMaxBound, Exposure, Shrubs);
	}
	return 1.0f - Exposure;

}

void UpdateTreeCover() {

	TreeCoverSnapshot Shrubs = {};
	PlayerTreeCover = ScanTreeCover(Shrubs);
	PublishSnapshot(Shrubs);

}

static void __stdcall AdjustDetectionLight(Actor* Target, SInt32* Args) {

	float Cover = PlayerTreeCover;
	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	if (!(Cover > 0.0f) || !Settings->TreeCover || Target != Player || !(UInt8)Args[kDetectionArgSneaking]) return;
	float Scale = 1.0f - Cover * Settings->TreeCoverLightReduction;
	if (Scale < 0.0f) Scale = 0.0f;
	if (Scale > 1.0f) Scale = 1.0f;
	Args[kDetectionArgLight] = (SInt32)(Args[kDetectionArgLight] * Scale);

}

// Replaces the detection call to the actor LOS test; thiscall with callee cleanup, so __fastcall fits.
static bool __fastcall DetectionLineOfSightHook(Actor* Observer, void* Edx, UInt32 Arg1, TESObjectREFR* Target, UInt32 Arg3, UInt32* Reason, UInt32 Arg5) {

	bool Visible = (UInt8)ThisCall(kActorHasLineOfSight, Observer, Arg1, Target, Arg3, Reason, Arg5);
	if (!Visible || Target != (TESObjectREFR*)Player || !TheSettingManager->SettingsGrass.TreeCoverBlockLOS) return Visible;

	TreeCoverSnapshot Shrubs;
	if (!ReadSnapshot(Shrubs) || !Shrubs.Count) return Visible;

	float EyeX = Observer->pos.x;
	float EyeY = Observer->pos.y;
	float EyeZ = Observer->pos.z + kTreeCoverEyeHeight * Observer->scale;
	float Depth = 0.0f;
	for (int i = 0; i < Shrubs.Count; i++)
		Depth += TreeCoverRayDepth(Shrubs.Shrubs[i], EyeX, EyeY, EyeZ, Shrubs.TorsoX, Shrubs.TorsoY, Shrubs.TorsoZ);
	float Threshold = TheSettingManager->SettingsGrass.TreeCoverLOSDepth;
	return !(Threshold > 0.0f && Depth >= Threshold);

}

// Reached by the original call, so [esp] is its return address and the cdecl args follow it.
static __declspec(naked) void DetectionLevelHook() {

	__asm {
		pushad
		lea		eax, [esp + 0x24]
		push	eax
		push	ebp
		call	AdjustDetectionLight
		popad
		jmp		kCalcDetectionLevel
	}

}

void CreateTreeCoverHook() {

	WriteRelCall(kDetectionLevelCall, (UInt32)DetectionLevelHook);
	WriteRelCall(kDetectionLosCall, (UInt32)DetectionLineOfSightHook);

}
