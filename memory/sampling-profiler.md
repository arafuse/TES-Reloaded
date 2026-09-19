---
name: sampling-profiler
description: "In-plugin main-thread sampling profiler (Develop.ProfileSampler) - what it measures, how to read its output, and the two numbers you must NOT trust"
metadata:
  type: project
---

`TESReloaded/Core/SampleProfiler.{h,cpp}`. A background thread suspends the game's main thread
(~760 Hz effective at the default 1000 requested), records EIP (exclusive) and scans 3 KB of stack for
call-preceded return addresses (inclusive), tagged by phase: inside `RenderHook::TrackRender` or
outside it. Symbolized at report time via dbghelp - see [[oblivion-pdb-symbols]].

Enable with `[Develop] ProfileSampler = <key code>` (e.g. 68 = F10; code default 0 = off); the key
toggles start / stop+report. `ProfileSamplerHz` defaults to 1000. The report goes to
`OblivionReloaded.log` in the game folder. Set `ProfileFrame = 0` during sampler runs - its per-draw
QPC calls pollute the results. Overhead while running is ~0.2-0.5% of one core.

**Reading it - two traps:**

1. **Percentages are per-phase shares, not time.** A frame-capped run idles in
   `NtWaitForAlertByThreadId` and that idle inflates the update phase. ALWAYS
   convert to ms/frame first: `ms_per_sample = 1000 / effective_Hz`, then
   `phase_ms_per_frame = phase_share x (effective_Hz / FPS) x ms_per_sample`.
   Cross-check that update + render = 1000/FPS.
2. **Inclusive numbers for system DLLs are garbage.** The inclusive view merges by
   nearest symbol; ntdll/d3d9/kernel32 export so few symbols that unrelated functions
   collapse onto one name (hundreds of percent on e.g. `RtlReleaseSRWLockShared`).
   Trust inclusive ONLY for Oblivion.exe (IDA PDB names every function) and
   OblivionReloaded.dll (real PDB). `RenderHook::TrackRender`'s inclusive share
   matching the render phase split is a good sanity check that the scan works.

**Deadlock safety (do not "simplify" this):** between SuspendThread and
ResumeThread the sampler does ONLY GetThreadContext + memcpy. No allocation, no
VirtualQuery, no lock of any kind - the main thread may be stopped inside the
allocator or loader, and taking either lock there hard-hangs the game. That is why
the stack ceiling is read once from the TIB in `Start()` (which runs on the main
thread) instead of being queried per sample.

**Why it exists:** `FrameProfiler`'s envelope is the engine render call, so the whole game update
phase (AI, Havok, animation) is invisible to it. Example use: [[many-actors-havok-cost]].
