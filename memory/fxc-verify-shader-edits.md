---
name: fxc-verify-shader-edits
description: "Verify .fx.hlsl / .pso.hlsl edits with standalone fxc, and prove a gated edit is a no-op, without a C++ build"
metadata:
  type: reference
---

Shader-only edits can be fully verified without MSBuild. Two fxc copies are installed:
`C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe` and
`C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe` (also `x64`).

- Effects (`.fx.hlsl`, have `technique{}`): `/T fx_2_0`
- Raw shaders (`.pso.hlsl`/`.vso.hlsl`): `/T ps_3_0` or `/T vs_3_0` with `/E main`
- Pass `/I <the shader's own directory>`: the game's D3DX resolves nested relative includes
  (`../Shadows/Includes/...`) from the top-level file's dir, fxc from the including file.
- `/Fc out.asm` dumps the assembly (check for real `if_*`/`endif` around `texld`, instruction counts).

```powershell
& $fxc /T fx_2_0 /D "SOME_FLAG=1" /Fo out.fxo OblivionReloaded\Shaders\Shadows\ShadowsExteriors.fx.hlsl
```

ShadowsExteriors.fx.hlsl currently compiles with a few pre-existing `X3206` implicit-truncation
warnings and no `X3570`; compare against HEAD before attributing a warning to an edit.

To prove a `#define`-gated debug/feature block is inert when off, compile the HEAD version and
byte-compare the .fxo:

```powershell
$txt = (git show HEAD:path/to/Shader.fx.hlsl) -join "`r`n"
[System.IO.File]::WriteAllText("$out\head.fx.hlsl", $txt, (New-Object System.Text.UTF8Encoding($false)))
```

`Out-File -Encoding utf8` writes a BOM that makes fxc fail with "compilation failed; no code
produced" — use `UTF8Encoding($false)` as above. Matching SHA256 on the two .fxo files proves the
gated-off build is unchanged.

Related: [[shader-pipeline-facts]], [[shader-deployment-workflow]], [[build-via-powershell-not-bash]]
