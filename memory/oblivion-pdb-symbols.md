---
name: oblivion-pdb-symbols
description: "Oblivion.exe CAN be symbolized - a community IDA-derived Oblivion.pdb in the Plugins folder is GUID-stamped to match the exe, so dbghelp resolves engine addresses to real names"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 88be16ed-3132-4290-9a9c-b47a9192e4cc
  modified: 2026-09-08T12:51:41.891Z
---

`C:\Games\Steam\steamapps\common\Oblivion\Data\OBSE\Plugins\Oblivion.pdb` (~3 MB, dated
2026-01-21, shipped alongside CobbCrashLogger/CrashLoggerHelper) is a community
IDA-exported symbol file **stamped with the exe's own RSDS GUID**
(`7bbd0a3a-8959-474f-aba3-082cd018c220`, age 1), so dbghelp loads it against
`Oblivion.exe` at base `0x400000` with no override needed.

- 63,813 symbols, of which ~21k are real RE'd names (`Calc_DetectionLevel`,
  `HighProcess::GetFurniture`, `LowProcess_HandleCrime`, `bhk*`, `TESActorBase*`,
  `MagicTarget_ProcessEffects`, …). The rest are IDA `sub_XXXXXX` autonames, so an
  address in an un-RE'd region resolves to `sub_86CC90+0x3A20` — still a stable
  function-start anchor, just unnamed.
- Symbol lookup wants `SYMOPT_UNDNAME | SYMOPT_LOAD_ANYTHING`; the search path must
  include the Plugins folder. `SymGetModuleInfo64` reports SymType=3 (Cv) and
  NumSyms=0 even on a good load — neither means failure, check `LoadedPdbName`.
- `SYMBOL_INFO` marshalling gotcha: `SizeOfStruct` = 88, `MaxNameLen` at offset 80,
  `Name` at offset 84; the buffer must be 8-aligned (it holds ULONG64 fields).

**Why:** it removes the usual blocker on engine-level perf and crash work in this
fork — raw addresses no longer have to be matched by hand against the scattered
`#define k<Name> 0x00XXXXXX` constants in the codebase.

**How to apply:** any tool that needs engine function names (the in-plugin
[[sampling-profiler]], a crash triage, an xref hunt) can just call dbghelp. See
`TESReloaded/Core/SampleProfiler.cpp` for the in-process pattern (dynamic
GetProcAddress, private session handle `0xB105F00D` so other OBSE plugins' dbghelp
sessions are untouched).
