# Sun Shadow Stealth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When the player stands in exterior sun/moon shadow, scale the sun's share of their light level (the value sneak detection and five other engine consumers read) by how much the shadow visibly darkens them.

**Architecture:** A `WriteRelCall` at 0x6561FD wraps the one call that adds the unoccluded sun term inside `HighProcess_GetLightLevel`. For the player, the result is multiplied by a main-thread-published `PlayerSunLightScale`. The scale comes from a GPU probe: an 8×1 effect pass samples the existing sun shadow maps at 8 points on the player's body, right after the mid-scene sun-shadow apply. It is read back asynchronously through a 3-slot ring of render targets and event queries, so the CPU never waits on the GPU.

**Tech Stack:** C++ (MSVC v145, x86, inline asm), Direct3D 9 + D3DX effects (fx_2_0 / ps_3_0), OBSE plugin memory patching (`WriteRelCall`).

**Spec:** `docs/superpowers/specs/2026-09-27-sun-shadow-stealth-design.md` (read it first; its "Findings" section has the RE'd engine facts). Engine facts are also in memory `actor-light-level-formula`.

## Global Constraints

- Build ONLY through the PowerShell tool (never Bash; Bash's TEMP breaks MSBuild with a fake MSB3073), from the solution:
  `& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal`
  Success = exit code 0 and no `error` lines. If the post-build copy fails because the game has the DLL locked, the compile itself still succeeded; report that and don't "fix" it.
- fxc for shader checks (PowerShell): `$fxc = 'C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe'`. Effects compile with `/T fx_2_0`; pass `/I OblivionReloaded\Shaders\Shadows` when the file includes the shared lookup. Write temp copies with `New-Object System.Text.UTF8Encoding($false)` (a BOM makes fxc fail).
- The existing `ShadowsExteriors` apply shader's compiled output must stay **byte-identical** to HEAD (Task 1 proves it).
- No `for each` (MSVC-only syntax that clangd can't parse). Match the surrounding code style: tabs, `Type	Name` column alignment in declarations, `///` doc comments on public functions, inline comments only when necessary and at most 1–3 lines.
- Commits: `feat(Shadows): …` / `refactor(Shadows): …` for code; memory/spec/plan changes go in separate `docs:` commits. End every commit message with the attribution lines from the session's system reminder, if one is present.
- The `EffectRecord` sampler binding is by ORDINAL: a `.fx.hlsl` must declare its `TESR_` samplers in its own top-level file (not an include) with registers contiguous from `s0`. `TextureManager::LoadTexture` parses the top-level source only.
- The exe is large-address-aware: D3DX effect parameters must be looked up with `GetParameterByName` and used via the returned handle. NEVER pass a name string as a `D3DXHANDLE` (memory `d3dx-laa-handle-trap`).
- `HighProcess_GetLightLevel` may run on the threaded-AI thread. The hook reads only `Player` and a published `volatile float`. It never touches the scene graph, settings or D3D.
- Compiled shaders (`*.fx`, `*.pso`, `*.vso`) are gitignored; never commit them.

**User decisions (already made):**
- Scope: sun/moon shadow only, in exteriors. Interiors and point-light terms stay vanilla.
- Gating: always on for the player, sneaking or not. The hook wraps the sun term at its source, so all six light-level consumers agree.
- Strength follows the visual shadow Darkness (weather tier, fog-blended `ShaderConst.Shadow.Data.y`), not a separate INI strength.
- Shadow source: a GPU probe of the mod's own sun shadow maps (not Havok rays, not CPU readback of whole maps).
- Switch: `[Exteriors] SunShadowStealth` in `Shadows.ini` (code default 0, shipped 1), with an in-game menu entry; Tree Cover stays independent and stacks.

**Refinements to the spec, decided while planning (Task 6 writes them back into the spec):**
1. Readback uses a ring of 3 `D3DPOOL_DEFAULT` 8×1 R32F render targets, each with a `D3DQUERYTYPE_EVENT` query. `GetRenderTargetData` runs only on a slot whose query reports done. The spec's plan (a sysmem ring read with `D3DLOCK_DONOTWAIT`) would not avoid the stall, because `GetRenderTargetData` synchronizes on its source.
2. This fork has no device-reset path (no surfaces are recreated anywhere), so probe resources are created lazily and released only when the feature is switched off.
3. Scale formula: `scale = lerp(1, D, shadowed)` at every hour, the spec's formula. A planned dawn/dusk fade through `ShadowLightDir.w` was removed at the user's direction (commit 9652283): the moon casts shadows too, so shade must hide the player at night and through the handover.
4. Probe points are pushed along the sun direction until the sun ray leaves a body capsule (radius 25, height 120 standing / 80 sneaking, × `Player->scale`) plus 10 units. A fixed 35 units would leave the lower points inside the body under a high sun.
5. The probe is skipped (scale 1.0) when `ShadowLightDir.z <= 0`: a light below the horizon can't light the player.
6. The diagnostic log reports the raw and scaled player sun term, not the point-light sum.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `OblivionReloaded/Shaders/Shadows/ShadowsExteriorsLookup.hlsl` | Create | Shared sun-cascade lookup (near/far/skin PCF, coverage, `StaticTerm`). Declares nothing; the includer provides the globals. Not `.fx.hlsl`, so `CompileShaders` never compiles it standalone. |
| `OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl` | Modify | Apply shader; now `#include`s the lookup. Output byte-identical. |
| `OblivionReloaded/Shaders/Shadows/ShadowProbe.fx.hlsl` | Create | 8×1 probe effect: one raw sun visibility per player probe point. |
| `TESReloaded/Core/SunShadowStealth.h/.cpp` | Create | Engine hook, published scale, probe lifecycle/render/readback, diagnostics. |
| `TESReloaded/Core/SettingManager.h/.cpp` | Modify | `Exteriors.SunShadowStealth`, `Develop.LogSunShadowStealth`. |
| `OblivionReloaded/Shaders/Shadows/Shadows.ini` | Modify | Shipped `SunShadowStealth = 1`. |
| `TESReloaded/Core/ShaderManager.cpp` | Modify | Call `UpdateSunShadowStealth()` per update and `RenderSunShadowProbe()` after the sun apply. |
| `OblivionReloaded/Main.cpp` | Modify | Install the hook. |
| `OblivionReloaded/OblivionReloaded.vcxproj(.filters)` | Modify | Add the new source files. |

---

### Task 1: Extract the shared sun-cascade lookup include

**Goal:** Move the cascade lookup functions from `ShadowsExteriors.fx.hlsl` into `ShadowsExteriorsLookup.hlsl` with byte-identical compiled output.

**Files:**
- Create: `OblivionReloaded/Shaders/Shadows/ShadowsExteriorsLookup.hlsl`
- Modify: `OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl:124-256`

**Acceptance Criteria:**
- [ ] `ShadowsExteriorsLookup.hlsl` holds `LookupFar`, `GetLightAmountFar`, `CascadeCoverage`, `Lookup`, `GetLightAmountSkin`, `GetLightAmount`, `StaticTerm` with their existing comments, verbatim.
- [ ] `ShadowsExteriors.fx.hlsl` keeps `AddProximityLight` and gains `#include "ShadowsExteriorsLookup.hlsl"` where the moved block was.
- [ ] fxc output of the new file has the same SHA256 as fxc output of the HEAD version (prints `True`).

**Verify:** the PowerShell hash comparison in Step 3 prints `True`.

**Steps:**

- [ ] **Step 1: Create the include.** Create `OblivionReloaded/Shaders/Shadows/ShadowsExteriorsLookup.hlsl`. It starts with this header, followed by lines 124–175 of the current `ShadowsExteriors.fx.hlsl` (from the `// Explicit LOD 0 rather than tex2D…` comment through the end of `Lookup`), then lines 186–256 (from the `// Actor overlay: …` comment through the end of `StaticTerm`), copied **verbatim**:

```hlsl
// Sun shadow cascade lookup shared by ShadowsExteriors.fx.hlsl and ShadowProbe.fx.hlsl.
// Declares nothing: the including effect must declare TESR_ShadowData, TESR_ShadowLightDir,
// TESR_ShadowMapBufferSkin and a `darkness` float before including it. Samplers must stay in the
// including file, because TextureManager::LoadTexture parses only the top-level source.

```

Lines 176–183 (`AddProximityLight`) stay in `ShadowsExteriors.fx.hlsl`.

- [ ] **Step 2: Replace the block in the apply shader.** In `ShadowsExteriors.fx.hlsl`, delete lines 124–175 and 184–256. At line 123 (the blank line after `getRawNormal`), the file must now read:

```hlsl
float AddProximityLight(float4 WorldPos, float4 ExternalLightPos) {

	if (ExternalLightPos.w) {
		float distToExternalLight = distance(WorldPos.xyz, ExternalLightPos.xyz);
		return (saturate(1.000f - (distToExternalLight / (ExternalLightPos.w))));
	}
	return 0.0f;
}

#include "ShadowsExteriorsLookup.hlsl"

float4 Shadow(VSOUT IN) : COLOR0{
```

- [ ] **Step 3: Prove the compiled output is unchanged** (PowerShell, repo root):

```powershell
$fxc = 'C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe'
$out = Join-Path $env:TEMP 'ssprobe'; New-Item -ItemType Directory -Force $out | Out-Null
$txt = (git show HEAD:OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl) -join "`r`n"
[System.IO.File]::WriteAllText("$out\head.fx.hlsl", $txt, (New-Object System.Text.UTF8Encoding($false)))
& $fxc /nologo /T fx_2_0 /Fo "$out\head.fxo" "$out\head.fx.hlsl"
& $fxc /nologo /T fx_2_0 /I OblivionReloaded\Shaders\Shadows /Fo "$out\new.fxo" OblivionReloaded\Shaders\Shadows\ShadowsExteriors.fx.hlsl
(Get-FileHash "$out\head.fxo").Hash -eq (Get-FileHash "$out\new.fxo").Hash
```

Expected: both compiles succeed (the pre-existing `X3206` warnings are fine), and the last line prints `True`. If it prints `False`, dump both with `/Fc` and diff the assembly. Fix the move until it prints `True`; don't accept a difference.

- [ ] **Step 4: Commit**

```bash
git add OblivionReloaded/Shaders/Shadows/ShadowsExteriorsLookup.hlsl OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl
git commit -m "refactor(Shadows): Share the sun cascade lookup through an include"
```

```json:metadata
{"files": ["OblivionReloaded/Shaders/Shadows/ShadowsExteriorsLookup.hlsl", "OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl"], "verifyCommand": "PowerShell: fxc fx_2_0 HEAD vs new ShadowsExteriors, Get-FileHash equality prints True", "acceptanceCriteria": ["lookup functions moved verbatim into ShadowsExteriorsLookup.hlsl", "AddProximityLight stays; #include replaces the block", "fxc output SHA256 identical to HEAD"], "modelTier": "mechanical"}
```

---

### Task 2: Sun shadow probe effect

**Goal:** Add `ShadowProbe.fx.hlsl`, an effect that writes into an 8×1 target the raw sun visibility (0 = occluded, 1 = lit) at 8 world-space points.

**Files:**
- Create: `OblivionReloaded/Shaders/Shadows/ShadowProbe.fx.hlsl`

**Acceptance Criteria:**
- [ ] Compiles with fxc `/T fx_2_0` with no errors.
- [ ] Samplers are `TESR_ShadowMapBufferNear/Far/Skin/NearPrev/FarPrev` at `s0`–`s4`, declared in this file.
- [ ] The point array is named `SunShadowProbePoints` (no `TESR_` prefix), `float4[8]`.
- [ ] Pixel `i` evaluates `StaticTerm` (plus the crossfade) at `SunShadowProbePoints[i]` with `darkness = 0` and a lit value of 1.

**Verify:** `& $fxc /nologo /T fx_2_0 /I OblivionReloaded\Shaders\Shadows /Fo OblivionReloaded\Shaders\Shadows\ShadowProbe.fx OblivionReloaded\Shaders\Shadows\ShadowProbe.fx.hlsl` → exit code 0, "compilation succeeded".

**Steps:**

- [ ] **Step 1: Write the effect** at `OblivionReloaded/Shaders/Shadows/ShadowProbe.fx.hlsl`:

```hlsl
// Sun shadow probe for Oblivion Reloaded: samples the exterior sun shadow maps at the player probe
// points built by SunShadowStealth.cpp, writing one raw visibility (0 = occluded, 1 = lit) per pixel
// of an 8x1 target.

float4x4 TESR_WorldViewProjectionTransform;
float4x4 TESR_ShadowCameraToLightTransformNear;
float4x4 TESR_ShadowCameraToLightTransformFar;
float4x4 TESR_ShadowCameraToLightTransformSkin;
float4x4 TESR_ShadowCameraToLightTransformNearPrev;
float4x4 TESR_ShadowCameraToLightTransformFarPrev;
float4 TESR_ShadowData;
float4 TESR_ShadowBiasDeferred; // zw = near/far depth bias (normalized)
float4 TESR_ShadowFadeData;

// World-space probe points, uploaded by SunShadowStealth.cpp with SetVectorArray. No TESR_ prefix,
// so EffectRecord::CreateCT leaves it alone.
float4 SunShadowProbePoints[8];

// Bound by ordinal (see ShadowsExteriors.fx.hlsl), so the registers stay contiguous from s0.
sampler2D TESR_ShadowMapBufferNear : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFar : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferSkin : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferNearPrev : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_ShadowMapBufferFarPrev : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };

// The lookup returns darkness for an occluded tap and clamp(TESR_ShadowLightDir.w, darkness, 1) for
// a lit one; pinning those to 0 and 1 makes it return raw visibility.
static const float darkness = 0.0f;
static const float4 TESR_ShadowLightDir = float4(0.0f, 0.0f, 0.0f, 1.0f);

#include "ShadowsExteriorsLookup.hlsl"

struct VSOUT
{
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN
{
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN)
{
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float4 Probe(float2 vpos : VPOS) : COLOR0 {
	float4 probePoint = SunShadowProbePoints[0];
	[unroll] for (int i = 1; i < 8; i++) {
		if (vpos.x > i - 0.5f) probePoint = SunShadowProbePoints[i];
	}

	// Points have no surface normal: depth bias only, no normal offset or terminator ramp.
	float4 pos = mul(float4(probePoint.xyz, 1.0f), TESR_WorldViewProjectionTransform);
	float4 ShadowSkin = mul(pos, TESR_ShadowCameraToLightTransformSkin);
	float biasNear = TESR_ShadowBiasDeferred.z;
	float biasFar = TESR_ShadowBiasDeferred.w;

	float visibility = StaticTerm(TESR_ShadowMapBufferNear, TESR_ShadowMapBufferFar,
	                              TESR_ShadowCameraToLightTransformNear, TESR_ShadowCameraToLightTransformFar,
	                              pos, pos, ShadowSkin, biasNear, biasFar, 1.0f);
	if (TESR_ShadowFadeData.x < 1.0f) {
		float prevVisibility = StaticTerm(TESR_ShadowMapBufferNearPrev, TESR_ShadowMapBufferFarPrev,
		                                  TESR_ShadowCameraToLightTransformNearPrev, TESR_ShadowCameraToLightTransformFarPrev,
		                                  pos, pos, ShadowSkin, biasNear, biasFar, 1.0f);
		visibility = lerp(prevVisibility, visibility, TESR_ShadowFadeData.x);
	}
	return float4(saturate(visibility), 0.0f, 0.0f, 1.0f);
}

technique {

	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 Probe();
	}

}
```

- [ ] **Step 2: Compile it** (PowerShell, repo root). This also drops the `.fx` binary where the game loads it, so play-testing doesn't need `CompileShaders = 1`:

```powershell
$fxc = 'C:\Development\Microsoft\DirectX SDK (June 2010)\Utilities\bin\x86\fxc.exe'
& $fxc /nologo /T fx_2_0 /I OblivionReloaded\Shaders\Shadows /Fo OblivionReloaded\Shaders\Shadows\ShadowProbe.fx OblivionReloaded\Shaders\Shadows\ShadowProbe.fx.hlsl
```

Expected: `compilation succeeded`, exit 0. Confirm `git status` doesn't list `ShadowProbe.fx` (gitignored `*.fx`).

- [ ] **Step 3: Commit**

```bash
git add OblivionReloaded/Shaders/Shadows/ShadowProbe.fx.hlsl
git commit -m "feat(Shadows): Sun shadow probe effect"
```

```json:metadata
{"files": ["OblivionReloaded/Shaders/Shadows/ShadowProbe.fx.hlsl"], "verifyCommand": "PowerShell: fxc /T fx_2_0 /I OblivionReloaded\\Shaders\\Shadows ShadowProbe.fx.hlsl -> compilation succeeded", "acceptanceCriteria": ["compiles with fxc fx_2_0", "samplers Near/Far/Skin/NearPrev/FarPrev at s0-s4 in this file", "point array SunShadowProbePoints float4[8]", "pixel i = StaticTerm + crossfade at point i with darkness 0 / lit 1"], "modelTier": "mechanical"}
```

---

### Task 3: Settings — SunShadowStealth and LogSunShadowStealth

**Goal:** Add the `[Exteriors] SunShadowStealth` switch (load, save, menu, setter, shipped INI) and the `[Develop] LogSunShadowStealth` diagnostic flag.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h` (`DevelopStruct` ~line 283, `ExteriorsStruct` ~line 345)
- Modify: `TESReloaded/Core/SettingManager.cpp` (Develop load ~line 442, Exteriors load ~line 1251, save ~line 1619, menu map ~line 2278, setter ~line 3069)
- Modify: `OblivionReloaded/Shaders/Shadows/Shadows.ini`

**Acceptance Criteria:**
- [ ] `SettingsShadows.Exteriors.SunShadowStealth` (bool) loads from `Shadows.ini` `[Exteriors] SunShadowStealth`, default 0.
- [ ] It is written on save, listed in the Shadows/Exteriors menu map, and settable from the menu.
- [ ] `SettingsMain.Develop.LogSunShadowStealth` (UInt8) loads from `[Develop] LogSunShadowStealth`, default 0.
- [ ] `Shadows.ini` ships `SunShadowStealth = 1` with a comment.
- [ ] The solution builds (exit 0).

**Verify:** the MSBuild command from Global Constraints → exit code 0, no `error` lines.

**Steps:**

- [ ] **Step 1: Struct fields.** In `SettingManager.h`, `DevelopStruct`, after `UInt8	NearShellDebug;` add:

```cpp
		UInt8	LogSunShadowStealth;	// logs the player sun shadow probe about once a second
```

In `ExteriorsStruct`, after `bool                UsePostProcessing;` add:

```cpp
		bool				SunShadowStealth;		// [Exteriors] SunShadowStealth: sun shadow lowers the player's light level
```

- [ ] **Step 2: Load.** In `SettingManager.cpp`, after the `NearShellDebug` load line (~442):

```cpp
	SettingsMain.Develop.LogSunShadowStealth = GetPrivateProfileIntA("Develop", "LogSunShadowStealth", 0, Filename);
```

After `SettingsShadows.Exteriors.UsePostProcessing = GetPrivateProfileIntA("Exteriors", "UsePostProcessing", 1, Filename);` (~1251):

```cpp
	SettingsShadows.Exteriors.SunShadowStealth = GetPrivateProfileIntA("Exteriors", "SunShadowStealth", 0, Filename);
```

- [ ] **Step 3: Save.** In the `"Shadows"` save branch, after the `UsePostProcessing` `WritePrivateProfileStringA` line (~1619):

```cpp
			WritePrivateProfileStringA("Exteriors", "SunShadowStealth", ToString(SettingsShadows.Exteriors.SunShadowStealth).c_str(), Filename);
```

- [ ] **Step 4: Menu map.** In the `"Shadows"`/`"Exteriors"` menu branch, after `Settings["UsePostProcessing"] = …;` (~2278):

```cpp
				Settings["SunShadowStealth"] = SettingsShadows.Exteriors.SunShadowStealth;
```

- [ ] **Step 5: Setter.** In `SetMenuSetting`'s `"Shadows"`/`"Exteriors"` chain, before the `else if (!strcmp(Setting, "UsePostProcessing"))` branch (~3069):

```cpp
				// UpdateSunShadowStealth creates or releases the probe on the next frame.
				else if (!strcmp(Setting, "SunShadowStealth")) {
					SettingsShadows.Exteriors.SunShadowStealth = Value;
				}
```

- [ ] **Step 6: Shipped INI.** In `OblivionReloaded/Shaders/Shadows/Shadows.ini`, `[Exteriors]`, directly after the `UsePostProcessing=1` line:

```ini
; SunShadowStealth: standing in sun or moon shadow lowers the player's light level for sneak detection
; by as much as the shadow visibly darkens them (the Darkness tier for the current weather).
; Needs UsePostProcessing. Torches and other point lights still count in full.
SunShadowStealth         = 1
```

- [ ] **Step 7: Build** with the MSBuild command from Global Constraints. Expected: exit 0.

- [ ] **Step 8: Commit**

```bash
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp OblivionReloaded/Shaders/Shadows/Shadows.ini
git commit -m "feat(Shadows): SunShadowStealth and LogSunShadowStealth settings"
```

```json:metadata
{"files": ["TESReloaded/Core/SettingManager.h", "TESReloaded/Core/SettingManager.cpp", "OblivionReloaded/Shaders/Shadows/Shadows.ini"], "verifyCommand": "PowerShell MSBuild TESReloaded.sln Release x86 /t:OblivionReloaded -> exit 0", "acceptanceCriteria": ["Exteriors.SunShadowStealth loads default 0", "saved, in menu map, settable", "Develop.LogSunShadowStealth loads default 0", "Shadows.ini ships SunShadowStealth = 1 with comment", "solution builds"], "modelTier": "mechanical"}
```

---

### Task 4: Light-level sun-term hook

**Goal:** Add the `SunShadowStealth` module with the 0x6561FD hook, which multiplies the player's sun term by a published scale (1.0 until Task 5 publishes one), and install it.

**Files:**
- Create: `TESReloaded/Core/SunShadowStealth.h`
- Create: `TESReloaded/Core/SunShadowStealth.cpp`
- Modify: `OblivionReloaded/Main.cpp:18,60`
- Modify: `OblivionReloaded/OblivionReloaded.vcxproj`, `OblivionReloaded/OblivionReloaded.vcxproj.filters`

**Acceptance Criteria:**
- [ ] `CreateSunShadowStealthHook()` patches the `call` at 0x6561FD to a naked stub that does `mov edx, edi` then jumps to a `__fastcall` wrapper.
- [ ] The wrapper calls the original 0x7D31B0 as a thiscall `(float, float, float, UInt32)` returning float, and returns `Term * PlayerSunLightScale` only when the actor is `Player`.
- [ ] The hook is installed from `Main.cpp` right after `CreateTreeCoverHook()`.
- [ ] Both files are in the vcxproj and filters; `compile_commands.json` is regenerated; the solution builds.

**Verify:** the MSBuild command → exit 0; `powershell -File Tools\GenerateCompileCommands.ps1` → exit 0.

**Steps:**

- [ ] **Step 1: Header** `TESReloaded/Core/SunShadowStealth.h` (Task 5 adds two more declarations):

```cpp
#pragma once

/// Sun shadow stealth: while the player stands in exterior sun (or moon) shadow, the sun's share of
/// their light level is scaled down by as much as the shadow visibly darkens them, measured by a GPU
/// probe of the sun shadow maps at points on the player's body. Oblivion only.

/// Wraps the sun term in HighProcess_GetLightLevel (0x655FE0). Always installed; the scale it applies
/// stays 1.0 unless SettingsShadows.Exteriors.SunShadowStealth is on and the sun maps are probed.
void CreateSunShadowStealthHook();
```

- [ ] **Step 2: Source** `TESReloaded/Core/SunShadowStealth.cpp`:

```cpp
#include "SunShadowStealth.h"

static const UInt32	kSunTermCall					= 0x006561FD; // HighProcess_GetLightLevel's exterior sun term
static const UInt32	kShadowSceneLightContribution	= 0x007D31B0;

static volatile float PlayerSunLightScale = 1.0f;
static volatile float LastPlayerSunTerm = 0.0f;

static float CallShadowSceneLightContribution(void* Light, float X, float Y, float Z, UInt32 Exclude) {

	class T {}; union { UInt32 x; float(T::* m)(float, float, float, UInt32); } u = { kShadowSceneLightContribution };
	return ((T*)Light->*u.m)(X, Y, Z, Exclude);

}

// Replaces the sun-term call; thiscall with callee cleanup, so __fastcall fits once the stub puts the actor in edx.
static float __fastcall SunTermHook(void* SunLight, Actor* Owner, float X, float Y, float Z, UInt32 Exclude) {

	float Term = CallShadowSceneLightContribution(SunLight, X, Y, Z, Exclude);
	if (Owner != Player) return Term;
	LastPlayerSunTerm = Term;
	return Term * PlayerSunLightScale;

}

// edi holds the actor for the whole of HighProcess_GetLightLevel.
static __declspec(naked) void SunTermStub() {

	__asm {
		mov		edx, edi
		jmp		SunTermHook
	}

}

void CreateSunShadowStealthHook() {

	WriteRelCall(kSunTermCall, (UInt32)SunTermStub);

}
```

If `Owner != Player` fails to compile because of the `PlayerCharacter*` → `Actor*` comparison, write `Owner != (Actor*)Player`.

- [ ] **Step 3: Install.** In `OblivionReloaded/Main.cpp`, add `#include "SunShadowStealth.h"` after `#include "TreeCover.h"` (line 18), and after `CreateTreeCoverHook();` (line 60):

```cpp
			CreateSunShadowStealthHook();
```

- [ ] **Step 4: Project files.** In `OblivionReloaded.vcxproj`, add `<ClInclude Include="..\TESReloaded\Core\SunShadowStealth.h" />` after the `TreeCoverMath.h` ClInclude, and `<ClCompile Include="..\TESReloaded\Core\SunShadowStealth.cpp" />` after the `TreeCover.cpp` ClCompile. In `OblivionReloaded.vcxproj.filters`, add after the matching TreeCover entries:

```xml
    <ClInclude Include="..\TESReloaded\Core\SunShadowStealth.h">
      <Filter>Core</Filter>
    </ClInclude>
```

```xml
    <ClCompile Include="..\TESReloaded\Core\SunShadowStealth.cpp">
      <Filter>Core</Filter>
    </ClCompile>
```

- [ ] **Step 5: Build and regenerate clangd data** (PowerShell): run the MSBuild command (expected exit 0), then `powershell -File Tools\GenerateCompileCommands.ps1` (expected exit 0; the output is gitignored).

- [ ] **Step 6: Disassemble the patch site** as a sanity check. The patched bytes live only in memory at runtime, so instead confirm statically that 0x6561FD is still a 5-byte `E8` call to 0x7D31B0 in the exe, and that `edi` isn't written between 0x656041 and 0x6561FD. Use Python + capstone (installed), reading `C:\Games\Steam\steamapps\common\Oblivion\Oblivion.exe` with the image base 0x400000 and the section table. The script must not be named `dis.py` (it shadows the stdlib module). Expected: `call 0x7d31b0` at 0x6561FD, and no instruction in 0x656041–0x6561FD has `edi` as a destination (`mov edi,`, `pop edi`, `lea edi,`, `xchg … edi`). Report what you find; if either fails, STOP and report rather than improvise.

- [ ] **Step 7: Commit**

```bash
git add TESReloaded/Core/SunShadowStealth.h TESReloaded/Core/SunShadowStealth.cpp OblivionReloaded/Main.cpp OblivionReloaded/OblivionReloaded.vcxproj OblivionReloaded/OblivionReloaded.vcxproj.filters
git commit -m "feat(Shadows): Hook the player's sun term in the light level"
```

```json:metadata
{"files": ["TESReloaded/Core/SunShadowStealth.h", "TESReloaded/Core/SunShadowStealth.cpp", "OblivionReloaded/Main.cpp", "OblivionReloaded/OblivionReloaded.vcxproj", "OblivionReloaded/OblivionReloaded.vcxproj.filters"], "verifyCommand": "PowerShell MSBuild TESReloaded.sln Release x86 /t:OblivionReloaded -> exit 0; capstone check call 0x7d31b0 at 0x6561FD and edi unchanged", "acceptanceCriteria": ["WriteRelCall 0x6561FD to naked stub mov edx,edi / jmp fastcall wrapper", "wrapper calls 0x7D31B0 thiscall float and scales only for Player", "installed after CreateTreeCoverHook", "vcxproj + filters updated, compile_commands regenerated, build passes"], "modelTier": "standard"}
```

---

### Task 5: Probe render, async readback and scale publication

**Goal:** Build the probe points, render the probe after the sun apply, read it back through the event-query ring, publish `PlayerSunLightScale`, gate and manage the resources, and log diagnostics.

**Files:**
- Modify: `TESReloaded/Core/SunShadowStealth.h`
- Modify: `TESReloaded/Core/SunShadowStealth.cpp`
- Modify: `TESReloaded/Core/ShaderManager.cpp:9` (include), `:2680` (`UpdateConstants`), `:3598-3601` (`RenderShadowsMidScene`)

**Acceptance Criteria:**
- [ ] `UpdateSunShadowStealth()` publishes 1.0 and discards in-flight probes whenever any gate fails: setting off, `UsePostProcessing` off, no `ShadowsExteriorsEffect`, no `Player`, no worldspace, `ShadowBiasAdaptive.w < 0.5`, or `ShadowLightDir.z <= 0`. Turning the setting off releases all probe resources.
- [ ] The probe effect and resources are created lazily, the first time the gate opens. A failed creation logs once and isn't retried until the setting is turned off and on again.
- [ ] `RenderSunShadowProbe()` reads back only slots whose event query returns `S_OK` (no GPU wait), skips rendering when the next slot is still pending, and uses a `GetParameterByName` handle for `SunShadowProbePoints`.
- [ ] `RenderShadowsMidScene` calls it right after the sun apply's `Render` and then restores render target 0 to `SceneRT`.
- [ ] Scale = `lerp(1, D, shadowed)` with `D = ShaderConst.Shadow.Data.y` (no dawn/dusk fade), clamped to [0, 1], with non-finite visibilities treated as lit.
- [ ] With `LogSunShadowStealth` set, one log line per second gives the visibilities, shadowed, D, L, scale, and the raw and scaled sun term. The first line also says whether `directionalLight` is in `ShadowSceneNode::lights`.
- [ ] The solution builds.

**Verify:** the MSBuild command → exit 0.

**Steps:**

- [ ] **Step 1: Declarations.** Append to `SunShadowStealth.h`:

```cpp

/// Gates the feature, creates or releases the probe resources, and publishes 1.0 whenever the sun
/// maps can't be probed. Main thread, from ShaderManager::UpdateConstants.
void UpdateSunShadowStealth();

/// Publishes the scale from any finished probe, then renders this frame's probe. Main thread, from
/// RenderShadowsMidScene straight after the sun shadow apply, with that apply's device state bound.
/// Changes render target 0; the caller restores it.
void RenderSunShadowProbe(IDirect3DDevice9* Device);
```

- [ ] **Step 2: Constants and state.** In `SunShadowStealth.cpp`, after the existing `static volatile float` lines, add:

```cpp
static const UInt32	kShadowSceneNode	= 0x00B42F54;
static const int	kProbePointCount	= 8;
static const int	kProbeRingSize		= 3;
static const int	kProbeHeightCount	= 4;
static const float	kProbeHeights[kProbeHeightCount] = { 0.2f, 0.45f, 0.7f, 0.95f }; // fractions of body height
static const float	kBodyRadius			= 25.0f;
static const float	kStandingHeight		= 120.0f;
static const float	kSneakingHeight		= 80.0f;
static const float	kLateralOffset		= 15.0f;
static const float	kExitMargin			= 10.0f;
static const UInt32	kMovementSneak		= 0x400;
static const DWORD	kLogIntervalMs		= 1000;

/// One in-flight probe: its render target and the event query issued after it was drawn.
struct ProbeSlot {
	IDirect3DSurface9*	Target;
	IDirect3DQuery9*	Done;
	bool				Pending;
};

static EffectRecord*		ProbeEffect = NULL;
static D3DXHANDLE			ProbePointsHandle = NULL;
static ProbeSlot			Ring[kProbeRingSize] = {};
static int					NextSlot = 0;
static IDirect3DSurface9*	ReadbackSurface = NULL;
static bool					ProbeFailed = false;
static bool					ProbeArmed = false;
static float				Visibility[kProbePointCount] = {};
static float				LastShadowed = 0.0f;
static DWORD				LastLogTick = 0;
static bool					LoggedLightListCheck = false;
```

- [ ] **Step 3: Resource lifecycle.** Add below the state:

```cpp
static void ReleaseProbeResources() {

	for (int i = 0; i < kProbeRingSize; i++) {
		if (Ring[i].Target) Ring[i].Target->Release();
		if (Ring[i].Done) Ring[i].Done->Release();
		Ring[i] = {};
	}
	if (ReadbackSurface) ReadbackSurface->Release();
	ReadbackSurface = NULL;
	if (ProbeEffect) TheShaderManager->DisposeEffect(ProbeEffect);
	ProbeEffect = NULL;
	ProbePointsHandle = NULL;
	NextSlot = 0;

}

static bool CreateProbeResources(IDirect3DDevice9* Device) {

	char Filename[MAX_PATH];
	strcpy(Filename, EffectsPath);
	strcat(Filename, "Shadows\\ShadowProbe.fx");
	ProbeEffect = new EffectRecord();
	if (!TheShaderManager->LoadEffect(ProbeEffect, Filename, NULL)) {
		ProbeEffect = NULL; // LoadEffect already disposed it
		return false;
	}
	ProbePointsHandle = ProbeEffect->Effect->GetParameterByName(NULL, "SunShadowProbePoints");
	if (!ProbePointsHandle) return false;
	for (int i = 0; i < kProbeRingSize; i++) {
		if (FAILED(Device->CreateRenderTarget(kProbePointCount, 1, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &Ring[i].Target, NULL))) return false;
		if (FAILED(Device->CreateQuery(D3DQUERYTYPE_EVENT, &Ring[i].Done))) return false;
	}
	return SUCCEEDED(Device->CreateOffscreenPlainSurface(kProbePointCount, 1, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &ReadbackSurface, NULL));

}
```

- [ ] **Step 4: Gate and per-update entry point.**

```cpp
static bool SunMapsProbeable() {

	SettingsShadowStruct::ExteriorsStruct* Exteriors = &TheSettingManager->SettingsShadows.Exteriors;
	ShaderConstants::ShadowMapStruct* ShadowMap = &TheShaderManager->ShaderConst.ShadowMap;
	return Exteriors->UsePostProcessing && TheShaderManager->ShadowsExteriorsEffect && Player && Player->GetWorldSpace()
		&& ShadowMap->ShadowBiasAdaptive.w >= 0.5f && ShadowMap->ShadowLightDir.z > 0.0f;

}

static void DiscardProbes() {

	for (int i = 0; i < kProbeRingSize; i++) Ring[i].Pending = false;
	PlayerSunLightScale = 1.0f;

}

void UpdateSunShadowStealth() {

	if (!TheSettingManager->SettingsShadows.Exteriors.SunShadowStealth) {
		ReleaseProbeResources();
		ProbeFailed = false;
		ProbeArmed = false;
		PlayerSunLightScale = 1.0f;
		return;
	}
	ProbeArmed = SunMapsProbeable();
	if (!ProbeArmed) {
		DiscardProbes();
		return;
	}
	if (!ProbeEffect && !ProbeFailed && !CreateProbeResources(TheRenderManager->device)) {
		Logger::Log("SunShadowStealth: probe resources unavailable (is Shadows\\ShadowProbe.fx compiled?); the player's light level stays vanilla.");
		ReleaseProbeResources();
		ProbeFailed = true;
	}

}
```

- [ ] **Step 5: Probe points.**

```cpp
// Each point is pushed along the sun direction until the sun ray has left a body capsule, so the
// player's own geometry in the actor shadow map never shadows it.
static void BuildProbePoints(D3DXVECTOR4* Points) {

	const D3DXVECTOR4& Sun = TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir;
	bool Sneaking = Player->process && (Player->process->GetMovementFlags() & kMovementSneak);
	float Height = (Sneaking ? kSneakingHeight : kStandingHeight) * Player->scale;
	float Radius = kBodyRadius * Player->scale;
	float Horizontal = sqrtf(Sun.x * Sun.x + Sun.y * Sun.y);
	float SideX = 1.0f;
	float SideY = 0.0f;
	if (Horizontal > 1e-3f) {
		SideX = -Sun.y / Horizontal;
		SideY = Sun.x / Horizontal;
	}
	for (int h = 0; h < kProbeHeightCount; h++) {
		float Z = Height * kProbeHeights[h];
		float Exit = Horizontal > 1e-3f ? Radius / Horizontal : FLT_MAX;
		Exit = min(Exit, (Height - Z) / Sun.z) + kExitMargin;
		for (int s = 0; s < 2; s++) {
			float Side = s ? kLateralOffset : -kLateralOffset;
			D3DXVECTOR4& P = Points[h * 2 + s];
			P.x = Player->pos.x + SideX * Side + Sun.x * Exit;
			P.y = Player->pos.y + SideY * Side + Sun.y * Exit;
			P.z = Player->pos.z + Z + Sun.z * Exit;
			P.w = 1.0f;
		}
	}

}
```

`Sun.z > 0` is guaranteed by the gate, so the division is safe. If `min` isn't available as a macro in this TU, use `std::min`.

- [ ] **Step 6: Scale publication and readback.**

```cpp
static void PublishScale() {

	float Sum = 0.0f;
	for (int i = 0; i < kProbePointCount; i++) {
		float V = Visibility[i];
		Sum += (V >= 0.0f && V <= 1.0f) ? V : 1.0f;
	}
	LastShadowed = 1.0f - Sum / kProbePointCount;
	float Darkness = TheShaderManager->ShaderConst.Shadow.Data.y;
	float Lit = std::clamp(TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir.w, Darkness, 1.0f);
	float ShadowRatio = Lit > 0.0f ? Darkness / Lit : 1.0f;
	float Scale = std::lerp(1.0f, ShadowRatio, LastShadowed);
	PlayerSunLightScale = (Scale >= 0.0f && Scale <= 1.0f) ? Scale : 1.0f;

}

// Oldest first; a slot is copied only once its event query reports the GPU is done with it.
static void ReadFinishedProbes(IDirect3DDevice9* Device) {

	for (int k = 0; k < kProbeRingSize; k++) {
		ProbeSlot& Slot = Ring[(NextSlot + k) % kProbeRingSize];
		if (!Slot.Pending) continue;
		if (Slot.Done->GetData(NULL, 0, 0) != S_OK) return;
		Slot.Pending = false;
		D3DLOCKED_RECT Locked;
		if (FAILED(Device->GetRenderTargetData(Slot.Target, ReadbackSurface))) continue;
		if (FAILED(ReadbackSurface->LockRect(&Locked, NULL, D3DLOCK_READONLY))) continue;
		memcpy(Visibility, Locked.pBits, sizeof(Visibility));
		ReadbackSurface->UnlockRect();
		PublishScale();
	}

}
```

- [ ] **Step 7: Diagnostics.**

```cpp
static void LogProbe() {

	if (!TheSettingManager->SettingsMain.Develop.LogSunShadowStealth) return;
	DWORD Now = GetTickCount();
	if (Now - LastLogTick < kLogIntervalMs) return;
	LastLogTick = Now;
	if (!LoggedLightListCheck) {
		LoggedLightListCheck = true;
		ShadowSceneNode* SceneNode = *(ShadowSceneNode**)kShadowSceneNode;
		bool Listed = false;
		if (SceneNode) {
			for (NiTList<ShadowSceneLight>::Entry* Entry = SceneNode->lights.start; Entry; Entry = Entry->next)
				if (Entry->data == SceneNode->directionalLight) Listed = true;
		}
		Logger::Log("SunShadowStealth: directional light %s in ShadowSceneNode::lights", Listed ? "IS (sun double-counted)" : "is not");
	}
	float Darkness = TheShaderManager->ShaderConst.Shadow.Data.y;
	float Lit = std::clamp(TheShaderManager->ShaderConst.ShadowMap.ShadowLightDir.w, Darkness, 1.0f);
	float Scale = PlayerSunLightScale;
	float Term = LastPlayerSunTerm;
	Logger::Log("SunShadowStealth: vis %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f shadowed %.2f D %.2f L %.2f scale %.2f sun term %.3f -> %.3f",
		Visibility[0], Visibility[1], Visibility[2], Visibility[3], Visibility[4], Visibility[5], Visibility[6], Visibility[7],
		LastShadowed, Darkness, Lit, Scale, Term, Term * Scale);

}
```

- [ ] **Step 8: Render entry point.**

```cpp
void RenderSunShadowProbe(IDirect3DDevice9* Device) {

	if (!ProbeArmed || !ProbeEffect || TheShaderManager->ShaderConst.ShadowMap.ShadowBiasAdaptive.w < 0.5f) return;
	ReadFinishedProbes(Device);
	LogProbe();
	ProbeSlot& Slot = Ring[NextSlot];
	if (Slot.Pending) return;

	D3DXVECTOR4 Points[kProbePointCount];
	BuildProbePoints(Points);
	if (FAILED(Device->SetRenderTarget(0, Slot.Target))) return;
	ProbeEffect->SetCT();
	ProbeEffect->Effect->SetVectorArray(ProbePointsHandle, Points, kProbePointCount);
	UINT Passes;
	ProbeEffect->Effect->Begin(&Passes, NULL);
	ProbeEffect->Effect->BeginPass(0);
	Device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	ProbeEffect->Effect->EndPass();
	ProbeEffect->Effect->End();
	Slot.Done->Issue(D3DISSUE_END);
	Slot.Pending = true;
	NextSlot = (NextSlot + 1) % kProbeRingSize;

}
```

- [ ] **Step 9: Wire into ShaderManager.** In `TESReloaded/Core/ShaderManager.cpp`, add `#include "SunShadowStealth.h"` after `#include "TreeCover.h"` (line 9). After `UpdateTreeCover();` (~line 2680) add:

```cpp
	UpdateSunShadowStealth();
```

In `RenderShadowsMidScene`, replace

```cpp
	if (DoSun) {
		ShadowsExteriorsEffect->SetCT();
		ShadowsExteriorsEffect->Render(Device, SceneRT, RenderedSurface, false);
	}
```

with

```cpp
	if (DoSun) {
		ShadowsExteriorsEffect->SetCT();
		ShadowsExteriorsEffect->Render(Device, SceneRT, RenderedSurface, false);
		RenderSunShadowProbe(Device);
		Device->SetRenderTarget(0, SceneRT);
	}
```

The state block the caller applies afterwards doesn't cover render targets, which is why target 0 is restored explicitly. It does cover the viewport that `SetRenderTarget` resets.

- [ ] **Step 10: Build** with the MSBuild command → exit 0. Fix any compile errors while keeping the logic above; report any deviation.

- [ ] **Step 11: Commit**

```bash
git add TESReloaded/Core/SunShadowStealth.h TESReloaded/Core/SunShadowStealth.cpp TESReloaded/Core/ShaderManager.cpp
git commit -m "feat(Shadows): Probe the player's sun shadow for their light level"
```

```json:metadata
{"files": ["TESReloaded/Core/SunShadowStealth.h", "TESReloaded/Core/SunShadowStealth.cpp", "TESReloaded/Core/ShaderManager.cpp"], "verifyCommand": "PowerShell MSBuild TESReloaded.sln Release x86 /t:OblivionReloaded -> exit 0", "acceptanceCriteria": ["gate failures publish 1.0 and discard in-flight probes; setting off releases resources", "lazy creation, single logged failure latch", "readback only on S_OK event query, skip when next slot pending, GetParameterByName handle", "RenderShadowsMidScene calls probe after sun apply and restores RT0 to SceneRT", "scale = lerp(1, D/clamp(w,D,1), shadowed) clamped with NaN guard", "1 Hz diagnostic log incl. directional-light-in-list check", "solution builds"], "modelTier": "standard"}
```

---

### Task 6: Play-test with the user and record results

**Goal:** Confirm the feature in game with the user, then write the planning refinements and the measured results back into the spec and memory.

**Files:**
- Modify: `docs/superpowers/specs/2026-09-27-sun-shadow-stealth-design.md`
- Modify: `memory/actor-light-level-formula.md`

**Acceptance Criteria:**
- [ ] The user has run the play-test checklist below with `[Develop] LogSunShadowStealth = 1`, and the log lines are captured.
- [ ] The first log line reports `directional light is not in ShadowSceneNode::lights`. If it says it IS, stop and redesign with the user (the sun would be counted twice).
- [ ] In clear noon sun, the logged `scale` is ≈ 1.0 on open ground and ≈ the clear Darkness (0.5 in the shipped INI) under a dense canopy or on a building's shaded side. It is ≈ 0.7 or 0.9 in the same shade in cloudy or rainy weather, is the same in 1st and 3rd person, and stays 1.0 in an interior (no log lines).
- [ ] The spec's "Components" section records planning refinements 1–6 from this plan's header. `actor-light-level-formula.md` records the runtime confirmation of the light-list finding.

**Verify:** the captured `OblivionReloaded.log` lines, as described in the criteria above.

**Steps:**

- [ ] **Step 1: Ask the user to set up and play.** The coordinator can't run the game. Ask the user to:
  1. Make sure `OblivionReloaded\Shaders\Shadows\ShadowProbe.fx` exists (Task 2 compiled it), or set `[Develop] CompileShaders = 1` for one launch.
  2. Add `LogSunShadowStealth = 1` under `[Develop]` in `Data\OBSE\Plugins\OblivionReloaded.ini`.
  3. Walk through the checklist: open ground vs. dense canopy vs. building shade at clear noon; the same shade in cloudy or rainy weather; toggle 1st/3rd person in shade; equip a torch in shade (the sun term drops, but the torch term isn't in the log line; check the sneak eye); enter an interior; toggle SunShadowStealth off in the menu (the scale reads 1.0 right away and log lines stop).
  4. Paste the `SunShadowStealth:` lines from `OblivionReloaded.log`.

- [ ] **Step 2: Evaluate against the Acceptance Criteria.** If the scale reads shadowed in open sun (self-shadowing), raise `kExitMargin` or `kBodyRadius` and re-test. If it never reads shadowed under obvious shade, check the log's `vis` values and whether `ShadowBiasAdaptive.w` is live. Report findings before changing any constant.

- [ ] **Step 3: Record.** Add a "Planning refinements" subsection at the end of the spec's Components section, listing refinements 1–6 from this plan's header verbatim. Add the play-test results under Testing. Append one line to `memory/actor-light-level-formula.md`: the light-list check result and the date.

- [ ] **Step 4: Commit** (docs only):

```bash
git add docs/superpowers/specs/2026-09-27-sun-shadow-stealth-design.md memory/actor-light-level-formula.md
git commit -m "docs: Record sun shadow stealth play-test"
```

```json:metadata
{"files": ["docs/superpowers/specs/2026-09-27-sun-shadow-stealth-design.md", "memory/actor-light-level-formula.md"], "verifyCommand": "User-run play-test; captured OblivionReloaded.log SunShadowStealth lines", "acceptanceCriteria": ["log captured with LogSunShadowStealth=1", "directional light not in SSN lights list", "scale ~1 open sun, ~Darkness in shade per weather tier, same 1st/3rd person, 1.0 interiors", "spec records refinements 1-6 and results; memory records light-list confirmation"], "modelTier": "standard"}
```
