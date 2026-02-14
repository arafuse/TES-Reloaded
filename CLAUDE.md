# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Oblivion Reloaded E3 Custom is a C++ graphics enhancement and gameplay mod for TES IV: Oblivion. It is a fork of [TES-Reloaded](https://github.com/mcstfuerson/TES-Reloaded), which is a multi-game framework also supporting Fallout: New Vegas and Skyrim. This fork focuses on Oblivion-specific customizations.

The plugin loads via OBSE (Oblivion Script Extender) and hooks into the game engine at runtime using Microsoft Detours and direct memory patching to intercept rendering, form loading, input, and other subsystems.

## Build

Must build via the solution file (not the .vcxproj directly) because `$(SolutionDir)` is used in force-include paths:

```
powershell -Command "& 'C:\Development\Microsoft\Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' 'C:\Users\Adam\Code\Oblivion\Oblivion Reloaded E3 Custom\TESReloaded.sln' /p:Configuration=Release /p:Platform=x86 /t:OblivionReloaded /v:minimal"
```

- **Platform:** x86 in the .sln (maps to Win32 in the .vcxproj)
- **Toolset:** v145 (Visual Studio 2019)
- **Output:** `OblivionReloaded\Release\OblivionReloaded.dll`
- **Preprocessor defines:** `OBLIVION` selects Oblivion-specific code paths (vs `NEWVEGAS` or `SKYRIM`)
- **Force-included header:** `TESReloaded/Framework/Framework.h` — pulled into every compilation unit automatically
- **External dependencies:** DirectX SDK (June 2010), d3dx9.lib, dxguid.lib, NVAPI
- **Post-build:** Copies DLL + PDB to `C:\Games\Steam\steamapps\common\Oblivion\Data\OBSE\Plugins\`

There are no tests or linting tools configured.

## Architecture

### Code Organization

```
TESReloaded/
  Framework/     Foundational utilities shared across all game targets
  Core/          Manager singletons, hooks, and feature modules
OblivionReloaded/
  Main.cpp       OBSE plugin entry point (OBSEPlugin_Query, OBSEPlugin_Load)
NewVegasReloaded/  (Not actively developed in this fork)
SkyrimReloaded/    (Not actively developed in this fork)
```

### Framework Layer (`TESReloaded/Framework/`)

- **Framework.h** — Master header force-included everywhere. Pulls in Windows, STL, DirectX, Detours, NVAPI, and all framework headers.
- **GameNi.h** — Gamebryo/NetImmerse engine class definitions (~4,500 lines). Reverse-engineered structs matching the game's memory layout.
- **Game.h/.cpp** — Game initialization hooks that capture pointers to engine singletons (renderer, player, scene graph, TES world, etc.) as they are created.
- **Types.h** — `ThisCall` templates for invoking game engine methods by raw address: `ThisCall(0x00804000, instance, arg1, arg2)`.
- **SafeWrite.h/.cpp** — Memory patching: `SafeWrite8/16/32()`, `WriteRelJump()`, `WriteRelCall()`.
- **Detours/** — Microsoft Detours library for runtime function hooking.

### Manager Singletons (`TESReloaded/Core/Managers.h`)

Global singletons declared as `The*Manager` (e.g., `TheShaderManager`, `TheSettingManager`). All are created during plugin load and accessed globally:

| Manager | Role |
|---------|------|
| **SettingManager** | Loads/manages INI configuration (`OblivionReloaded.ini`, weather INI) |
| **ShaderManager** | Manages 32 post-processing effect types (bloom, SMAA, TAA, god rays, shadows, etc.) and shader constants |
| **RenderManager** | Extends NiDX9Renderer; manages D3D9 pipeline, camera data, depth buffers |
| **ShadowManager** | Shadow map generation (near/far/interior/point light passes) |
| **TextureManager** | Render targets and sampler states for the post-processing pipeline |
| **EquipmentManager** | Weapon/shield positioning, dual-wielding, mounted combat |
| **CommandManager** | OBSE console command registration |
| **KeyboardManager** | Input handling |
| **FrameRateManager** | FPS timing |
| **GameMenuManager** | Game UI integration |
| **ScriptManager** | Script system hooks |

### Hook System

Hooks follow a consistent pattern using Detours:

1. **Address defines** — Each hooked function has a `#define k<Name> 0x00XXXXXX` with the Oblivion memory address (conditional on `OBLIVION`/`NEWVEGAS`/`SKYRIM`).
2. **Detour setup** — `DetourTransactionBegin()` / `DetourAttach()` / `DetourTransactionCommit()` in `Create*Hook()` functions.
3. **Direct patches** — `WriteRelJump()` / `WriteRelCall()` / `SafeWrite*()` for simpler redirections.

Key hook files:
- **RenderHook.cpp** — Main render pipeline, HDR, scene graph rendering
- **ShaderIOHook.cpp** (~15k lines) — Intercepts shader creation/loading; largest file in the codebase
- **FormHook.cpp** — Intercepts form/object loading (weather, water, animations)
- **Game.cpp** (~13k lines) — Engine initialization hooks, settings application

### Feature Modules (Conditionally Loaded)

Each has a `Create*Hook()` function called from `Main.cpp` based on INI settings:

GrassMode, CameraMode, EquipmentMode, MountedCombat, SleepingMode, Dodge, FlyCam, WeatherMode, Animation, MemoryManagement, D3D9Hook (debug)

### Multi-Game Conditionals

Game-specific code uses `#if defined(OBLIVION)` / `#elif defined(NEWVEGAS)` / `#elif defined(SKYRIM)`. This fork only builds the Oblivion target, but the shared code retains all three paths. When editing shared code in `TESReloaded/`, be aware that changes inside `#if defined(OBLIVION)` blocks only affect Oblivion.

### Plugin Load Sequence (`OblivionReloaded/Main.cpp`)

`OBSEPlugin_Load` orchestrates initialization:
1. Logger + CommandManager + SettingManager created
2. `PerformGameInitialization()` — hooks engine singleton creation
3. Core hooks installed: ShaderIO, Render, FormLoad, Settings, Script, Shadows, WeatherMode, Animation
4. Conditional feature hooks based on INI settings
5. Direct memory patches (antialiasing/HDR unlock, death reload timer)

### Debugging

Set `#define WaitForDebugger 1` in `Main.cpp` to spin until a debugger attaches. Enable `Develop.LogShaders` in the INI to activate D3D9 shader logging hooks.
