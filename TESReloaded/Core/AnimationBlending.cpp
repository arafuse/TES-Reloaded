#include <intrin.h>
#include "AnimationBlending.h"

static const UInt32 kPlaySequenceBlendTime	= 0x004748D3; // Inside ActorAnimData::PlaySequence (0x474530): blend /= fAnimationMult
static const UInt32 kPlaySequenceBlendReturn	= 0x004748E1;
static const UInt32 kStopSequence			= 0x00470FC0; // ActorAnimData::StopSequence(slot, easeOutTime)
static const UInt32 kStopSequenceEnd		= 0x004710AF;
static const UInt32 kNoBlendFlag			= 0xC4;	// ActorAnimData byte: the next sequence starts without a blend
static const int	kSlotCount				= 5;
static const int	kSlotAll				= 5;	// StopSequence fans out to every slot
static const int	kSlotUpperBody			= 6;	// StopSequence fans out to slots 1-3

static const UInt8 kPlaySequenceBlendBytes[] = {
	0xD9, 0x44, 0x24, 0x34,				// fld	dword ptr [esp+0x34]
	0xD8, 0x35, 0x30, 0x65, 0xB0, 0x00,	// fdiv	dword ptr [fAnimationMult]
	0xD9, 0x5C, 0x24, 0x34,				// fstp	dword ptr [esp+0x34]
};
static const UInt8 kStopSequenceBytes[] = { 0x8B, 0x44, 0x24, 0x04, 0x56, 0x57 };

// StopSequence calls that must stay instant: PlaySequence clearing a slot right before it blends from the
// pose (or resetting the player on a camera switch), and the stop that precedes a knockdown no-blend reset.
static const UInt32 kInstantStopReturns[] = {
	0x00474671, 0x00474680, 0x0047468F, 0x0047469E, 0x0047474C, 0x00474779,
	0x0065574D,
};

static float MinBlendTime = 0.0f;
static float ActionMinBlendTime = 0.0f;
static float StopBlendTime = 0.0f;
static bool LogTransitions = false;

static bool IsActionGroup(UInt8 Group) {

	switch (Group) {
		case TESAnimGroup::kAnimGroup_DodgeForward:
		case TESAnimGroup::kAnimGroup_DodgeBack:
		case TESAnimGroup::kAnimGroup_DodgeLeft:
		case TESAnimGroup::kAnimGroup_DodgeRight:
		case TESAnimGroup::kAnimGroup_Death:
		case TESAnimGroup::kAnimGroup_JumpStart:
		case TESAnimGroup::kAnimGroup_JumpLoop:
		case TESAnimGroup::kAnimGroup_JumpLand:
			return true;
		default:
			return (Group >= TESAnimGroup::kAnimGroup_Equip && Group <= TESAnimGroup::kAnimGroup_Stagger) ||
				   (Group >= TESAnimGroup::kAnimGroup_CastSelf && Group <= TESAnimGroup::kAnimGroup_CastTargetAlt);
	}

}

static UInt8 GroupOf(BSAnimGroupSequence* Sequence) {

	return Sequence && Sequence->animGroup ? Sequence->animGroup->animGroup : 0xFF;

}

static bool HasNoBlendFlag(ActorAnimData* Data) {

	return *((UInt8*)Data + kNoBlendFlag) != 0;

}

static void __cdecl AdjustBlendTime(float* Blend, ActorAnimData* Data, BSAnimGroupSequence* NewSequence, BSAnimGroupSequence* OldSequence) {

	float EngineBlend = *Blend;
	if (!(EngineBlend > 0.0f)) return;

	UInt8 Group = GroupOf(NewSequence);
	float Minimum = IsActionGroup(Group) ? ActionMinBlendTime : MinBlendTime;
	if (EngineBlend >= Minimum) return;

	*Blend = Minimum;
	if (LogTransitions) Logger::Log("AnimationBlending: %08X group %u -> %u blend %.3f -> %.3f s", (UInt32)Data, GroupOf(OldSequence), Group, EngineBlend, Minimum);

}

// Replaces the 14 bytes that divide the blend time by fAnimationMult; edi holds the slot's previous
// sequence (NULL when it was not animating), ebp the new one, esi the ActorAnimData.
static __declspec(naked) void PlaySequenceBlendTimeHook() {

	__asm {
		pushad
		lea		eax, [esp + 0x20 + 0x34]
		push	edi
		push	ebp
		push	esi
		push	eax
		call	AdjustBlendTime
		add		esp, 0x10
		popad
		fld		dword ptr [esp + 0x34]
		fdiv	dword ptr ds:[0x00B06530]
		fstp	dword ptr [esp + 0x34]
		jmp		kPlaySequenceBlendReturn
	}

}

static void (__fastcall* StopSequence)(ActorAnimData*, void*, int, float) = (void (__fastcall*)(ActorAnimData*, void*, int, float))kStopSequence;

// A fan-out stop (slot 5 or 6) calls itself per slot with the fade it was given, already adjusted or not.
static bool MustStopInstantly(void* ReturnAddress) {

	if ((UInt32)ReturnAddress > kStopSequence && (UInt32)ReturnAddress < kStopSequenceEnd) return true;
	for (UInt32 Address : kInstantStopReturns) {
		if ((UInt32)ReturnAddress == Address) return true;
	}
	return false;

}

static bool IsPlaying(ActorAnimData* Data, int Slot) {

	if (Slot == kSlotUpperBody) return IsPlaying(Data, 1) || IsPlaying(Data, 2) || IsPlaying(Data, 3);
	if (Slot < 0 || Slot >= kSlotCount) return false;
	BSAnimGroupSequence* Sequence = Data->animSequences[Slot];
	return Sequence && (Sequence->state == NiControllerSequence::kState_Animating || Sequence->state == NiControllerSequence::kState_EaseIn);

}

// Detour on a thiscall with callee cleanup, so __fastcall with a dummy edx matches it.
static void __fastcall TrackStopSequence(ActorAnimData* Data, void* Edx, int Slot, float EaseOut) {

	if (EaseOut < StopBlendTime && Slot != kSlotAll && !HasNoBlendFlag(Data) && IsPlaying(Data, Slot) && !MustStopInstantly(_ReturnAddress())) {
		if (LogTransitions) Logger::Log("AnimationBlending: %08X stop slot %d ease out %.3f -> %.3f s (caller %08X)", (UInt32)Data, Slot, EaseOut, StopBlendTime, (UInt32)_ReturnAddress());
		EaseOut = StopBlendTime;
	}
	StopSequence(Data, Edx, Slot, EaseOut);

}

void CreateAnimationBlendingHook() {

	const SettingsMainStruct::AnimationBlendingStruct& Settings = TheSettingManager->SettingsMain.AnimationBlending;
	MinBlendTime = max(0.0f, Settings.MinBlendTime);
	ActionMinBlendTime = max(0.0f, Settings.ActionMinBlendTime);
	StopBlendTime = max(0.0f, Settings.StopBlendTime);
	LogTransitions = TheSettingManager->SettingsMain.Develop.LogAnimationBlending;

	if (MinBlendTime > 0.0f || ActionMinBlendTime > 0.0f) {
		if (memcmp((void*)kPlaySequenceBlendTime, kPlaySequenceBlendBytes, sizeof(kPlaySequenceBlendBytes)))
			Logger::Log("AnimationBlending: the blend time site %08X is already patched by another plugin; start blends are not changed.", kPlaySequenceBlendTime);
		else
			WriteRelJump(kPlaySequenceBlendTime, (UInt32)PlaySequenceBlendTimeHook);
	}

	if (StopBlendTime > 0.0f) {
		if (memcmp((void*)kStopSequence, kStopSequenceBytes, sizeof(kStopSequenceBytes))) {
			Logger::Log("AnimationBlending: StopSequence %08X is already hooked by another plugin; stops are not changed.", kStopSequence);
		}
		else {
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(&(PVOID&)StopSequence, TrackStopSequence);
			DetourTransactionCommit();
		}
	}

	Logger::Log("AnimationBlending: min blend %.3f s, action min blend %.3f s, stop ease out %.3f s", MinBlendTime, ActionMinBlendTime, StopBlendTime);

}
