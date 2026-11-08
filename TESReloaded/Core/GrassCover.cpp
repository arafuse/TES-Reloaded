#include "GrassCover.h"
#include "GrassCoverMath.h"
#include "RenderHook.h"
#include "Seqlock.h"

static const UInt32	kGrassMap				= 0x00B2CBC4; // NiTMap<GRAS formID, grass type>, main thread only
static const UInt32	kGrassMapBucketCount	= 0x04;
static const UInt32	kGrassMapBuckets		= 0x08;
static const UInt32	kGrassMapMaxBuckets		= 65536;
static const UInt32	kGrassTypeBatchList		= 0x24;
static const UInt32	kGrassBatchOwner		= 0x00;
static const UInt32	kGrassBatchCapacity		= 0x0C;
static const UInt32	kGrassBatchUsed			= 0x0E;
static const UInt32	kGrassBatchInstances	= 0x10;
static const float	kGrassBatchReach		= 6000.0f; // a batch spans at most one cell, so no instance is farther than this from its first
static const float	kGrassCoverTorsoHeight	= 50.0f;
static const UInt32	kMovementSneak			= 0x400;
static const UInt32	kMovementSwim			= 0x800;
static const int	kGrassCoverMaxClumps	= 96;

struct GrassMapItem {
	GrassMapItem*	Next;
	UInt32			FormID;
	UInt8*			Type;
};

struct GrassBatchNode {
	GrassBatchNode*	Next;
	GrassBatchNode*	Prev;
	UInt8*			Batch;
};

/// The tall clumps around the sneaking player this frame, for the line-of-sight hook.
struct GrassCoverSnapshot {
	int			Count;
	float		TorsoX, TorsoY, TorsoZ;
	GrassClump	Clumps[kGrassCoverMaxClumps];
};

/// Inputs shared by every batch of one scan, and the nearest-clump list it fills.
struct GrassCoverScan {
	float				PX, PY, PZ;
	float				Radius;
	float				GroundcoverMaxHeight;
	float				ScaleX, ScaleZ, ScaleBias;
	GrassCoverParams	Params;
	GrassCoverSnapshot*	Out;
	float				DistSqs[kGrassCoverMaxClumps];
};

static Seqlock<GrassCoverSnapshot>	Snapshot;
static volatile float				PlayerGrassCover = 0.0f;

// The player's live grass collision radius, or 0 when grass collision isn't bending grass this frame.
static float PlayerCollisionRadius() {

	if (!TheSettingManager->SettingsMain.Shaders.Grass || TheShaderManager->GrassCollisionSourceCount <= 0) return 0.0f;
	const ShaderManager::GrassSource& Source = TheShaderManager->GrassCollisionSources[0];
	if (Source.kind != ShaderManager::GrassSource_Player) return 0.0f;
	return TheSettingManager->SettingsGrass.CollisionRadius * Source.radiusScale;

}

static void ScanGrassBatch(GrassCoverScan& Scan, UInt8* Batch) {

	NiGeometry* Owner = *(NiGeometry**)(Batch + kGrassBatchOwner);
	UInt16 Capacity = *(UInt16*)(Batch + kGrassBatchCapacity);
	UInt16 Used = *(UInt16*)(Batch + kGrassBatchUsed);
	const float* Instances = *(const float**)(Batch + kGrassBatchInstances);
	if (!Owner || !Owner->geomData || !Instances || !Used || Used > Capacity) return;

	float Reach = kGrassBatchReach + Scan.Radius;
	float FirstX = Instances[0] - Scan.PX;
	float FirstY = Instances[1] - Scan.PY;
	if (FirstX * FirstX + FirstY * FirstY > Reach * Reach) return;

	GrassModelMetrics Model = GetGrassModelMetrics(Owner);
	if (Model.Height <= Scan.GroundcoverMaxHeight) return;
	float HalfWidth = Model.HalfWidth * Scan.ScaleX;
	float Near = Scan.Radius + HalfWidth;

	GrassCoverSnapshot& Out = *Scan.Out;
	for (UInt16 i = 0; i < Used; i++) {
		const float* Instance = Instances + i * 4;
		float DX = Instance[0] - Scan.PX;
		float DY = Instance[1] - Scan.PY;
		float DistSq = DX * DX + DY * DY;
		if (DistSq > Near * Near) continue;
		float HeightFactor = 0.01f * Instance[3] * Scan.ScaleZ + Scan.ScaleBias;
		GrassClump Clump;
		if (!GrassCoverClumpOf(Scan.PX, Scan.PY, Scan.PZ, Instance[0], Instance[1], Instance[2], Model.Height, HeightFactor, HalfWidth, Scan.Params, Clump)) continue;
		GrassCoverKeepNearest(Out.Clumps, Scan.DistSqs, Out.Count, kGrassCoverMaxClumps, Clump, DistSq);
	}

}

static float ScanGrassCover(GrassCoverSnapshot& Out) {

	const SettingsGrassStruct& Settings = TheSettingManager->SettingsGrass;
	if ((!Settings.GrassCover && !Settings.GrassCoverBlockLOS) || !Player || !Player->process || !Player->parentCell || !Player->GetWorldSpace()) {
		return 0.0f;
	}
	UInt32 Movement = Player->process->GetMovementFlags();
	if (!(Movement & kMovementSneak) || (Movement & kMovementSwim)) return 0.0f;

	UInt32 BucketCount = *(UInt32*)(kGrassMap + kGrassMapBucketCount);
	GrassMapItem** Buckets = *(GrassMapItem***)(kGrassMap + kGrassMapBuckets);
	if (!Buckets || BucketCount > kGrassMapMaxBuckets) return 0.0f;

	GrassCoverScan Scan = {};
	Scan.PX = Player->pos.x;
	Scan.PY = Player->pos.y;
	Scan.PZ = Player->pos.z;
	Scan.Radius = Settings.GrassCoverRadius;
	Scan.GroundcoverMaxHeight = Settings.GroundcoverMaxHeight;
	Scan.ScaleX = Settings.ScaleX;
	Scan.ScaleZ = Settings.ScaleZ;
	Scan.ScaleBias = Settings.MinHeight;
	Scan.Params.MinHeight = Settings.GrassCoverMinHeight;
	Scan.Params.FullHeight = Settings.GrassCoverFullHeight;
	Scan.Params.Opacity = Settings.GrassCoverOpacity;
	Scan.Params.CollisionRadius = PlayerCollisionRadius();
	Scan.Params.PushStrength = Settings.CollisionStrength;
	Scan.Params.FlattenStrength = Settings.CollisionFlattenStrength;
	Scan.Out = &Out;

	Out.TorsoX = Scan.PX;
	Out.TorsoY = Scan.PY;
	Out.TorsoZ = Scan.PZ + kGrassCoverTorsoHeight;

	for (UInt32 b = 0; b < BucketCount; b++) {
		for (GrassMapItem* Item = Buckets[b]; Item; Item = Item->Next) {
			if (!Item->Type) continue;
			for (GrassBatchNode* Node = *(GrassBatchNode**)(Item->Type + kGrassTypeBatchList); Node; Node = Node->Next) {
				if (Node->Batch) ScanGrassBatch(Scan, Node->Batch);
			}
		}
	}
	return GrassCoverAt(Out.Clumps, Out.Count, Scan.PX, Scan.PY);

}

void UpdateGrassCover() {

	static GrassCoverSnapshot Next;
	Next = {};
	PlayerGrassCover = ScanGrassCover(Next);
	Snapshot.Publish(Next);

}

float GetPlayerGrassCover() {

	return PlayerGrassCover;

}

float GetGrassSightDepth(float EyeX, float EyeY, float EyeZ) {

	GrassCoverSnapshot Clumps;
	if (!Snapshot.Read(Clumps) || !Clumps.Count) return 0.0f;
	return GrassCoverRayDepth(Clumps.Clumps, Clumps.Count, EyeX, EyeY, EyeZ, Clumps.TorsoX, Clumps.TorsoY, Clumps.TorsoZ);

}
