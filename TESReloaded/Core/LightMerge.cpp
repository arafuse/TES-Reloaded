#include "LightMerge.h"
#include "FrameProfiler.h"
#include "LightMergeMath.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

	const UInt32 kShadowSceneNodeTable = 0x00B42F54;
	const UInt32 kSceneLightProcess    = 0x007C78D0;
	const UInt32 kLightGeometryWalk    = 0x007C5F60;

	const UInt16 kLightStateCulled     = 0xFF;
	const double kLoadGapMs            = 1000.0;
	const int    kDebugWindowFrames    = 300;

	/// Light counts sampled from the scene node's list after the last light pass of the frame.
	struct LightStats {
		unsigned Lights, Dynamic, Visible, GeomPairs;
	};

	/// Per-frame cost of the engine light pass and the dynamic-light scene walks.
	struct FrameCost {
		double   ProcessMs, WalkMs;
		unsigned ProcessCalls, Walks;
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
	double         gFrameMs = 0.0;
	TESObjectCELL* gLastCell = NULL;
	int            gDebugFramesLeft = 0;
	unsigned       gDebugFrameIndex = 0;
	int            gDepth = 0;
	unsigned       gClusters = 0, gMembers = 0, gHidden = 0;

	LightMergeMath::Clusterer                           gClusterer;
	std::vector<LightMergeMath::LightInput>             gInputs;
	std::vector<ShadowSceneLight*>                      gInputScene;
	std::vector<Override>                               gOverrides;
	std::unordered_map<const NiPointLight*, MergeState> gStates;
	std::unordered_set<const void*>                     gLastLeaders;

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

	/// True on the first frame after a load screen, a long pause or a cell change.
	bool DetectDiscontinuity(double NowMs) {
		bool Gap = gLastApplyMs < 0.0 || NowMs - gLastApplyMs > kLoadGapMs;
		TESObjectCELL* Cell = Player ? Player->parentCell : NULL;
		bool CellChanged = Cell != gLastCell;
		gLastCell = Cell;
		return Gap || CellChanged;
	}

	bool IsMagicLight(NiPointLight* Light) {
		NiNode* Parent = Light->m_parent;
		return Parent && Parent->m_pcName && strstr(Parent->m_pcName, "agic") != NULL;
	}

	bool IsPlayerLight(NiPointLight* Light) {
		NiNode* ThirdPerson = Player->GetNode();
		NiNode* FirstPerson = Player->firstPersonNiNode;
		for (NiNode* Node = Light->m_parent; Node; Node = Node->m_parent)
			if (Node == ThirdPerson || Node == FirstPerson) return true;
		return false;
	}

	/// Live dynamic point lights of the scene node; carried NPC lights are the eligible ones.
	void CollectInputs(ShadowSceneNode* Node) {
		gInputs.clear();
		gInputScene.clear();
		for (NiTList<ShadowSceneLight>::Entry* E = Node->lights.start; E; E = E->next) {
			ShadowSceneLight* Scene = E->data;
			if (!Scene || !IsDynamic(Scene) || !IsPointLight(Scene)) continue;
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
			In.Eligible = Light->CanCarry && !IsMagicLight(Light) && !IsPlayerLight(Light);
			gInputs.push_back(In);
			gInputScene.push_back(Scene);
		}
	}

	LightMergeMath::Params CurrentParams() {
		const SettingsMainStruct::LightMergeStruct& S = TheSettingManager->SettingsMain.LightMerge;
		LightMergeMath::Params P = { S.JoinDistance, S.LeaveDistance, S.FadeTime, S.BrightnessExponent, S.ColorTolerance, S.RadiusTolerance };
		return P;
	}

	/// Writes this frame's merge into the engine lights, remembering every original value.
	void ApplyMerge() {
		const std::vector<LightMergeMath::LightOutput>& Outputs = gClusterer.Outputs();
		std::unordered_set<const void*> Leaders;
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

	void LogDebugRow() {
		const FrameCost& F = gFrame;
		Logger::Log("[LightMerge] frame %3u  %7.2f ms | walks %3u %6.3f ms | process %u %6.3f ms | lights %u dyn %u vis %u geom %u | merged %u in %u, hidden %u",
			gDebugFrameIndex, gFrameMs, F.Walks, F.WalkMs, F.ProcessCalls, F.ProcessMs,
			F.Stats.Lights, F.Stats.Dynamic, F.Stats.Visible, F.Stats.GeomPairs, gMembers, gClusters, gHidden);
	}
}

void CreateLightMergeHook() {

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)SceneLightProcess, &TrackSceneLightProcess);
	DetourAttach(&(PVOID&)LightGeometryWalk, &TrackLightGeometryWalk);
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
		double NowMs = TheFrameRateManager->GetPerformance();
		bool Discontinuity = DetectDiscontinuity(NowMs);
		gFrameMs = gLastApplyMs < 0.0 ? 0.0 : NowMs - gLastApplyMs;
		gLastApplyMs = NowMs;
		gFrame = FrameCost();
		gClusters = gMembers = gHidden = 0;
		if (Discontinuity && TheSettingManager->SettingsMain.Develop.LightMergeDebug) {
			gDebugFramesLeft = kDebugWindowFrames;
			gDebugFrameIndex = 0;
			Logger::Log("[LightMerge] ---- load or cell change: logging %d frames ----", kDebugWindowFrames);
		}

		if (!TheSettingManager->SettingsMain.LightMerge.Enabled) return;
		ShadowSceneNode* Node = SceneNode();
		if (!Node || !Player || !Player->parentCell) { gClusterer.Reset(); return; }
		// Keys are scene-light pointers, whose addresses a load or cell change may reuse.
		if (Discontinuity) gClusterer.Reset();
		CollectInputs(Node);
		float Delta = Discontinuity ? 0.0f : (float)(gFrameMs / 1000.0);
		gClusterer.Update(gInputs.data(), (int)gInputs.size(), CurrentParams(), Delta, Discontinuity);
		ApplyMerge();
	}

	void RestoreFrame() {
		if (--gDepth > 0) return;
		gDepth = 0;
		RestoreMerge();
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
			LogDebugRow();
			gDebugFramesLeft--;
			gDebugFrameIndex++;
		}
	}

	bool IsHidden(const NiPointLight* Light) {
		return false;
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
