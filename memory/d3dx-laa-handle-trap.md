---
name: d3dx-laa-handle-trap
description: Oblivion.exe is Large Address Aware; D3DX9 constant-table/effect handles to records above 2GB are misread as name strings unless the LARGEADDRESSAWARE flag is passed — crashes or silently mis-binds runtime-loaded shaders
metadata:
  node_type: memory
  type: project
  originSessionId: 06ab3614-5c69-4a08-affb-d478b4e65948
  modified: 2026-09-26T01:46:07.908Z
---

Oblivion.exe has the LAA bit set (PE characteristics `0x0123`), so the heap goes above 2GB in long sessions. D3DX9_43 hands out a `D3DXHANDLE` as the **negated** pointer to its internal record (`GetConstant` at d3dx9_43 RVA `0xEB0D0`). `GetConstantDesc` (RVA `0xEAFF0`) tests `(tableFlags | handle) < 0`: a negative value is a handle, and anything else is parsed as a `const char*` name (RVA `0xEAE0D`). When a record is above 2GB, `-ptr` is positive and D3DX dereferences it as a string.

- **Crash seen 2026-09-25:** entering an interior ran `CreateShader("InteriorShadows")` → `ShaderRecord::CreateCT` → access violation at `d3dx9_43+0xEAE1A` (`movsx eax, byte ptr [edi]`, where edi is the bogus "string").
- **Silent variant:** when `-ptr` happens to be readable, `GetConstantDesc` just returns `D3DERR_INVALIDCALL` (`0x8876086C`) and leaves `ConstantDesc` stale. Unchecked, this mis-binds `TESR_` constants with no crash.
- Startup loads sit low in memory and are usually fine; runtime reloads (interior/exterior, dialog shader swaps) are what hit it.

**Fix in ShaderManager.cpp:** `D3DXGetShaderConstantTableEx(..., D3DXCONSTTABLE_LARGEADDRESSAWARE, ...)`, `D3DXCreateEffectFromFileA(..., D3DXFX_LARGEADDRESSAWARE, ...)`, and `continue` when `GetConstantDesc`/`GetParameterDesc` fails.

**Why:** with the flag set, D3DX checks the table flag instead of the handle's sign bit.

**How to apply:** any new `ID3DXConstantTable`/`ID3DXEffect` must be created with the LAA flag. With the flag set, D3DX **rejects string names passed as handles**, so look up names with `GetConstantByName`/`GetParameterByName` and never pass `"TESR_x"` directly to Set*/GetDesc calls. `D3DXCompileShader`'s `ppConstantTable` has no LAA option, so re-fetch the table with the Ex call if you ever keep it.
