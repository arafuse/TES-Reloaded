#include "HavokStripMath.h"
#include <cstdio>
#include <vector>

using namespace HavokStripMath;

static int Failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++Failures; } } while (0)

static bool Same(StripHit a, StripHit b) { return a.Strip == b.Strip && a.Local == b.Local; }

static void CheckAllMatchWalk(const std::vector<uint16_t>& Lengths) {
	StripTable Table;
	CHECK(BuildStripTable(Lengths.data(), (uint32_t)Lengths.size(), Table));
	for (uint32_t Tri = 0; Tri < Table.TotalTris + 8; Tri++) {
		StripHit Walk = WalkStrips(Lengths.data(), (uint32_t)Lengths.size(), Tri);
		StripHit Hit = LookupStrip(Table, Tri);
		if (!Same(Walk, Hit)) {
			std::printf("  tri %u: table (%u,%u) walk (%u,%u)\n", Tri, Hit.Strip, Hit.Local, Walk.Strip, Walk.Local);
			++Failures;
			return;
		}
	}
}

static void TableMatchesWalkForEveryTriangle() {
	CheckAllMatchWalk({ 3, 5, 4, 3, 10, 3 });
	std::vector<uint16_t> Random;
	uint32_t Seed = 1;
	for (int i = 0; i < 143; i++) {
		Seed = Seed * 1664525u + 1013904223u;
		Random.push_back((uint16_t)(3 + (Seed >> 16) % 7));
	}
	CheckAllMatchWalk(Random);
}

static void ZeroTriangleStripsAreSkipped() {
	std::vector<uint16_t> Lengths = { 2, 4, 2, 2, 5, 2 };
	StripTable Table;
	CHECK(BuildStripTable(Lengths.data(), 6, Table));
	CHECK(Same(LookupStrip(Table, 0), { 1, 0 }));
	CHECK(Same(LookupStrip(Table, 1), { 1, 1 }));
	CHECK(Same(LookupStrip(Table, 2), { 4, 0 }));
	CHECK(Same(LookupStrip(Table, 4), { 4, 2 }));
	CheckAllMatchWalk(Lengths);
}

static void OutOfRangeTriangleMatchesWalk() {
	std::vector<uint16_t> Lengths = { 4, 4 };
	StripTable Table;
	CHECK(BuildStripTable(Lengths.data(), 2, Table));
	CHECK(Same(LookupStrip(Table, 4), { 2, 0 }));
	CHECK(Same(LookupStrip(Table, 0xFFFFF), { 2, 0xFFFFB }));
	CHECK(Same(WalkStrips(Lengths.data(), 2, 0xFFFFF), { 2, 0xFFFFB }));
}

static void ZeroStrips() {
	CHECK(Same(WalkStrips(nullptr, 0, 7), { 0, 7 }));
	StripTable Table;
	CHECK(BuildStripTable(nullptr, 0, Table));
	CHECK(Same(LookupStrip(Table, 7), { 0, 7 }));
}

static void DegenerateStripRejectsTable() {
	std::vector<uint16_t> Short = { 4, 1, 4 };
	std::vector<uint16_t> Empty = { 4, 0, 4 };
	StripTable Table;
	CHECK(!BuildStripTable(Short.data(), 3, Table));
	CHECK(!BuildStripTable(Empty.data(), 3, Table));
	CHECK(Same(WalkStrips(Short.data(), 3, 3), { 1, 1 }));
}

static void TableTooLargeIsRejected() {
	std::vector<uint16_t> Five(5, 65535);
	std::vector<uint16_t> Four(4, 65535);
	StripTable Table;
	CHECK(!BuildStripTable(Five.data(), 5, Table));
	CHECK(BuildStripTable(Four.data(), 4, Table));
	CHECK(Table.TotalTris == 262132);
}

static void IndexOffsetIsSumOfEarlierLengths() {
	std::vector<uint16_t> Lengths = { 3, 5, 4 };
	StripTable Table;
	CHECK(BuildStripTable(Lengths.data(), 3, Table));
	CHECK(IndexOffset(0, LookupStrip(Table, 0)) == 0);
	CHECK(IndexOffset(1, LookupStrip(Table, 1)) == 3);
	CHECK(IndexOffset(4, LookupStrip(Table, 4)) == 8);
	CHECK(IndexOffset(6, LookupStrip(Table, 6)) == 12);
}

int main() {
	TableMatchesWalkForEveryTriangle();
	ZeroTriangleStripsAreSkipped();
	OutOfRangeTriangleMatchesWalk();
	ZeroStrips();
	DegenerateStripRejectsTable();
	TableTooLargeIsRejected();
	IndexOffsetIsSumOfEarlierLengths();
	if (Failures) std::printf("%d failures\n", Failures);
	else std::printf("HavokStripMath: all tests passed\n");
	return Failures ? 1 : 0;
}
