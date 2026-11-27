#include "HavokStripProfile.h"

#include <intrin.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace HavokStripProfile {

	namespace {

		const UInt32 kGetFirstKeySlot   = 0x00A9D030;   // slot 8, a shared `return 0` (0x6F7070)
		const UInt32 kGetNextKeySlot    = 0x00A9D034;   // slot 9, 0x915C60; returns -1 after the last key
		const UInt32 kGetChildShapeSlot = 0x00A9D038;   // hkNiTriStripsShape vtable 0xA9D010, slot 10
		const UInt32 kGetChildShape     = 0x00915D00;
		const UInt32 kStripLookupPatch  = 0x00915D3F;   // start of the strip walk; a jump here means another plugin replaced it
		const UInt32 kSubPartsOffset    = 0x28;         // shape: array of { NiTriStripsData*, UInt32 } per subpart
		const UInt32 kSubPartCountOffset = 0x30;
		const UInt32 kStatModelOffset   = 0x24;         // TESObjectSTAT: TESBoundObject (0x24), then TESModel

		/// The engine functions that sweep every key of a shape collection, by entry address.
		struct SweepFunction { UInt32 Start; const char* Name; };
		const SweepFunction SweepFunctions[] = {
			{ 0x00904EB0, "collection agent 904EB0" },
			{ 0x009050F0, "collection agent 9050F0" },
			{ 0x00905370, "collection agent 905370" },
			{ 0x009156E0, "collection getAabb" },
			{ 0x00915AE0, "collection castRay" },
		};

		/// Owner search: dwords scanned per object, pointers followed from the collision object, and
		/// the cap on objects visited per collision object.
		const UInt32 kScanBytes   = 0x40;
		const int    kSearchDepth = 5;
		const int    kMaxVisited  = 512;
		const size_t kMaxOwners   = 8;

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

		/// One shape's or one engine function's full sweeps. Ticks are TSC, net of the recording done
		/// inside the sweep, and cover only sweeps that reached the -1 key.
		struct SweepStats {
			unsigned long long Sweeps     = 0;
			unsigned long long Unfinished = 0;
			unsigned long long Ticks      = 0;
			unsigned long long Keys       = 0;
		};

		UInt32             gFirstKeyOriginal = 0;
		UInt32             gNextKeyOriginal  = 0;
		const void*        gSweepShape    = NULL;
		const void*        gSweepCaller   = NULL;
		unsigned long long gSweepStart    = 0;
		unsigned long long gSweepOverhead = 0;
		unsigned long long gSweepKeys     = 0;
		std::unordered_map<const void*, SweepStats> gSweepShapes;
		std::unordered_map<const void*, SweepStats> gSweepCallers;
		LARGE_INTEGER      gCalQpc        = {};
		unsigned long long gCalTsc        = 0;
		std::unordered_map<const void*, std::vector<std::string>> gOwners;
		UInt32             gRdataStart    = 0;
		UInt32             gRdataEnd      = 0;

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
				if (GetCurrentThreadId() == gMainThread) {
					unsigned long long Begin = gSweepShape ? __rdtsc() : 0;
					Record(Shape, Key, _ReturnAddress());
					if (Begin) gSweepOverhead += __rdtsc() - Begin;
				}
				else
					InterlockedIncrement(&gOffThread);
			}
			return (void*)ThisCall(gOriginal, Shape, Key, Buffer);
		}

		void AddSweep(SweepStats& Stats, bool Finished, unsigned long long Ticks) {
			Stats.Sweeps++;
			Stats.Keys += gSweepKeys;
			if (Finished) Stats.Ticks += Ticks;
			else Stats.Unfinished++;
		}

		/// Closes the open sweep. A sweep that never reached the -1 key (an early-out loop, or a lone
		/// getFirstKey) is counted but not timed.
		void CloseSweep(bool Finished, unsigned long long End) {
			unsigned long long Elapsed = End - gSweepStart;
			unsigned long long Ticks = Finished && Elapsed > gSweepOverhead ? Elapsed - gSweepOverhead : 0;
			AddSweep(gSweepShapes[gSweepShape], Finished, Ticks);
			AddSweep(gSweepCallers[gSweepCaller], Finished, Ticks);
			gSweepShape = NULL;
		}

		/// Slot 8 opens a sweep: every engine loop over the keys starts here.
		UInt32 __fastcall TrackGetFirstKey(void* Shape, void* Edx) {
			if (gRecording && GetCurrentThreadId() == gMainThread) {
				if (gSweepShape) CloseSweep(false, 0);
				gSweepShape = Shape;
				gSweepCaller = _ReturnAddress();
				gSweepKeys = 0;
				gSweepOverhead = 0;
				gSweepStart = __rdtsc();
			}
			return ThisCall(gFirstKeyOriginal, Shape);
		}

		/// Slot 9 closes the sweep when it runs out of keys. It looks up each candidate key itself,
		/// through slot 10, so its recording is inside the sweep and subtracted as overhead.
		UInt32 __fastcall TrackGetNextKey(void* Shape, void* Edx, UInt32 Key) {
			UInt32 Next = ThisCall(gNextKeyOriginal, Shape, Key);
			if (Shape == gSweepShape && GetCurrentThreadId() == gMainThread) {
				gSweepKeys++;
				if (Next == 0xFFFFFFFF) CloseSweep(true, __rdtsc());
			}
			return Next;
		}

		void Calibrate() {
			QueryPerformanceCounter(&gCalQpc);
			gCalTsc = __rdtsc();
		}

		/// TSC ticks per millisecond over the window since the last Calibrate.
		double TicksPerMs() {
			LARGE_INTEGER Now, Frequency;
			QueryPerformanceCounter(&Now);
			QueryPerformanceFrequency(&Frequency);
			unsigned long long Tsc = __rdtsc();
			double Ms = 1000.0 * (Now.QuadPart - gCalQpc.QuadPart) / Frequency.QuadPart;
			return Ms > 0.0 ? (Tsc - gCalTsc) / Ms : 0.0;
		}

		const char* SweepFunctionName(const void* ReturnAddress) {
			UInt32 Address = (UInt32)ReturnAddress;
			const char* Name = "other";
			for (const SweepFunction& Function : SweepFunctions)
				if (Address >= Function.Start && Address - Function.Start < 0x400) Name = Function.Name;
			return Name;
		}

		/// Kept free of C++ objects: it guards the owner search's reads with SEH.
		bool ReadDword(UInt32 Address, UInt32* Value) {
			__try {
				*Value = *(const UInt32*)Address;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool ReadString(const char* Source, char* Destination, size_t Size) {
			__try {
				size_t i = 0;
				for (; i + 1 < Size && Source[i]; i++) Destination[i] = Source[i];
				Destination[i] = 0;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				Destination[0] = 0;
				return false;
			}
		}

		void FindRdata() {
			UInt8* Base = (UInt8*)GetModuleHandleA(NULL);
			IMAGE_NT_HEADERS* Headers = (IMAGE_NT_HEADERS*)(Base + ((IMAGE_DOS_HEADER*)Base)->e_lfanew);
			IMAGE_SECTION_HEADER* Section = IMAGE_FIRST_SECTION(Headers);
			for (UInt16 i = 0; i < Headers->FileHeader.NumberOfSections; i++, Section++) {
				if (memcmp(Section->Name, ".rdata", 6)) continue;
				gRdataStart = (UInt32)Base + Section->VirtualAddress;
				gRdataEnd = gRdataStart + Section->Misc.VirtualSize;
			}
		}

		/// True for a pointer to an object whose first dword is an Oblivion.exe vtable, which covers
		/// both the Gamebryo/bhk wrappers and the Havok objects behind them.
		bool IsEngineObject(UInt32 Pointer) {
			UInt32 Vtable;
			if (Pointer < 0x10000 || (Pointer & 3) || !ReadDword(Pointer, &Vtable)) return false;
			return Vtable >= gRdataStart && Vtable < gRdataEnd;
		}

		/// Depth-limited search of the object graph below Object for a dword equal to Target.
		bool GraphContains(UInt32 Object, UInt32 Target, int Depth, UInt32* Visited, int& VisitedCount) {
			for (UInt32 Offset = 4; Offset < kScanBytes; Offset += 4) {
				UInt32 Value;
				if (!ReadDword(Object + Offset, &Value)) return false;
				if (Value == Target) return true;
				if (Depth <= 1 || !IsEngineObject(Value)) continue;
				bool Seen = false;
				for (int i = 0; i < VisitedCount && !Seen; i++) Seen = Visited[i] == Value;
				if (Seen) continue;
				if (VisitedCount == kMaxVisited) return false;
				Visited[VisitedCount++] = Value;
				if (GraphContains(Value, Target, Depth - 1, Visited, VisitedCount)) return true;
			}
			return false;
		}

		bool IsKindOf(NiObject* Object, const char* TypeName) {
			for (NiRTTI* Type = Object->GetType(); Type; Type = Type->parent)
				if (Type->name && !strcmp(Type->name, TypeName)) return true;
			return false;
		}

		std::string DescribeOwner(TESObjectREFR* Ref, NiNode* Root, NiAVObject* Object) {
			TESForm* Base = Ref->baseForm;
			char Model[MAX_PATH] = "(not a STAT)";
			char Plugin[0x104] = "?";
			if (Base && Base->formType == TESForm::FormType::kFormType_Stat) {
				UInt32 Path;
				if (!ReadDword((UInt32)Base + kStatModelOffset + offsetof(TESModel, nifPath), &Path) || !Path ||
					!ReadString((const char*)Path, Model, sizeof(Model)))
					strcpy_s(Model, "?");
			}
			if (Base && Base->modRefList.First.item) ReadString(Base->modRefList.First.item->name, Plugin, sizeof(Plugin));
			char Line[1024];
			_snprintf_s(Line, _TRUNCATE, "ref %08X base %08X (form type %u, %s) at (%.0f, %.0f, %.0f), collision on '%s' under '%s', model '%s'",
				Ref->refID, Base ? Base->refID : 0, Base ? Base->formType : 0, Plugin, Ref->pos.x, Ref->pos.y, Ref->pos.z,
				Object->m_pcName ? Object->m_pcName : "", Root->m_pcName ? Root->m_pcName : "", Model);
			return Line;
		}

		/// Visits Object and its descendants; any whose collision object leads to Target is a candidate
		/// owner. The pointer search can reach other references (e.g. through shared Havok objects).
		/// The search never re-enters the scene graph through the visited AV object or the root.
		void FindInSubtree(TESObjectREFR* Ref, NiNode* Root, NiAVObject* Object, UInt32 Target, std::vector<std::string>& Owners, int Depth) {
			if (Owners.size() >= kMaxOwners || Depth > 64) return;
			if (Object->m_spCollisionObject) {
				UInt32 Visited[kMaxVisited] = { (UInt32)Object, (UInt32)Root, (UInt32)Object->m_spCollisionObject };
				int VisitedCount = 3;
				if (GraphContains((UInt32)Object->m_spCollisionObject, Target, kSearchDepth, Visited, VisitedCount))
					Owners.push_back(DescribeOwner(Ref, Root, Object));
			}
			if (!IsKindOf(Object, "NiNode")) return;
			NiTArray<NiAVObject*>& Children = ((NiNode*)Object)->m_children;
			for (UInt16 i = 0; i < Children.end; i++)
				if (NiAVObject* Child = Children.data[i]) FindInSubtree(Ref, Root, Child, Target, Owners, Depth + 1);
		}

		void FindInCell(TESObjectCELL* Cell, UInt32 Target, std::vector<std::string>& Owners) {
			if (!Cell) return;
			for (TList<TESObjectREFR>::Entry* Entry = &Cell->objectList.First; Entry; Entry = Entry->next) {
				TESObjectREFR* Ref = Entry->item;
				NiNode* Root;
				if (!Ref || !(Root = Ref->GetNode())) continue;
				FindInSubtree(Ref, Root, Root, Target, Owners, 0);
			}
		}

		/// The loaded references whose collision reaches Shape; computed once per shape pointer.
		const std::vector<std::string>& FindOwners(const void* Shape) {
			auto Found = gOwners.find(Shape);
			if (Found != gOwners.end()) return Found->second;
			std::vector<std::string>& Owners = gOwners[Shape];
			if (!Player || !Tes) return Owners;
			if (Player->GetWorldSpace()) {
				for (UInt32 x = 0; x < *SettingGridsToLoad; x++)
					for (UInt32 y = 0; y < *SettingGridsToLoad; y++)
						FindInCell(Tes->gridCellArray->GetCell(x, y), (UInt32)Shape, Owners);
			}
			else {
				FindInCell(Player->parentCell, (UInt32)Shape, Owners);
			}
			return Owners;
		}

		void Clear() {
			gCalls = gWalked = 0;
			for (int b = 0; b < WalkBinCount; b++) gWalkBins[b] = 0;
			gData.clear();
			gCallers.clear();
			gLastData = NULL;
			gLastDataStats = NULL;
			InterlockedExchange(&gOffThread, 0);
			gSweepShapes.clear();
			gSweepCallers.clear();
			gSweepShape = NULL;
			Calibrate();
		}

		void ReportSweeps(double Inv) {
			double PerMs = TicksPerMs();
			if (PerMs <= 0.0) return;
			SweepStats Total;
			for (const auto& Entry : gSweepShapes) {
				Total.Sweeps += Entry.second.Sweeps;
				Total.Unfinished += Entry.second.Unfinished;
				Total.Ticks += Entry.second.Ticks;
				Total.Keys += Entry.second.Keys;
			}
			Logger::Log("[FrameProfile]   -- hkNiTriStripsShape full sweeps (getFirstKey to the -1 key; includes each triangle's narrowphase, net of this recording) --");
			Logger::Log("[FrameProfile]   sweeps %.1f, %.4f ms, keys %.1f, unfinished (counted, not timed) %.1f, shapes %u",
				Total.Sweeps * Inv, Total.Ticks / PerMs * Inv, Total.Keys * Inv, Total.Unfinished * Inv, (UInt32)gSweepShapes.size());

			std::vector<std::pair<const void*, SweepStats>> Callers(gSweepCallers.begin(), gSweepCallers.end());
			std::sort(Callers.begin(), Callers.end(), [](const auto& a, const auto& b) { return a.second.Ticks > b.second.Ticks; });
			Logger::Log("[FrameProfile]   by sweep site (getFirstKey return address):");
			for (size_t i = 0; i < Callers.size() && i < 8; i++) {
				const SweepStats& S = Callers[i].second;
				Logger::Log("[FrameProfile]   0x%08X %-24s sweeps %8.1f  %8.4f ms  keys %9.1f  unfinished %6.1f",
					(UInt32)Callers[i].first, SweepFunctionName(Callers[i].first), S.Sweeps * Inv, S.Ticks / PerMs * Inv, S.Keys * Inv, S.Unfinished * Inv);
			}

			std::vector<std::pair<const void*, SweepStats>> Shapes(gSweepShapes.begin(), gSweepShapes.end());
			std::sort(Shapes.begin(), Shapes.end(), [](const auto& a, const auto& b) { return a.second.Ticks > b.second.Ticks; });
			Logger::Log("[FrameProfile]   top shapes by sweep time (candidates: loaded references whose collision graph reaches the shape; the heuristic search also matches unrelated references):");
			for (size_t i = 0; i < Shapes.size() && i < 5; i++) {
				const void* Shape = Shapes[i].first;
				const SweepStats& S = Shapes[i].second;
				UInt32 SubParts = 0, SubPartArray = 0, Data = 0, Strips = 0, Tris = 0;
				if (ReadDword((UInt32)Shape + kSubPartCountOffset, &SubParts) && SubParts &&
					ReadDword((UInt32)Shape + kSubPartsOffset, &SubPartArray) && ReadDword(SubPartArray, &Data) && Data) {
					ReadDword(Data + offsetof(NiTriStripsData, NumStrips), &Strips);
					ReadDword(Data + offsetof(NiTriStripsData, NumTriangles), &Tris);
				}
				Logger::Log("[FrameProfile]   shape 0x%08X subparts %u first data 0x%08X (%u strips, %u tris): sweeps %.1f, %.4f ms, keys %.1f",
					(UInt32)Shape, SubParts, Data, Strips & 0xFFFF, Tris & 0xFFFF, S.Sweeps * Inv, S.Ticks / PerMs * Inv, S.Keys * Inv);
				if (i >= 3) continue;
				const std::vector<std::string>& Owners = FindOwners(Shape);
				if (Owners.empty()) Logger::Log("[FrameProfile]     candidate: none found in the loaded cells");
				for (const std::string& Owner : Owners) Logger::Log("[FrameProfile]     candidate: %s", Owner.c_str());
			}
		}
	}

	void SetEnabled(bool Enabled) {
		if (Enabled && !gInstalled) {
			gInstalled = true;
			gMainThread = GetCurrentThreadId();
			gOriginal = *(UInt32*)kGetChildShapeSlot;
			gFirstKeyOriginal = *(UInt32*)kGetFirstKeySlot;
			gNextKeyOriginal = *(UInt32*)kGetNextKeySlot;
			FindRdata();
			Calibrate();
			SafeWrite32(kGetChildShapeSlot, (UInt32)&TrackGetChildShape);
			SafeWrite32(kGetFirstKeySlot, (UInt32)&TrackGetFirstKey);
			SafeWrite32(kGetNextKeySlot, (UInt32)&TrackGetNextKey);
			Logger::Log("[HavokStrips] hooked hkNiTriStripsShape slots 8/9/10 (were 0x%08X 0x%08X 0x%08X, engine 0x%08X)",
				gFirstKeyOriginal, gNextKeyOriginal, gOriginal, kGetChildShape);
		}
		gRecording = Enabled;
	}

	void Report(int Frames) {
		if (!gInstalled || Frames <= 0) return;
		double Inv = 1.0 / Frames;
		Logger::Log("[FrameProfile]   -- hkNiTriStripsShape triangle lookup (0x915D00; update phase; per frame; recording re-walks the strips) --");
		Logger::Log("[FrameProfile]   calls %.1f, strips walked %.1f (%.1f per call), off-main-thread calls dropped %.1f, distinct strip data %u",
			gCalls * Inv, gWalked * Inv, gCalls ? (double)gWalked / gCalls : 0.0, gOffThread * Inv, (UInt32)gData.size());
		if (*(const UInt8*)kStripLookupPatch == 0xE9)
			Logger::Log("[FrameProfile]   (strip walk patched, e.g. by ProtoPlugin's HavokStripCache: counts are the walk it replaced)");

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
		ReportSweeps(Inv);
		Clear();
	}
}
