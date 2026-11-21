#include "HavokStripCache.h"
#include "HavokStripMath.h"

#include <unordered_map>
#include <vector>

namespace {

	const UInt32 kStripLookupPatch    = 0x00915D3F;
	const UInt32 kStripLookupReturn   = 0x00915DA1;
	const UInt32 kStripsDataDestructor = 0x00719F80;
	const UInt32 kNiTriStripsDataVtbl = 0x00A7F32C;
	const UInt8  kExpectedBytes[7]    = { 0x8B, 0x40, 0x20, 0x89, 0x54, 0x24, 0x2C };
	const UInt32 kMinCachedStrips     = 16;
	const int    kStatsFrames         = 600;
	const int    kMaxMismatchLogs     = 8;

	struct Entry {
		bool                       Cacheable = false;
		HavokStripMath::StripTable Table;
	};

	bool  gInstalled  = false;
	bool  gVerify     = false;
	DWORD gMainThread = 0;

	// Main-thread state: only ResolveStrip and DrainFreed touch these.
	std::unordered_map<const void*, Entry*> gEntries;
	const void* gLastData  = NULL;
	Entry*      gLastEntry = NULL;
	LONG        gFreedSeen = 0;

	// Written by the destructor hook on any thread, under gFreedLock.
	SRWLOCK                  gFreedLock = SRWLOCK_INIT;
	std::vector<const void*> gFreed;
	volatile LONG            gFreedCount = 0;

	struct Stats {
		unsigned long long Cached = 0, Walked = 0, Mismatches = 0, Drains = 0, Freed = 0;
	};
	Stats gStats;
	int   gMismatchLogs = 0;
	int   gFrames = 0;

	void (__fastcall* StripsDataDestructor)(NiTriStripsData*, void*) = (void (__fastcall*)(NiTriStripsData*, void*))kStripsDataDestructor;

	void __fastcall TrackStripsDataDestructor(NiTriStripsData* This, void* Edx) {
		AcquireSRWLockExclusive(&gFreedLock);
		gFreed.push_back(This);
		InterlockedIncrement(&gFreedCount);
		ReleaseSRWLockExclusive(&gFreedLock);
		StripsDataDestructor(This, Edx);
	}

	void DrainFreed() {
		std::vector<const void*> Freed;
		AcquireSRWLockExclusive(&gFreedLock);
		Freed.swap(gFreed);
		gFreedSeen = gFreedCount;
		ReleaseSRWLockExclusive(&gFreedLock);
		gStats.Drains++;
		for (const void* Data : Freed) {
			auto It = gEntries.find(Data);
			if (It == gEntries.end()) continue;
			delete It->second;
			gEntries.erase(It);
			gStats.Freed++;
		}
		gLastData = NULL;
		gLastEntry = NULL;
	}

	Entry* FindEntry(NiTriStripsData* Data) {
		if (Data == gLastData) return gLastEntry;
		Entry*& Slot = gEntries[Data];
		if (!Slot) {
			Slot = new Entry();
			Slot->Cacheable = *(UInt32*)Data == kNiTriStripsDataVtbl &&
				HavokStripMath::BuildStripTable(Data->StripLengths, Data->NumStrips, Slot->Table);
		}
		gLastData = Data;
		gLastEntry = Slot;
		return Slot;
	}

	HavokStripMath::StripHit Resolve(NiTriStripsData* Data, UInt32 Tri) {
		if (gMainThread && GetCurrentThreadId() == gMainThread && Data->NumStrips >= kMinCachedStrips) {
			if (gFreedCount != gFreedSeen) DrainFreed();
			Entry* E = FindEntry(Data);
			if (E->Cacheable) {
				gStats.Cached++;
				HavokStripMath::StripHit Hit = HavokStripMath::LookupStrip(E->Table, Tri);
				if (!gVerify) return Hit;
				HavokStripMath::StripHit Walk = HavokStripMath::WalkStrips(Data->StripLengths, Data->NumStrips, Tri);
				if (Walk.Strip != Hit.Strip || Walk.Local != Hit.Local) {
					gStats.Mismatches++;
					if (gMismatchLogs++ < kMaxMismatchLogs)
						Logger::Log("[HavokStripCache] MISMATCH data 0x%08X tri %u: cached (%u,%u) walk (%u,%u)",
							(UInt32)Data, Tri, Hit.Strip, Hit.Local, Walk.Strip, Walk.Local);
				}
				return Walk;
			}
		}
		gStats.Walked++;
		return HavokStripMath::WalkStrips(Data->StripLengths, Data->NumStrips, Tri);
	}

	/// Called from StripLookupStub: returns the local triangle in eax and the strip in edx.
	UInt64 __fastcall ResolveStrip(NiTriStripsData* Data, UInt32 Tri) {
		HavokStripMath::StripHit Hit = Resolve(Data, Tri);
		return (UInt64)Hit.Local | ((UInt64)Hit.Strip << 32);
	}

	// Entered at 0x915D3F with eax = data, ecx = shape key, edx = StripLengths, ebx = NumStrips,
	// esi = strip indices; leaves for 0x915DA1 with the loop's outputs (spec: engine facts).
	__declspec(naked) void StripLookupStub() {
		__asm {
			mov		[esp + 0x2C], edx
			mov		[esp + 0x20], ebx
			and		ecx, 0xFFFFF
			push	eax
			push	ecx
			mov		edx, ecx
			mov		ecx, eax
			call	ResolveStrip
			pop		ecx
			// IndexOffset: (tri - local) + 2 * strip index entries precede the strip.
			sub		ecx, eax
			lea		ecx, [ecx + edx * 2]
			lea		esi, [esi + ecx * 2]
			mov		ecx, eax
			pop		eax
			mov		eax, [eax + 0x20]
			mov		[esp + 0x24], eax
			mov		[esp + 0x0C], ecx
			jmp		kStripLookupReturn
		}
	}
}

void CreateHavokStripCacheHook() {

	if (memcmp((const void*)kStripLookupPatch, kExpectedBytes, sizeof(kExpectedBytes))) {
		Logger::Log("[HavokStripCache] unexpected bytes at 0x915D3F (another patch?); not installed");
		return;
	}
	gVerify = TheSettingManager->SettingsMain.Develop.HavokStripCacheVerify != 0;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)StripsDataDestructor, &TrackStripsDataDestructor);
	if (DetourTransactionCommit() != NO_ERROR) {
		Logger::Log("[HavokStripCache] destructor hook failed; not installed");
		return;
	}
	WriteRelJump(kStripLookupPatch, (UInt32)&StripLookupStub);
	gInstalled = true;
	Logger::Log("[HavokStripCache] installed (verify %u)", gVerify ? 1u : 0u);

}

namespace HavokStripCache {

	void OnFrame() {
		if (!gInstalled) return;
		if (!gMainThread) gMainThread = GetCurrentThreadId();
		if (!gVerify || ++gFrames < kStatsFrames) return;
		Logger::Log("[HavokStripCache] %d frames: cached %llu walked %llu mismatches %llu entries %u drains %llu freed %llu",
			gFrames, gStats.Cached, gStats.Walked, gStats.Mismatches, (UInt32)gEntries.size(), gStats.Drains, gStats.Freed);
		gStats = Stats();
		gFrames = 0;
	}

	bool Installed() { return gInstalled; }
}
