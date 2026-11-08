---
name: protoplugin-sibling
description: "The sibling ProtoPlugin repo (../ProtoPlugin) is loaded in the same game: the engine sites it patches (check before hooking the same ones), its RE tools (petool.py, livestacks.py for hangs), and the OBSE loader-thread deadlock rule"
metadata:
  type: reference
---

`C:\Users\Adam\Code\Oblivion\ProtoPlugin` builds `ProtoPlugin.dll`, an OBSE plugin loaded next to
Oblivion Reloaded (INI `Data\OBSE\Plugins\ProtoPlugin.ini`). It also vendors a patched xOBSE. Its
`memory/` holds the full write-ups for cell loading, load screens, file IO and the combat CPU spike.

**Sites it rewrites** (1.2.416). If OR ever hooks one of these, the two plugins will collide:
- IO drain call 0x40D8D8 and 0x43416B (BudgetGovernor steers `iPostProcessMilliseconds` at 0xB048E4
  every frame); billboard completion calls 0x437680 / 0x437687 (BillboardBatch)
- `GridDistantArray::Shift` vtable slot and DistantLOD purge calls 0x48365B / 0x483FDF / 0x48400D (LODSweep)
- player control handler 0x671620: jump sites 0x672570 / 0x672A6B / 0x672A8D (AutoMove), activate
  call 0x67318A (LootGuard); reticle icon call 0x5A8D36 (LootGuard)
- knockdown update 0x6545E0: four `GetAV(Fatigue)` calls 0x654644 / 0x654888 / 0x6549A7 / 0x654C83
  (RagdollRecovery; holds ragdolls down, distinct from OR's RagdollCollision layer matrix)
- follow speed recursion 0x5E38AA (FollowCycleGuard); cell-reset delete 0x4CFBEF (ResetSparesActors)

**Tools** in its `tools/` (usage in `tools/README.md`):
- `petool.py`: static disassembly and caller search on Oblivion.exe. Stack offsets it prints are
  module offsets: `4B9C93` means 0x8B9C93. An MSVC SEH prologue pushes a `.text` scope-table pointer
  that a stack scan reports as a fake frame.
- `livestacks.py`: per-thread stacks, return-address scans, critical-section owners and handles of a
  frozen game (VS Break All first; it never suspends threads). Needs a 64-bit Python (WOW64 target).
  Use it for hangs; the VS debugger shows only one thread.
- `fixmips.py`: adds mip chains to DDS files ([[load-screen-texture-creation]]).

**Loader-thread deadlock rule.** xOBSE once ran event handlers under its global lock; a handler that
made the main thread wait on the IOManager deadlocked against a `BSTaskThread` loader firing an OBSE
hook (fixed in the vendored xOBSE, fbb5cf5). Any OR hook on a form-load or cell-link path
(e.g. FormHook) runs on that loader thread and must not take a lock the main thread can hold across an
engine wait. The tell is `IOManager` on the main stack plus a task thread blocked in a lock.
