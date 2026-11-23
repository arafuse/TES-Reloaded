---
name: clangd-compile-commands
description: "clangd/LSP setup — Tools/GenerateCompileCommands.ps1 makes the gitignored compile_commands.json from the vcxproj; why -ferror-limit=0 and i686 are needed; `for each` loops are unparseable by clang"
metadata:
  type: reference
---

`Tools/GenerateCompileCommands.ps1` writes `compile_commands.json` (gitignored) at the repo root by
running MSBuild `-getProperty/-getItem` on the vcxproj (no build) and emitting clang-cl commands.
Re-run it after adding source files or changing compiler settings. clangd is VS's own
(`VC\Tools\Llvm\x64\bin`, on the user PATH for the clangd-lsp plugin, which spawns bare `clangd`).

Non-obvious flags the generator adds:
- `--target=i686-pc-windows-msvc`: the MSVC `__asm` blocks only parse for 32-bit x86.
- `/clang:-ferror-limit=0`: a header opened on its own is ALSO pulled in by the forced `/FI Framework.h`
  chain. clangd skips that self-include, so later chain headers that need it cascade into errors. At the
  default limit of 20, the fatal "too many errors" kills the parse before the header's body (Game.h,
  GameNi.h, Types.h and SettingManager.h get no LSP without it).
- `-Wno-enum-enum-conversion` (C++26 makes cross-enum arithmetic an error; cl accepts it) and `-Wno-address-of-temporary`.

Don't use MSVC's `for each (T x in C)`: clang can't parse it, and the codebase has none. MSVC's form
silently downcasts array elements (e.g. `NiD3DPixelShaderEx*` over a `NiD3DPixelShader*[]`), so the
range-for equivalent needs an explicit cast, or ShaderManager::LoadShader/UnloadShader, which take
the base type.

Known clang-only errors: `ok ? "a" : "b"` string-literal ternaries passed to Logger::Log's `char*`.

Test LSP without the plugin by driving clangd over stdio from a Python client; `clangd --check=<file>`
prints main-file diagnostics, but its `tweak: ... FAIL` lines are refactoring probes, not compile errors.
