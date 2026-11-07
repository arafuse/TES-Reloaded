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

// Fills the c240-c243 block for one tree and returns how many sources reach it (0 = no bend).
static int BuildTreeCollisionConstants(NiGeometry* Geometry, const NiTransform* WorldTransform, bool Active, D3DXVECTOR4* Constants) {

	memset(Constants, 0, sizeof(D3DXVECTOR4) * 4);
	int Count = 0;

	SettingsGrassStruct* Settings = &TheSettingManager->SettingsGrass;
	bool Eligible = Active && Settings->TreeCollision && Player && WorldTransform && WorldTransform->scale > 0.0f;
	NiNode* Tree = Eligible ? FindTreeNode(Geometry) : NULL;
	if (Tree && Tree->m_kWorldBound.Radius > 0.0f && Tree->m_kWorldBound.Radius <= Settings->TreeCollisionMaxBound) {
		float Bound = Tree->m_kWorldBound.Radius;
		float InvScale = 1.0f / WorldTransform->scale;
		float* Slots[3] = { &Constants[1].x, &Constants[1].z, &Constants[2].x };
		float* Radii[3] = { &Constants[0].x, &Constants[2].w, &Constants[3].w };
		float* Weights = &Constants[3].x;
		// The list is in priority order, so a tree reached by more than three sources keeps the first.
		int SourceCount = TheShaderManager->GrassCollisionSourceCount;
		for (int i = 0; i < SourceCount && Count < 3; i++) {
			const ShaderManager::GrassSource& Source = TheShaderManager->GrassCollisionSources[i];
			if (Source.weight == 0.0f) continue;
			float Radius = Settings->TreeCollisionRadius * Source.radiusScale;
			float Reach = Bound + Radius;
			NiPoint3 Offset;
			Offset.x = Source.x - WorldTransform->pos.x;
			Offset.y = Source.y - WorldTransform->pos.y;
			Offset.z = 0.0f;
			if (Offset.x * Offset.x + Offset.y * Offset.y > Reach * Reach) continue;
			NiPoint3 Local = WorldTransform->rot < Offset;
			Slots[Count][0] = Local.x * InvScale;
			Slots[Count][1] = Local.y * InvScale;
			*Radii[Count] = Radius * InvScale;
			Weights[Count] = Source.weight;
			Count++;
		}
		if (Count) {
			Constants[0].y = Settings->TreeCollisionStrength * InvScale;
			Constants[0].z = Settings->TreeCollisionFlattenStrength * InvScale;
			Constants[0].w = (float)Count;
			Constants[2].z = WorldTransform->scale / Bound;
		}
	}
	return Count;

}

void SetTreeCollisionConstants(NiGeometry* Geometry, const NiTransform* WorldTransform, bool Active) {

	D3DXVECTOR4 Constants[4];
	BuildTreeCollisionConstants(Geometry, WorldTransform, Active, Constants);
	TheRenderManager->device->SetVertexShaderConstantF(kTreeCollisionRegister, (const float*)Constants, 4);

}

bool TreeCollisionReaches(NiGeometry* Geometry, const NiTransform* WorldTransform) {

	D3DXVECTOR4 Constants[4];
	return BuildTreeCollisionConstants(Geometry, WorldTransform, true, Constants) > 0;

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
