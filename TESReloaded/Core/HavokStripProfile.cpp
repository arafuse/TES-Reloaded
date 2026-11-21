#include "HavokStripProfile.h"

#include <intrin.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace HavokStripProfile {

	namespace {

		const UInt32 kGetChildShapeSlot = 0x00A9D038;   // hkNiTriStripsShape vtable 0xA9D010, slot 10
		const UInt32 kGetChildShape     = 0x00915D00;

		/// Walk-length bins: strips skipped before the key's strip was found.
		const int WalkBinCount = 7;
		const char* const WalkBinNames[WalkBinCount] = { "0", "1-3", "4-15", "16-63", "64-255", "256-1023", ">=1024" };

		struct DataStats {
			const char*        TypeName  = "?";
			UInt32             NumStrips = 0;
			UInt32             NumTris   = 0;
			unsigned long long Calls     = 0;
			unsigned long long Walked    = 0;
		};

		struct CallerStats {
			unsigned long long Calls  = 0;
			unsigned long long Walked = 0;
		};

		UInt32             gOriginal      = 0;
		bool               gInstalled     = false;
		volatile bool      gRecording     = false;
		DWORD              gMainThread    = 0;
		volatile LONG      gOffThread     = 0;
		unsigned long long gCalls         = 0;
		unsigned long long gWalked        = 0;
		unsigned long long gWalkBins[WalkBinCount] = {};
		std::unordered_map<const void*, DataStats>   gData;
		std::unordered_map<const void*, CallerStats> gCallers;
		const void*        gLastData      = NULL;
		DataStats*         gLastDataStats = NULL;

		int WalkBin(UInt32 Walk) {
			if (Walk == 0) return 0;
			if (Walk < 4) return 1;
			if (Walk < 16) return 2;
			if (Walk < 64) return 3;
			if (Walk < 256) return 4;
			if (Walk < 1024) return 5;
			return 6;
		}

		const char* RttiName(NiObject* Object) {
			NiRTTI* Type = Object->GetType();
			return Type && Type->name ? Type->name : "?";
		}

		/// Mirrors the engine's walk (0x915D60): subpart = Key >> 20, triangle = Key & 0xFFFFF, and
		/// each strip of L indices holds L - 2 triangles, compared unsigned as the engine does.
		void Record(void* Shape, UInt32 Key, const void* Caller) {
			UInt8* SubParts = *(UInt8**)((UInt8*)Shape + 0x28);
			NiTriStripsData* Data = *(NiTriStripsData**)(SubParts + (Key >> 20) * 8);
			UInt32 Tri = Key & 0xFFFFF;
			UInt32 Walk = 0;
			for (; Walk < Data->NumStrips; Walk++) {
				UInt32 Length = Data->StripLengths[Walk];
				if (Tri < Length - 2) break;
				Tri -= Length - 2;
			}

			gCalls++;
			gWalked += Walk;
			gWalkBins[WalkBin(Walk)]++;

			if (Data != gLastData) {
				auto Inserted = gData.try_emplace(Data);
				DataStats& Stats = Inserted.first->second;
				if (Inserted.second) Stats.TypeName = RttiName(Data);
				Stats.NumStrips = Data->NumStrips;
				Stats.NumTris = Data->NumTriangles;
				gLastData = Data;
				gLastDataStats = &Stats;
			}
			gLastDataStats->Calls++;
			gLastDataStats->Walked += Walk;

			CallerStats& ByCaller = gCallers[Caller];
			ByCaller.Calls++;
			ByCaller.Walked += Walk;
		}

		void* __fastcall TrackGetChildShape(void* Shape, void* Edx, UInt32 Key, void* Buffer) {
			if (gRecording) {
				if (GetCurrentThreadId() == gMainThread)
					Record(Shape, Key, _ReturnAddress());
				else
					InterlockedIncrement(&gOffThread);
			}
			return (void*)ThisCall(gOriginal, Shape, Key, Buffer);
		}

		void Clear() {
			gCalls = gWalked = 0;
			for (int b = 0; b < WalkBinCount; b++) gWalkBins[b] = 0;
			gData.clear();
			gCallers.clear();
			gLastData = NULL;
			gLastDataStats = NULL;
			InterlockedExchange(&gOffThread, 0);
		}
	}

	void SetEnabled(bool Enabled) {
		if (Enabled && !gInstalled) {
			gInstalled = true;
			gMainThread = GetCurrentThreadId();
			gOriginal = *(UInt32*)kGetChildShapeSlot;
			SafeWrite32(kGetChildShapeSlot, (UInt32)&TrackGetChildShape);
			Logger::Log("[HavokStrips] hooked hkNiTriStripsShape slot 10 (was 0x%08X, engine 0x%08X)", gOriginal, kGetChildShape);
		}
		gRecording = Enabled;
	}

	void Report(int Frames) {
		if (!gInstalled || Frames <= 0) return;
		double Inv = 1.0 / Frames;
		Logger::Log("[FrameProfile]   -- hkNiTriStripsShape triangle lookup (0x915D00; update phase; per frame; recording re-walks the strips) --");
		Logger::Log("[FrameProfile]   calls %.1f, strips walked %.1f (%.1f per call), off-main-thread calls dropped %.1f, distinct strip data %u",
			gCalls * Inv, gWalked * Inv, gCalls ? (double)gWalked / gCalls : 0.0, gOffThread * Inv, (UInt32)gData.size());

		std::string Line = "[FrameProfile]   calls by strips walked:";
		char Buffer[160];
		for (int b = 0; b < WalkBinCount; b++) {
			_snprintf_s(Buffer, _TRUNCATE, "  %s %.1f", WalkBinNames[b], gWalkBins[b] * Inv);
			Line += Buffer;
		}
		Logger::Log("%s", Line.c_str());

		std::vector<std::pair<const void*, DataStats>> Data(gData.begin(), gData.end());
		std::sort(Data.begin(), Data.end(), [](const auto& a, const auto& b) { return a.second.Walked > b.second.Walked; });
		Logger::Log("[FrameProfile]   top strip data by strips walked (pointers may be reused within a window):");
		Logger::Log("[FrameProfile]   %-10s %-18s %7s %7s %10s %12s %8s %7s", "data", "type", "strips", "tris", "calls", "walked", "walk/c", "share");
		for (size_t i = 0; i < Data.size() && i < 10; i++) {
			const DataStats& S = Data[i].second;
			Logger::Log("[FrameProfile]   0x%08X %-18.18s %7u %7u %10.1f %12.1f %8.1f %6.1f%%",
				(UInt32)Data[i].first, S.TypeName, S.NumStrips, S.NumTris, S.Calls * Inv, S.Walked * Inv,
				S.Calls ? (double)S.Walked / S.Calls : 0.0, gWalked ? 100.0 * S.Walked / gWalked : 0.0);
		}

		std::vector<std::pair<const void*, CallerStats>> Callers(gCallers.begin(), gCallers.end());
		std::sort(Callers.begin(), Callers.end(), [](const auto& a, const auto& b) { return a.second.Walked > b.second.Walked; });
		Logger::Log("[FrameProfile]   top call sites (return address) by strips walked:");
		for (size_t i = 0; i < Callers.size() && i < 8; i++) {
			const CallerStats& C = Callers[i].second;
			Logger::Log("[FrameProfile]   0x%08X  calls %10.1f  walked %12.1f  (%5.1f%%)",
				(UInt32)Callers[i].first, C.Calls * Inv, C.Walked * Inv, gWalked ? 100.0 * C.Walked / gWalked : 0.0);
		}
		Clear();
	}
}
