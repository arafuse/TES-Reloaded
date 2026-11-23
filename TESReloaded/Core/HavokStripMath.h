#pragma once

#include <cstdint>
#include <vector>

// --- hkNiTriStripsShape triangle lookup math (HavokStripCache) -----------------------------------
// Engine-free. The engine finds a shape key's triangle by walking the strip lengths from strip 0
// (0x915D60); StripTable answers the same question in O(1). The harness in Tools/Tests proves the
// two agree for every triangle, including out-of-range keys and zero-triangle strips.
namespace HavokStripMath {

	/// The strip a triangle falls in and its triangle index within that strip.
	struct StripHit {
		uint32_t Strip;
		uint32_t Local;
	};

	/// Largest triangle count that gets a table (caps StripOf at 512 KB).
	const uint32_t MaxTableTris = 262144;

	/// Prefix triangle counts and a per-triangle strip index for one NiTriStripsData.
	struct StripTable {
		std::vector<uint32_t> Prefix;   ///< NumStrips + 1 entries: triangles before strip i
		std::vector<uint16_t> StripOf;  ///< TotalTris entries: the strip each triangle is in
		uint32_t NumStrips = 0;
		uint32_t TotalTris = 0;
	};

	/// The engine's loop: a strip of L indices holds L - 2 triangles, compared unsigned, so a strip
	/// shorter than 2 stops the walk. Past the last strip the result is (NumStrips, remaining).
	inline StripHit WalkStrips(const uint16_t* Lengths, uint32_t NumStrips, uint32_t Tri) {
		uint32_t Strip = 0;
		for (; Strip < NumStrips; Strip++) {
			uint32_t Triangles = (uint32_t)Lengths[Strip] - 2;
			if (Tri < Triangles) break;
			Tri -= Triangles;
		}
		return { Strip, Tri };
	}

	/// Builds the table; false when a strip is shorter than 2 or the total exceeds MaxTableTris.
	inline bool BuildStripTable(const uint16_t* Lengths, uint32_t NumStrips, StripTable& Out) {
		uint32_t Total = 0;
		for (uint32_t i = 0; i < NumStrips; i++) {
			if (Lengths[i] < 2) return false;
			Total += Lengths[i] - 2;
			if (Total > MaxTableTris) return false;
		}
		Out.NumStrips = NumStrips;
		Out.TotalTris = Total;
		Out.Prefix.resize(NumStrips + 1);
		Out.StripOf.resize(Total);
		uint32_t Running = 0;
		for (uint32_t i = 0; i < NumStrips; i++) {
			Out.Prefix[i] = Running;
			for (uint32_t t = 0; t < (uint32_t)Lengths[i] - 2; t++) Out.StripOf[Running + t] = (uint16_t)i;
			Running += Lengths[i] - 2;
		}
		Out.Prefix[NumStrips] = Running;
		return true;
	}

	/// The same answer as WalkStrips, from a table.
	inline StripHit LookupStrip(const StripTable& Table, uint32_t Tri) {
		if (Tri < Table.TotalTris) {
			uint32_t Strip = Table.StripOf[Tri];
			return { Strip, Tri - Table.Prefix[Strip] };
		}
		return { Table.NumStrips, Tri - Table.TotalTris };
	}

	/// Index entries before the hit strip: its triangles before it plus two per earlier strip.
	inline uint32_t IndexOffset(uint32_t Tri, StripHit Hit) {
		return Tri - Hit.Local + 2 * Hit.Strip;
	}
}
