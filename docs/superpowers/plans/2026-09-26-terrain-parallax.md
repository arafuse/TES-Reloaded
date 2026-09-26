# Terrain Parallax Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Near land gets single-tap parallax on every layer pass, driven by the height in each landscape diffuse map's alpha, tunable from `Terrain.ini` and off by default.

**Architecture:** The near-land VSOs (SLS2042/2043) emit a tangent-space eye vector plus eye distance on the free TEXCOORD1. The near-land PSOs (SLS2048/2049) shift their UV with a shared `TerrainParallaxUV()` helper before every base/normal map sample, using a new CPU constant `TESR_TerrainParallaxData` (c7) packed in `ShaderManager::UpdateTerrain` from two new `SettingsTerrain` fields.

**Tech Stack:** HLSL SM3 (runtime-compiled by the plugin via D3DX), C++ (MSVC v145, x86), OBSE plugin.

**Spec:** `docs/superpowers/specs/2026-09-26-terrain-parallax-design.md`

## Global Constraints

- Oblivion only. No new C++ source files (no vcxproj / `compile_commands.json` changes). One new shader include: `OblivionReloaded/Shaders/Terrain/Includes/Parallax.hlsl`.
- Coding style (AGENTS.md): public symbols get `///` doc comments; self-documenting code; inline comments only when necessary and at most 1–3 lines.
- Build ONLY through the PowerShell tool (Bash's TEMP breaks MSBuild with a fake MSB3073):
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  The post-build step copies the DLL into the game's Plugins folder; the game must not be running.
- Shader facts (do not re-derive): the game's `Data\Shaders\OblivionReloaded` is a symlink to `OblivionReloaded\Shaders`. Edited `.hlsl` only takes effect after the game recompiles it with `[Develop] CompileShaders = 1` in the game's `Data\OBSE\Plugins\OblivionReloaded.ini`; the compiled `.vso`/`.pso` next to each `.hlsl` are gitignored (the game writes them).
- Offline shader check uses `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe`, `/T vs_3_0` or `/T ps_3_0`, `/E main`, `/I OblivionReloaded\Shaders\Terrain`.
- Register/slot facts: PSO constant **c7** is unused in SLS2048/2049 (they use c1–c4, c6). Interpolator **TEXCOORD1** is unused by our SLS2042/2043/2048/2049 overrides. `EyePosition` (c25, model space) is already declared in both VSOs; `TanSpaceProj` is already `#define`d in both VSO input structs.
- Constant packing (exact): `x = ParallaxScale`, `y = -0.5 * ParallaxScale`, `z = -2 / ParallaxFadeDistance`, `w = 2`; when `ParallaxFadeDistance <= 0`: `z = 0`, `w = 1`.
- Settings in `SettingsTerrain` / `Shaders\Terrain\Terrain.ini` `[Default]`: `ParallaxScale` (code default `0.0`), `ParallaxFadeDistance` (code default `8000.0`). Shipped INI uses the same values.
- `TESR_TerrainParallaxData` is mapped in `SetConstantTableValue2` (not `Value1`, which is already ~92 `else if` deep; MSVC caps nesting at 128).
- Commits: code as `feat(Terrain): …` / `fix(Terrain): …`; spec/plan/memory changes only in separate `docs:` commits.

**User decisions (already made):**
- Quality: single-tap offset (stock PAR formula), no ray march.
- Layers: parallax on every layer pass (SLS2048 and SLS2049), each layer using its own height.
- Settings: own `Terrain.ini` keys `ParallaxScale` (0 = off, default) and `ParallaxFadeDistance`, menu-editable, independent of POM.
- No relief shadows on terrain; LOD land (SLS2001/2068) untouched.

---

## File Structure

| File | Responsibility |
|---|---|
| `TESReloaded/Core/SettingManager.h` (modify) | Two new `SettingsTerrainStruct` fields |
| `TESReloaded/Core/SettingManager.cpp` (modify) | Load / save / menu list / menu set for the two fields |
| `TESReloaded/Core/ShaderManager.h` (modify) | `TerrainStruct::ParallaxData` constant |
| `TESReloaded/Core/ShaderManager.cpp` (modify) | Name → constant mapping; packing in `UpdateTerrain` |
| `OblivionReloaded/Shaders/Terrain/Terrain.ini` (modify) | Shipped defaults |
| `OblivionReloaded/Shaders/Terrain/Includes/Parallax.hlsl` (create) | `TESR_TerrainParallaxData` + `TerrainParallaxUV()` |
| `OblivionReloaded/Shaders/Terrain/SLS2042.vso.hlsl`, `SLS2043.vso.hlsl` (modify) | Emit `ParallaxView : TEXCOORD1` |
| `OblivionReloaded/Shaders/Terrain/SLS2048.pso.hlsl`, `SLS2049.pso.hlsl` (modify) | Sample base/normal at the displaced UV |

---

### Task 1: Terrain parallax settings and shader constant

**Goal:** `SettingsTerrain.ParallaxScale` / `ParallaxFadeDistance` load, save and show in the menu, and are packed every frame into `ShaderConst.Terrain.ParallaxData`, bound to the name `TESR_TerrainParallaxData`.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h:444-449` (`SettingsTerrainStruct`)
- Modify: `TESReloaded/Core/SettingManager.cpp:627` (load, after `MiddleSpecular`), `:1673` (save), `:2333` (menu list), `:3152` (menu set)
- Modify: `TESReloaded/Core/ShaderManager.h:110-112` (`TerrainStruct`)
- Modify: `TESReloaded/Core/ShaderManager.cpp:470-471` (`SetConstantTableValue2`), `:2199-2204` (`UpdateTerrain`)
- Modify: `OblivionReloaded/Shaders/Terrain/Terrain.ini`

**Acceptance Criteria:**
- [x] `grep -c "ParallaxScale\|ParallaxFadeDistance" TESReloaded/Core/SettingManager.cpp` prints `12` (load 4 lines, save 2, list 2, set 4)
- [x] `grep -c "TESR_TerrainParallaxData" TESReloaded/Core/ShaderManager.cpp` prints `1`, and it is inside `SetConstantTableValue2`
- [x] Both new struct fields have `///<` doc comments; `UpdateTerrain` has a `///` doc comment
- [x] `Terrain.ini` `[Default]` contains `ParallaxScale = 0.0` and `ParallaxFadeDistance = 8000.0`
- [x] Release build succeeds with 0 errors

**Verify:** the two greps above → `12` and `1`; MSBuild command (Global Constraints) → `Build succeeded`-style output with 0 errors.

**Steps:**

- [x] **Step 1: Add the settings fields** — in `SettingManager.h`, `SettingsTerrainStruct` becomes:

```cpp
struct SettingsTerrainStruct {
	float DistantSpecular;
	float DistantNoise;
	float NearSpecular;
	float MiddleSpecular;
	float ParallaxScale;			///< Near-land parallax UV offset per unit of height (diffuse alpha); 0 disables it.
	float ParallaxFadeDistance;		///< Eye distance at which near-land parallax has faded to zero; <= 0 never fades.
};
```

- [x] **Step 2: Load them** — in `SettingManager.cpp`, directly after the `SettingsTerrain.MiddleSpecular = atof(value);` line (~627):

```cpp
	GetPrivateProfileStringA("Default", "ParallaxScale", "0.0", value, SettingStringBuffer, Filename);
	SettingsTerrain.ParallaxScale = atof(value);
	GetPrivateProfileStringA("Default", "ParallaxFadeDistance", "8000.0", value, SettingStringBuffer, Filename);
	SettingsTerrain.ParallaxFadeDistance = atof(value);
```

- [x] **Step 3: Save them** — in the `"Terrain"` save branch (~1673), after the `NearSpecular` write:

```cpp
			WritePrivateProfileStringA("Default", "ParallaxFadeDistance", ToString(SettingsTerrain.ParallaxFadeDistance).c_str(), Filename);
			WritePrivateProfileStringA("Default", "ParallaxScale", ToString(SettingsTerrain.ParallaxScale).c_str(), Filename);
```

- [x] **Step 4: Menu list** — in the `"Terrain"` branch that fills `Settings[...]` (~2333), after `Settings["NearSpecular"] = ...;`:

```cpp
			Settings["ParallaxFadeDistance"] = SettingsTerrain.ParallaxFadeDistance;
			Settings["ParallaxScale"] = SettingsTerrain.ParallaxScale;
```

- [x] **Step 5: Menu set** — in the `"Terrain"` branch that assigns from `Setting` (~3152), after the `NearSpecular` pair:

```cpp
			else if (!strcmp(Setting, "ParallaxFadeDistance"))
				SettingsTerrain.ParallaxFadeDistance = Value;
			else if (!strcmp(Setting, "ParallaxScale"))
				SettingsTerrain.ParallaxScale = Value;
```

- [x] **Step 6: Constant storage** — in `ShaderManager.h`, `TerrainStruct` becomes:

```cpp
	struct TerrainStruct {
		D3DXVECTOR4		Data;
		D3DXVECTOR4		ParallaxData;	// x = scale, y = -0.5 * scale, z = fade slope, w = fade bias
	};
```

- [x] **Step 7: Name mapping** — in `ShaderManager.cpp` `SetConstantTableValue2`, insert before the final `else {` (after the `TESR_PrevWorldViewProjectionTransform` branch):

```cpp
	else if (!strcmp(Name, "TESR_TerrainParallaxData"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.Terrain.ParallaxData;
```

- [x] **Step 8: Packing** — replace `ShaderManager::UpdateTerrain` with:

```cpp
/// Packs the terrain shader settings: specular/noise tuning, and the near-land parallax scale with
/// its centering bias and a distance fade (full strength to half the fade distance, zero at it).
void ShaderManager::UpdateTerrain(ShaderConstants& ShaderConst) {
	ShaderConst.Terrain.Data.x = TheSettingManager->SettingsTerrain.DistantSpecular;
	ShaderConst.Terrain.Data.y = TheSettingManager->SettingsTerrain.DistantNoise;
	ShaderConst.Terrain.Data.z = TheSettingManager->SettingsTerrain.NearSpecular;
	ShaderConst.Terrain.Data.w = TheSettingManager->SettingsTerrain.MiddleSpecular;

	float Scale = TheSettingManager->SettingsTerrain.ParallaxScale;
	float FadeDistance = TheSettingManager->SettingsTerrain.ParallaxFadeDistance;
	ShaderConst.Terrain.ParallaxData.x = Scale;
	ShaderConst.Terrain.ParallaxData.y = -0.5f * Scale;
	ShaderConst.Terrain.ParallaxData.z = FadeDistance > 0.0f ? -2.0f / FadeDistance : 0.0f;
	ShaderConst.Terrain.ParallaxData.w = FadeDistance > 0.0f ? 2.0f : 1.0f;
}
```

- [x] **Step 9: Shipped INI** — `OblivionReloaded/Shaders/Terrain/Terrain.ini` becomes:

```ini
[Default]
DistantSpecular = 0.0
DistantNoise    = 0.3
NearSpecular    = 0.0
MiddleSpecular  = 0.0
ParallaxScale   = 0.0
ParallaxFadeDistance = 8000.0
```

- [x] **Step 10: Verify** — run the two greps from Acceptance Criteria (expect `12` and `1`), then the MSBuild command from Global Constraints through the PowerShell tool. Expect 0 errors.

- [x] **Step 11: Commit**

```bash
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp TESReloaded/Core/ShaderManager.h TESReloaded/Core/ShaderManager.cpp OblivionReloaded/Shaders/Terrain/Terrain.ini
git commit -m "feat(Terrain): Add near-land parallax settings and shader constant"
```

```json:metadata
{"files": ["TESReloaded/Core/SettingManager.h", "TESReloaded/Core/SettingManager.cpp", "TESReloaded/Core/ShaderManager.h", "TESReloaded/Core/ShaderManager.cpp", "OblivionReloaded/Shaders/Terrain/Terrain.ini"], "verifyCommand": "grep -c \"ParallaxScale\\|ParallaxFadeDistance\" TESReloaded/Core/SettingManager.cpp (expect 12); grep -c TESR_TerrainParallaxData TESReloaded/Core/ShaderManager.cpp (expect 1); MSBuild Release x86 via PowerShell (0 errors)", "acceptanceCriteria": ["SettingManager.cpp grep count is 12", "TESR_TerrainParallaxData mapped once, in SetConstantTableValue2", "new fields and UpdateTerrain documented", "Terrain.ini ships ParallaxScale = 0.0 and ParallaxFadeDistance = 8000.0", "Release build has 0 errors"], "modelTier": "mechanical"}
```

---

### Task 2: Parallax in the near-land shaders

**Goal:** SLS2042/2043 emit `ParallaxView : TEXCOORD1` and SLS2048/2049 sample their base and normal maps at the UV returned by `TerrainParallaxUV()`, all four compiling cleanly with fxc.

**Files:**
- Create: `OblivionReloaded/Shaders/Terrain/Includes/Parallax.hlsl`
- Modify: `OblivionReloaded/Shaders/Terrain/SLS2042.vso.hlsl`
- Modify: `OblivionReloaded/Shaders/Terrain/SLS2043.vso.hlsl`
- Modify: `OblivionReloaded/Shaders/Terrain/SLS2048.pso.hlsl`
- Modify: `OblivionReloaded/Shaders/Terrain/SLS2049.pso.hlsl`

**Acceptance Criteria:**
- [x] fxc compiles all four files (`vs_3_0` for the VSOs, `ps_3_0` for the PSOs) with exit code 0 and no warning that the HEAD version of the same file doesn't also produce
- [x] No `tex2D(BaseMap, IN.BaseUV` or `tex2D(NormalMap, IN.BaseUV` remains in SLS2048/2049 (`grep -c` → `0` for each file)
- [x] The PSO disassembly (`/Fc`) lists `TESR_TerrainParallaxData` at `c7`, and the VSO disassembly writes `o` register semantics `texcoord1`
- [x] Every existing VSO output line is unchanged (diff shows only additions in the VSOs)

**Verify:** the PowerShell fxc loop in Step 6 → `OK` for all four files; the `grep -c` checks → `0`.

**Steps:**

- [x] **Step 1: Create `OblivionReloaded/Shaders/Terrain/Includes/Parallax.hlsl`**

```hlsl
// Single-tap parallax for the near-land layer passes (SLS2048 base layer, SLS2049 blended layers).
//
// Each pass reads the height from its own diffuse map's alpha at the un-displaced UV and shifts the
// UV along the tangent-space view vector by (height - 0.5) * scale, like the stock PAR shaders,
// faded out with eye distance. Layer blend weights come from vertex colors and are not affected.

float4 TESR_TerrainParallaxData : register(c7);    // x = scale, y = -0.5 * scale, z = fade slope, w = fade bias

// HeightMap    : sampler whose alpha holds the height (the layer's base map)
// BaseUV       : un-displaced texture coordinate
// ParallaxView : xyz = tangent-space surface->eye vector (need not be normalized), w = eye distance
// Returns the displaced texture coordinate.
float2 TerrainParallaxUV(sampler2D HeightMap, float2 BaseUV, float4 ParallaxView) {
    float height = tex2D(HeightMap, BaseUV).a;
    float fade = saturate(ParallaxView.w * TESR_TerrainParallaxData.z + TESR_TerrainParallaxData.w);
    return BaseUV + (height * TESR_TerrainParallaxData.x + TESR_TerrainParallaxData.y) * fade * normalize(ParallaxView.xyz).xy;
}
```

- [x] **Step 2: SLS2042.vso.hlsl** — add the output to `VS_OUTPUT`, directly after `float2 texcoord_0 : TEXCOORD0;`:

```hlsl
    float4 ParallaxView : TEXCOORD1;
```

and in `main`, directly after `OUT.texcoord_0.xy = IN.texcoord_0.xy;`:

```hlsl
    float3 eyeVec = EyePosition.xyz - IN.position.xyz;
    OUT.ParallaxView.xyz = mul(TanSpaceProj, eyeVec);
    OUT.ParallaxView.w = length(eyeVec);
```

- [x] **Step 3: SLS2043.vso.hlsl** — the identical two additions: `float4 ParallaxView : TEXCOORD1;` after `float2 texcoord_0 : TEXCOORD0;` in `VS_OUTPUT`, and after `OUT.texcoord_0.xy = IN.texcoord_0.xy;` in `main`:

```hlsl
    float3 eyeVec = EyePosition.xyz - IN.position.xyz;
    OUT.ParallaxView.xyz = mul(TanSpaceProj, eyeVec);
    OUT.ParallaxView.w = length(eyeVec);
```

- [x] **Step 4: SLS2048.pso.hlsl**
  - In `VS_OUTPUT`, replace `float2 NormalUV : TEXCOORD1;` with `float4 ParallaxView : TEXCOORD1;`.
  - Directly before `PS_OUTPUT main(VS_OUTPUT IN) {`, add `#include "Includes/Parallax.hlsl"` followed by a blank line.
  - As the first statement after the `#define` block and local declarations in `main` (i.e. directly before `r1.xyzw = tex2D(NormalMap, IN.BaseUV.xy);`), add:

```hlsl
    float2 uv = TerrainParallaxUV(BaseMap, IN.BaseUV.xy, IN.ParallaxView);
```

  - Change the three samples to use it:

```hlsl
    r1.xyzw = tex2D(NormalMap, uv);
```
```hlsl
    r0.xyz = tex2D(NormalMap, uv).xyz;
    r3.xyz = tex2D(BaseMap, uv).xyz;
```

- [x] **Step 5: SLS2049.pso.hlsl**
  - In `VS_OUTPUT`, replace `//float2 NormalUV : TEXCOORD1;` with `float4 ParallaxView : TEXCOORD1;`.
  - Directly before `PS_OUTPUT main(VS_OUTPUT IN) {`, add `#include "Includes/Parallax.hlsl"` followed by a blank line.
  - Directly before `r1.xyzw = tex2D(NormalMap, IN.BaseUV.xy);`, add:

```hlsl
    float2 uv = TerrainParallaxUV(BaseMap, IN.BaseUV.xy, IN.ParallaxView);
```

  - Change the three samples exactly as in Step 4 (`r1.xyzw = tex2D(NormalMap, uv);`, `r0.xyz = tex2D(NormalMap, uv).xyz;`, `r3.xyz = tex2D(BaseMap, uv).xyz;`). Leave the `OUT.color_0.a` layer-weight line untouched.

- [x] **Step 6: Compile new and HEAD versions with fxc** (PowerShell tool):

```powershell
$fxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe'
$dir = 'OblivionReloaded\Shaders\Terrain'
$out = "$env:TEMP\terrain-parallax-fxc"; New-Item -ItemType Directory -Force $out | Out-Null
foreach ($f in 'SLS2042.vso','SLS2043.vso','SLS2048.pso','SLS2049.pso') {
    $t = if ($f -like '*.vso') { 'vs_3_0' } else { 'ps_3_0' }
    $txt = (git show "HEAD:$dir/$f.hlsl") -join "`r`n"
    [System.IO.File]::WriteAllText("$out\head_$f.hlsl", $txt, (New-Object System.Text.UTF8Encoding($false)))
    $headWarn = & $fxc /nologo /T $t /E main /I $dir /Fo "$out\head_$f" "$out\head_$f.hlsl" 2>&1 | Select-String 'warning' | ForEach-Object { ($_ -replace '^.*?: ','') }
    $newOut = & $fxc /nologo /T $t /E main /I $dir /Fo "$out\$f" /Fc "$out\$f.asm" "$dir\$f.hlsl" 2>&1
    $ok = $LASTEXITCODE -eq 0
    $newWarn = $newOut | Select-String 'warning' | ForEach-Object { ($_ -replace '^.*?: ','') }
    $extra = $newWarn | Where-Object { $headWarn -notcontains $_ }
    "$f : " + $(if ($ok -and -not $extra) { 'OK' } else { "FAIL`n$($newOut -join "`n")" })
}
Select-String -Path "$out\SLS2048.pso.asm","$out\SLS2049.pso.asm" -Pattern 'TESR_TerrainParallaxData'
Select-String -Path "$out\SLS2042.vso.asm","$out\SLS2043.vso.asm" -Pattern 'texcoord1'
```

Expected: `OK` for all four; the PSO listings show `TESR_TerrainParallaxData   c7   1`; each VSO listing has a `dcl_texcoord1 o…` line. (If the HEAD copy fails to compile because `/I` resolution differs for a temp path, copy it into `$dir` under a temporary name instead, and delete it afterwards.)

- [x] **Step 7: Check no un-displaced samples remain**

```bash
grep -c "tex2D(BaseMap, IN.BaseUV\|tex2D(NormalMap, IN.BaseUV" OblivionReloaded/Shaders/Terrain/SLS2048.pso.hlsl OblivionReloaded/Shaders/Terrain/SLS2049.pso.hlsl
```

Expected: `...SLS2048.pso.hlsl:0` and `...SLS2049.pso.hlsl:0`.

- [x] **Step 8: Commit** (source only; the compiled `.vso`/`.pso` are regenerated in-game in Task 3)

```bash
git add OblivionReloaded/Shaders/Terrain/Includes/Parallax.hlsl OblivionReloaded/Shaders/Terrain/SLS2042.vso.hlsl OblivionReloaded/Shaders/Terrain/SLS2043.vso.hlsl OblivionReloaded/Shaders/Terrain/SLS2048.pso.hlsl OblivionReloaded/Shaders/Terrain/SLS2049.pso.hlsl
git commit -m "feat(Terrain): Single-tap parallax on the near-land layer passes"
```

```json:metadata
{"files": ["OblivionReloaded/Shaders/Terrain/Includes/Parallax.hlsl", "OblivionReloaded/Shaders/Terrain/SLS2042.vso.hlsl", "OblivionReloaded/Shaders/Terrain/SLS2043.vso.hlsl", "OblivionReloaded/Shaders/Terrain/SLS2048.pso.hlsl", "OblivionReloaded/Shaders/Terrain/SLS2049.pso.hlsl"], "verifyCommand": "PowerShell fxc loop from plan Task 2 Step 6 prints OK for SLS2042.vso, SLS2043.vso, SLS2048.pso, SLS2049.pso; grep -c for un-displaced samples prints 0 per PSO", "acceptanceCriteria": ["fxc compiles all four with no new warnings vs HEAD", "no tex2D(BaseMap|NormalMap, IN.BaseUV remains in SLS2048/2049", "TESR_TerrainParallaxData at c7 in both PSO listings; VSOs output texcoord1", "VSO diffs are additions only"], "modelTier": "mechanical"}
```

---

### Task 3: In-game verification and compiled shaders

**Goal:** The game recompiles the four land shaders, terrain is unchanged at `ParallaxScale = 0`, shows correctly oriented relief at ~0.04 with no fade seam, and the regenerated `.vso`/`.pso` binaries are committed.

This task needs the user to run the game; the coordinator drives it in conversation (not a subagent).

**Files:**
- Modify (regenerated by the game): `OblivionReloaded/Shaders/Terrain/SLS2042.vso`, `SLS2043.vso`, `SLS2048.pso`, `SLS2049.pso`
- Possibly modify: `OblivionReloaded/Shaders/Terrain/SLS2042.vso.hlsl`, `SLS2043.vso.hlsl` (tangent sign fix only)
- Possibly modify: `OblivionReloaded/Shaders/Terrain/Terrain.ini` (tuned `ParallaxScale` if the user wants it on by default)

**Acceptance Criteria:**
- [x] `OblivionReloaded.log` shows no shader compile error for SLS2042/2043/2048/2049
- [x] With `ParallaxScale = 0`, the user reports terrain looks identical to before
- [x] With `ParallaxScale ≈ 0.04`, the user reports visible relief on cobblestone / rocky layers with bumps raised consistently along both texture axes (after a sign fix if needed)
- [x] The user reports no visible seam at the near-land / LOD boundary and no distant shimmer
- [x] A `Develop.LogShaders` capture in an exterior shows SLS2042 paired only with SLS2048 and SLS2043 only with SLS2049 (or any other pairing's PSO is confirmed not to read TEXCOORD1)
- [x] `git status` shows the four regenerated binaries, committed

**Verify:** user confirmation of each visual criterion in conversation; `git log -1 --stat` lists the four `.vso`/`.pso` binaries.

**Steps:**

- [x] **Step 1: Build** — MSBuild command from Global Constraints (game closed). Expect 0 errors.
- [x] **Step 2: Ask the user** to set `[Develop] CompileShaders = 1` in `C:\Games\Steam\steamapps\common\Oblivion\Data\OBSE\Plugins\OblivionReloaded.ini`, confirm `[Shaders] Terrain = 1`, launch, and load an exterior save with cobblestone and rocky ground (e.g. outside a city gate).
- [x] **Step 3: Check the log** — `Select-String -Path 'C:\Games\Steam\steamapps\common\Oblivion\OblivionReloaded.log' -Pattern 'SLS204[2389]'` shows each loaded without an error line.
- [x] **Step 4: Inert at 0** — user compares against memory/screenshot of the previous build: no change.
- [x] **Step 5: Effect on** — user sets Terrain `ParallaxScale` to `0.04` from the in-game menu. Ask: do bumps look raised, and does the relief stay consistent when strafing left/right and walking forward/back? If one direction looks inverted (relief slides the wrong way along one axis), negate that component in **both** SLS2042.vso.hlsl and SLS2043.vso.hlsl directly after the `OUT.ParallaxView.w` line:

```hlsl
    OUT.ParallaxView.y = -OUT.ParallaxView.y;
```

  (use `.x` instead if the horizontal texture axis is the inverted one; use both if everything looks sunken). Re-run with CompileShaders = 1 and re-check.
- [x] **Step 6: Fade** — user looks toward distant land at the near/LOD boundary and pans: no ring or seam, no shimmer. If the fade is visible, try `ParallaxFadeDistance` values from the menu and record the chosen value.
- [x] **Step 7: Pairing** — user presses the `Develop.LogShaders` key in the exterior; search the log for `SLS2042` / `SLS2043` pass lines and confirm their PSOs are only SLS2048 / SLS2049. If another PSO appears (e.g. SLS2046), extract it (see memory `par-shader-interpolator-layout`) and confirm it does not declare `t1`.
- [x] **Step 8: Restore and commit** — user sets `CompileShaders = 0`; if the user wants parallax on by default, update `ParallaxScale` in `Terrain.ini` to the chosen value. Commit:

```bash
git add OblivionReloaded/Shaders/Terrain/SLS2042.vso OblivionReloaded/Shaders/Terrain/SLS2043.vso OblivionReloaded/Shaders/Terrain/SLS2048.pso OblivionReloaded/Shaders/Terrain/SLS2049.pso
git commit -m "feat(Terrain): Recompile near-land shaders with parallax"
```

  Any sign fix or INI change goes in its own `fix(Terrain): …` / `feat(Terrain): …` commit with its `.hlsl` and regenerated binaries.

```json:metadata
{"files": ["OblivionReloaded/Shaders/Terrain/SLS2042.vso", "OblivionReloaded/Shaders/Terrain/SLS2043.vso", "OblivionReloaded/Shaders/Terrain/SLS2048.pso", "OblivionReloaded/Shaders/Terrain/SLS2049.pso"], "verifyCommand": "User confirms visuals in conversation; Select-String on OblivionReloaded.log for SLS204[2389] shows no compile errors; git log -1 --stat lists the four binaries", "acceptanceCriteria": ["no compile errors for the four shaders in the log", "identical terrain at ParallaxScale 0", "correctly oriented relief at ~0.04 on both axes", "no fade seam or shimmer", "LogShaders pairing confirms no stock PSO reads TEXCOORD1", "regenerated binaries committed"], "modelTier": "standard"}
```

---

### Task 4: Memory note and plan bookkeeping

**Goal:** A memory file records the near-land pass structure and the measured tangent-sign / pairing results, indexed in `MEMORY.md`, and this plan's checkboxes are ticked — in one `docs:` commit.

**Files:**
- Create: `memory/terrain-land-passes.md`
- Modify: `memory/MEMORY.md`
- Modify: `docs/superpowers/plans/2026-09-26-terrain-parallax.md`

**Acceptance Criteria:**
- [x] `memory/terrain-land-passes.md` has the standard frontmatter (`name: terrain-land-passes`, `description`, `metadata.type: project`) and states: SLS2042→2048 base layer opaque, SLS2043→2049 per-layer alpha-blended by vertex weights; TEXCOORD1 carries `ParallaxView`; the Task 3 tangent-sign result (no flip / which axis flipped); the Task 3 pairing result; links `[[par-shader-interpolator-layout]]` and `[[shader-deployment-workflow]]`
- [x] `MEMORY.md` has one new line pointing to it
- [x] The commit touches only `memory/` and `docs/` files, with a `docs:` subject

**Verify:** `git show --stat HEAD` lists only `memory/terrain-land-passes.md`, `memory/MEMORY.md`, `docs/superpowers/plans/2026-09-26-terrain-parallax.md`.

**Steps:**

- [x] **Step 1: Write `memory/terrain-land-passes.md`** using the facts measured in Task 3 (fill the two bracketed results from Task 3's outcome before writing — they are measurements, not placeholders to leave in):

```markdown
---
name: terrain-land-passes
description: Near land is multipass — SLS2042→2048 opaque base layer, SLS2043→2049 one alpha-blended pass per extra layer (vertex-color weights); TEXCOORD1 = ParallaxView; measured land tangent sign
metadata:
  type: project
---

Near land draws the base texture layer with VS SLS2042 + PS SLS2048 (opaque), then each further
layer with VS SLS2043 + PS SLS2049, alpha-blended: the layer weight is
`dot(PSLightColor[1], COLOR0) + dot(PSLightColor[2], COLOR1)` (per-vertex weights, one-hot
selector constants). Each pass binds its own diffuse (s0) and normal map (s1), so per-pass
effects such as parallax see only that layer. LOD land is SLS2001/2068 (and SLS2064 VS).

Stock SLS2042/2043 are vs_2_0 and output oT1 = NormalUV; our overrides repurpose TEXCOORD1 as
`ParallaxView` (tangent-space eye vector, eye distance) for terrain parallax
(`Terrain/Includes/Parallax.hlsl`, constant `TESR_TerrainParallaxData` c7).
Measured pairing: <Task 3 Step 7 result>. Land tangent sign vs PAR convention: <Task 3 Step 5 result>.

Landscape diffuse alpha is a heightmap in the installed replacer (116/118 DXT5 maps full-range).

**How to apply:** per-layer effects go in both 2048 and 2049; anything that needs all layers at
once (height blending) would need single-pass land. See [[par-shader-interpolator-layout]],
[[shader-deployment-workflow]].
```

- [x] **Step 2: Index it** — append to `memory/MEMORY.md`:

```markdown
- [Near-land pass structure](terrain-land-passes.md) — SLS2042→2048 opaque base, SLS2043→2049 per-layer blend by vertex weights; TEXCOORD1 = terrain ParallaxView; measured tangent sign
```

- [x] **Step 3: Tick this plan's checkboxes** for Tasks 1–4.

- [x] **Step 4: Commit**

```bash
git add memory/terrain-land-passes.md memory/MEMORY.md docs/superpowers/plans/2026-09-26-terrain-parallax.md
git commit -m "docs: Record near-land pass structure and terrain parallax results"
```

```json:metadata
{"files": ["memory/terrain-land-passes.md", "memory/MEMORY.md", "docs/superpowers/plans/2026-09-26-terrain-parallax.md"], "verifyCommand": "git show --stat HEAD lists only memory/terrain-land-passes.md, memory/MEMORY.md, docs/superpowers/plans/2026-09-26-terrain-parallax.md", "acceptanceCriteria": ["memory file with frontmatter, pass structure, measured sign and pairing results, links", "MEMORY.md index line added", "docs-only commit"], "modelTier": "mechanical"}
```

---

## Execution notes

- Task 2: SLS2043 hit vs_3_0 X5622 (12 outputs); ruling: drop its dead TEXCOORD6/7 outputs and the
  matching SLS2049 input declarations.
- Task 3: relief slid along one axis; a Y flip moved the error to the other axis. Diagnostics showed
  the land TBN is exact and `EyePosition` (c25) is stale for land draws. Fixed by deriving the eye
  from `ModelViewProj` (bec0337); no sign flip. Compiled `.vso`/`.pso` are gitignored, so the
  "commit regenerated binaries" steps did not apply. User chose `ParallaxScale = 0.01`, default on
  (2eb09cf).
