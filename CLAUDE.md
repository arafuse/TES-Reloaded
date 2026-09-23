# CLAUDE.md

This file provides guidance to coding agents when working with code in this repository.

## Project Overview

Oblivion Reloaded E3 Custom is a C++ graphics enhancement and gameplay mod for TES IV: Oblivion. It is a fork of [TES-Reloaded](https://github.com/mcstfuerson/TES-Reloaded), which is a multi-game framework also supporting Fallout: New Vegas and Skyrim. This fork focuses on Oblivion-specific customizations.

The plugin loads via OBSE (Oblivion Script Extender) and hooks into the game engine at runtime using Microsoft Detours and direct memory patching to intercept rendering, form loading, input, and other subsystems.

## Build

Must build via the solution file (not the .vcxproj directly) because `$(SolutionDir)` is used in force-include paths. Run it through the PowerShell tool, not Bash (see memory `build-via-powershell-not-bash`):

```
powershell -Command "& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal"
```

- **Platform:** x86 in the .sln (maps to Win32 in the .vcxproj)
- **Toolset:** v145 (Visual Studio 2026 / VS 18)
- **Output:** `OblivionReloaded\Release\OblivionReloaded.dll`
- **Preprocessor defines:** `OBLIVION` selects Oblivion-specific code paths (vs `NEWVEGAS` or `SKYRIM`)
- **Force-included header:** `TESReloaded/Framework/Framework.h` — pulled into every compilation unit automatically
- **External dependencies:** DirectX SDK (June 2010), d3dx9.lib, dxguid.lib, NVAPI
- **Post-build:** Copies DLL + PDB to `C:\Games\Steam\steamapps\common\Oblivion\Data\OBSE\Plugins\`

There are no tests or linting tools configured.

For clangd (the clangd-lsp plugin), generate the gitignored `compile_commands.json` with `powershell -File Tools\GenerateCompileCommands.ps1`; re-run it after adding source files (see memory `clangd-compile-commands`).

Shaders are not copied by the build: the game's `Data\Shaders\OblivionReloaded` is a directory symlink to `OblivionReloaded\Shaders`. Edited `.hlsl` only takes effect after a recompile (`[Develop] CompileShaders = 1`).

## Architecture

### Code Organization

```
TESReloaded/
  Framework/     Foundational utilities shared across all game targets
  Core/          Manager singletons, hooks, and feature modules
OblivionReloaded/
  Main.cpp       OBSE plugin entry point (OBSEPlugin_Query, OBSEPlugin_Load)
  Shaders/       HLSL sources, per-effect INIs, and compiled shader caches
NewVegasReloaded/  (Not actively developed in this fork)
SkyrimReloaded/    (Not actively developed in this fork)
```

### Framework Layer (`TESReloaded/Framework/`)

- **Framework.h** — Master header force-included everywhere. Pulls in Windows, STL, DirectX, Detours, NVAPI, Bink, and all framework headers.
- **Game.h** (~13k lines) / **GameNi.h** (~4.2k lines) / **GameHavok.h** — Reverse-engineered game, Gamebryo/NetImmerse, and Havok structs matching the game's memory layout. Game.h and GameNi.h define most structs three times (NEWVEGAS / OBLIVION / SKYRIM blocks); see memory `gameh-multigame-blocks`.
- **Game.cpp** — `PerformGameInitialization()`: hooks that capture pointers to engine singletons (`Tes`, `Player`, `WorldSceneGraph`, etc.) as they are created. They are NULL until then.
- **Types.h** — `ThisCall` templates for invoking game engine methods by raw address: `ThisCall(0x00804000, instance, arg1, arg2)`.
- **SafeWrite.h/.cpp** — Memory patching: `SafeWrite8/16/32()`, `WriteRelJump()`, `WriteRelCall()`.
- **Detours/** — Microsoft Detours library for runtime function hooking.

### Manager Singletons (`TESReloaded/Core/Managers.h`)

Global singletons declared as `The*Manager` (e.g., `TheShaderManager`, `TheSettingManager`). All are created during plugin load and accessed globally:

| Manager | Role |
|---------|------|
| **SettingManager** | Loads/manages INI configuration (`OblivionReloaded.ini`, weather INI, per-effect shader INIs); also hosts the game-settings hooks |
| **ShaderManager** | Shader/effect records and shader constants; runs the 23 post-processing effect types (`EffectRecordType`: bloom, SMAA, TAA, god rays, shadows, volumetric fog/light, etc.) |
| **RenderManager** | Extends NiDX9Renderer; manages D3D9 pipeline, camera data, depth buffers |
| **ShadowManager** | Shadow map generation (exterior near/far/ortho/skin maps, point-light cube maps) |
| **TextureManager** | Render targets and sampler states for the post-processing pipeline |
| **EquipmentManager** | Weapon/shield positioning and dual-wielding |
| **CommandManager** | OBSE console command registration |
| **KeyboardManager** | Input handling |
| **FrameRateManager** | FPS timing (`GetPerformance()`; see memory `framerate-elapsedtime-is-dead`) |
| **GameMenuManager** | In-game settings menu |
| **ScriptManager** | Script system hooks |

### Hook System

Hooks follow a consistent pattern using Detours:

1. **Address defines** — Each hooked function has a `#define k<Name> 0x00XXXXXX` with the Oblivion memory address (conditional on `OBLIVION`/`NEWVEGAS`/`SKYRIM`).
2. **Detour setup** — `DetourTransactionBegin()` / `DetourAttach()` / `DetourTransactionCommit()` in `Create*Hook()` functions.
3. **Direct patches** — `WriteRelJump()` / `WriteRelCall()` / `SafeWrite*()` for simpler redirections.

Key hook files:
- **RenderHook.cpp** — Main render pipeline: frame/scene begin, WorldSceneGraph render, per-draw shader setup (mid-scene shadow apply at the first near-water draw), HDR and image-space hooks
- **ShaderIOHook.cpp** — Intercepts shader creation/loading and flags shaders by name (e.g. near water, POM shadow writers)
- **FormHook.cpp** — Intercepts form loading (idles, weather, water)
- **SettingManager.cpp** (`CreateSettingsHook`) — Game setting reads, load game, save settings
- **ShadowManager.cpp** (`CreateShadowsHook`, `CreateEditorShadowsHook`) — Shadow map rendering

The largest files are Game.h, ShaderManager.cpp (~4.4k lines), and SettingManager.cpp (~3.7k lines).

### Feature Modules

Always installed: WeatherMode, Animation (plus the core hooks above).

Installed from `Main.cpp` based on INI settings: MemoryManagement, RagdollCollision (`Main.RagdollActorCollision`), WeatherSmoothing (`Main.WeatherMinTransitionTime > 0`), GrassMode, CameraMode, EquipmentMode (`CreateEquipmentHook` in EquipmentManager.cpp), MountedCombat (requires EquipmentMode), SleepingMode, Dodge, FlyCam, D3D9Hook (`Develop.LogShaders`).

Other Core modules: WindowedMode (applied from a setting in SettingManager), PluginVersion, FrameProfiler (per-bucket render timing, `Develop.ProfileFrame`), SampleProfiler (main-thread sampling profiler, `Develop.ProfileSampler`).

### Multi-Game Conditionals

Game-specific code uses `#if defined(OBLIVION)` / `#elif defined(NEWVEGAS)` / `#elif defined(SKYRIM)`. This fork only builds the Oblivion target, but the shared code retains all three paths. When editing shared code in `TESReloaded/`, be aware that changes inside `#if defined(OBLIVION)` blocks only affect Oblivion.

### Plugin Load Sequence (`OblivionReloaded/Main.cpp`)

`OBSEPlugin_Load` orchestrates initialization:
1. Logger + CommandManager created, console commands registered
2. In the editor, only `CreateEditorShadowsHook()` runs; the rest is game-only
3. PluginVersion string, SettingManager created and settings loaded
4. `PerformGameInitialization()` — hooks engine singleton creation
5. Core hooks installed: ShaderIO, Render, FormLoad, Settings, Script, Shadows, WeatherMode, Animation
6. Conditional feature hooks based on INI settings
7. Direct memory patches (antialiasing/HDR unlock, death reload timer)

### Debugging

Set `#define WaitForDebugger 1` in `Main.cpp` to spin until a debugger attaches. `Develop.LogShaders` (a key code) installs the D3D9 device logging hooks and, when pressed, logs one frame's shader passes. `Develop.ProfileFrame` and `Develop.ProfileSampler` enable the two profilers (see memory `sampling-profiler`). Output goes to `OblivionReloaded.log` in the game folder.

### Coding style

- Public symbols should have appropriate documentation comments.
- Code should be self-documenting; avoid inline code comments unless absolutely necessary. Inline comments must be kept to 1-3 lines maximum.

## Design docs and memory

The agent's memory directory is a symbolic link to `memory/` in this repo, so agent memory gets committed.

Changes to memory and design documents must be self-contained in their own `docs:` commits.
