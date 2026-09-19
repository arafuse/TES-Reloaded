---
name: shader-pipeline-facts
description: "How OblivionReloaded compiles/loads HLSL shaders — preshaders, recompile gate, effect vs raw-shader split, silent flattening of tex2D branches. Read before optimizing shaders."
metadata:
  type: reference
---

Key facts about the shader pipeline in `OblivionReloaded/Shaders/` (see `TESReloaded/Core/ShaderManager.cpp`):

- **`.fx.hlsl` files (have `technique{}`) are D3DX9 effects**, compiled by `ShaderManager::CompileEffect`
  via `ID3DXEffectCompiler` and loaded with `D3DXCreateEffectFromFileA`. The only flag ever passed is
  `D3DXSHADER_PREFER_FLOW_CONTROL` (when the source contains `/Gfp`); `D3DXSHADER_NO_PRESHADER` is never
  set. So **uniform-only expressions are auto-extracted into CPU preshaders**, evaluated once per draw.
  Do NOT hand-hoist frame-constant math (exp/sigmoids/`nearZ`/`farZ`, matrix concatenation) to C++ for
  effect files — the runtime already does it.

- **`.pso.hlsl`/`.vso.hlsl` raw shaders** (Terrain, POM, Skin, Grass, Shadow maps, scene geometry)
  compile via `D3DXCompileShaderFromFileA` in `ShaderManager::CompileShader` to `ps_3_0`/`vs_3_0` —
  **no preshaders**. Uniform-only math here IS per-pixel/per-vertex, so hoisting it is a real win.
  `PREFER_FLOW_CONTROL` is set only for sources including `Includes/ShadowCube`.

- **Recompile gate:** edited `.hlsl` does NOT take effect until shaders are recompiled. Set
  `[Develop] CompileShaders = 1` in `OblivionReloaded.ini`, which recompiles EVERY shader at startup
  (no timestamp check). Output is cached beside each source with `.hlsl` stripped (`X.pso.hlsl` →
  `X.pso`, gitignored). Watch the log for compile errors.

- No INI-derived `#define`s reach the compiler (`pDefines` is NULL) — see [[shader-bake-defines]].

- fxc folds constant-integer `pow(x, 2.0)` → `x*x`; `pow(x, 1.5)` does NOT fold (write `x*sqrt(x)`).

- **A dynamic `if` around `tex2D` is SILENTLY FLATTENED.** An ordinary `if (uniform < x) { ...tex2D... }`
  compiles with exit 0 and no warning, but fxc hoists the fetches out and executes BOTH sides, leaving
  the condition as a `lrp` weight — `tex2D` needs implicit ddx/ddy, which can't be computed under
  divergent flow control. "The branch isn't taken, it costs nothing" must be measured, not assumed:
  check `/Fc` asm for real `if_*`/`endif` around the `texld`s. Fix: `tex2Dlod(s, float4(uv,0,0))` —
  lossless wherever the texture has one mip level (all the shadow maps do), no extra instructions,
  restores a genuine branch. `X3570 gradient instruction used in a loop` warnings are the tell.

- **Gradient instructions in dynamic loops:** inside a `[loop]`, `tex2D` is an error (`X3526: can't use
  gradient instructions in loops with break`). Use `tex2Dlod` (explicit LOD) or `[unroll]`.

How to compile/verify offline: [[fxc-verify-shader-edits]]. Deployment: [[shader-deployment-workflow]].
