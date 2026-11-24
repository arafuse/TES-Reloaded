#include "LightMerge.h"
#include "FrameProfiler.h"

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

	FrameCost      gFrame = {};
	double         gLastApplyMs = -1.0;
	double         gFrameMs = 0.0;
	TESObjectCELL* gLastCell = NULL;
	int            gDebugFramesLeft = 0;
	unsigned       gDebugFrameIndex = 0;

	inline ShadowSceneNode* SceneNode() { return *(ShadowSceneNode**)kShadowSceneNodeTable; }

	inline UInt8 IsDynamic(ShadowSceneLight* Light) { return *((UInt8*)Light + 0x104); }

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
		LARGE_INTEGER Freq; QueryPerformanceFrequency(&Freq);
		gFrame.ProcessMs += (double)(FrameProfiler::QpcNow() - Start) * 1000.0 / (double)Freq.QuadPart;
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
		LARGE_INTEGER Freq; QueryPerformanceFrequency(&Freq);
		gFrame.WalkMs += (double)(FrameProfiler::QpcNow() - Start) * 1000.0 / (double)Freq.QuadPart;
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

	void LogDebugRow() {
		const FrameCost& F = gFrame;
		Logger::Log("[LightMerge] frame %3u  %7.2f ms | walks %3u %6.3f ms | process %u %6.3f ms | lights %u dyn %u vis %u geom %u",
			gDebugFrameIndex, gFrameMs, F.Walks, F.WalkMs, F.ProcessCalls, F.ProcessMs,
			F.Stats.Lights, F.Stats.Dynamic, F.Stats.Visible, F.Stats.GeomPairs);
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
		double NowMs = TheFrameRateManager->GetPerformance();
		bool Discontinuity = DetectDiscontinuity(NowMs);
		gFrameMs = gLastApplyMs < 0.0 ? 0.0 : NowMs - gLastApplyMs;
		gLastApplyMs = NowMs;
		gFrame = FrameCost();
		if (Discontinuity && TheSettingManager->SettingsMain.Develop.LightMergeDebug) {
			gDebugFramesLeft = kDebugWindowFrames;
			gDebugFrameIndex = 0;
			Logger::Log("[LightMerge] ---- load or cell change: logging %d frames ----", kDebugWindowFrames);
		}
	}

	void RestoreFrame() {
		FrameProfiler::Count(FrameProfiler::Cnt_LightProcessCalls, gFrame.ProcessCalls);
		FrameProfiler::Count(FrameProfiler::Cnt_LightWalks, gFrame.Walks);
		FrameProfiler::Count(FrameProfiler::Cnt_Lights, gFrame.Stats.Lights);
		FrameProfiler::Count(FrameProfiler::Cnt_LightsDynamic, gFrame.Stats.Dynamic);
		FrameProfiler::Count(FrameProfiler::Cnt_LightsVisible, gFrame.Stats.Visible);
		FrameProfiler::Count(FrameProfiler::Cnt_LightGeomPairs, gFrame.Stats.GeomPairs);
		if (gDebugFramesLeft > 0) {
			LogDebugRow();
			gDebugFramesLeft--;
			gDebugFrameIndex++;
		}
	}
}
