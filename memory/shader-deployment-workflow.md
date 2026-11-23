---
name: shader-deployment-workflow
description: Shaders deploy via a directory symlink into the repo, so game-folder shader INI edits ARE repo edits; only DLL+PDB are copied by the build; .fx edits need CompileShaders=1
metadata:
  type: feedback
---

`C:\Games\Steam\steamapps\common\Oblivion\Data\Shaders\OblivionReloaded` is a **directory symlink** to
`<repo>\OblivionReloaded\Shaders`. So shader source and the per-effect INIs (`Shadows.ini`, `POM.ini`,
…) are deployed the moment they are edited in the repo — there is no copy step. The MSBuild post-build
event copies only `OblivionReloaded.dll` + `.pdb` to `Data\OBSE\Plugins\`. The main
`OblivionReloaded.ini` lives in that Plugins folder and is NOT in the repo.

**How to apply:**

- Don't write into the game folder's shader directory directly — edit the repo copy, which is the
  source of truth.
- When the user tunes a value in "the game folder's `Shadows.ini`", they are editing the **tracked repo
  file**. Expect it to show up in `git status`, and check its current value rather than assuming it
  matches the last commit.
- **Before committing a shader INI, diff it and check every value against what the commit intends.**
  An A/B run leaves the INI on whichever side was tested last, and a commit has already shipped the
  "off" side of an A/B this way. Stage INI edits on purpose, never through a blanket `git add -A` of
  the shader folder.
- After changing a `.hlsl`, say plainly that `[Develop] CompileShaders = 1` is required (code default
  0), or the stale compiled shader is used and an in-game test silently exercises the old code.
- Compiled shaders (`*.vso`, `*.pso`, `*.fxo`) are **gitignored** — the game writes them next to the
  `.hlsl` when compiling. Never plan to commit or review them; only the `.hlsl` sources ship.

Related: [[shader-pipeline-facts]], [[build-via-powershell-not-bash]].
