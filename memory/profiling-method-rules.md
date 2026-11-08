---
name: profiling-method-rules
description: "Rules for profiling Oblivion (from ProtoPlugin's campaigns) - PresentMon GPU-bound check before chasing a driver wait, perception check, wall not CPU, keep a control, count what you drop, C:\\Games is a symlink, Defender scans every loose-file open"
metadata:
  type: feedback
---

Method rules learned in the ProtoPlugin repo (`memory/profiling-lessons-oblivion.md` there).
Read before designing a new instrument or trusting a profile.

**A driver wait is a location, not a cause, until removing it removes the time.**
**Why:** the native-D3D9 wait under the skinned index Lock looked causal; removing the lock left the
same 5.6 ms wait, which was the CPU in Present on a GPU-bound frame ([[native-d3d9-skinned-index-stall]]).
**How to apply:** first check whether the frame is GPU-bound. One PresentMon capture answers it:
MsGPUBusy vs frame time, MsGPUWait, MsInPresentAPI. Then check that the sampler's game frame is a live
call, not a stale return address ([[sampling-profiler]]).

**Check the numbers against perception before optimising further.**
**Why:** removing ~10 ms of transition-frame CPU made no difference the user could feel.
**How to apply:** after the first measurable win, ask "does this feel different?" before chasing the
next-largest number.

**Measure wall time, not just CPU.**
**Why:** blocked frames are a hard visual stop, busy frames blend into judder. A CPU-only profile
cannot see a blocking stall.
**How to apply:** keep `blocked = wall - cpu` in the frame row. Attribute `Sleep` by caller before
subtracting it: the main thread also sleeps as an IO wait (0x434161 `Sleep(50)` before the drain,
0x43AF75 `Sleep(5)` retry); only 0x498F1F (the frame limiter 0x498F00) is idle.

**Keep a control population, and test the instrument before believing a null result.**
**Why:** without a control a hot chain may be what that code always does. An instrument that cannot
show a signal always reports its absence (bucketing on deep stacks manufactures "diffuse").
**How to apply:** retire a control only after it has enough samples to show a difference; before
concluding "no hot spot", re-key on the leaf or re-bucket coarser.

**Count what you drop.**
**How to apply:** every fixed-size capture buffer prints a `dropped` counter and every hook an
invocation count. A dead hook and a quiet session both print zero. Make capture buffers single-writer.

**Run a kill gate before building on a premise.**
**How to apply:** before a large build, name the measurement that would make it pointless and take it
first (one capture showed the cell-transition frame was CPU-bound and killed a streaming design).

**Environment traps on this machine:**
- `C:\Games` is a symlink to `D:\Games`; the game may report either path. When two paths differ in a
  measurement, check they differ only in the thing being tested (same volume, links resolved).
- Windows Defender costs ~3.2 ms per file OPEN (not per byte) outside an exclusion. The install is
  loose-file heavy (~18 opens/s in exterior play), so a frame opening three files takes a ~9.5 ms
  hit. The game folder is excluded (in-game opens ~130 µs). Rule Defender out by measurement before
  profiling a perceived stutter.
