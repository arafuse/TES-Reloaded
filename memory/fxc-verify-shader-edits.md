---
name: fxc-verify-shader-edits
description: "Verify .fx.hlsl / .pso.hlsl edits with standalone fxc, and prove a gated edit is a no-op, without a C++ build"
metadata: 
  node_type: memory
  type: reference
  originSessionId: 5a9cb70f-de60-4cbb-91a3-2fc5a9a717ba
  modified: 2026-07-29T11:17:16.404Z
---

Shader-only edits can be fully verified without MSBuild. fxc lives at
`C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe` (the DirectX SDK June 2010
copy is NOT installed). Effects compile with `/T fx_2_0`:

```powershell
& $fxc /T fx_2_0 /D "SOME_FLAG=1" /Fo out.fxo OblivionReloaded\Shaders\Shadows\ShadowsExteriors.fx.hlsl
```

The six `warning X3570: gradient instruction used in a loop with varying iteration` on
ShadowsExteriors.fx.hlsl are PRE-EXISTING (the PCF loops) — not caused by an edit.

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
