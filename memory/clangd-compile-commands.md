---
name: clangd-compile-commands
description: "clangd/LSP setup — Tools/GenerateCompileCommands.ps1 makes the gitignored compile_commands.json from the vcxproj; why -ferror-limit=0 and i686 are needed; `for each` loops are unparseable by clang"
metadata:
  node_type: memory
  type: reference
  originSessionId: 1f2b40ae-e847-436f-9009-6a4f0f8d1f86
  modified: 2026-09-23T22:50:40.754Z
---

`Tools/GenerateCompileCommands.ps1` writes `compile_commands.json` (gitignored) at the repo root by running MSBuild `-getProperty/-getItem` on the vcxproj (no build) and emitting clang-cl commands. Re-run it after adding source files or changing compiler settings. clangd is VS's own (`VC\Tools\Llvm\x64\bin`, added to the user PATH on 2026-09-23 for the clangd-lsp plugin, which spawns bare `clangd`).

Non-obvious flags the generator adds:
- `--target=i686-pc-windows-msvc`: 43 MSVC `__asm` blocks only parse for 32-bit x86.
- `/clang:-ferror-limit=0`: a header opened on its own is ALSO pulled in by the forced `/FI Framework.h` chain. clangd skips that self-include, so later chain headers that need it cascade into errors. At the default limit of 20, the fatal "too many errors" killed the parse before the header's body. Game.h, GameNi.h, Types.h and SettingManager.h had no LSP at all until this flag was added.
- `-Wno-enum-enum-conversion` (C++26 makes cross-enum arithmetic an error; cl accepts it) and `-Wno-address-of-temporary`.

Known clang-only errors that remain (MSVC accepts them):
- `for each (T x in C)`: ~55 uses, 39 in ShaderManager.cpp. clang can't parse them, so navigation inside those loop bodies is degraded. The fix is converting them to range-for.
- `ok ? "a" : "b"` passed to `char*` (Logger::Log).
- ShaderManager.cpp:850/875 pass `std::string` to varargs `%s`.
- ShadowManager.cpp:1290 `D3DXVECTOR4 = D3DXVECTOR3(...)`.

The last two are latent bugs: the string only works because MSVC's heap pointer comes first in long strings, and the vector's `w` is read past a 12-byte temporary.

Test LSP without the plugin by driving clangd over stdio from a Python client; `clangd --check=<file>` prints main-file diagnostics, but its `tweak: ... FAIL` lines are refactoring probes, not compile errors.
