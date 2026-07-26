# Adaptive Sun-Facing Shadow Bias Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the exterior sun-shadow bias adapt to whether a surface faces the sun, so back-face acne and sun-facing bleed-through stop trading against each other.

**Architecture:** The image-space apply shader `ShadowsExteriors.fx.hlsl` currently computes its slope-scaled bias from an *encoded* normal and an `abs()`'d light direction, so its `cosTheta` cannot tell sun-facing from sun-away. Fix the normal/light math to produce a real signed `N·L`, then use it three ways: a smoothstep terminator ramp that makes back-facing pixels take `darkness` directly without any depth compare, a clamped slope-scaled bias for both cascades, and a world-space normal offset specified in shadow-map texels. All of it sits behind an `AdaptiveBias` INI toggle so the old behavior stays reachable for A/B.

**Tech Stack:** HLSL (`fx_2_0` container, `vs_3_0`/`ps_3_0` shaders), C++ (MSVC v145, x86), D3D9 / DirectX SDK June 2010, OBSE plugin.

**Spec:** `docs/superpowers/specs/2026-07-25-adaptive-shadow-bias-design.md`

## Global Constraints

- **This repo has no automated tests and no linting.** Do not invent a test framework. Each task's verification is: (a) `fxc` compiles the effect, (b) MSBuild succeeds, (c) a stated in-game observation. The TDD "write a failing test" step is replaced by "establish the baseline you expect to change" — do not skip it, it is what makes each task falsifiable.
- **Build command** (must use the `.sln`, not the `.vcxproj` — `$(SolutionDir)` is used in force-include paths):
  ```
  "C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln" /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal
  ```
- **Shader compile check** (fxc is at the Windows 10 SDK path, and takes `-` style flags here, not `/`):
  ```
  "/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/fxc.exe" -T fx_2_0 -nologo "OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl" -Fo "$TMPDIR/shadowsexteriors.fxo"
  ```
  Baseline before any change: exit 0, `compilation object save succeeded`, and **285** pre-existing `warning X3570: gradient instruction used in a loop with varying iteration` lines, emitted from 3 call sites — the `tex2D` calls inside the PCF loops, which fxc unrolls. **X3570 count must not increase** — that is the signal that texture sampling accidentally landed inside a dynamic branch. Always count with `| grep -c X3570`; an earlier draft of this plan said 18, which came from reading a truncated `tail -20` rather than counting.
- **Force-included header:** `TESReloaded/Framework/Framework.h` is pulled into every translation unit automatically. Do not add includes for anything it already provides.
- **Multi-game conditionals:** this fork only builds Oblivion (`#if defined(OBLIVION)`). None of the files touched here need new conditionals — do not add any.
- **`Shadow.Data.x` is off limits.** It is used by unrelated code paths (`ShadowManager.cpp:590, 1002, 1644, 1682`). The new values go in a new `float4`, not in spare components of existing ones.
- **In-game changes require `Develop.CompileShaders = 1`** in the main INI for the `.fx` to be recompiled at load, plus the post-build step that copies the DLL to `C:\Games\Steam\steamapps\common\Oblivion\Data\OBSE\Plugins\`.
- **Commit after every task.** Do not batch.

---

## File Structure

| File | Responsibility | Change |
|------|---------------|--------|
| `TESReloaded/Core/SettingManager.h` | settings struct definitions | 3 new fields on `ExteriorsStruct` |
| `TESReloaded/Core/SettingManager.cpp` | INI read / write / JSON export / live edit | 4 sites |
| `TESReloaded/Core/ShaderManager.h` | shader constant storage | 1 new `D3DXVECTOR4` |
| `TESReloaded/Core/ShaderManager.cpp` | constant name binding | 1 new `strcmp` branch |
| `TESReloaded/Core/ShadowManager.h` | shadow manager interface | 1 new method declaration |
| `TESReloaded/Core/ShadowManager.cpp` | per-frame constant publish | rework `InitShadowBiasConstants`, call site in `RenderExteriorShadows` |
| `OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl` | image-space shadow apply | normal reconstruction, bias math, terminator ramp, toggle |
| `OblivionReloaded/Shaders/Shadows/Shadows.ini` | shipped defaults | 3 new keys, 4 retuned |

Task order is deliberate: **all CPU plumbing lands first as a verified no-op**, then the shader lands still behaving identically (toggle off), then a single INI flip turns the feature on. Each task is independently revertable, and the only visually-risky commit is the last one.

---

### Task 1: CPU plumbing (verified no-op)

Adds the settings, the new shader constant, and a per-frame publish that applies texel scaling to the normal-offset values. All of it is gated off, so this task must produce **zero** visual change.

**Files:**
- Modify: `TESReloaded/Core/SettingManager.h:326-329`
- Modify: `TESReloaded/Core/SettingManager.cpp:1252` (read), `:1583` (write), `:2231` (JSON), `:2972-2987` (live edit)
- Modify: `TESReloaded/Core/ShaderManager.h:68`
- Modify: `TESReloaded/Core/ShaderManager.cpp:350`
- Modify: `TESReloaded/Core/ShadowManager.h:101`
- Modify: `TESReloaded/Core/ShadowManager.cpp:167-173`, `:1181`
- Modify: `OblivionReloaded/Shaders/Shadows/Shadows.ini:16`
- Verify: build + in-game observation

**Interfaces:**
- Produces: `ShadowManager::PublishShadowBiasConstants(SettingsShadowStruct::ExteriorsStruct* Selected)` — publishes all four `ShadowBiasDeferred` components and the `x`/`y`/`z` components of `ShadowBiasAdaptive`. Task 2's shader reads them.
- Produces: `ShaderConst.ShadowMap.ShadowBiasAdaptive` (`D3DXVECTOR4`), bound to the HLSL name `TESR_ShadowBiasAdaptive`. Layout: `x` = terminator width, `y` = max slope clamp, `z` = enable (0 or 1), `w` = sun active (0 or 1).
- `w` is NOT written by `PublishShadowBiasConstants` — that function only runs on frames that already have sun, so it could never clear the flag. It is owned by `RenderExteriorShadows`, which sets it from `DoSun` on every exterior frame, before its `if (!DoOrtho && !DoSun) return;` early return. The apply effect runs on a broader condition than the shadow pass, so the shader ANDs the terminator ramp against this flag (`facing = max(facing, 1 - w)`); with `w = 0` the ramp disables itself, which is also the safe failure mode if nothing publishes it.
- Produces: `SettingsShadowStruct::ExteriorsStruct::AdaptiveBias` (`bool`), `::BiasTerminatorWidth` (`float`), `::BiasMaxSlope` (`float`).

- [ ] **Step 1: Establish the baseline you expect NOT to change**

Build the current tree unmodified and record the result, so "no change" in Step 12 is a claim about something you actually measured.

```
"C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln" /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal
```

Expected: `Build succeeded`, 0 errors. Note the warning count.

- [ ] **Step 2: Add the three settings fields**

In `TESReloaded/Core/SettingManager.h`, inside `struct ExteriorsStruct`, immediately after `float deferredFarConstBias;` (line 329):

```cpp
		bool				AdaptiveBias;			// 0 = legacy bias math (see Shadows.ini)
		float				BiasTerminatorWidth;	// smoothstep upper bound on N.L
		float				BiasMaxSlope;			// clamp on the slope-scaled bias multiplier
```

- [ ] **Step 3: Read the new keys from the INI**

In `TESReloaded/Core/SettingManager.cpp`, immediately after line 1252 (`SettingsShadows.Exteriors.deferredFarConstBias = atof(value);`):

```cpp
	// Defaults OFF: a Shadows.ini predating this feature must keep the legacy bias math. The
	// shipped INI carries the enable, so fresh installs still get it.
	SettingsShadows.Exteriors.AdaptiveBias = GetPrivateProfileIntA("Exteriors", "AdaptiveBias", 0, Filename);
	GetPrivateProfileStringA("Exteriors", "BiasTerminatorWidth", "0.15", value, SettingStringBuffer, Filename);
	SettingsShadows.Exteriors.BiasTerminatorWidth = atof(value);
	GetPrivateProfileStringA("Exteriors", "BiasMaxSlope", "4.0", value, SettingStringBuffer, Filename);
	SettingsShadows.Exteriors.BiasMaxSlope = atof(value);
```

**The `AdaptiveBias` code default must be `0`, not `1`.** The game reads its INI from
`Data\Shaders\OblivionReloaded\`, which is a deployed copy — the post-build step ships only the DLL
and PDB, so an existing install's Shadows.ini has no `AdaptiveBias` key. A default of `1` would
silently enable the feature there and change rendered shadows, breaking Task 1's defining no-op
property. A missing key must mean legacy; the shipped INI carries the enable (Task 3 flips it
to `1`), so fresh installs still get the feature.

**This must stay above line 1286.** That line copy-constructs `ExteriorsAlt` and `ExteriorsPrecip` from `Exteriors`, so any field read after it would be missing from the cloudy and precipitation weather tiers.

- [ ] **Step 4: Add the INI write-back**

In `TESReloaded/Core/SettingManager.cpp`, immediately after line 1583 (the `deferredFarConstBias` write):

```cpp
			WritePrivateProfileStringA("Exteriors", "AdaptiveBias", ToString(SettingsShadows.Exteriors.AdaptiveBias).c_str(), Filename);
			WritePrivateProfileStringA("Exteriors", "BiasTerminatorWidth", ToString(SettingsShadows.Exteriors.BiasTerminatorWidth).c_str(), Filename);
			WritePrivateProfileStringA("Exteriors", "BiasMaxSlope", ToString(SettingsShadows.Exteriors.BiasMaxSlope).c_str(), Filename);
```

- [ ] **Step 5: Add the JSON export**

In `TESReloaded/Core/SettingManager.cpp`, immediately after line 2231 (`Settings["deferredFarConstBias"] = ...`):

```cpp
					Settings["AdaptiveBias"] = SettingsShadows.Exteriors.AdaptiveBias;
					Settings["BiasTerminatorWidth"] = SettingsShadows.Exteriors.BiasTerminatorWidth;
					Settings["BiasMaxSlope"] = SettingsShadows.Exteriors.BiasMaxSlope;
```

- [ ] **Step 6: Rework the live-edit handlers**

In `TESReloaded/Core/SettingManager.cpp`, replace lines 2972-2987 entirely. The four existing handlers currently poke `ShaderConst.ShadowMap.ShadowBiasDeferred` directly; Step 9 makes that a per-frame publish, so a direct poke would be overwritten on the next frame. Drop the pokes and add the three new keys:

```cpp
					// The bias constants are republished every frame by
					// ShadowManager::PublishShadowBiasConstants, so these only update the setting.
					// Writing the shader constant here would be stomped on the next frame -- and
					// routing through the publish is what makes live edits take effect under the
					// cloudy/precipitation weather tiers too, which the old direct pokes did not.
					else if (!strcmp(Setting, "deferredNormBias")) {
						SettingsShadows.Exteriors.deferredNormBias = Value;
					}
					else if (!strcmp(Setting, "deferredFarNormBias")) {
						SettingsShadows.Exteriors.deferredFarNormBias = Value;
					}
					else if (!strcmp(Setting, "deferredConstBias")) {
						SettingsShadows.Exteriors.deferredConstBias = Value;
					}
					else if (!strcmp(Setting, "deferredFarConstBias")) {
						SettingsShadows.Exteriors.deferredFarConstBias = Value;
					}
					else if (!strcmp(Setting, "AdaptiveBias")) {
						SettingsShadows.Exteriors.AdaptiveBias = Value;
					}
					else if (!strcmp(Setting, "BiasTerminatorWidth")) {
						SettingsShadows.Exteriors.BiasTerminatorWidth = Value;
					}
					else if (!strcmp(Setting, "BiasMaxSlope")) {
						SettingsShadows.Exteriors.BiasMaxSlope = Value;
					}
```

- [ ] **Step 7: Add the shader constant storage**

In `TESReloaded/Core/ShaderManager.h`, inside `struct ShadowMapStruct`, immediately after `D3DXVECTOR4 ShadowBiasDeferred;` (line 68):

```cpp
		// x = terminator width, y = max slope clamp, z = adaptive enable (0/1), w = unused.
		D3DXVECTOR4		ShadowBiasAdaptive;
```

- [ ] **Step 8: Bind the constant by name**

In `TESReloaded/Core/ShaderManager.cpp`, immediately after line 350 (the `TESR_ShadowBiasDeferred` branch):

```cpp
	else if (!strcmp(Name, "TESR_ShadowBiasAdaptive"))
		FloatShaderValues[Index].Value = &TheShaderManager->ShaderConst.ShadowMap.ShadowBiasAdaptive;
```

Binding is by string lookup through effect reflection, so no register index needs assigning.

- [ ] **Step 9: Replace `InitShadowBiasConstants` with a reusable publish**

In `TESReloaded/Core/ShadowManager.h`, immediately after line 101 (`void InitShadowBiasConstants();`):

```cpp
	void					PublishShadowBiasConstants(SettingsShadowStruct::ExteriorsStruct* Selected);
```

In `TESReloaded/Core/ShadowManager.cpp`, replace the whole of `InitShadowBiasConstants` (lines 167-173) with:

```cpp
// Publish the deferred shadow bias constants. Two things are going on here:
//
// 1. The normal-offset values (.x/.y) are authored in shadow-map TEXELS, so they are scaled by each
//    cascade's world-units-per-texel (2 * Radius / Size). That keeps them correct if ShadowMapSize or
//    ShadowMapRadius is retuned. The legacy bias path wants the raw clip-space numbers instead, so the
//    scaling is skipped entirely when AdaptiveBias is off.
// 2. Bias is NOT weather-dependent, so the values come from the canonical Exteriors struct while only
//    the cascade geometry comes from the weather-selected one. This is also what makes live INI edits
//    take effect under the cloudy/precipitation tiers -- SelectExteriorShadowSettings returns a COPY
//    (ExteriorsAlt/ExteriorsPrecip, built at load), which the live-edit handlers never touch.
void ShadowManager::PublishShadowBiasConstants(SettingsShadowStruct::ExteriorsStruct* Selected) {
	auto& Ext = TheSettingManager->SettingsShadows.Exteriors;
	auto& Sm = TheShaderManager->ShaderConst.ShadowMap;

	float ScaleNear = 1.0f;
	float ScaleFar = 1.0f;
	if (Ext.AdaptiveBias) {
		if (Selected->ShadowMapSize[MapNear])
			ScaleNear = (2.0f * Selected->ShadowMapRadius[MapNear]) / (float)Selected->ShadowMapSize[MapNear];
		if (Selected->ShadowMapSize[MapFar])
			ScaleFar = (2.0f * Selected->ShadowMapRadius[MapFar]) / (float)Selected->ShadowMapSize[MapFar];
	}

	Sm.ShadowBiasDeferred.x = Ext.deferredNormBias * ScaleNear;
	Sm.ShadowBiasDeferred.y = Ext.deferredFarNormBias * ScaleFar;
	Sm.ShadowBiasDeferred.z = Ext.deferredConstBias;
	Sm.ShadowBiasDeferred.w = Ext.deferredFarConstBias;

	Sm.ShadowBiasAdaptive.x = Ext.BiasTerminatorWidth;
	Sm.ShadowBiasAdaptive.y = Ext.BiasMaxSlope;
	Sm.ShadowBiasAdaptive.z = Ext.AdaptiveBias ? 1.0f : 0.0f;
	Sm.ShadowBiasAdaptive.w = 0.0f;
}

// Startup publish. Uses the canonical Exteriors struct rather than SelectExteriorShadowSettings(),
// which depends on weather state that is not available this early.
void ShadowManager::InitShadowBiasConstants() {
	PublishShadowBiasConstants(&TheSettingManager->SettingsShadows.Exteriors);
}
```

- [ ] **Step 10: Republish every frame**

In `TESReloaded/Core/ShadowManager.cpp`, immediately after line 1181 (`ShadowData->w = 1.0f / (float)ShadowsExteriors->ShadowMapSize[MapFar];`), inside the same `if (DoSun)` block:

```cpp
		PublishShadowBiasConstants(ShadowsExteriors);
```

- [ ] **Step 11: Add the new keys to the shipped INI, gated OFF**

In `OblivionReloaded/Shaders/Shadows/Shadows.ini`, replace lines 13-16 with:

```ini
; Adaptive sun-facing bias. 0 = legacy bias math (encoded normal, abs()'d light dir, unclamped
; slope, clip-space normal offset). 1 = signed N.L, terminator ramp, clamped slope, world-space
; offset. The two deferred*NormBias keys change UNITS between the two paths -- see below.
AdaptiveBias            = 0
; Smoothstep upper bound on N.L. Below this, a surface is treated as facing away from the sun and
; takes the shadow term directly without a depth compare. Widen if grass/foliage speckles at
; dawn/dusk (their screen-space normals are the least reliable in the scene). AdaptiveBias only.
BiasTerminatorWidth     = 0.15
; Clamp on the slope-scaled bias multiplier. Final bias is deferred*ConstBias * (1 + slope), so
; this sets the peak at 1 + BiasMaxSlope times the constant. AdaptiveBias only.
BiasMaxSlope            = 4.0
; Normal offset. UNITS DEPEND ON AdaptiveBias: shadow-map texels when 1 (scaled on the CPU by each
; cascade's world-units-per-texel), raw clip-space when 0. Legacy values were -0.03 / -0.03.
deferredNormBias=-0.03
deferredFarNormBias=-0.03
; Constant depth bias, in normalized shadow-map depth.
deferredConstBias=0.0004
deferredFarConstBias=0.001
```

- [ ] **Step 12: Build and verify the no-op**

```
"C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln" /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal
```

Expected: `Build succeeded`, 0 errors, warning count unchanged from Step 1.

In-game expectation: **shadows look exactly as they did before.** `AdaptiveBias = 0` means `ScaleNear`/`ScaleFar` stay at 1.0, so `ShadowBiasDeferred` holds the same four values it always did, and `ShadowBiasAdaptive` is not referenced by any shader yet. If anything looks different, stop — the publish path has a bug and Task 2 will be built on sand.

- [ ] **Step 13: Commit**

```bash
git add TESReloaded/Core/SettingManager.h TESReloaded/Core/SettingManager.cpp \
        TESReloaded/Core/ShaderManager.h TESReloaded/Core/ShaderManager.cpp \
        TESReloaded/Core/ShadowManager.h TESReloaded/Core/ShadowManager.cpp \
        OblivionReloaded/Shaders/Shadows/Shadows.ini
git commit -m "feat(Shadows): plumbing for adaptive sun-facing shadow bias

Adds AdaptiveBias/BiasTerminatorWidth/BiasMaxSlope settings and the
TESR_ShadowBiasAdaptive constant, and moves the deferred bias constants
to a per-frame publish that scales the normal-offset values from texels
to world units per cascade.

Gated off (AdaptiveBias = 0), so this is a no-op: the scaling is skipped
and no shader reads the new constant yet.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EUVNS2C6XDg8GY3WS2hjet"
```

---

### Task 2: Shader rewrite behind the toggle

Rewrites the bias math with both paths present. Still `AdaptiveBias = 0`, so this task must also produce zero visual change — the legacy branch is reconstructed to be bit-exact.

**Files:**
- Modify: `OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl:46` (declaration), `:107-123` (normals), `:126-155` (far lookup), `:197-227` (light amount), `:229-269` (main)
- Verify: fxc + build + in-game observation

**Interfaces:**
- Consumes: `TESR_ShadowBiasAdaptive` from Task 1 (`x` = terminator width, `y` = max slope clamp, `z` = enable, `w` = unused), and `TESR_ShadowBiasDeferred` with `.x`/`.y` pre-scaled to world units when enabled.
- Produces: `getRawNormal(float2 UVCoord)` returning `float3` — the bare `normalize(cross(dx, dy))`, sign unresolved. Replaces `getNormals`.
- Produces: `LookupFar(float4 ShadowPos, float2 OffSet, float bias)`, `GetLightAmountFar(float4 ShadowPos, float bias)`, and `GetLightAmount(float4 WorldPos, float4 ShadowPos, float4 ShadowPosFar, float4 ShadowPosSkin, float biasNear, float biasFar)` — all gain explicit bias parameters.

- [ ] **Step 1: Capture the fxc baseline**

```bash
cd "/c/Users/Adam/Code/Oblivion/Oblivion Reloaded E3 Custom"
"/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/fxc.exe" -T fx_2_0 -nologo \
  "OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl" -Fo "$TMPDIR/baseline.fxo" 2>&1 \
  | grep -c X3570
```

Expected: `285` (3 call sites, unrolled). Record it — Step 7 checks this number did not grow.

- [ ] **Step 2: Declare the new constant**

In `ShadowsExteriors.fx.hlsl`, immediately after line 46 (`float4 TESR_ShadowBiasDeferred;`):

```hlsl
float4 TESR_ShadowBiasAdaptive; // x = terminator width, y = max slope clamp, z = adaptive enable, w = unused
```

- [ ] **Step 3: Replace `getNormals` with `getRawNormal`**

Replace lines 107-123 in full:

```hlsl
// Screen-space normal reconstruction: the world-space cross of the depth derivatives. The SIGN is
// deliberately left unresolved -- the cross product's orientation depends on tap winding, and the two
// bias paths disambiguate it differently (the adaptive path flips toward the camera, the legacy path
// mirrors z and encodes). Both derive from this single result so the depth taps happen only once.
float3 getRawNormal(float2 UVCoord)
{
	float depth = readDepth(UVCoord);
	float3 pos = getPosition(UVCoord, depth);

	float3 left = pos - getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(-1, 0), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(-1, 0)));
	float3 right = getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(1, 0), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(1, 0))) - pos;
	float3 up = pos - getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(0, -1), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(0, -1)));
	float3 down = getPosition(UVCoord + TESR_ReciprocalResolution.xy * float2(0, 1), readDepth(UVCoord + TESR_ReciprocalResolution.xy * float2(0, 1))) - pos;
	// Shorter derivative wins: at a silhouette the far side spans a depth discontinuity.
	float3 dx = length(left) < length(right) ? left : right;
	float3 dy = length(up) < length(down) ? up : down;

	return normalize(cross(dx, dy));
}
```

- [ ] **Step 4: Give the far cascade a real bias parameter**

Replace lines 126-132 (the `LookupFar` body and the `GetLightAmountFar` signature). `LookupFar` currently hardcodes `TESR_ShadowBiasDeferred.w` and never sees a slope term, despite the far cascade being 4x coarser per texel than near:

```hlsl
float LookupFar(float4 ShadowPos, float2 OffSet, float bias) {
	float Shadow = tex2D(TESR_ShadowMapBufferFar, ShadowPos.xy + float2(OffSet.x * TESR_ShadowData.w, OffSet.y * TESR_ShadowData.w)).r;
	if (Shadow < ShadowPos.z - bias) return darkness;
	return clamp(TESR_ShadowLightDir.w, darkness, 1.0f);
}

float GetLightAmountFar(float4 ShadowPos, float bias) {
```

Then in the `GetLightAmountFar` body, replace line 149:

```hlsl
			Shadow += LookupFar(ShadowPos, float2(x, y), bias);
```

- [ ] **Step 5: Thread both biases through `GetLightAmount`**

Replace line 197 (the signature):

```hlsl
float GetLightAmount(float4 WorldPos, float4 ShadowPos, float4 ShadowPosFar, float4 ShadowPosSkin, float biasNear, float biasFar) {
```

Replace line 208 (the out-of-near-bounds fallback):

```hlsl
		return GetLightAmountFar(ShadowPosFar, biasFar);
```

Replace line 216 (the near PCF tap):

```hlsl
			Shadow += Lookup(ShadowPos, float2(x, y), biasNear);
```

Replace line 223 (the actor overlay — it samples a map allocated at the NEAR resolution, so it takes the near bias):

```hlsl
	Shadow = min(Shadow, GetLightAmountSkin(ShadowPosSkin, biasNear));
```

- [ ] **Step 6: Rewrite the main shader body**

Replace lines 246-266 (the whole `if (world_pos.z > ...)` block) in full. Note the structure: the branch produces **only scalars and positions**. Every `tex2D` stays outside it, on a single shared call site — that is what keeps the X3570 gradient-warning count from growing.

```hlsl
	if (world_pos.z > -2147483000.0f) { // pre-water depth excludes the water surface; gate effectively off (sky handled by the rawDepth early-out + GetLightAmount frustum bounds)
		float fogCoeff = (saturate((distance(world_pos, TESR_CameraPosition.xyz) - ((TESR_FogData.y - 2000))) / 1000)) + 1.0f;
		float4 world_pos_trans = mul(world_pos, TESR_WorldTransform);
		float3 raw = getRawNormal(IN.UVCoord);

		float4 posNear;
		float4 posFar;
		float biasNear;
		float biasFar;
		float facing;

		if (TESR_ShadowBiasAdaptive.z > 0.5f) {
			// Resolve the reconstruction's sign: a visible surface must face the camera.
			float3 viewRay = normalize(toWorld(IN.UVCoord));
			float3 N = (dot(raw, viewRay) > 0.0f) ? -raw : raw;
			float3 L = normalize(TESR_ShadowLightDir.xyz); // points TOWARD the sun; no abs()
			float ndl = dot(N, L);

			// A surface pointing away from the sun is self-shadowed by its own geometry. Ramp it to
			// the shadow term directly -- it never reaches the depth compare, so no bias value can
			// produce acne there. Smoothstep rather than a hard cutoff because the reconstructed
			// normals are noisy and a binary test would speckle along the terminator.
			// The max() guards BiasTerminatorWidth = 0, which would otherwise divide by zero inside
			// smoothstep and produce NaN; at 1e-4 it degenerates to a hard cutoff, as intended.
			facing = smoothstep(0.0f, max(TESR_ShadowBiasAdaptive.x, 1e-4f), ndl);

			// sqrt(1-x*x)/x == tan(acos(x)), without the transcendentals and clamped. The unclamped
			// form diverges at grazing angles, which is what the legacy path below still does.
			float ndlSafe = max(ndl, 0.05f);
			float slope = min(sqrt(1.0f - ndlSafe * ndlSafe) / ndlSafe, TESR_ShadowBiasAdaptive.y);
			biasNear = TESR_ShadowBiasDeferred.z * (1.0f + slope);
			biasFar = TESR_ShadowBiasDeferred.w * (1.0f + slope);

			// World-space normal offset, growing with tilt, applied BEFORE the transform chain.
			// TESR_ShadowBiasDeferred.x/.y arrive pre-scaled from texels to world units per cascade.
			float sinT = sqrt(saturate(1.0f - ndl * ndl));
			posNear = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.x * sinT, 1.0f), TESR_WorldViewProjectionTransform);
			posFar = mul(float4(world_pos.xyz + N * TESR_ShadowBiasDeferred.y * sinT, 1.0f), TESR_WorldViewProjectionTransform);
		}
		else {
			// Legacy path, bit-exact with the pre-adaptive shader: encoded normal mirrored in z, an
			// abs()'d light dir (both of which force everything into the positive octant, which is
			// why cosTheta could never tell sun-facing from sun-away), unclamped slope, flat far
			// bias, and a normal offset applied in CLIP space after the WVP multiply.
			float4 normal = float4((float3(raw.x, raw.y, -raw.z) + 1) / 2, 1);
			float4 lightDir = abs(TESR_ShadowLightDir);
			float3 n = normalize(normal);
			float3 l = normalize(lightDir);
			float cosTheta = clamp(dot(n, l), 0, 1);
			biasNear = TESR_ShadowBiasDeferred.z * tan(acos(cosTheta));
			biasFar = TESR_ShadowBiasDeferred.w;

			float4 pos = mul(world_pos, TESR_WorldViewProjectionTransform);
			posNear = pos;
			posFar = pos;
			posNear.xyz = posNear.xyz + (normal.xyz * TESR_ShadowBiasDeferred.x);
			posFar.xyz = posFar.xyz + (normal.xyz * TESR_ShadowBiasDeferred.y);
			facing = 1.0f; // no terminator ramp: lerp below collapses to the raw map term
		}

		float4 ShadowNear = mul(posNear, TESR_ShadowCameraToLightTransformNear);
		float4 ShadowFar = mul(posFar, TESR_ShadowCameraToLightTransformFar);
		float4 ShadowSkin = mul(posNear, TESR_ShadowCameraToLightTransformSkin);
		float mapShadow = GetLightAmount(world_pos_trans, ShadowNear, ShadowFar, ShadowSkin, biasNear, biasFar);

		// `darkness` is exactly what Lookup returns on its shadowed branch, so the forced-shadow
		// path and the map path agree. facing == 1 collapses this to mapShadow.
		float Shadow = lerp(darkness, mapShadow, facing);
		color.rgb *= saturate(Shadow * fogCoeff) * float3(1.0f, 1.0f, 1.0f);
	}
```

- [ ] **Step 7: Compile the shader and check the warning count did not grow**

```bash
cd "/c/Users/Adam/Code/Oblivion/Oblivion Reloaded E3 Custom"
"/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x86/fxc.exe" -T fx_2_0 -nologo \
  "OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl" -Fo "$TMPDIR/adaptive.fxo" 2>&1 \
  | tee "$TMPDIR/fxc.log" | grep -c X3570
echo "EXIT=$?"; grep -E "error|succeeded" "$TMPDIR/fxc.log"
```

Expected: exit 0, `compilation object save succeeded`, no `error` lines, and the X3570 count still `285` (the Step 1 baseline). **If the count grew, a `tex2D` ended up inside the `TESR_ShadowBiasAdaptive.z` branch** — move it back out to the shared call site rather than suppressing the warning.

- [ ] **Step 8: Build**

```
"C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln" /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal
```

Expected: `Build succeeded`, 0 errors. (The C++ side is untouched in this task; this confirms nothing else references `getNormals`.)

- [ ] **Step 9: Verify the legacy path in-game**

With `Develop.CompileShaders = 1` and `AdaptiveBias` still `0`, load an exterior in clear daytime weather.

Expected: **shadows identical to Task 1.** The legacy branch reproduces the original math exactly, including the `norm.z *= -1` mirror and the `(n+1)/2` encoding. Any visible difference means the legacy reconstruction is wrong — fix it here, because Task 3 relies on this branch being a trustworthy A/B reference.

- [ ] **Step 10: Commit**

```bash
git add OblivionReloaded/Shaders/Shadows/ShadowsExteriors.fx.hlsl
git commit -m "feat(Shadows): adaptive sun-facing bias math in the apply shader

Replaces getNormals with getRawNormal (unencoded, sign unresolved) and
adds an adaptive branch: signed N.L from a camera-disambiguated normal
and a non-abs()'d light dir, a smoothstep terminator ramp that takes
back-facing pixels straight to darkness without a depth compare, a
clamped slope-scaled bias now threaded through both cascades, and a
world-space normal offset.

The branch produces only scalars and positions; all texture sampling
stays on a shared call site outside it. Legacy path reconstructed
bit-exact and still selected (AdaptiveBias = 0), so no visual change.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EUVNS2C6XDg8GY3WS2hjet"
```

---

### Task 3: Enable and retune

Flips the toggle and retunes the four bias values. This is the only task with an intended visual change.

**Files:**
- Modify: `OblivionReloaded/Shaders/Shadows/Shadows.ini:16-25` (the block written in Task 1 Step 11)
- Verify: in-game A/B

**Interfaces:**
- Consumes: everything from Tasks 1 and 2. No code changes in this task.

- [ ] **Step 1: Confirm the A/B reference still holds**

Before changing anything, load an exterior in clear daytime weather with `AdaptiveBias = 0` and find a spot showing **both** symptoms the spec describes: acne on a surface facing away from the sun, and (after lowering `deferredConstBias`) bleed-through on a sun-facing surface. Note the location so you can return to it.

This is the falsifiable baseline. Without it, "looks better" in Step 3 is not a claim about anything.

- [ ] **Step 2: Flip the toggle and apply the new defaults**

In `OblivionReloaded/Shaders/Shadows/Shadows.ini`, set:

```ini
AdaptiveBias            = 1
BiasTerminatorWidth     = 0.15
BiasMaxSlope            = 4.0
deferredNormBias        = 1.5
deferredFarNormBias     = 1.5
deferredConstBias       = 0.00015
deferredFarConstBias    = 0.0004
```

The normal-offset keys are now **texels**, not clip-space — the CPU scaling from Task 1 Step 9 activates with the toggle. The const biases drop because the peak is now `const * (1 + BiasMaxSlope)` = 5x the constant, where the legacy path's `tan(acos())` was unclamped.

Leave the pre-change values recorded in the spec (`docs/superpowers/specs/2026-07-25-adaptive-shadow-bias-design.md`, §8) — reverting `AdaptiveBias` to `0` alone is **not** a fair A/B, because the two normal-offset keys mean different things on each path.

- [ ] **Step 3: Verify in-game at the Step 1 location**

Expected:
- Acne on sun-away surfaces is **gone**, and stays gone as `deferredConstBias` is lowered further — those pixels no longer reach a depth compare at all.
- No shadow bleed-through on sun-facing surfaces at the lowered `deferredConstBias`.
- No dark speckling on grass or foliage, and no dark rims on object silhouettes. If there is speckling, widen `BiasTerminatorWidth` first (0.15 → 0.25 → 0.35); the spec's deferred normal-confidence gate is the fallback if widening alone does not settle it.
- Shadows do not detach from their casters (peter-panning). If they do, `deferredNormBias` is too high — it is in texels now, so 1.5 means a 1.5-texel push.

These INI values are starting points. Tune them at this step and commit whatever lands.

- [ ] **Step 4: Commit**

```bash
git add OblivionReloaded/Shaders/Shadows/Shadows.ini
git commit -m "feat(Shadows): enable adaptive sun-facing bias by default

Flips AdaptiveBias on and retunes the four bias values for the new math:
normal offsets are now in shadow-map texels (CPU-scaled per cascade),
and the const biases drop because the slope multiplier is clamped at
1 + BiasMaxSlope rather than the legacy unclamped tan(acos()).

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EUVNS2C6XDg8GY3WS2hjet"
```

---

## Notes for the implementer

**Why the legacy branch exists at all.** It is not defensive programming — it is the measuring instrument. Task 2 is verified entirely by "in-game output did not change", which only works if the legacy branch is bit-exact. Do not simplify it, and do not let it drift.

**Why `facing` is computed in the branch rather than at the call site.** The legacy path sets `facing = 1.0f`, which makes `lerp(darkness, mapShadow, facing)` collapse to `mapShadow` exactly. Hoisting the smoothstep out of the branch would apply the terminator ramp to the legacy path too, breaking the bit-exactness Task 2 depends on.

**Grass is a shadow receiver here.** The apply pass runs mid-scene, right before the first near-water draw, and `TESR_DepthBufferPreWater` is resolved at that same moment — so it holds land, grass, and the submerged floor. Depth-derived normals on grass blades are the least reliable in the frame, which is why the terminator is a ramp and not a cutoff.

**`SelectExteriorShadowSettings()` returns a copy.** `ExteriorsAlt` and `ExteriorsPrecip` are copy-constructed from `Exteriors` once at load (`SettingManager.cpp:1286-1289`). Live-edit handlers only mutate `Exteriors`. That is why `PublishShadowBiasConstants` deliberately reads bias from the canonical struct and takes only cascade geometry from the selected one.
