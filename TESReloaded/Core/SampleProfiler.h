#pragma once

// --- Main-thread sampling profiler (Develop.ProfileSampler) -------------------
// FrameProfiler answers "which PLUGIN bucket got slower", but its envelope is the
// engine render call, so anything the game does outside it - AI, Havok, animation,
// the whole update phase - is invisible to it and does not even land in "Other".
// This profiler answers the complementary question: "which machine code was the
// main thread actually executing?", including code we never hooked.
//
// It runs a background thread that periodically suspends the game's main thread,
// reads EIP, and copies a slice of its stack. EIP gives EXCLUSIVE (leaf) cost;
// the stack slice is scanned for plausible return addresses to give INCLUSIVE
// cost, which is what identifies a subsystem rather than a leaf utility. Samples
// are split by phase (inside RenderHook::TrackRender vs. everything else), so
// plugin render cost and engine update cost never get conflated - the distinction
// that matters when the suspect ("many actors is slow") has a candidate cause on
// both sides.
//
// Addresses are symbolized at REPORT time via dbghelp against whatever PDBs are
// on the symbol path; the community Oblivion.pdb resolves most engine functions
// by name. Sampling itself never symbolizes and never allocates.
//
// Two accuracy caveats, both inherent to the technique and neither fixable here:
//
//  - The inclusive scan is a HEURISTIC. It accepts a stack dword when it points
//    into a loaded module and the bytes just before it decode as a call. Stale
//    frames left below the live stack pointer therefore produce false positives,
//    so read inclusive numbers as "this subsystem is implicated", never as an
//    exact share. Exclusive numbers carry no such caveat.
//  - A function reached through two different call sites within one stack is
//    counted twice inclusively, because de-duplication happens on the raw return
//    address, before symbols exist.
namespace SampleProfiler {

	// Written by RenderHook::TrackRender, read by the sampler thread. Not
	// synchronized: a torn read is impossible for a bool and a sample landing on
	// the wrong side of the boundary costs one sample out of tens of thousands.
	extern volatile bool InRender;

	// Polled once per frame from TrackRender; starts sampling on the first press
	// and stops-and-reports on the next. No-op unless Develop.ProfileSampler is set.
	void CheckKey();

	bool IsRunning();

}
