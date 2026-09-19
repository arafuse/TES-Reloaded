#include "SampleProfiler.h"
#include "Managers.h"

#include <tlhelp32.h>
#include <dbghelp.h>
#include <intrin.h>
#include <map>
#include <vector>
#include <algorithm>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace SampleProfiler {

	volatile bool InRender = false;

	namespace {

		// Bytes of stack copied per sample for the inclusive scan. Deep enough to
		// span the engine's update call chains, shallow enough that the main thread
		// stays suspended for only a couple of microseconds.
		const UInt32 StackScanBytes = 3072;
		const int    MaxModules     = 192;
		const int    TableBits      = 16;
		const UInt32 TableSize      = 1u << TableBits;
		const UInt32 TableMax       = (TableSize * 7) / 10; // keep linear probing short
		const int    MaxPerSample   = 96;                   // unique return addresses kept per sample
		const int    TopN           = 40;

		struct ModuleEntry {
			UInt32 Base;
			UInt32 Size;
			char   Name[64];
			char   Path[MAX_PATH];
		};
		ModuleEntry gModules[MaxModules];
		int         gModuleCount = 0;

		// Fixed-capacity open-addressing histogram. Fixed capacity is the point: the
		// sampler thread must never allocate, because it runs while the main thread
		// may be suspended inside the allocator.
		struct Table {
			UInt32* Key;    // 0 = empty slot; address 0 is never sampled
			UInt32* Count;
			UInt32  Used;
		};
		Table  gExcl[2];                    // [0] update phase, [1] render phase
		Table  gIncl;
		UInt32 gModuleSamples[MaxModules + 1] = {}; // last slot = address outside every known module

		HANDLE        gThread     = NULL;
		HANDLE        gMain       = NULL;
		UInt32        gStackBase  = 0;
		volatile LONG gRunning    = 0;
		LONGLONG      gQpcFreq    = 0;
		LONGLONG      gStartQpc   = 0;
		LONGLONG      gStopQpc    = 0;
		UInt32        gSamples    = 0;
		// Counted by CheckKey, which the render hook calls exactly once per frame.
		// Without it a report cannot be converted from sample shares into ms/frame
		// without someone reading an FPS counter off the screen, and a report that
		// needs an external number to interpret is a report that gets misread.
		volatile LONG gFrames    = 0;
		UInt32        gPhaseCount[2] = { 0, 0 };
		UInt32        gDropped    = 0;      // samples whose keys did not fit the table
		UInt32        gSuspendFail = 0;
		int           gHz         = 1000;

		// --- histogram ---------------------------------------------------------

		bool AllocTable(Table& T) {
			T.Key   = (UInt32*)calloc(TableSize, sizeof(UInt32));
			T.Count = (UInt32*)calloc(TableSize, sizeof(UInt32));
			T.Used  = 0;
			return T.Key && T.Count;
		}

		void FreeTable(Table& T) {
			free(T.Key); free(T.Count);
			T.Key = NULL; T.Count = NULL; T.Used = 0;
		}

		void Bump(Table& T, UInt32 Addr) {
			if (!T.Key) return;
			UInt32 h = (Addr * 2654435761u) >> (32 - TableBits);
			for (;;) {
				UInt32 k = T.Key[h];
				if (k == Addr) { T.Count[h]++; return; }
				if (k == 0) {
					if (T.Used >= TableMax) { gDropped++; return; }
					T.Key[h] = Addr; T.Count[h] = 1; T.Used++;
					return;
				}
				h = (h + 1) & (TableSize - 1);
			}
		}

		// --- modules -----------------------------------------------------------

		// Sorted by base so the per-candidate lookup is a binary search: the inclusive
		// scan runs this up to MaxPerSample times per sample.
		void BuildModuleTable() {
			gModuleCount = 0;
			HANDLE Snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
			if (Snap == INVALID_HANDLE_VALUE) return;
			// The W variants, converted down: dbghelp's ANSI entry points need char paths.
			MODULEENTRY32W Me;
			Me.dwSize = sizeof(Me);
			for (BOOL ok = Module32FirstW(Snap, &Me); ok && gModuleCount < MaxModules; ok = Module32NextW(Snap, &Me)) {
				ModuleEntry& E = gModules[gModuleCount++];
				E.Base = (UInt32)Me.modBaseAddr;
				E.Size = (UInt32)Me.modBaseSize;
				WideCharToMultiByte(CP_ACP, 0, Me.szModule,  -1, E.Name, sizeof(E.Name), NULL, NULL);
				WideCharToMultiByte(CP_ACP, 0, Me.szExePath, -1, E.Path, sizeof(E.Path), NULL, NULL);
				E.Name[sizeof(E.Name) - 1] = 0;
				E.Path[sizeof(E.Path) - 1] = 0;
			}
			CloseHandle(Snap);

			for (int i = 1; i < gModuleCount; i++) {
				ModuleEntry Tmp = gModules[i];
				int j = i - 1;
				while (j >= 0 && gModules[j].Base > Tmp.Base) { gModules[j + 1] = gModules[j]; j--; }
				gModules[j + 1] = Tmp;
			}
		}

		int ModuleOf(UInt32 Addr) {
			int lo = 0, hi = gModuleCount - 1, found = -1;
			while (lo <= hi) {
				int mid = (lo + hi) / 2;
				if (gModules[mid].Base <= Addr) { found = mid; lo = mid + 1; }
				else hi = mid - 1;
			}
			if (found < 0) return -1;
			if (Addr < gModules[found].Base + gModules[found].Size) return found;
			return -1;
		}

		// A stack dword is treated as a return address when the bytes immediately
		// before it decode as one of x86's call encodings. This is the standard
		// heuristic; see the header for what it can and cannot be trusted to say.
		bool LooksLikeReturnAddress(UInt32 Ret, int Mod) {
			if (Ret < gModules[Mod].Base + 8) return false;
			const UInt8* p = (const UInt8*)Ret;
			if (p[-5] == 0xE8) return true;                             // call rel32
			if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;   // call reg / call [reg]
			if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;   // call [reg+disp8]
			if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;   // call [reg+idx*s+disp8]
			if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;   // call [disp32] / [reg+disp32]
			if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;   // call [reg+idx*s+disp32]
			return false;
		}

		// --- sampling ----------------------------------------------------------

		// A module can unload while sampling, and a candidate that was inside it when
		// the module table was built would then fault on the call-byte probe. One bad
		// sample is worth dropping; taking the game down mid-test is not.
		void ScanStack(const UInt32* Words, UInt32 WordCount, UInt32* Seen, int SeenCount) {
			__try {
				for (UInt32 i = 0; i < WordCount && SeenCount < MaxPerSample; i++) {
					UInt32 v = Words[i];
					if (v < 0x10000) continue;
					int m = ModuleOf(v);
					if (m < 0 || !LooksLikeReturnAddress(v, m)) continue;
					int d = 0;
					for (; d < SeenCount; d++) if (Seen[d] == v) break;
					if (d < SeenCount) continue;
					Seen[SeenCount++] = v;
					Bump(gIncl, v);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		void TakeSample(UInt8* StackBuf) {
			CONTEXT Ctx;
			memset(&Ctx, 0, sizeof(Ctx));
			Ctx.ContextFlags = CONTEXT_CONTROL;

			UInt32 Eip = 0, Esp = 0, Copied = 0;
			bool   Phase = false;

			// Everything between Suspend and Resume must be lock-free and
			// allocation-free: the main thread may be holding the heap or loader lock
			// at the moment it is stopped, and taking either here would deadlock the
			// game outright. GetThreadContext and memcpy from an already-committed
			// stack range satisfy that; a VirtualQuery would NOT, which is why the
			// stack ceiling is captured once up front instead.
			if (SuspendThread(gMain) == (DWORD)-1) { gSuspendFail++; return; }
			Phase = InRender;
			if (GetThreadContext(gMain, &Ctx)) {
				Eip = Ctx.Eip;
				Esp = Ctx.Esp;
				if (Esp && gStackBase > Esp) {
					UInt32 Avail = gStackBase - Esp;
					Copied = Avail < StackScanBytes ? Avail : StackScanBytes;
					memcpy(StackBuf, (const void*)Esp, Copied);
				}
			}
			ResumeThread(gMain);

			if (!Eip) return;

			gSamples++;
			int Ph = Phase ? 1 : 0;
			gPhaseCount[Ph]++;

			int Mod = ModuleOf(Eip);
			gModuleSamples[Mod < 0 ? MaxModules : Mod]++;
			Bump(gExcl[Ph], Eip);

			// The leaf frame itself is inclusive too, so a function that is both hot
			// and shallow does not vanish from the inclusive view.
			UInt32 Seen[MaxPerSample];
			int    SeenCount = 0;
			Seen[SeenCount++] = Eip;
			Bump(gIncl, Eip);

			ScanStack((const UInt32*)StackBuf, Copied / 4, Seen, SeenCount);
		}

		DWORD WINAPI SamplerThread(LPVOID) {
			UInt8* StackBuf = (UInt8*)malloc(StackScanBytes);
			if (!StackBuf) return 0;

			HANDLE Timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!Timer) Timer = CreateWaitableTimerW(NULL, FALSE, NULL);

			LONGLONG Period100ns = 10000000LL / (gHz > 0 ? gHz : 1000);
			UInt32   Rng = 0x9E3779B9u ^ GetTickCount();

			while (InterlockedCompareExchange(&gRunning, 1, 1) == 1) {
				// Jitter the interval by +/-40%. A fixed period can phase-lock to the
				// game's frame cadence and then systematically over- or under-sample
				// one part of the frame, which would corrupt the render/update split.
				Rng ^= Rng << 13; Rng ^= Rng >> 17; Rng ^= Rng << 5;
				LONGLONG Jittered = Period100ns + (LONGLONG)(Rng % (UInt32)(Period100ns * 4 / 5 + 1)) - Period100ns * 2 / 5;
				if (Jittered < 1000) Jittered = 1000;

				if (Timer) {
					LARGE_INTEGER Due;
					Due.QuadPart = -Jittered;
					SetWaitableTimer(Timer, &Due, 0, NULL, NULL, FALSE);
					WaitForSingleObject(Timer, 100);
				}
				else Sleep(1);

				TakeSample(StackBuf);
			}

			if (Timer) CloseHandle(Timer);
			free(StackBuf);
			return 0;
		}

		// --- symbolization (report time only) ----------------------------------

		typedef DWORD   (WINAPI* SymSetOptions_t)(DWORD);
		typedef BOOL    (WINAPI* SymInitialize_t)(HANDLE, PCSTR, BOOL);
		typedef DWORD64 (WINAPI* SymLoadModuleEx_t)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD, void*, DWORD);
		typedef BOOL    (WINAPI* SymFromAddr_t)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
		typedef BOOL    (WINAPI* SymCleanup_t)(HANDLE);

		HMODULE           gDbgHelp = NULL;
		SymFromAddr_t     gSymFromAddr = NULL;
		SymCleanup_t      gSymCleanup = NULL;
		// A private handle value, never a real process handle: dbghelp keys its symbol
		// sessions by this, and reusing GetCurrentProcess() would fight any other OBSE
		// plugin (a crash logger, say) that has its own session open.
		HANDLE            gSymProc = (HANDLE)0xB105F00D;

		void SymbolsOpen() {
			gDbgHelp = LoadLibraryA("dbghelp.dll");
			if (!gDbgHelp) { Logger::Log("[Sampler] dbghelp.dll unavailable; reporting raw addresses."); return; }

			SymSetOptions_t   pSetOptions = (SymSetOptions_t)GetProcAddress(gDbgHelp, "SymSetOptions");
			SymInitialize_t   pInitialize = (SymInitialize_t)GetProcAddress(gDbgHelp, "SymInitialize");
			SymLoadModuleEx_t pLoadModule = (SymLoadModuleEx_t)GetProcAddress(gDbgHelp, "SymLoadModuleEx");
			gSymFromAddr = (SymFromAddr_t)GetProcAddress(gDbgHelp, "SymFromAddr");
			gSymCleanup  = (SymCleanup_t)GetProcAddress(gDbgHelp, "SymCleanup");
			if (!pSetOptions || !pInitialize || !pLoadModule || !gSymFromAddr) {
				Logger::Log("[Sampler] dbghelp entry points missing; reporting raw addresses.");
				gSymFromAddr = NULL;
				return;
			}

			char GameDir[MAX_PATH];
			GetModuleFileNameA(NULL, GameDir, MAX_PATH);
			char* Slash = strrchr(GameDir, '\\');
			if (Slash) *Slash = 0;

			char Search[MAX_PATH * 2];
			_snprintf(Search, sizeof(Search) - 1, "%s;%s\\Data\\OBSE\\Plugins", GameDir, GameDir);
			Search[sizeof(Search) - 1] = 0;

			pSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_LOAD_ANYTHING);
			if (!pInitialize(gSymProc, Search, FALSE)) {
				Logger::Log("[Sampler] SymInitialize failed; reporting raw addresses.");
				gSymFromAddr = NULL;
				return;
			}
			for (int i = 0; i < gModuleCount; i++)
				pLoadModule(gSymProc, NULL, gModules[i].Path, NULL, gModules[i].Base, gModules[i].Size, NULL, 0);

			Logger::Log("[Sampler] symbols: search path \"%s\"", Search);
		}

		void SymbolsClose() {
			if (gSymCleanup && gSymFromAddr) gSymCleanup(gSymProc);
			if (gDbgHelp) FreeLibrary(gDbgHelp);
			gDbgHelp = NULL; gSymFromAddr = NULL; gSymCleanup = NULL;
		}

		// Resolves Addr to "name+0xoff". SymBase receives the symbol's start address so
		// the inclusive view can merge call sites that land in the same function.
		void Describe(UInt32 Addr, char* Out, int OutSize, UInt32* SymBase) {
			int Mod = ModuleOf(Addr);
			if (SymBase) *SymBase = Addr;

			if (gSymFromAddr) {
				// SYMBOL_INFO carries ULONG64 fields, so the over-allocated tail buffer
				// that holds the name has to be 8-aligned, not merely byte-addressable.
				ULONG64      Buffer[(sizeof(SYMBOL_INFO) + 1024) / sizeof(ULONG64) + 1];
				SYMBOL_INFO* Si = (SYMBOL_INFO*)Buffer;
				memset(Buffer, 0, sizeof(Buffer));
				Si->SizeOfStruct = sizeof(SYMBOL_INFO);
				Si->MaxNameLen   = 1000;
				DWORD64 Disp = 0;
				if (gSymFromAddr(gSymProc, Addr, &Disp, Si)) {
					if (SymBase) *SymBase = (UInt32)Si->Address;
					if (Out && OutSize > 0) {
						_snprintf(Out, OutSize, "%s+0x%X  [%s]", Si->Name, (UInt32)Disp, Mod >= 0 ? gModules[Mod].Name : "?");
						Out[OutSize - 1] = 0;
					}
					return;
				}
			}
			if (Out && OutSize > 0) {
				if (Mod >= 0) _snprintf(Out, OutSize, "%s+0x%X", gModules[Mod].Name, Addr - gModules[Mod].Base);
				else          _snprintf(Out, OutSize, "0x%08X  <no module>", Addr);
				Out[OutSize - 1] = 0;
			}
		}

		// --- report ------------------------------------------------------------

		struct Row { UInt32 Addr; UInt32 Count; };
		bool RowGreater(const Row& a, const Row& b) { return a.Count > b.Count; }

		// MsPerCount converts one sample into milliseconds per frame, so every row is
		// directly comparable against a FrameProfiler bucket without any arithmetic.
		void ReportTable(const Table& T, const char* Title, UInt32 Denominator, double MsPerCount, bool MergeBySymbol) {
			Logger::Log("[Sampler]   -- %s --", Title);
			if (!T.Key || !Denominator) { Logger::Log("[Sampler]      (no samples)"); return; }

			std::vector<Row> Rows;
			if (MergeBySymbol) {
				// Merge first, THEN rank: a function entered from six call sites is one
				// hotspot, not six also-rans that each miss the cut.
				std::map<UInt32, UInt32> Merged;
				for (UInt32 i = 0; i < TableSize; i++) {
					if (!T.Key[i]) continue;
					UInt32 Base = T.Key[i];
					Describe(T.Key[i], NULL, 0, &Base);
					Merged[Base] += T.Count[i];
				}
				for (std::map<UInt32, UInt32>::const_iterator it = Merged.begin(); it != Merged.end(); ++it) {
					Row r = { it->first, it->second };
					Rows.push_back(r);
				}
			}
			else {
				for (UInt32 i = 0; i < TableSize; i++) {
					if (!T.Key[i]) continue;
					Row r = { T.Key[i], T.Count[i] };
					Rows.push_back(r);
				}
			}

			std::sort(Rows.begin(), Rows.end(), RowGreater);
			int Shown = (int)Rows.size() < TopN ? (int)Rows.size() : TopN;
			for (int i = 0; i < Shown; i++) {
				char Name[512];
				Describe(Rows[i].Addr, Name, sizeof(Name), NULL);
				Logger::Log("[Sampler]     %6.2f %%  %8.4f ms/frame  %7u  %s",
					Rows[i].Count * 100.0 / Denominator, Rows[i].Count * MsPerCount, Rows[i].Count, Name);
			}
			Logger::Log("[Sampler]      (%u distinct entries)", (UInt32)Rows.size());
		}

		void Report() {
			double Seconds = gQpcFreq ? (double)(gStopQpc - gStartQpc) / (double)gQpcFreq : 0.0;
			double Inv = gSamples ? 100.0 / gSamples : 0.0;

			UInt32 Frames     = (UInt32)gFrames;
			double MsPerSmpl  = gSamples ? Seconds * 1000.0 / gSamples : 0.0;
			double MsPerCount = Frames ? MsPerSmpl / Frames : 0.0;
			double Fps        = (Frames && Seconds > 0.0) ? Frames / Seconds : 0.0;

			Logger::Log("[Sampler] ==================== report ====================");
			Logger::Log("[Sampler]   %u samples over %.1f s (%.0f Hz effective, %d Hz requested)",
				gSamples, Seconds, Seconds > 0.0 ? gSamples / Seconds : 0.0, gHz);
			Logger::Log("[Sampler]   %u frames, %.1f FPS, %.3f ms/frame", Frames, Fps, Fps > 0.0 ? 1000.0 / Fps : 0.0);
			// The ms/frame figures below are what to compare between runs. Raw
			// percentages are shares of a frame whose LENGTH changed, so a bucket can
			// grow in ms while its percentage falls - and a frame-capped run parks its
			// idle time in the update phase, inflating that share for no work at all.
			Logger::Log("[Sampler]   phase split:  update %u (%.1f %%, %.3f ms/frame)   render %u (%.1f %%, %.3f ms/frame)",
				gPhaseCount[0], gPhaseCount[0] * Inv, gPhaseCount[0] * MsPerCount,
				gPhaseCount[1], gPhaseCount[1] * Inv, gPhaseCount[1] * MsPerCount);
			if (gDropped)     Logger::Log("[Sampler]   WARNING: %u histogram entries dropped (table full); long tail is under-reported.", gDropped);
			if (gSuspendFail) Logger::Log("[Sampler]   WARNING: %u suspend failures.", gSuspendFail);

			SymbolsOpen();

			Logger::Log("[Sampler]   -- modules (exclusive, all samples) --");
			{
				std::vector<Row> Rows;
				for (int i = 0; i < gModuleCount; i++) {
					if (!gModuleSamples[i]) continue;
					Row r = { (UInt32)i, gModuleSamples[i] };
					Rows.push_back(r);
				}
				if (gModuleSamples[MaxModules]) { Row r = { (UInt32)MaxModules, gModuleSamples[MaxModules] }; Rows.push_back(r); }
				std::sort(Rows.begin(), Rows.end(), RowGreater);
				for (size_t i = 0; i < Rows.size() && i < 12; i++)
					Logger::Log("[Sampler]     %6.2f %%  %8.4f ms/frame  %7u  %s", Rows[i].Count * Inv, Rows[i].Count * MsPerCount, Rows[i].Count,
						Rows[i].Addr == MaxModules ? "<outside any module>" : gModules[Rows[i].Addr].Name);
			}

			ReportTable(gExcl[0], "EXCLUSIVE, UPDATE phase (game logic: AI, Havok, animation, scripts)", gPhaseCount[0], MsPerCount, false);
			ReportTable(gExcl[1], "EXCLUSIVE, RENDER phase (inside RenderHook::TrackRender)",            gPhaseCount[1], MsPerCount, false);
			ReportTable(gIncl,    "INCLUSIVE, all samples (heuristic stack scan; merged by function)",   gSamples,       MsPerCount, true);

			SymbolsClose();
			Logger::Log("[Sampler] ================== end report ==================");
		}

		// --- lifecycle ---------------------------------------------------------

		void Start() {
			// Runs on the main thread, which is exactly what makes the stack ceiling
			// readable straight out of our own TIB.
			NT_TIB* Tib = (NT_TIB*)__readfsdword(0x18);
			gStackBase = (UInt32)Tib->StackBase;

			gMain = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
			if (!gMain) { Logger::Log("[Sampler] OpenThread failed (%u); not starting.", GetLastError()); return; }

			if (!AllocTable(gExcl[0]) || !AllocTable(gExcl[1]) || !AllocTable(gIncl)) {
				Logger::Log("[Sampler] histogram allocation failed; not starting.");
				FreeTable(gExcl[0]); FreeTable(gExcl[1]); FreeTable(gIncl);
				CloseHandle(gMain); gMain = NULL;
				return;
			}
			memset(gModuleSamples, 0, sizeof(gModuleSamples));
			gSamples = 0; gPhaseCount[0] = gPhaseCount[1] = 0; gDropped = 0; gSuspendFail = 0;
			InterlockedExchange(&gFrames, 0);

			BuildModuleTable();

			LARGE_INTEGER f, t;
			QueryPerformanceFrequency(&f); gQpcFreq = f.QuadPart;
			QueryPerformanceCounter(&t);   gStartQpc = t.QuadPart;

			gHz = TheSettingManager->SettingsMain.Develop.ProfileSamplerHz;
			if (gHz < 50)   gHz = 50;
			if (gHz > 8000) gHz = 8000;

			InterlockedExchange(&gRunning, 1);
			gThread = CreateThread(NULL, 0, SamplerThread, NULL, 0, NULL);
			if (!gThread) {
				InterlockedExchange(&gRunning, 0);
				Logger::Log("[Sampler] CreateThread failed (%u); not starting.", GetLastError());
				return;
			}
			SetThreadPriority(gThread, THREAD_PRIORITY_HIGHEST);
			Logger::Log("[Sampler] STARTED at %d Hz, %u modules, stack ceiling 0x%08X. Press the key again to stop and report.",
				gHz, gModuleCount, gStackBase);
		}

		void Stop() {
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t); gStopQpc = t.QuadPart;

			InterlockedExchange(&gRunning, 0);
			if (gThread) {
				// The sampler can be inside a suspend/resume pair; give it room to finish
				// one, because abandoning it there would leave the game frozen.
				if (WaitForSingleObject(gThread, 3000) == WAIT_TIMEOUT)
					Logger::Log("[Sampler] WARNING: sampler thread did not exit in time.");
				CloseHandle(gThread);
				gThread = NULL;
			}
			Logger::Log("[Sampler] STOPPED.");
			Report();

			FreeTable(gExcl[0]); FreeTable(gExcl[1]); FreeTable(gIncl);
			if (gMain) { CloseHandle(gMain); gMain = NULL; }
		}

	}

	void CheckKey() {
		if (InterlockedCompareExchange(&gRunning, 1, 1) == 1) InterlockedIncrement(&gFrames);
		if (!TheSettingManager) return;
		UInt8 Key = TheSettingManager->SettingsMain.Develop.ProfileSampler;
		if (!Key || !TheKeyboardManager) return;
		if (!TheKeyboardManager->OnKeyDown(Key)) return;   // edge, not level: OnKeyPressed stays true while held
		if (InterlockedCompareExchange(&gRunning, 1, 1) == 1) Stop();
		else Start();
	}

	bool IsRunning() { return InterlockedCompareExchange(&gRunning, 1, 1) == 1; }

}
