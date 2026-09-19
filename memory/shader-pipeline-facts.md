---
name: shader-pipeline-facts
description: "How OblivionReloaded compiles/loads HLSL shaders — preshaders, recompile gate, effect vs raw-shader split. Read before optimizing shaders."
metadata: 
  node_type: memory
  type: reference
  originSessionId: 7db762f9-deec-4798-81cc-4e0406184574
  modified: 2026-08-07T12:26:49.800Z
---

Key facts about the shader pipeline in `OblivionReloaded/Shaders/` (verified in `TESReloaded/Core/ShaderManager.cpp`), relevant when optimizing HLSL:

- **`.fx.hlsl` files (have `technique{}`) are D3DX9 effects.** Loaded via `D3DXCreateEffectFromFileA` (`EffectRecord::LoadEffect`) and compiled by `CompileEffect` (~line 680) with flags = NULL — `D3DXSHADER_NO_PRESHADER` is NOT set. So **uniform-only expressions are auto-extracted into CPU preshaders** and evaluated once per draw during `Effect->Begin`. Do NOT bother manually hoisting frame-constant math (e.g. `exp`/sigmoids/`nearZ`/`farZ`, matrix concatenation) to C++ for effect files — the runtime already does it. The canonical preshader use (matrix `mul` of two uniforms in a `static const`) works and offloads to CPU.

- **`.pso.hlsl`/`.vso.hlsl` raw shaders** (Terrain, POM, Skin, Shadow includes, scene geometry) compile via `D3DXCompileShaderFromFileA` (`CompileShader`, ~line 656) to `ps_3_0`/`vs_3_0` — **no preshaders**. Uniform-only math here IS per-pixel/per-vertex, so hoisting it (or pre-concatenating matrices) is a real win.

- **Recompile gate:** edited `.hlsl` does NOT take effect until shaders are recompiled. Set `[Develop] CompileShaders=1` in `OblivionReloaded.ini` (read at `SettingManager.cpp:421`; triggers `CompileShaders(ShadersPath)` at `ShaderManager.cpp:908`). Compiled output is cached as an **extension-less file beside each `.hlsl`** (not a timestamp check). Watch the load log for `D3DXCompileShaderFromFileA`/effect compile errors.

- fxc folds constant-integer `pow(x, 2.0)`→`x*x` already; `pow(x, 1.5)` does NOT fold (replace with `x*sqrt(x)` manually).

- **You CAN validate shader compiles offline** (don't need to launch the game): `fxc.exe` is at `C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x64\fxc.exe`. Raw shaders: `fxc /T ps_3_0 /E main /I <shaderDir> <file>`. Effects (`.fx.hlsl` with `technique{}`): `fxc /T fx_2_0 /I <shaderDir> <file>`. Pass `/I <the shader's own directory>` — the game's D3DX resolves nested relative includes (`../Shadows/Includes/...`) from the top-level `.pso` dir, but fxc resolves from the including file, so `/I` fixes it. Exit 0 = compiles.

- **Gotcha — a dynamic `if` around `tex2D` is SILENTLY FLATTENED (verified 2026-08-07, feat/shadow-fade).** Not just loops: an ordinary `if (uniform < x) { ...tex2D... }` compiles with exit 0 and no warning, but fxc hoists the fetches out and executes BOTH sides unconditionally, leaving the condition as a bare `lrp` weight. Cause is the same — `tex2D` needs implicit ddx/ddy, which can't be computed under divergent flow control. **So "the branch isn't taken, it costs nothing" is worthless as an assumption; it must be measured.** Check with `fxc /T ps_3_0 /E <entry> /Fc out.asm` and look for real `if_lt`/`endif` around the `texld`s rather than an unconditional sequence. Fix: `tex2Dlod(s, float4(uv,0,0))` — lossless wherever the texture has `Levels=1` (all the shadow maps do), costs 0 extra instructions per tap, and restores a genuine branch. The `X3570 gradient instruction used in a loop` warning count is the tell: ShadowsExteriors went 888 → 0 with the conversion, and 166 instructions became skippable.

- **Gotcha — gradient instructions in dynamic loops:** inside a `[loop]` (dynamic flow control), `tex2D` is illegal (`error X3526: can't use gradient instructions in loops with break`) because it needs implicit ddx/ddy gradients. Use `tex2Dlod(s, float4(uv, 0, 0))` (explicit LOD, no gradients) — lossless when sampling a full-res buffer where only LOD 0 is used. This is why the original GodRays light-shaft loop used `[unroll(50)]`.