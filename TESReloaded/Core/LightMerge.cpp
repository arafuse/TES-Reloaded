#include "LightMerge.h"
#include "FrameProfiler.h"
#include "SampleProfiler.h"
#include "LightMergeMath.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

	const UInt32 kShadowSceneNodeTable = 0x00B42F54;
	const UInt32 kSceneLightProcess    = 0x007C78D0;
	const UInt32 kLightGeometryWalk    = 0x007C5F60;
	const UInt32 kLightCullProcess     = 0x007D6390;
	const UInt32 kLightAffectsObject   = 0x007D2EA0;
	const UInt32 kActorLightLevel      = 0x00655FE0;
	const UInt32 kLightLevelFalloff    = 0x007D31B0;
	const UInt32 kActorOwnLight        = 0x004D7FC0;

	const UInt16 kLightStateCulled     = 0xFF;
	const double kLoadGapMs            = 1000.0;
	const double kSnapWindowMs         = 1000.0;
	const int    kMaxSkeletonDepth     = 32;
	const int    kDebugWindowFrames    = 300;
	const int    kDumpEveryFrames      = 30;
	const int    kMaxTimedLights       = 64;
	const int    kMaxProbeLights       = 64;

	/// Light counts sampled from the scene node's list after the last light pass of the frame.
	struct LightStats {
		unsigned Lights, Dynamic, Visible, GeomPairs;
	};

	/// Per-frame cost of the engine light pass and the dynamic-light scene walks.
	struct FrameCost {
		double   ProcessMs, WalkMs, CullMs;
		unsigned ProcessCalls, Walks, Culls;
		LightStats Stats;
	};

	/// A light value ApplyFrame changed, to put back in RestoreFrame.
	struct Override {
		ShadowSceneLight* Scene;
		NiPointLight*     Light;
		NiColor           Diff;
		float             Radius;
		NiPoint3          Pos;
		bool              Leader;
	};

	/// What the merge decided for one light this frame, for the queries.
	struct MergeState {
		int   Override;
		bool  Leader;
		bool  Hidden;
		float Reach;
	};

	FrameCost      gFrame = {};
	double         gLastApplyMs = -1.0;
	double         gBreakMs = -1.0e12;
	double         gFrameMs = 0.0;
	const void*    gLastScene = NULL;
	int            gDebugFramesLeft = 0;
	unsigned       gDebugFrameIndex = 0;
	int            gDepth = 0;
	bool           gAutoSampling = false;
	unsigned       gAffectTests = 0, gAffectRejects = 0;

	/// Every merged light's real values from the last ApplyFrame, keyed by scene light; its Hidden
	/// entries are the refused followers. Kept until the next ApplyFrame, because the engine also
	/// links lights to objects in the update phase, outside the render.
	LightMergeMath::TrueLightTable gTrueLights;

	/// Set while ApplyFrame's values are in the engine lights (until RestoreFrame), so a light level
	/// taken meanwhile, possibly on the AI thread, reads the true values instead.
	volatile LONG gApplied = 0;
	DWORD         gMainThread = 0;
	volatile LONG gLoggedOffThreadLevel = 0;

	/// What the engine's light-level loop summed for one actor, seen through its falloff calls.
	struct LightLevelProbe {
		bool                    Active, Captured, Overflow;
		float                   X, Y, Z;
		int                     Seen;
		const ShadowSceneLight* SeenLights[kMaxProbeLights];
	};
	thread_local LightLevelProbe tProbe;
	unsigned gLevelFixes = 0;
	unsigned       gClusters = 0, gMembers = 0, gHidden = 0;

	LightMergeMath::Clusterer                           gClusterer;
	std::vector<LightMergeMath::LightInput>             gInputs;
	std::vector<ShadowSceneLight*>                      gInputScene;
	std::vector<Override>                               gOverrides;
	std::unordered_map<const NiPointLight*, MergeState> gStates;
	std::unordered_set<const void*>                     gLastLeaders;

	/// Per-light time in the engine's per-light cull (0x7D6390) this frame, for the debug dump.
	struct LightCullTime { ShadowSceneLight* Light; double Ms; unsigned Calls; };
	LightCullTime  gCullTimes[kMaxTimedLights];
	int            gCullTimeCount = 0;

	inline ShadowSceneNode* SceneNode() { return *(ShadowSceneNode**)kShadowSceneNodeTable; }

	inline UInt8 IsDynamic(ShadowSceneLight* Light) { return *((UInt8*)Light + 0x104); }
	inline UInt8 IsPointLight(ShadowSceneLight* Light) { return *((UInt8*)Light + 0xFC); }

	inline double QpcMs(LONGLONG Ticks) {
		LARGE_INTEGER Freq; QueryPerformanceFrequency(&Freq);
		return (double)Ticks * 1000.0 / (double)Freq.QuadPart;
	}

	LightStats SampleLights(ShadowSceneNode* Node) {
		LightStats S = {};
		for (NiTList<ShadowSceneLight>::Entry* E = Node->lights.start; E; E = E->next) {
			ShadowSceneLight* Light = E->data;
			if (!Light) continue;
			S.Lights++;
			if (IsDynamic(Light)) S.Dynamic++;
			if (Light->lightState != kLightStateCulled) {
				S.Visible++;
				S.GeomPairs += Light->unkE4.numItems;
			}
		}
		return S;
	}

	void (__fastcall* SceneLightProcess)(ShadowSceneNode*, void*, NiCamera*) = (void (__fastcall*)(ShadowSceneNode*, void*, NiCamera*))kSceneLightProcess;
	void __fastcall TrackSceneLightProcess(ShadowSceneNode* This, void* Edx, NiCamera* Camera) {
		LONGLONG Start = FrameProfiler::QpcNow();
		{
			FrameProfiler::Scope ProcessScope(FrameProfiler::Buck_LightProcess);
			SceneLightProcess(This, Edx, Camera);
		}
		gFrame.ProcessMs += QpcMs(FrameProfiler::QpcNow() - Start);
		gFrame.ProcessCalls++;
		gFrame.Stats = SampleLights(This);
	}

	void (__fastcall* LightGeometryWalk)(void*, void*, ShadowSceneLight*) = (void (__fastcall*)(void*, void*, ShadowSceneLight*))kLightGeometryWalk;
	void __fastcall TrackLightGeometryWalk(void* This, void* Edx, ShadowSceneLight* Light) {
		LONGLONG Start = FrameProfiler::QpcNow();
		{
			FrameProfiler::Scope WalkScope(FrameProfiler::Buck_LightWalk);
			LightGeometryWalk(This, Edx, Light);
		}
		gFrame.WalkMs += QpcMs(FrameProfiler::QpcNow() - Start);
		gFrame.Walks++;
	}

	bool IsHiddenScene(const ShadowSceneLight* Light) {
		const LightMergeMath::TrueLight* L = gTrueLights.Read().Find(Light);
		return L && L->Hidden;
	}

	/// 0x7D2EA0, "does this light reach this object": the cull re-asks it for every light on every
	/// visible object each frame (0x7C6100), and the light's update-phase scene walk prunes on it.
	/// Answering no for an absorbed follower drops it from every object, which is what saves the time.
	bool (__fastcall* LightAffectsObject)(ShadowSceneLight*, void*, NiAVObject*, void*) = (bool (__fastcall*)(ShadowSceneLight*, void*, NiAVObject*, void*))kLightAffectsObject;
	bool __fastcall TrackLightAffectsObject(ShadowSceneLight* This, void* Edx, NiAVObject* Object, void* Light) {
		gAffectTests++;
		if (IsHiddenScene(This)) {
			gAffectRejects++;
			return false;
		}
		return LightAffectsObject(This, Edx, Object, Light);
	}

	/// 0x7D31B0, one light's share of an actor's light level: (1 - clamp(d/r)^2) * max(diffuse),
	/// skipped for the actor's own light. Only the light level calls it. Records what the NPC loop
	/// summed, and while a frame's merge is applied answers for merged lights from their true values.
	float (__fastcall* LightLevelFalloff)(ShadowSceneLight*, void*, float, float, float, const void*) = (float (__fastcall*)(ShadowSceneLight*, void*, float, float, float, const void*))kLightLevelFalloff;
	float __fastcall TrackLightLevelFalloff(ShadowSceneLight* This, void* Edx, float X, float Y, float Z, const void* Exclude) {
		LightLevelProbe& P = tProbe;
		if (P.Active) {
			P.Captured = true;
			P.X = X; P.Y = Y; P.Z = Z;
			if (P.Seen < kMaxProbeLights) P.SeenLights[P.Seen++] = This;
			else P.Overflow = true;
		}
		float Share = LightLevelFalloff(This, Edx, X, Y, Z, Exclude);
		if (!gApplied) return Share;
		const LightMergeMath::TrueLight* L = gTrueLights.Read().Find(This);
		if (!L || L->Source == Exclude) return Share;
		float At[3] = { X, Y, Z };
		return LightMergeMath::LightLevelShare(*L, At, Exclude);
	}

	/// 0x655FE0, an actor's light level (0-100) for detection. The player's path sums the scene
	/// node's own light list, which keeps hidden followers. An NPC's sums the lights linked to its
	/// lighting property, which the refused followers have left, so their share is added back from
	/// the true values; adding before the 100 clamp equals adding after it, as every share is >= 0.
	float (__fastcall* ActorLightLevel)(void*, void*, Actor*, UInt32) = (float (__fastcall*)(void*, void*, Actor*, UInt32))kActorLightLevel;

	/// The light 0x655FE0 leaves out of an actor's level: with its flag set, the actor's own carried
	/// light (0x4D7FC0 on the actor), else none. Read here because the sun call that ends the
	/// engine's sum passes no exclude, so the probe cannot be trusted for it.
	const void* ActorOwnLight(Actor* Subject, UInt32 Flag) {
		if (!(UInt8)Flag) return NULL;
		void** Own = (void**)ThisCall(kActorOwnLight, Subject);
		return Own ? *Own : NULL;
	}

	/// On the main thread only (where the scene node's list cannot change underneath): is this scene
	/// light still live, so the engine's own falloff can be called on it?
	bool IsLiveOnMainThread(const void* Scene) {
		if (GetCurrentThreadId() != gMainThread || gApplied) return false;
		ShadowSceneNode* Node = SceneNode();
		if (!Node) return false;
		for (NiTList<ShadowSceneLight>::Entry* E = Node->lights.start; E; E = E->next)
			if (E->data == Scene) return true;
		return false;
	}

	/// A hidden follower's share as vanilla would have summed it: only if it would have been linked
	/// to the actor (the reach test on the actor's bound, planes included); through the engine's own
	/// falloff when it is safe to call (so its +0xD0 cache is written as in vanilla), else from the
	/// true values.
	float HiddenFollowerShare(const LightMergeMath::TrueLight& H, const NiBound& Bound, const float At[3], const void* Own) {
		float Center[3] = { Bound.Center.x, Bound.Center.y, Bound.Center.z };
		if (!LightMergeMath::ReachesSphere(H, Center, Bound.Radius)) return 0.0f;
		if (IsLiveOnMainThread(H.Key))
			return LightLevelFalloff((ShadowSceneLight*)H.Key, NULL, At[0], At[1], At[2], Own);
		return LightMergeMath::LightLevelShare(H, At, Own);
	}
	float __fastcall TrackActorLightLevel(void* Process, void* Edx, Actor* Subject, UInt32 Flag) {
		if (gApplied && GetCurrentThreadId() != gMainThread && !InterlockedExchange(&gLoggedOffThreadLevel, 1))
			Logger::Log("[LightMerge] an actor light level ran on another thread during the render (answered from true values)");
		LightMergeMath::TrueLightTable::View Merged = gTrueLights.Read();
		bool AnyHidden = false;
		for (int h = 0; h < Merged.Count && !AnyHidden; h++) AnyHidden = Merged.Lights[h].Hidden;
		if (!AnyHidden || (void*)Subject == (void*)Player) return ActorLightLevel(Process, Edx, Subject, Flag);
		LightLevelProbe Outer = tProbe;
		LightLevelProbe& P = tProbe;
		P.Active = true; P.Captured = false; P.Overflow = false; P.Seen = 0;
		float Level = ActorLightLevel(Process, Edx, Subject, Flag);
		P.Active = false;
		float Added = 0.0f;
		NiNode* Root = Subject->GetNode();
		if (P.Captured && !P.Overflow && Root) {
			float At[3] = { P.X, P.Y, P.Z };
			const void* Own = ActorOwnLight(Subject, Flag);
			for (int h = 0; h < Merged.Count; h++) {
				const LightMergeMath::TrueLight& H = Merged.Lights[h];
				if (!H.Hidden) continue;
				bool Summed = false;
				for (int i = 0; i < P.Seen && !Summed; i++) Summed = P.SeenLights[i] == H.Key;
				if (!Summed) Added += HiddenFollowerShare(H, Root->m_kWorldBound, At, Own);
			}
		}
		tProbe = Outer;
		if (Added <= 0.0f) return Level;
		gLevelFixes++;
		return std::fmin(100.0f, Level + Added * 100.0f);
	}

	void AddCullTime(ShadowSceneLight* Light, double Ms) {
		for (int i = 0; i < gCullTimeCount; i++)
			if (gCullTimes[i].Light == Light) { gCullTimes[i].Ms += Ms; gCullTimes[i].Calls++; return; }
		if (gCullTimeCount < kMaxTimedLights) gCullTimes[gCullTimeCount++] = { Light, Ms, 1 };
	}

	void (__fastcall* LightCullProcess)(ShadowSceneLight*, void*, void*) = (void (__fastcall*)(ShadowSceneLight*, void*, void*))kLightCullProcess;
	void __fastcall TrackLightCullProcess(ShadowSceneLight* This, void* Edx, void* Arg) {
		LONGLONG Start = FrameProfiler::QpcNow();
		LightCullProcess(This, Edx, Arg);
		double Ms = QpcMs(FrameProfiler::QpcNow() - Start);
		gFrame.CullMs += Ms;
		gFrame.Culls++;
		AddCullTime(This, Ms);
	}

	const char* NodeName(NiNode* Node) {
		return Node && Node->m_pcName ? Node->m_pcName : "-";
	}

	bool IsCarriedByActor(NiPointLight* Light);

	/// One line per scene light: the flags the merge filters on, and where the light hangs.
	void DumpLights() {
		ShadowSceneNode* Node = SceneNode();
		if (!Node) return;
		for (NiTList<ShadowSceneLight>::Entry* E = Node->lights.start; E; E = E->next) {
			ShadowSceneLight* Scene = E->data;
			if (!Scene) continue;
			UInt8* B = (UInt8*)Scene;
			NiPointLight* L = Scene->sourceLight;
			double Ms = 0.0; unsigned Calls = 0;
			for (int i = 0; i < gCullTimeCount; i++)
				if (gCullTimes[i].Light == Scene) { Ms = gCullTimes[i].Ms; Calls = gCullTimes[i].Calls; }
			if (!L) {
				Logger::Log("[LightMerge]   ssl %p no source light, cull %u %.3f ms", Scene, Calls, Ms);
				continue;
			}
			NiNode* P1 = L->m_parent;
			NiNode* P2 = P1 ? P1->m_parent : NULL;
			NiNode* P3 = P2 ? P2->m_parent : NULL;
			Logger::Log("[LightMerge]   ssl %p F4 %u FC %u 104 %u state %u geom %u | carry %u actor %u ref %u r %.0f diff (%.2f %.2f %.2f) dim %.2f pos (%.0f %.0f %.0f) | cull %u %.3f ms | %s < %s < %s",
				Scene, B[0xF4], B[0xFC], B[0x104], (unsigned)Scene->lightState, Scene->unkE4.numItems,
				(unsigned)L->CanCarry, IsCarriedByActor(L) ? 1u : 0u, (unsigned)L->m_uiRefCount, L->Spec.r, L->Diff.r, L->Diff.g, L->Diff.b, L->Dimmer,
				L->m_worldTransform.pos.x, L->m_worldTransform.pos.y, L->m_worldTransform.pos.z,
				Calls, Ms, NodeName(P1), NodeName(P2), NodeName(P3));
		}
	}

	/// True on the first frame after a load screen, a long pause, or a move to another worldspace or
	/// interior. Crossing an exterior cell border keeps the groups (and their fades) going.
	bool DetectDiscontinuity(double NowMs) {
		TESObjectCELL* Cell = Player ? Player->parentCell : NULL;
		const void* Scene = Cell && Cell->worldSpace ? (const void*)Cell->worldSpace : (const void*)Cell;
		if (!Scene) {
			gLastScene = NULL;
			return false;
		}
		double GapMs = gLastApplyMs < 0.0 ? kLoadGapMs + 1.0 : NowMs - gLastApplyMs;
		bool Break = LightMergeMath::IsSceneBreak(gLastScene, Scene, GapMs, kLoadGapMs);
		gLastScene = Scene;
		return Break;
	}

	bool IsMagicLight(NiPointLight* Light) {
		NiNode* Parent = Light->m_parent;
		return Parent && Parent->m_pcName && strstr(Parent->m_pcName, "agic") != NULL;
	}

	/// True when the light hangs under a skeleton bone ("Bip01 ..."), i.e. an actor carries it; a
	/// carryable light placed in the world or dropped on the ground has no skeleton above it.
	bool IsCarriedByActor(NiPointLight* Light) {
		int Depth = 0;
		for (NiNode* Node = Light->m_parent; Node && Depth < kMaxSkeletonDepth; Node = Node->m_parent, Depth++)
			if (Node->m_pcName && !strncmp(Node->m_pcName, "Bip01", 5)) return true;
		return false;
	}

	bool IsPlayerLight(NiPointLight* Light) {
		NiNode* ThirdPerson = Player->GetNode();
		NiNode* FirstPerson = Player->firstPersonNiNode;
		for (NiNode* Node = Light->m_parent; Node; Node = Node->m_parent)
			if (Node == ThirdPerson || Node == FirstPerson) return true;
		return false;
	}

	/// Live point lights of the scene node; carried NPC lights are the eligible ones. Carried torches
	/// leave +0x104 clear, so it is not a filter.
	void CollectInputs(ShadowSceneNode* Node) {
		gInputs.clear();
		gInputScene.clear();
		for (NiTList<ShadowSceneLight>::Entry* E = Node->lights.start; E; E = E->next) {
			ShadowSceneLight* Scene = E->data;
			if (!Scene || !IsPointLight(Scene)) continue;
			NiPointLight* Light = Scene->sourceLight;
			if (!Light || Light->m_uiRefCount <= 1) continue;
			LightMergeMath::LightInput In = {};
			In.Key = Scene;
			In.Pos[0] = Light->m_worldTransform.pos.x;
			In.Pos[1] = Light->m_worldTransform.pos.y;
			In.Pos[2] = Light->m_worldTransform.pos.z;
			In.Color[0] = Light->Diff.r;
			In.Color[1] = Light->Diff.g;
			In.Color[2] = Light->Diff.b;
			In.Radius = Light->Spec.r;
			In.Eligible = Light->CanCarry && !IsMagicLight(Light) && !IsPlayerLight(Light) && IsCarriedByActor(Light);
			gInputs.push_back(In);
			gInputScene.push_back(Scene);
		}
	}

	LightMergeMath::Params CurrentParams() {
		const SettingsMainStruct::LightMergeStruct& S = TheSettingManager->SettingsMain.LightMerge;
		LightMergeMath::Params P = { S.JoinDistance, S.LeaveDistance, S.FadeTime, S.BrightnessExponent, S.ColorTolerance, S.RadiusTolerance };
		return LightMergeMath::Sanitize(P);
	}

	/// Publishes the true values of every light this frame's merge changes, before it changes them.
	void PublishTrueLights(const std::vector<LightMergeMath::LightOutput>& Outputs) {
		LightMergeMath::TrueLight* Out = gTrueLights.BeginWrite();
		int Count = 0;
		for (size_t i = 0; i < Outputs.size() && Count < LightMergeMath::TrueLightTable::Capacity; i++) {
			const LightMergeMath::LightOutput& O = Outputs[i];
			if (O.Role == LightMergeMath::Role_Solo) continue;
			const LightMergeMath::LightInput& In = gInputs[i];
			LightMergeMath::TrueLight& T = Out[Count++];
			T.Key = gInputScene[i];
			T.Source = gInputScene[i]->sourceLight;
			for (int k = 0; k < 3; k++) T.Pos[k] = In.Pos[k];
			T.Radius = In.Radius;
			T.MaxDiffuse = std::fmax(In.Color[0], std::fmax(In.Color[1], In.Color[2]));
			T.Hidden = O.Hidden;
			const UInt8* B = (const UInt8*)gInputScene[i];
			T.PlanesActive = *(const UInt32*)(B + 0x14C) != 0;
			T.PlaneMask = *(const UInt32*)(B + 0x1B0);
			memcpy(T.Planes, B + 0x150, sizeof(T.Planes));
		}
		gTrueLights.Publish(Count);
	}

	/// Writes this frame's merge into the engine lights, remembering every original value.
	void ApplyMerge() {
		const std::vector<LightMergeMath::LightOutput>& Outputs = gClusterer.Outputs();
		std::unordered_set<const void*> Leaders;
		PublishTrueLights(Outputs);
		InterlockedExchange(&gApplied, 1);
		for (size_t i = 0; i < Outputs.size(); i++) {
			const LightMergeMath::LightOutput& O = Outputs[i];
			if (O.Role == LightMergeMath::Role_Solo) continue;
			NiPointLight* Light = gInputScene[i]->sourceLight;
			bool Leader = O.Role == LightMergeMath::Role_Leader;
			Override Saved = { gInputScene[i], Light, Light->Diff, Light->Spec.r, Light->m_worldTransform.pos, Leader };
			MergeState State = { (int)gOverrides.size(), Leader, O.Hidden, Leader ? O.Radius - gInputs[i].Radius : 0.0f };
			gOverrides.push_back(Saved);
			gStates[Light] = State;
			Light->Diff.r *= O.ColorScale;
			Light->Diff.g *= O.ColorScale;
			Light->Diff.b *= O.ColorScale;
			gMembers++;
			if (O.Hidden) gHidden++;
			if (!Leader) continue;
			gClusters++;
			Leaders.insert(O.Key);
			Light->Spec.r = O.Radius;
			Light->m_worldTransform.pos.x = O.Pos[0];
			Light->m_worldTransform.pos.y = O.Pos[1];
			Light->m_worldTransform.pos.z = O.Pos[2];
		}
		if (TheSettingManager->SettingsMain.Develop.LightMergeDebug) {
			for (const void* Key : Leaders)
				if (!gLastLeaders.count(Key)) Logger::Log("[LightMerge] cluster formed: leader %p", Key);
			for (const void* Key : gLastLeaders)
				if (!Leaders.count(Key)) Logger::Log("[LightMerge] cluster ended: leader %p", Key);
		}
		gLastLeaders.swap(Leaders);
	}

	/// Puts back every value ApplyMerge changed, for lights still in the scene node's list.
	void RestoreMerge() {
		if (gOverrides.empty()) { gStates.clear(); return; }
		ShadowSceneNode* Node = SceneNode();
		std::unordered_set<ShadowSceneLight*> Live;
		if (Node)
			for (NiTList<ShadowSceneLight>::Entry* E = Node->lights.start; E; E = E->next) Live.insert(E->data);
		for (const Override& O : gOverrides) {
			if (!Live.count(O.Scene) || O.Scene->sourceLight != O.Light) continue;
			O.Light->Diff = O.Diff;
			if (!O.Leader) continue;
			O.Light->Spec.r = O.Radius;
			O.Light->m_worldTransform.pos = O.Pos;
		}
		gOverrides.clear();
		gStates.clear();
	}

	/// LightMergeDebug >= 2: samples the main thread from a load until the scene's lights go out.
	/// The report's own time is kept out of the next frame's gap so it is not taken for a load.
	void StopAutoSampler(const char* Why) {
		if (!gAutoSampling) return;
		gAutoSampling = false;
		Logger::Log("[LightMerge] sampler stopped (%s) at debug frame %u", Why, gDebugFrameIndex);
		double Before = TheFrameRateManager->GetPerformance();
		SampleProfiler::End();
		gLastApplyMs += TheFrameRateManager->GetPerformance() - Before;
	}

	void UpdateAutoSampler(bool WindowStart) {
		if (TheSettingManager->SettingsMain.Develop.LightMergeDebug < 2) return;
		if (gAutoSampling) {
			if (gFrame.Stats.Lights == 0) StopAutoSampler("lights out");
			else if (gDebugFramesLeft <= 1) StopAutoSampler("window end");
			return;
		}
		if (!WindowStart || gFrame.Stats.Lights == 0 || SampleProfiler::IsRunning()) return;
		Logger::Log("[LightMerge] sampler started with %u lights", gFrame.Stats.Lights);
		SampleProfiler::Begin();
		gAutoSampling = SampleProfiler::IsRunning();
	}

	void LogDebugRow() {
		const FrameCost& F = gFrame;
		Logger::Log("[LightMerge] frame %3u  %7.2f ms | walks %3u %6.3f ms | process %u %6.3f ms | light culls %3u %6.3f ms | lights %u dyn %u vis %u geom %u | merged %u in %u, hidden %u | reach tests %u, refused %u | npc light fixes %u",
			gDebugFrameIndex, gFrameMs, F.Walks, F.WalkMs, F.ProcessCalls, F.ProcessMs, F.Culls, F.CullMs,
			F.Stats.Lights, F.Stats.Dynamic, F.Stats.Visible, F.Stats.GeomPairs, gMembers, gClusters, gHidden, gAffectTests, gAffectRejects, gLevelFixes);
	}
}

void CreateLightMergeHook() {

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)SceneLightProcess, &TrackSceneLightProcess);
	DetourAttach(&(PVOID&)LightGeometryWalk, &TrackLightGeometryWalk);
	if (TheSettingManager->SettingsMain.Develop.LightMergeDebug) DetourAttach(&(PVOID&)LightCullProcess, &TrackLightCullProcess);
	if (TheSettingManager->SettingsMain.LightMerge.Enabled) {
		DetourAttach(&(PVOID&)LightAffectsObject, &TrackLightAffectsObject);
		DetourAttach(&(PVOID&)ActorLightLevel, &TrackActorLightLevel);
		DetourAttach(&(PVOID&)LightLevelFalloff, &TrackLightLevelFalloff);
	}
	if (DetourTransactionCommit() != NO_ERROR) {
		Logger::Log("[LightMerge] light pass hooks failed; not installed");
		return;
	}
	Logger::Log("[LightMerge] installed (enabled %u, debug %u)",
		TheSettingManager->SettingsMain.LightMerge.Enabled ? 1u : 0u, (unsigned)TheSettingManager->SettingsMain.Develop.LightMergeDebug);

}

namespace LightMerge {

	void ApplyFrame() {
		// The save thumbnail re-enters kRender; only the outermost frame applies and restores.
		if (++gDepth > 1) return;
		gMainThread = GetCurrentThreadId();
		double NowMs = TheFrameRateManager->GetPerformance();
		bool Discontinuity = DetectDiscontinuity(NowMs);
		gFrameMs = gLastApplyMs < 0.0 ? 0.0 : NowMs - gLastApplyMs;
		gLastApplyMs = NowMs;
		gFrame = FrameCost();
		gCullTimeCount = 0;
		gClusters = gMembers = gHidden = 0;
		if (Discontinuity && TheSettingManager->SettingsMain.Develop.LightMergeDebug) {
			StopAutoSampler("load or scene change");
			gDebugFramesLeft = kDebugWindowFrames;
			gDebugFrameIndex = 0;
			Logger::Log("[LightMerge] ---- load or scene change: logging %d frames ----", kDebugWindowFrames);
		}

		if (!TheSettingManager->SettingsMain.LightMerge.Enabled) return;
		ShadowSceneNode* Node = SceneNode();
		if (!Node || !Player || !Player->parentCell) {
			gClusterer.Reset();
			gTrueLights.Publish(0);
			return;
		}
		// Keys are scene-light pointers, whose addresses a load or cell change may reuse.
		if (Discontinuity) {
			gClusterer.Reset();
			gBreakMs = NowMs;
		}
		CollectInputs(Node);
		bool Snap = Discontinuity || LightMergeMath::IsSnapWindow(NowMs - gBreakMs, kSnapWindowMs);
		float Delta = Snap ? 0.0f : (float)(gFrameMs / 1000.0);
		gClusterer.Update(gInputs.data(), (int)gInputs.size(), CurrentParams(), Delta, Snap);
		ApplyMerge();
	}

	void RestoreFrame() {
		if (--gDepth > 0) return;
		gDepth = 0;
		RestoreMerge();
		InterlockedExchange(&gApplied, 0);
		FrameProfiler::Count(FrameProfiler::Cnt_LightProcessCalls, gFrame.ProcessCalls);
		FrameProfiler::Count(FrameProfiler::Cnt_LightWalks, gFrame.Walks);
		FrameProfiler::Count(FrameProfiler::Cnt_Lights, gFrame.Stats.Lights);
		FrameProfiler::Count(FrameProfiler::Cnt_LightsDynamic, gFrame.Stats.Dynamic);
		FrameProfiler::Count(FrameProfiler::Cnt_LightsVisible, gFrame.Stats.Visible);
		FrameProfiler::Count(FrameProfiler::Cnt_LightGeomPairs, gFrame.Stats.GeomPairs);
		FrameProfiler::Count(FrameProfiler::Cnt_MergeClusters, gClusters);
		FrameProfiler::Count(FrameProfiler::Cnt_MergeMembers, gMembers);
		FrameProfiler::Count(FrameProfiler::Cnt_MergeHidden, gHidden);
		if (gDebugFramesLeft > 0) {
			UpdateAutoSampler(gDebugFrameIndex == 0);
			LogDebugRow();
			if (gDebugFrameIndex % kDumpEveryFrames == 0) DumpLights();
			gDebugFramesLeft--;
			gDebugFrameIndex++;
		}
		gAffectTests = gAffectRejects = gLevelFixes = 0;
	}

	bool IsHidden(const NiPointLight* Light) {
		auto It = gStates.find(Light);
		return It != gStates.end() && It->second.Hidden;
	}

	bool TruePosition(const NiPointLight* Light, NiPoint3* Out) {
		auto It = gStates.find(Light);
		if (It == gStates.end() || !It->second.Leader) return false;
		*Out = gOverrides[It->second.Override].Pos;
		return true;
	}

	float ShadowReach(const NiPointLight* Light) {
		auto It = gStates.find(Light);
		return It == gStates.end() ? 0.0f : It->second.Reach;
	}
}
