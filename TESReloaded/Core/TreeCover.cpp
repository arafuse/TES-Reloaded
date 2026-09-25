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

static volatile float PlayerTreeCover = 0.0f;

static NiNode* FindRefTreeNode(NiNode* Root) {

	if (*(void**)Root == VFTBSTreeNode) return Root;
	for (int i = 0; i < Root->m_children.end; i++) {
		NiAVObject* Child = Root->m_children.data[i];
		if (Child && *(void**)Child == VFTBSTreeNode) return (NiNode*)Child;
	}
	return NULL;

}

static void AccumulateTreeCover(TList<TESObjectREFR>::Entry* Entry, TreeCoverInput& In, float MaxBound, float& Exposure) {

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
		Exposure *= 1.0f - TreeCoverAt(In);
	}

}

void UpdateTreeCover() {

	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	if (!Settings->TreeCover || !Player || !Player->process || !Player->parentCell) {
		PlayerTreeCover = 0.0f;
		return;
	}
	UInt32 Movement = Player->process->GetMovementFlags();
	if (!(Movement & kMovementSneak) || (Movement & kMovementSwim)) {
		PlayerTreeCover = 0.0f;
		return;
	}

	TreeCoverInput In = {};
	In.QX = Player->pos.x;
	In.QY = Player->pos.y;
	In.QZ = Player->pos.z + kTreeCoverTorsoHeight;
	bool Bending = Settings->TreeCollision && TheShaderManager->GrassCollisionSourceCount > 0;
	In.PushStrength = Bending ? Settings->TreeCollisionStrength : 0.0f;

	float Exposure = 1.0f;
	if (Player->GetWorldSpace()) {
		for (UInt32 x = 0; x < *SettingGridsToLoad; x++) {
			for (UInt32 y = 0; y < *SettingGridsToLoad; y++) {
				TESObjectCELL* Cell = Tes->gridCellArray->GetCell(x, y);
				if (Cell) AccumulateTreeCover(&Cell->objectList.First, In, Settings->TreeCollisionMaxBound, Exposure);
			}
		}
	}
	else {
		AccumulateTreeCover(&Player->parentCell->objectList.First, In, Settings->TreeCollisionMaxBound, Exposure);
	}
	PlayerTreeCover = 1.0f - Exposure;

}

static void __stdcall AdjustDetectionLight(Actor* Target, SInt32* Args) {

	float Cover = PlayerTreeCover;
	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	if (Cover <= 0.0f || !Settings->TreeCover || Target != Player || !(UInt8)Args[kDetectionArgSneaking]) return;
	float Scale = 1.0f - Cover * Settings->TreeCoverLightReduction;
	if (Scale < 0.0f) Scale = 0.0f;
	Args[kDetectionArgLight] = (SInt32)(Args[kDetectionArgLight] * Scale);

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

}
