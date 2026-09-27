# Sun Shadow Stealth — Design

## Goal

When the player stands in sun (or moon) shadow in an exterior, lower their light level for sneak
detection by as much as the shadow visibly darkens them. Driven by the mod's own sun shadow maps, so
it matches what is on screen: foliage canopies, buildings, cliffs and other actors all count.

## Findings that shape the design

Statically RE'd 2026-09-27 (capstone + the IDA-derived PDB, memory `oblivion-pdb-symbols`).

- **Detection arg 5 is `HighProcess_GetLightLevel` 0x655FE0**, the target process's vfunc 0x3AC
  (also 0x3AC on MiddleHighProcess; Low/MiddleLow/Base use 0x60D080), thiscall `(Actor*, 0)`,
  `ret 8`, returning a float 0–100. Formula:
  `light = clamp((Σ point lights + T) × 100, 0, 100)`, where the point-light sum is distance falloff
  only (no occlusion) and `T` is the ambient or sun term:
  - interior (`TESObjectCELL_IsInterior` on the parent cell): `T` = max component of the sun
    NiLight's ambient colour, an out-param of `sub_7C6570`;
  - exterior: `T = sub_7D31B0(ShadowSceneNode->directionalLight, pos, 0)`, called at
    **0x6561FD**. For a directional light (SSL `+0xFC` == 0) this is `max(r,g,b)` of the light's
    diffuse, with **no occlusion at all**. That is why deep shade in daylight scores the same as open
    ground.
- **The actor is in `edi`** at 0x6561FD (loaded from the first stack arg at 0x656041, unchanged
  through the function). The call is thiscall, `ecx` = sun `ShadowSceneLight*`, stack =
  `float x, float y, float z, UInt32 exclude`, `ret 0x10`, result on `st(0)`.
- **The sun is not double-counted.** `ShadowSceneNode::directionalLight` (+0x118) is a separate SSL
  owned by the node: `sub_7C5850` allocates it (0x220 bytes) and binds the light via `0x7D3400`
  without inserting it into `lights` (+0xE4). Only the 0x6561FD call adds the sun, for the player
  and NPCs alike. To be confirmed once at runtime by the debug log (see Testing).
- **Other consumers** of vfunc 0x3AC: `AI_GetDetected` (0x4F64A9), `sub_5E8B80`, two
  `Actor_ProcessAction` attack/enchantment paths, `sub_60A640`, and the detection call 0x5F65E2.
  Wrapping at the source changes all of them consistently.
- **The sun shadow maps.** The static near/far cascades (cached, crossfaded) exclude actors; the
  per-frame MapSkin overlay holds actors, skinned geometry and (under `DynamicTrees`) trees, and the
  apply min-combines it with the near map. The apply (`ShadowsExteriors.fx.hlsl`) samples these with
  camera-relative `TESR_ShadowCameraToLightTransform*` matrices. Its lookups return
  `clamp(TESR_ShadowLightDir.w, darkness, 1)` for an occluded tap, with `darkness` = `ShadowData.y`
  (weather tier, fog-blended).
- **Threading.** `bUseThreadedAI=1` in the user's Oblivion.ini; which thread runs the light query is
  unverified (memory `sneak-detection-formula`). The hook must read only a main-thread-published
  scalar.

## Decisions

| Question | Decision |
|---|---|
| Which shadows | Sun/moon only, exteriors only. Interiors and point-light terms unchanged |
| When | Always, whether or not the player is sneaking. Wrapped at the source, so every light-level consumer agrees |
| Strength | Follows the visual shadow: `T' = T × lerp(1, Darkness, shadowed)` with the live, weather/fog-blended `ShadowData.y` |
| Shadow source | GPU probe of the existing sun shadow maps with an async readback. Havok rays miss foliage; CPU map readback stalls |
| Switch | `[Exteriors] SunShadowStealth` in `Shadows.ini` (code default 0, shipped 1), in-game menu entry |
| Tree Cover | Independent; its arg-5 scale multiplies after this one |

## Components

### 1. Engine hook — `TESReloaded/Core/SunShadowStealth.cpp/.h`

- `CreateSunShadowStealthHook()`: `WriteRelCall(0x6561FD, SunTermStub)`, always installed from
  `Main.cpp` next to `CreateTreeCoverHook()`, gated at runtime on the setting.
- `SunTermStub` is naked: `mov edx, edi` then `jmp` to a `__fastcall` wrapper
  `(ShadowSceneLight* Sun, Actor* Actor, float X, float Y, float Z, UInt32 Exclude)`. That signature
  matches the thiscall's callee cleanup (`ret 0x10`). The wrapper calls the original 0x7D31B0 and,
  when `Actor == Player`, multiplies the result by `PlayerSunLightScale`.
- `PlayerSunLightScale`: an aligned `static volatile float`, written only by the main thread. It is
  1.0 whenever the feature is off or no valid measurement exists.

### 2. Probe points (CPU, main thread)

- 8 points: 4 heights × 2 lateral offsets (±15 units perpendicular to the sun's horizontal
  direction). Heights are fractions of a standing (~128 × `Player->scale`) or sneaking (~90) body,
  chosen from the movement flags (sneak `0x400`, as in TreeCover).
- Each point is pushed **~35 units along the normalized sun direction** (`ShadowLightDir.xyz`). The
  player is drawn into MapSkin, so a point inside the body would read its own occlusion; a point on
  the sun side of the body is not occluded by it.
- Uploaded as camera-relative `float4 TESR_SunShadowProbe[8]` (position − camera, as the apply's
  `world_pos − TESR_CameraPosition`).

### 3. Probe shader — `OblivionReloaded/Shaders/Shadows/ShadowProbe.fx.hlsl`

- The cascade lookup (`LookupFar`, `GetLightAmountFar`, `CascadeCoverage`, `Lookup`,
  `GetLightAmountSkin`, `GetLightAmount`, `StaticTerm`) moves from `ShadowsExteriors.fx.hlsl` into a
  shared include, `ShadowsExteriorsLookup.hlsl`, used by both effects. The refactor must be
  **byte-identical**: compile `ShadowsExteriors` with fxc before and after and compare the output
  (memory `fxc-verify-shader-edits`).
- The probe pixel shader renders an 8×1 target: pixel *i* fetches `TESR_SunShadowProbe[i]` and
  runs the same near/far/skin lookup and static crossfade. Points have no surface normal, so it
  uses the depth bias alone (`ShadowBiasDeferred.zw`) with no normal offset and no
  facing/terminator ramp. It writes raw visibility, `(s − darkness) / max(1 − darkness, 1e-3)`
  clamped 0–1, so darkness is applied once, on the CPU.
- Loaded as its own effect record under the Shadows folder. It exists only when the feature and
  `Exteriors.UsePostProcessing` are both on.

### 4. Render + readback (ShaderManager, main thread)

- Runs right after the sun-shadow apply in `RenderShadowsMidScene()`, reusing its captured state and
  device setup, and only when that apply ran (`DoSun`) and `ShadowBiasAdaptive.w` (sun maps live)
  is set.
- Target: 8×1 `D3DFMT_R32F` render target, then `GetRenderTargetData` into a ring of 3
  `D3DPOOL_SYSTEMMEM` surfaces. Each frame reads the surface written two frames earlier with
  `LockRect(D3DLOCK_READONLY | D3DLOCK_DONOTWAIT)`. If it is still busy, the frame keeps the last
  value (never stalls).
- `shadowed = 1 − mean(visibility)`; publish
  `PlayerSunLightScale = lerp(1, ShadowData.y, shadowed)`.
- Snap to 1.0 and discard the ring's pending reads when any gate fails: feature off, interior,
  `UsePostProcessing` off, sun maps not live, no Player, or a device reset. A stale readback is never
  published, so there is no ramp-in lag on the frame the gate opens.
- Surfaces are created lazily and released with the other effect surfaces on device reset.

### 5. Settings

- `SettingsShadows.Exteriors.SunShadowStealth` (UInt8): INI load and save, `Settings[...]` menu map,
  and the setter branch, following the `UsePostProcessing` pattern in `SettingManager.cpp`. Shipped
  `Shadows.ini` gets the key with a comment.
- Toggling it at runtime creates or disposes the probe effect, like `ShadowsExteriors` does.

### 6. Diagnostics

- `[Develop] LogSunShadowStealth` (code default 0). When set, logs about once a second the
  player's vanilla sun term, the point-light sum, the probe visibilities, `shadowed` and the
  published scale. The first log line in an exterior also reports whether `directionalLight` appears
  in `ShadowSceneNode::lights`, closing the double-count question.

## Error handling

- Every D3D call's HRESULT is checked. On failure the feature publishes 1.0 (vanilla) and logs once.
- The hook never dereferences anything but its arguments and `Player`. `Player` NULL means no scale.

## Testing (no test harness in this repo)

1. Build (Release, x86). Recompile shaders (`[Develop] CompileShaders = 1`); byte-compare the
   refactored `ShadowsExteriors` against HEAD.
2. In game, with `LogSunShadowStealth = 1`:
   - clear noon, open ground vs. under a dense tree canopy vs. against the shaded side of a building:
     the logged scale ≈ 1.0, ≈ Darkness (0.5), ≈ Darkness;
   - same shade spots in cloudy and rainy weather: the scale tracks 0.7 and 0.9;
   - 1st and 3rd person give the same result (MapSkin self-shadow excluded by the sun-side offset);
   - with a torch equipped in shade, only the sun term drops;
   - enter an interior: scale 1.0 and no probe work;
   - toggle `SunShadowStealth` off in the menu: scale 1.0 immediately.
3. Detection sanity: the sneak eye icon and an NPC's reaction differ between a sunlit and a shaded
   approach at equal distance, sneaking at equal skill.
