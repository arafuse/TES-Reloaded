#include "TreeCollision.h"

static const UInt32 kBranchShaderVtable				= 0x00A9459C; // SpeedTreeBranchShader
static const UInt32 kSetupTransformationsSlot		= 0x34;       // vtable slot 13; the batch loop calls it once per tree
static const UInt32 kLeafUpdatePipeline				= 0x007F0BC0; // SpeedTreeLeafShader UpdatePipeline body, called once per tree
static const void*	VFTBSTreeNode					= (void*)0x00A65854;
static const UInt32 kTreeCollisionRegister			= 240;
static const int	kTreeNodeMaxDepth				= 6;

class TreeCollisionHook {
public:
	UInt32 TrackBranchSetupTransformations(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, NiBound*);
	UInt32 TrackLeafUpdatePipeline(NiGeometry*, NiSkinInstance*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, NiBound*);
};

UInt32 (__thiscall TreeCollisionHook::* BranchSetupTransformations)(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, NiBound*);
UInt32 (__thiscall TreeCollisionHook::* TrackBranchSetupTransformations)(NiGeometry*, NiSkinInstance*, NiSkinPartition::Partition*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, NiBound*);
UInt32 TreeCollisionHook::TrackBranchSetupTransformations(NiGeometry* Geometry, NiSkinInstance* SkinInstance, NiSkinPartition::Partition* SkinPartition, NiGeometryBufferData* GeometryBufferData, NiPropertyState* PropertyState, NiDynamicEffectState* EffectState, NiTransform* WorldTransform, NiBound* WorldBound) {

	UInt32 Result = (this->*BranchSetupTransformations)(Geometry, SkinInstance, SkinPartition, GeometryBufferData, PropertyState, EffectState, WorldTransform, WorldBound);
	SetTreeCollisionConstants(Geometry, WorldTransform, true);
	return Result;

}

UInt32 (__thiscall TreeCollisionHook::* LeafUpdatePipeline)(NiGeometry*, NiSkinInstance*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, NiBound*);
UInt32 (__thiscall TreeCollisionHook::* TrackLeafUpdatePipeline)(NiGeometry*, NiSkinInstance*, NiGeometryBufferData*, NiPropertyState*, NiDynamicEffectState*, NiTransform*, NiBound*);
UInt32 TreeCollisionHook::TrackLeafUpdatePipeline(NiGeometry* Geometry, NiSkinInstance* SkinInstance, NiGeometryBufferData* GeometryBufferData, NiPropertyState* PropertyState, NiDynamicEffectState* EffectState, NiTransform* WorldTransform, NiBound* WorldBound) {

	UInt32 Result = (this->*LeafUpdatePipeline)(Geometry, SkinInstance, GeometryBufferData, PropertyState, EffectState, WorldTransform, WorldBound);
	SetTreeCollisionConstants(Geometry, WorldTransform, true);
	return Result;

}

static NiNode* FindTreeNode(NiGeometry* Geometry) {

	int Depth = 0;
	for (NiNode* Node = Geometry ? Geometry->m_parent : NULL; Node && Depth < kTreeNodeMaxDepth; Node = Node->m_parent, Depth++) {
		if (*(void**)Node == VFTBSTreeNode) return Node;
	}
	return NULL;

}

void SetTreeCollisionConstants(NiGeometry* Geometry, const NiTransform* WorldTransform, bool Active) {

	D3DXVECTOR4 Constants[4];
	memset(Constants, 0, sizeof(Constants));

	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	bool Eligible = Active && Settings->TreeCollision && Player && WorldTransform && WorldTransform->scale > 0.0f;
	NiNode* Tree = Eligible ? FindTreeNode(Geometry) : NULL;
	if (Tree && Tree->m_kWorldBound.Radius > 0.0f && Tree->m_kWorldBound.Radius <= Settings->TreeCollisionMaxBound) {
		float Bound = Tree->m_kWorldBound.Radius;
		float InvScale = 1.0f / WorldTransform->scale;
		float Reach = Bound + Settings->TreeCollisionRadius;
		float SourceWeights[3] = { 1.0f, TheShaderManager->GrassCollisionWeights[0], TheShaderManager->GrassCollisionWeights[1] };
		float* Slots[3] = { &Constants[1].x, &Constants[1].z, &Constants[2].x };
		float* Weights = &Constants[3].x;
		int Count = 0;
		int SourceCount = min(TheShaderManager->GrassCollisionSourceCount, 3);
		for (int i = 0; i < SourceCount; i++) {
			NiPoint3 Offset;
			Offset.x = TheShaderManager->GrassCollisionSources[i].x - WorldTransform->pos.x;
			Offset.y = TheShaderManager->GrassCollisionSources[i].y - WorldTransform->pos.y;
			Offset.z = 0.0f;
			if (Offset.x * Offset.x + Offset.y * Offset.y > Reach * Reach) continue;
			NiPoint3 Local = WorldTransform->rot < Offset;
			Slots[Count][0] = Local.x * InvScale;
			Slots[Count][1] = Local.y * InvScale;
			Weights[Count] = SourceWeights[i];
			Count++;
		}
		if (Count) {
			Constants[0] = D3DXVECTOR4(Settings->TreeCollisionRadius * InvScale, Settings->TreeCollisionStrength * InvScale, Settings->TreeCollisionFlattenStrength * InvScale, (float)Count);
			Constants[2].z = WorldTransform->scale / Bound;
		}
	}
	TheRenderManager->device->SetVertexShaderConstantF(kTreeCollisionRegister, (const float*)Constants, 4);

}

void CreateTreeCollisionHook() {

	UInt32 Slot = kBranchShaderVtable + kSetupTransformationsSlot;
	*((UInt32*)&BranchSetupTransformations)	= *(UInt32*)Slot;
	TrackBranchSetupTransformations			= &TreeCollisionHook::TrackBranchSetupTransformations;
	// A vtable patch, not a Detour: the original is the BSShader base implementation, shared by
	// every other shader.
	SafeWrite32(Slot, *((UInt32*)&TrackBranchSetupTransformations));

	*((int*)&LeafUpdatePipeline)			= kLeafUpdatePipeline;
	TrackLeafUpdatePipeline					= &TreeCollisionHook::TrackLeafUpdatePipeline;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)LeafUpdatePipeline, *((PVOID*)&TrackLeafUpdatePipeline));
	DetourTransactionCommit();

}
